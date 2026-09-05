// The arm64 encoder: one instruction of text, four bytes out.
//
// Only what a person actually writes when patching is here. That is a smaller
// set than the architecture: a patch replaces a branch, blanks an instruction,
// forces a register to a value, or returns early. Anything not recognised is
// refused by name rather than silently encoded wrong, because a wrong
// instruction in a patched binary is far worse than a refusal.
#include "text.hh"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace astral_internal {
namespace assembler {
namespace {

struct Register {
    int number = 0;
    bool wide = false; // x rather than w
    bool stack = false;
};

bool parse_register(const std::string &text, Register &out)
{
    const std::string name = lower(trim(text));
    if (name == "sp" || name == "xzr" || name == "wzr") {
        out.number = 31;
        out.wide = name != "wzr";
        out.stack = name == "sp";
        return true;
    }
    if (name.size() < 2 || (name[0] != 'x' && name[0] != 'w'))
        return false;
    for (size_t i = 1; i < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i])))
            return false;
    const int number = std::atoi(name.c_str() + 1);
    if (number < 0 || number > 30)
        return false;
    out.number = number;
    out.wide = name[0] == 'x';
    out.stack = false;
    return true;
}

// An address operand, written as a bare number: a listing prints branch targets
// that way, and so does anyone editing one.
bool parse_address(const std::string &text, uint64_t &out)
{
    int64_t value = 0;
    if (!parse_immediate(text, value))
        return false;
    out = static_cast<uint64_t>(value);
    return true;
}

uint32_t sf_bit(const Register &r) { return r.wide ? 0x80000000u : 0u; }

// The condition codes, in the order the architecture numbers them.
const std::map<std::string, uint32_t> &conditions()
{
    static const std::map<std::string, uint32_t> table = {
        {"eq", 0}, {"ne", 1}, {"cs", 2},  {"hs", 2}, {"cc", 3}, {"lo", 3},
        {"mi", 4}, {"pl", 5}, {"vs", 6},  {"vc", 7}, {"hi", 8}, {"ls", 9},
        {"ge", 10}, {"lt", 11}, {"gt", 12}, {"le", 13}, {"al", 14},
    };
    return table;
}

// MOVZ/MOVK build any 64-bit value sixteen bits at a time.
Result move_immediate(const Register &destination, int64_t value)
{
    uint64_t bits = static_cast<uint64_t>(value);
    if (!destination.wide) {
        if (value < -(int64_t(1) << 31) || value > 0xffffffffLL)
            return fail("the value does not fit in a 32-bit register");
        bits &= 0xffffffffULL;
    }
    const int chunks = destination.wide ? 4 : 2;
    Result result;
    result.ok = true;
    if (bits == 0) {
        // Zero is one MOVZ with nothing shifted, not a skipped chunk followed
        // by a shifted one.
        return word_of(sf_bit(destination) | 0x52800000u |
                        static_cast<uint32_t>(destination.number));
    }
    bool first = true;
    for (int i = 0; i < chunks; ++i) {
        const uint16_t chunk = static_cast<uint16_t>((bits >> (i * 16)) & 0xffff);
        if (chunk == 0 && !(first && i == chunks - 1))
            continue;
        const uint32_t opcode = first ? 0x52800000u : 0x72800000u; // MOVZ : MOVK
        const uint32_t word = sf_bit(destination) | opcode | (static_cast<uint32_t>(i) << 21) |
                              (static_cast<uint32_t>(chunk) << 5) |
                              static_cast<uint32_t>(destination.number);
        const Result one = word_of(word);
        result.bytes.insert(result.bytes.end(), one.bytes.begin(), one.bytes.end());
        first = false;
    }
    return result;
}


// A memory operand, written the way a listing prints one: [x0], [sp, #0x10],
// [x1, #-8]. Only the base-plus-offset form is read, because that is the only
// one the compiler generates and the only one a patch usually needs.
struct Memory {
    Register base;
    int64_t offset = 0;
};

bool parse_memory(const std::string &text, Memory &out)
{
    std::string body = trim(text);
    if (body.size() < 3 || body.front() != '[' || body.back() != ']')
        return false;
    body = body.substr(1, body.size() - 2);
    const size_t comma = body.find(',');
    const std::string base = comma == std::string::npos ? body : body.substr(0, comma);
    if (!parse_register(base, out.base) || !out.base.wide)
        return false;
    out.offset = 0;
    if (comma == std::string::npos)
        return true;
    return parse_immediate(body.substr(comma + 1), out.offset);
}

// Everything a load or a store needs to know about its width and its sign.
struct Access {
    int size_field = 3;   // 0 byte, 1 halfword, 2 word, 3 doubleword
    bool loading = false;
    bool sign_extending = false;
};

