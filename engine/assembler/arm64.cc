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

    return unknown_mnemonic(line, target);
}

} // namespace assembler
} // namespace astral_internal
