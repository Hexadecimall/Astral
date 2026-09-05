// The 32-bit ARM encoder: the classic four-byte A32 encoding, and the two-byte
// Thumb forms that cover what a patch usually needs.
//
// Almost every A32 instruction carries a condition in its top four bits, so
// "moveq" is "mov" with a condition rather than a separate instruction. That is
// handled once, here, instead of in each instruction.
#include "text.hh"

#include <cctype>
#include <map>

namespace astral_internal {
namespace assembler {
namespace {

// The condition codes, in the order the architecture numbers them. Shared by
// every A32 instruction.
const std::map<std::string, uint32_t> &conditions()
{
    static const std::map<std::string, uint32_t> table = {
        {"eq", 0},  {"ne", 1},  {"cs", 2}, {"hs", 2}, {"cc", 3}, {"lo", 3},
        {"mi", 4},  {"pl", 5},  {"vs", 6}, {"vc", 7}, {"hi", 8}, {"ls", 9},
        {"ge", 10}, {"lt", 11}, {"gt", 12},{"le", 13},{"al", 14},
    };
    return table;
}

bool parse_register(const std::string &text, int &out)
{
    std::string name = lower(trim(text));
    if (name == "sp") { out = 13; return true; }
    if (name == "lr") { out = 14; return true; }
    if (name == "pc") { out = 15; return true; }
    if (name.size() < 2 || name[0] != 'r')
        return false;
    for (size_t i = 1; i < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i])))
            return false;
    const int number = std::atoi(name.c_str() + 1);
    if (number < 0 || number > 15)
        return false;
    out = number;
    return true;
}

// A32 immediates are an eight-bit value turned by an even number of places.
// Not every number can be written; the ones that cannot need a different
// instruction, and saying so is better than encoding something else.
bool encode_immediate(uint32_t value, uint32_t &out)
{
    for (int rotation = 0; rotation < 16; ++rotation) {
        const uint32_t rotated = (value << (rotation * 2)) | (value >> (32 - rotation * 2));
        if (rotated <= 0xff) {
            out = (static_cast<uint32_t>(rotation) << 8) | rotated;
            return true;
        }
    }
    return false;
}

// Splits a trailing condition off a mnemonic: "bne" is "b" with ne.
void split_condition(const std::string &mnemonic, const std::string &stem, std::string &base,
                     uint32_t &condition, bool &has_condition)
{
    base = mnemonic;
    condition = 14; // always
    has_condition = false;
    if (mnemonic.size() <= stem.size() || mnemonic.compare(0, stem.size(), stem) != 0)
        return;
    const auto found = conditions().find(mnemonic.substr(stem.size()));
    if (found == conditions().end())
        return;
    base = stem;
    condition = found->second;
    has_condition = true;
}

} // namespace