bool parse_access(const std::string &mnemonic, Access &out)
{
    static const std::map<std::string, Access> table = {
        {"str",   {3, false, false}}, {"ldr",   {3, true,  false}},
        {"strb",  {0, false, false}}, {"ldrb",  {0, true,  false}},
        {"strh",  {1, false, false}}, {"ldrh",  {1, true,  false}},
        {"ldrsb", {0, true,  true}},  {"ldrsh", {1, true,  true}},
        {"ldrsw", {2, true,  true}},
        {"stur",  {3, false, false}}, {"ldur",  {3, true,  false}},
        {"sturb", {0, false, false}}, {"ldurb", {0, true,  false}},
        {"sturh", {1, false, false}}, {"ldurh", {1, true,  false}},
        {"ldursb",{0, true,  true}},  {"ldursh",{1, true,  true}},
        {"ldursw",{2, true,  true}},
    };
    const auto found = table.find(mnemonic);
    if (found == table.end())
        return false;
    out = found->second;
    return true;
}

// The bitfield-move pair, which every fixed shift and every width extension is
// written in terms of.
uint32_t bitfield(bool wide, int opc, uint32_t immr, uint32_t imms, int source, int destination)
{
    // opc 0 signed, 1 unsigned. N matches sf, which is what the 64-bit forms use.
    const uint32_t base = opc == 0 ? 0x13000000u : 0x53000000u;
    const uint32_t sf = wide ? 0x80400000u : 0u; // sf and N together
    return base | sf | ((immr & 0x3f) << 16) | ((imms & 0x3f) << 10) |
           (static_cast<uint32_t>(source) << 5) | static_cast<uint32_t>(destination);
}

} // namespace