Result assemble_arm32(Target target, const Line &line, uint64_t address)
{
    const std::vector<std::string> &ops = line.operands;
    std::string m = line.mnemonic;

    if (target == Target::Thumb) {
        // Thumb is two bytes wide, which is all a patch of one instruction has
        // room for. Only the forms that fit are written.
        if (m == "nop") {
            if (!ops.empty())
                return wrong_operand_count(line, 0);
            return halfword_of(0xbf00);
        }
        if (m == "bx") {
            int through = 0;
            if (ops.size() != 1 || !parse_register(ops[0], through))
                return fail("bx takes one register");
            return halfword_of(static_cast<uint16_t>(0x4700 | (through << 3)));
        }
        if (m == "mov" || m == "movs") {
            int destination = 0;
            int64_t value = 0;
            if (ops.size() != 2 || !parse_register(ops[0], destination))
                return fail("mov takes a register and a register or immediate");
            int source = 0;
            if (parse_register(ops[1], source))
                return halfword_of(static_cast<uint16_t>(0x4600 | ((destination & 8) << 4) |
                                                         (source << 3) | (destination & 7)));
            if (!parse_immediate(ops[1], value) || value < 0 || value > 255 || destination > 7)
                return fail("a Thumb immediate move takes r0 to r7 and a value from 0 to 255");
            return halfword_of(static_cast<uint16_t>(0x2000 | (destination << 8) | value));
        }
        return unknown_mnemonic(line, target);
    }

    // A32 from here: every instruction is four bytes and carries a condition.
    std::string base;
    uint32_t condition = 14;
    bool conditional = false;
    for (const char *stem : {"b", "bl", "bx", "mov", "mvn", "add", "sub", "cmp", "cmn", "and",
                             "orr", "eor", "tst", "teq", "mul", "nop", "push", "pop"}) {
        std::string candidate;
        uint32_t found = 14;
        bool has = false;
        split_condition(m, stem, candidate, found, has);
        if (has && candidate.size() > base.size()) {
            base = candidate;
            condition = found;
            conditional = true;
        }
    }
    if (!conditional)
        base = m;
    const uint32_t cond = condition << 28;

    if (base == "nop") {
        if (!ops.empty())
            return wrong_operand_count(line, 0);
        return word_of(cond | 0x0320f000u);
    }
    if (base == "bx") {
        int through = 0;
        if (ops.size() != 1 || !parse_register(ops[0], through))
            return fail("bx takes one register");
        return word_of(cond | 0x012fff10u | static_cast<uint32_t>(through));
    }
    if (base == "b" || base == "bl") {
        int64_t target_address = 0;
        if (ops.size() != 1 || !parse_immediate(ops[0], target_address))
            return fail(base + " takes one address to branch to");
        // The distance is measured from two instructions ahead, which is where
        // the program counter reads on this architecture.
        const int64_t distance = target_address - static_cast<int64_t>(address) - 8;
        if ((distance & 3) != 0)
            return fail("a branch target has to be a multiple of four");
        const int64_t words = distance >> 2;
        if (words < -(int64_t(1) << 23) || words >= (int64_t(1) << 23))
            return fail("that is too far to reach with a single branch");
        return word_of(cond | (base == "b" ? 0x0a000000u : 0x0b000000u) |
                       (static_cast<uint32_t>(words) & 0x00ffffffu));
    }

    // The data-processing instructions share one encoding and differ only in
    // which four-bit slot they occupy.
    static const std::map<std::string, uint32_t> slots = {
        {"and", 0}, {"eor", 1}, {"sub", 2}, {"rsb", 3}, {"add", 4}, {"adc", 5},
        {"sbc", 6}, {"rsc", 7}, {"tst", 8}, {"teq", 9}, {"cmp", 10}, {"cmn", 11},
        {"orr", 12}, {"mov", 13}, {"bic", 14}, {"mvn", 15},
    };
    const auto slot = slots.find(base);
    if (slot != slots.end()) {
        const bool single_source = base == "mov" || base == "mvn";
        const bool compares = base == "cmp" || base == "cmn" || base == "tst" || base == "teq";
        const size_t wanted = (single_source || compares) ? 2 : 3;
        if (ops.size() != wanted)
            return wrong_operand_count(line, wanted);
        int destination = 0;
        int first = 0;
        if (compares) {
            if (!parse_register(ops[0], first))
                return fail(base + " compares a register");
            destination = 0;
        } else if (single_source) {
            if (!parse_register(ops[0], destination))
                return fail(base + " writes to a register");
            first = 0;
        } else {
            if (!parse_register(ops[0], destination) || !parse_register(ops[1], first))
                return fail(base + " takes registers for its destination and first source");
        }
        const std::string &last = ops.back();
        const uint32_t opcode = slot->second << 21;
        // Comparisons always set the flags; the others do not unless asked.
        const uint32_t set_flags = compares ? 0x00100000u : 0u;
        int second = 0;
        if (parse_register(last, second))
            return word_of(cond | opcode | set_flags | (static_cast<uint32_t>(first) << 16) |
                           (static_cast<uint32_t>(destination) << 12) |
                           static_cast<uint32_t>(second));
        int64_t value = 0;
        if (!parse_immediate(last, value))
            return fail(base + " takes a register or an immediate last");
        uint32_t encoded = 0;
        if (!encode_immediate(static_cast<uint32_t>(value), encoded))
            return fail("that value cannot be written as an ARM immediate, which is eight bits "
                        "turned by an even number of places");
        return word_of(cond | 0x02000000u | opcode | set_flags |
                       (static_cast<uint32_t>(first) << 16) |
                       (static_cast<uint32_t>(destination) << 12) | encoded);
    }

    return unknown_mnemonic(line, target);
}

} // namespace assembler
} // namespace astral_internal