Result assemble_arm64(Target target, const Line &line, uint64_t address)
{
    const std::string &mnemonic = line.mnemonic;
    const std::vector<std::string> &operands = line.operands;
    auto wrong_count = [&](size_t wanted) { return wrong_operand_count(line, wanted); };

    if (mnemonic == "nop") {
        if (!operands.empty())
            return wrong_count(0);
        return word_of(0xd503201fu);
    }
    if (mnemonic == "ret") {
        if (operands.empty())
            return word_of(0xd65f03c0u);
        Register link;
        if (operands.size() != 1 || !parse_register(operands[0], link))
            return fail("ret takes no operand, or one register to return through");
        return word_of(0xd65f0000u | (static_cast<uint32_t>(link.number) << 5));
    }
    if (mnemonic == "brk" || mnemonic == "svc") {
        int64_t value = 0;
        if (operands.size() != 1 || !parse_immediate(operands[0], value) || value < 0 ||
            value > 0xffff)
            return fail(mnemonic + " takes one immediate between 0 and 0xffff");
        const uint32_t opcode = mnemonic == "brk" ? 0xd4200000u : 0xd4000001u;
        return word_of(opcode | (static_cast<uint32_t>(value) << 5));
    }

    if (mnemonic == "mov" || mnemonic == "movz" || mnemonic == "movk") {
        if (operands.size() != 2)
            return wrong_count(2);
        Register destination;
        if (!parse_register(operands[0], destination))
            return fail("the destination of " + mnemonic + " has to be a register");
        Register source;
        if (mnemonic == "mov" && parse_register(operands[1], source)) {
            if (destination.wide != source.wide)
                return fail("both registers have to be the same width");
            // MOV Rd, Rn is ORR Rd, ZR, Rn; moving to or from sp is an ADD.
            if (destination.stack || source.stack)
                return word_of(sf_bit(destination) | 0x91000000u |
                                (static_cast<uint32_t>(source.number) << 5) |
                                static_cast<uint32_t>(destination.number));
            return word_of(sf_bit(destination) | 0x2a0003e0u |
                            (static_cast<uint32_t>(source.number) << 16) |
                            static_cast<uint32_t>(destination.number));
        }
        int64_t value = 0;
        if (!parse_immediate(operands[1], value))
            return fail("the source of " + mnemonic + " has to be a register or an immediate");
        if (mnemonic == "movk") {
            if (value < 0 || value > 0xffff)
                return fail("movk takes an immediate between 0 and 0xffff");
            return word_of(sf_bit(destination) | 0x72800000u |
                            (static_cast<uint32_t>(value) << 5) |
                            static_cast<uint32_t>(destination.number));
        }
        return move_immediate(destination, value);
    }

    if (mnemonic == "add" || mnemonic == "sub" || mnemonic == "cmp" || mnemonic == "cmn") {
        const bool comparing = mnemonic == "cmp" || mnemonic == "cmn";
        const size_t wanted = comparing ? 2 : 3;
        if (operands.size() != wanted)
            return wrong_count(wanted);
        Register destination;
        Register first;
        if (comparing) {
            if (!parse_register(operands[0], first))
                return fail(mnemonic + " compares a register");
            destination = first;
            destination.number = 31; // the result is discarded
        } else {
            if (!parse_register(operands[0], destination) || !parse_register(operands[1], first))
                return fail(mnemonic + " takes registers for its destination and first source");
        }
        const std::string &last = operands[comparing ? 1 : 2];
        const bool subtracting = mnemonic == "sub" || mnemonic == "cmp";
        Register second;
        if (parse_register(last, second)) {
            const uint32_t opcode = subtracting ? 0x4b000000u : 0x0b000000u;
            const uint32_t flags = comparing ? 0x20000000u : 0u;
            return word_of(sf_bit(destination) | opcode | flags |
                            (static_cast<uint32_t>(second.number) << 16) |
                            (static_cast<uint32_t>(first.number) << 5) |
                            static_cast<uint32_t>(destination.number));
        }
        int64_t value = 0;
        if (!parse_immediate(last, value))
            return fail(mnemonic + " takes a register or an immediate last");
        uint32_t shift = 0;
        if (value < 0 || value > 0xfff) {
            if (value >= 0 && value <= 0xfff000 && (value & 0xfff) == 0) {
                shift = 1;
                value >>= 12;
            } else {
                return fail("the immediate has to be 0 to 0xfff, or that shifted left by twelve");
            }
        }
        const uint32_t opcode = subtracting ? 0x51000000u : 0x11000000u;
        const uint32_t flags = comparing ? 0x20000000u : 0u;
        return word_of(sf_bit(destination) | opcode | flags | (shift << 22) |
                        (static_cast<uint32_t>(value) << 10) |
                        (static_cast<uint32_t>(first.number) << 5) |
                        static_cast<uint32_t>(destination.number));
    }

    if (mnemonic == "and" || mnemonic == "orr" || mnemonic == "eor") {
        if (operands.size() != 3)
            return wrong_count(3);
        Register destination, first, second;
        if (!parse_register(operands[0], destination) || !parse_register(operands[1], first) ||
            !parse_register(operands[2], second))
            return fail(mnemonic + " takes three registers; an immediate form is not written yet");
        const uint32_t opcode = mnemonic == "and" ? 0x0a000000u
                                                  : (mnemonic == "orr" ? 0x2a000000u : 0x4a000000u);
        return word_of(sf_bit(destination) | opcode |
                        (static_cast<uint32_t>(second.number) << 16) |
                        (static_cast<uint32_t>(first.number) << 5) |
                        static_cast<uint32_t>(destination.number));
    }

    if (mnemonic == "br" || mnemonic == "blr") {
        Register through;
        if (operands.size() != 1 || !parse_register(operands[0], through) || !through.wide)
            return fail(mnemonic + " takes one 64-bit register");
        const uint32_t opcode = mnemonic == "br" ? 0xd61f0000u : 0xd63f0000u;
        return word_of(opcode | (static_cast<uint32_t>(through.number) << 5));
    }

    if (mnemonic == "b" || mnemonic == "bl") {
        uint64_t target_address = 0;
        if (operands.size() != 1 || !parse_address(operands[0], target_address))
            return fail(mnemonic + " takes one address to branch to");
        const int64_t distance = static_cast<int64_t>(target_address) - static_cast<int64_t>(address);
        if ((distance & 3) != 0)
            return fail("a branch target has to be a multiple of four");
        const int64_t words = distance >> 2;
        if (words < -(int64_t(1) << 25) || words >= (int64_t(1) << 25))
            return fail("that is too far to reach with a single branch");
        const uint32_t opcode = mnemonic == "b" ? 0x14000000u : 0x94000000u;
        return word_of(opcode | (static_cast<uint32_t>(words) & 0x03ffffffu));
    }

    if (mnemonic.compare(0, 2, "b.") == 0) {
        const std::string condition = mnemonic.substr(2);
        const auto found = conditions().find(condition);
        if (found == conditions().end())
            return fail("there is no condition called " + condition);
        uint64_t target_address = 0;
        if (operands.size() != 1 || !parse_address(operands[0], target_address))
            return fail(mnemonic + " takes one address to branch to");
        const int64_t distance = static_cast<int64_t>(target_address) - static_cast<int64_t>(address);
        if ((distance & 3) != 0)
            return fail("a branch target has to be a multiple of four");
        const int64_t words = distance >> 2;
        if (words < -(int64_t(1) << 18) || words >= (int64_t(1) << 18))
            return fail("that is too far to reach with a conditional branch");
        return word_of(0x54000000u | ((static_cast<uint32_t>(words) & 0x7ffffu) << 5) |
                        found->second);
    }

    if (mnemonic == "cbz" || mnemonic == "cbnz") {
        Register tested;
        uint64_t target_address = 0;
        if (operands.size() != 2 || !parse_register(operands[0], tested) ||
            !parse_address(operands[1], target_address))
            return fail(mnemonic + " takes a register and an address");
        const int64_t distance = static_cast<int64_t>(target_address) - static_cast<int64_t>(address);
        if ((distance & 3) != 0)
            return fail("a branch target has to be a multiple of four");
        const int64_t words = distance >> 2;
        if (words < -(int64_t(1) << 18) || words >= (int64_t(1) << 18))
            return fail("that is too far to reach with a compare and branch");
        const uint32_t opcode = mnemonic == "cbz" ? 0x34000000u : 0x35000000u;
        return word_of(sf_bit(tested) | opcode | ((static_cast<uint32_t>(words) & 0x7ffffu) << 5) |
                        static_cast<uint32_t>(tested.number));
    }


    if (mnemonic == "mvn") {
        if (operands.size() != 2)
            return wrong_count(2);
        Register destination, source;
        if (!parse_register(operands[0], destination) || !parse_register(operands[1], source))
            return fail("mvn takes two registers");
        // ORN Rd, ZR, Rm: the register form of "the other bits".
        return word_of(sf_bit(destination) | 0x2a200000u |
                        (static_cast<uint32_t>(source.number) << 16) | (31u << 5) |
                        static_cast<uint32_t>(destination.number));
    }

    {
        Access access;
        if (parse_access(mnemonic, access)) {
            if (operands.size() != 2)
                return wrong_count(2);
            Register value;
            Memory memory;
            if (!parse_register(operands[0], value))
                return fail(mnemonic + " takes a register and a memory operand");
            if (!parse_memory(operands[1], memory))
                return fail("the memory operand has to be [base] or [base, #offset]");
            // For the unsuffixed forms the register says the width: ldr w0 is
            // four bytes, ldr x0 is eight.
            if (access.size_field == 3 && !value.wide)
                access.size_field = 2;
            if (access.sign_extending && access.size_field == 2 && !value.wide)
                return fail("ldrsw always widens into an x register");
            // opc says load or store, and for a load whether the sign is kept.
            uint32_t opc;
            if (!access.loading)
                opc = 0;
            else if (!access.sign_extending)
                opc = 1;
            else
                opc = value.wide ? 2 : 3;
            const uint32_t size = static_cast<uint32_t>(access.size_field);
            const uint32_t common = (size << 30) | 0x38000000u | (opc << 22) |
                                    (static_cast<uint32_t>(memory.base.number) << 5) |
                                    static_cast<uint32_t>(value.number);
            const bool unscaled = mnemonic.compare(0, 3, "stu") == 0 ||
                                  mnemonic.compare(0, 3, "ldu") == 0;
            const int64_t scale = int64_t(1) << access.size_field;
            if (!unscaled && memory.offset >= 0 && (memory.offset % scale) == 0 &&
                (memory.offset / scale) <= 0xfff) {
                // The scaled form reaches furthest, so prefer it whenever the
                // offset is a whole number of elements.
                return word_of(common | 0x01000000u |
                                (static_cast<uint32_t>(memory.offset / scale) << 10));
            }
            if (memory.offset < -256 || memory.offset > 255)
                return fail("that offset is out of reach; it has to be a multiple of the "
                            "access width up to 4095 of them, or -256 to 255 bytes");
            return word_of(common | ((static_cast<uint32_t>(memory.offset) & 0x1ffu) << 12));
        }
    }

    if (mnemonic == "mul" || mnemonic == "sdiv" || mnemonic == "udiv" ||
        mnemonic == "lsl" || mnemonic == "lsr" || mnemonic == "asr") {
        if (operands.size() != 3)
            return wrong_count(3);
        Register destination, first;
        if (!parse_register(operands[0], destination) || !parse_register(operands[1], first))
            return fail(mnemonic + " takes registers for its destination and first source");
        const bool wide = destination.wide;
        Register second;
        if (parse_register(operands[2], second)) {
            uint32_t opcode = 0;
            if (mnemonic == "mul")
                opcode = 0x1b007c00u;      // MADD with the addend discarded
            else if (mnemonic == "sdiv")
                opcode = 0x1ac00c00u;
            else if (mnemonic == "udiv")
                opcode = 0x1ac00800u;
            else if (mnemonic == "lsl")
                opcode = 0x1ac02000u;
            else if (mnemonic == "lsr")
                opcode = 0x1ac02400u;
            else
                opcode = 0x1ac02800u;
            return word_of(sf_bit(destination) | opcode |
                            (static_cast<uint32_t>(second.number) << 16) |
                            (static_cast<uint32_t>(first.number) << 5) |
                            static_cast<uint32_t>(destination.number));
        }
        int64_t amount = 0;
        if (!parse_immediate(operands[2], amount))
            return fail(mnemonic + " takes a register or an immediate last");
        if (mnemonic == "mul" || mnemonic == "sdiv" || mnemonic == "udiv")
            return fail(mnemonic + " takes three registers; there is no immediate form");
        const int64_t width = wide ? 64 : 32;
        if (amount < 0 || amount >= width)
            return fail("a shift amount has to be between 0 and one less than the register width");
        if (mnemonic == "lsl")
            return word_of(bitfield(wide, 1, static_cast<uint32_t>((width - amount) % width),
                                     static_cast<uint32_t>(width - 1 - amount),
                                     first.number, destination.number));
        return word_of(bitfield(wide, mnemonic == "asr" ? 0 : 1, static_cast<uint32_t>(amount),
                                 static_cast<uint32_t>(width - 1), first.number,
                                 destination.number));
    }

    if (mnemonic == "msub" || mnemonic == "madd") {
        if (operands.size() != 4)
            return wrong_count(4);
        Register destination, first, second, addend;
        if (!parse_register(operands[0], destination) || !parse_register(operands[1], first) ||
            !parse_register(operands[2], second) || !parse_register(operands[3], addend))
            return fail(mnemonic + " takes four registers");
        const uint32_t opcode = mnemonic == "madd" ? 0x1b000000u : 0x1b008000u;
        return word_of(sf_bit(destination) | opcode |
                        (static_cast<uint32_t>(second.number) << 16) |
                        (static_cast<uint32_t>(addend.number) << 10) |
                        (static_cast<uint32_t>(first.number) << 5) |
                        static_cast<uint32_t>(destination.number));
    }

    if (mnemonic == "sxtb" || mnemonic == "sxth" || mnemonic == "sxtw" || mnemonic == "uxtb" ||
        mnemonic == "uxth") {
        if (operands.size() != 2)
            return wrong_count(2);
        Register destination, source;
        if (!parse_register(operands[0], destination) || !parse_register(operands[1], source))
            return fail(mnemonic + " takes two registers");
        const bool is_signed = mnemonic[0] == 's';
        if (!is_signed && destination.wide)
            return fail(mnemonic + " widens into a w register; the upper half is cleared anyway");
        if (mnemonic == "sxtw" && !destination.wide)
            return fail("sxtw widens into an x register");
        const uint32_t imms = mnemonic[3] == 'b' ? 7u : (mnemonic[3] == 'h' ? 15u : 31u);
        return word_of(bitfield(destination.wide, is_signed ? 0 : 1, 0, imms, source.number,
                                 destination.number));
    }

    if (mnemonic == "cset" || mnemonic == "csel" || mnemonic == "csinc") {
        Register destination;
        if (operands.empty() || !parse_register(operands[0], destination))
            return fail(mnemonic + " writes to a register");
        const std::string condition = lower(trim(operands.back()));
        const auto found = conditions().find(condition);
        if (found == conditions().end())
            return fail("there is no condition called " + condition);
        if (mnemonic == "cset") {
            if (operands.size() != 2)
                return wrong_count(2);
            // CSINC Rd, ZR, ZR with the condition inverted: one when it holds.
            const uint32_t inverted = found->second ^ 1u;
            return word_of(sf_bit(destination) | 0x1a800400u | (31u << 16) | (inverted << 12) |
                            (31u << 5) | static_cast<uint32_t>(destination.number));
        }
        if (operands.size() != 4)
            return wrong_count(4);
        Register first, second;
        if (!parse_register(operands[1], first) || !parse_register(operands[2], second))
            return fail(mnemonic + " takes three registers and a condition");
        const uint32_t opcode = mnemonic == "csel" ? 0x1a800000u : 0x1a800400u;
        return word_of(sf_bit(destination) | opcode |
                        (static_cast<uint32_t>(second.number) << 16) | (found->second << 12) |
                        (static_cast<uint32_t>(first.number) << 5) |
                        static_cast<uint32_t>(destination.number));
    }

    return unknown_mnemonic(line, target);
}

} // namespace assembler
} // namespace astral_internal
