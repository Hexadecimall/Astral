// The x86 encoder, for both widths: one line of text, the bytes it stands for.
//
// x86 instructions vary in length, which changes what patching means. On a
// fixed-width machine a replacement has to be exactly as long as what it
// replaces; here it only has to be no longer, and the caller pads the rest with
// no-ops. That is why this returns the shortest encoding it can rather than a
// canonical one.
//
// Intel order throughout: the destination is written first.
#include "text.hh"

#include <cctype>
#include <cstdlib>
#include <map>

namespace astral_internal {
namespace assembler {
namespace {

struct Register {
    int number = 0;
    int width = 0; // in bits: 8, 16, 32 or 64
};

// The general registers, in the order the encoding numbers them.
const std::map<std::string, Register> &registers()
{
    static const std::map<std::string, Register> table = [] {
        std::map<std::string, Register> t;
        static const char *low[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
        for (int i = 0; i < 8; ++i) {
            t[std::string("r") + low[i]] = {i, 64};
            t[std::string("e") + low[i]] = {i, 32};
            t[low[i]] = {i, 16};
        }
        static const char *byte_names[] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"};
        for (int i = 0; i < 8; ++i)
            t[byte_names[i]] = {i, 8};
        // The registers the 64-bit extension added, in each width.
        for (int i = 8; i < 16; ++i) {
            const std::string base = "r" + std::to_string(i);
            t[base] = {i, 64};
            t[base + "d"] = {i, 32};
            t[base + "w"] = {i, 16};
            t[base + "b"] = {i, 8};
        }
        return t;
    }();
    return table;
}

bool parse_register(const std::string &text, Register &out)
{
    const auto found = registers().find(lower(trim(text)));
    if (found == registers().end())
        return false;
    out = found->second;
    return true;
}

// The prefix that says which of the wider registers are in play, and whether
// the operation is 64 bits wide. Omitted entirely when it would be empty.
void add_rex(std::vector<uint8_t> &bytes, bool wide, int reg, int rm)
{
    uint8_t rex = 0x40;
    if (wide)
        rex |= 0x08;
    if (reg >= 8)
        rex |= 0x04;
    if (rm >= 8)
        rex |= 0x01;
    if (rex != 0x40)
        bytes.push_back(rex);
}

// Two registers addressing each other directly.
uint8_t modrm_register(int reg, int rm)
{
    return static_cast<uint8_t>(0xc0 | ((reg & 7) << 3) | (rm & 7));
}

void push_u32(std::vector<uint8_t> &bytes, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

// The arithmetic instructions share one encoding, differing only in which
// three-bit slot they occupy.
const std::map<std::string, int> &arithmetic()
{
    static const std::map<std::string, int> table = {
        {"add", 0}, {"or", 1}, {"adc", 2}, {"sbb", 3},
        {"and", 4}, {"sub", 5}, {"xor", 6}, {"cmp", 7},
    };
    return table;
}

// The condition codes, in the order the encoding numbers them.
const std::map<std::string, uint8_t> &conditions()
{
    static const std::map<std::string, uint8_t> table = {
        {"o", 0},  {"no", 1}, {"b", 2},  {"nae", 2}, {"c", 2},  {"nb", 3},  {"ae", 3}, {"nc", 3},
        {"z", 4},  {"e", 4},  {"nz", 5}, {"ne", 5},  {"be", 6}, {"na", 6},  {"a", 7},  {"nbe", 7},
        {"s", 8},  {"ns", 9}, {"p", 10}, {"pe", 10}, {"np", 11},{"po", 11}, {"l", 12}, {"nge", 12},
        {"ge", 13},{"nl", 13},{"le", 14},{"ng", 14}, {"g", 15}, {"nle", 15},
    };
    return table;
}

} // namespace

Result assemble_x86(Target target, const Line &line, uint64_t address)
{
    const bool sixty_four = target == Target::X86_64;
    const std::string &m = line.mnemonic;
    const std::vector<std::string> &ops = line.operands;

    auto width_prefix = [&](std::vector<uint8_t> &bytes, const Register &r) {
        if (r.width == 16)
            bytes.push_back(0x66);
    };
    auto check_width = [&](const Register &r) -> Result {
        if (r.width == 64 && !sixty_four)
            return fail("a 64-bit register cannot be used in 32-bit code");
        if (r.number >= 8 && !sixty_four)
            return fail("that register only exists in 64-bit code");
        Result ok;
        ok.ok = true;
        return ok;
    };

    if (m == "nop") {
        if (!ops.empty())
            return wrong_operand_count(line, 0);
        return bytes_of({0x90});
    }
    if (m == "ret") {
        if (ops.empty())
            return bytes_of({0xc3});
        int64_t pop = 0;
        if (ops.size() != 1 || !parse_immediate(ops[0], pop) || pop < 0 || pop > 0xffff)
            return fail("ret takes no operand, or how many bytes to drop");
        return bytes_of({0xc2, static_cast<uint8_t>(pop), static_cast<uint8_t>(pop >> 8)});
    }
    if (m == "int3") {
        if (!ops.empty())
            return wrong_operand_count(line, 0);
        return bytes_of({0xcc});
    }
    if (m == "leave") {
        if (!ops.empty())
            return wrong_operand_count(line, 0);
        return bytes_of({0xc9});
    }
    if (m == "syscall") {
        if (!sixty_four)
            return fail("syscall is 64-bit only; 32-bit code uses int 0x80");
        return bytes_of({0x0f, 0x05});
    }
    if (m == "cdq" || m == "cqo") {
        std::vector<uint8_t> bytes;
        if (m == "cqo") {
            if (!sixty_four)
                return fail("cqo is 64-bit only");
            bytes.push_back(0x48);
        }
        bytes.push_back(0x99);
        return bytes_of(bytes);
    }

    if (m == "push" || m == "pop") {
        if (ops.size() != 1)
            return wrong_operand_count(line, 1);
        Register r;
        if (parse_register(ops[0], r)) {
            const Result bad = check_width(r);
            if (!bad.ok)
                return bad;
            // In 64-bit code push and pop are always the full width, with no
            // prefix needed to say so.
            if (sixty_four ? r.width != 64 : r.width != 32)
                return fail(m + " works on the machine's own width");
            std::vector<uint8_t> bytes;
            if (r.number >= 8)
                bytes.push_back(0x41);
            bytes.push_back(static_cast<uint8_t>((m == "push" ? 0x50 : 0x58) + (r.number & 7)));
            return bytes_of(bytes);
        }
        int64_t value = 0;
        if (m == "push" && parse_immediate(ops[0], value)) {
            if (value >= -128 && value <= 127)
                return bytes_of({0x6a, static_cast<uint8_t>(value)});
            std::vector<uint8_t> bytes{0x68};
            push_u32(bytes, static_cast<uint32_t>(value));
            return bytes_of(bytes);
        }
        return fail(m + " takes a register" + (m == "push" ? " or an immediate" : ""));
    }

    if (m == "mov") {
        if (ops.size() != 2)
            return wrong_operand_count(line, 2);
        Register destination;
        if (!parse_register(ops[0], destination))
            return fail("Astral writes mov to a register; memory operands are not written yet");
        const Result bad = check_width(destination);
        if (!bad.ok)
            return bad;
        Register source;
        if (parse_register(ops[1], source)) {
            if (source.width != destination.width)
                return fail("both registers have to be the same width");
            std::vector<uint8_t> bytes;
            width_prefix(bytes, destination);
            if (sixty_four)
                add_rex(bytes, destination.width == 64, source.number, destination.number);
            bytes.push_back(destination.width == 8 ? 0x88 : 0x89);
            bytes.push_back(modrm_register(source.number, destination.number));
            return bytes_of(bytes);
        }
        int64_t value = 0;
        if (!parse_immediate(ops[1], value))
            return fail("the source of mov has to be a register or an immediate");
        std::vector<uint8_t> bytes;
        width_prefix(bytes, destination);
        if (destination.width == 8) {
            if (value < -128 || value > 255)
                return fail("that value does not fit in a byte");
            if (sixty_four)
                add_rex(bytes, false, 0, destination.number);
            bytes.push_back(static_cast<uint8_t>(0xb0 + (destination.number & 7)));
            bytes.push_back(static_cast<uint8_t>(value));
            return bytes_of(bytes);
        }
        if (sixty_four)
            add_rex(bytes, destination.width == 64, 0, destination.number);
        bytes.push_back(static_cast<uint8_t>(0xb8 + (destination.number & 7)));
        if (destination.width == 16) {
            bytes.push_back(static_cast<uint8_t>(value));
            bytes.push_back(static_cast<uint8_t>(value >> 8));
        } else if (destination.width == 64) {
            for (int i = 0; i < 8; ++i)
                bytes.push_back(static_cast<uint8_t>(static_cast<uint64_t>(value) >> (i * 8)));
        } else {
            push_u32(bytes, static_cast<uint32_t>(value));
        }
        return bytes_of(bytes);
    }

    const auto arithmetic_slot = arithmetic().find(m);
    if (arithmetic_slot != arithmetic().end()) {
        if (ops.size() != 2)
            return wrong_operand_count(line, 2);
        Register destination;
        if (!parse_register(ops[0], destination))
            return fail(m + " is written to a register; memory operands are not written yet");
        const Result bad = check_width(destination);
        if (!bad.ok)
            return bad;
        Register source;
        if (parse_register(ops[1], source)) {
            if (source.width != destination.width)
                return fail("both registers have to be the same width");
            std::vector<uint8_t> bytes;
            width_prefix(bytes, destination);
            if (sixty_four)
                add_rex(bytes, destination.width == 64, source.number, destination.number);
            bytes.push_back(static_cast<uint8_t>(arithmetic_slot->second * 8 +
                                                 (destination.width == 8 ? 0x00 : 0x01)));
            bytes.push_back(modrm_register(source.number, destination.number));
            return bytes_of(bytes);
        }
        int64_t value = 0;
        if (!parse_immediate(ops[1], value))
            return fail(m + " takes a register or an immediate");
        std::vector<uint8_t> bytes;
        width_prefix(bytes, destination);
        if (sixty_four)
            add_rex(bytes, destination.width == 64, 0, destination.number);
        // A value that fits in one byte gets the short form.
        if (destination.width != 8 && value >= -128 && value <= 127) {
            bytes.push_back(0x83);
            bytes.push_back(modrm_register(arithmetic_slot->second, destination.number));
            bytes.push_back(static_cast<uint8_t>(value));
            return bytes_of(bytes);
        }
        bytes.push_back(destination.width == 8 ? 0x80 : 0x81);
        bytes.push_back(modrm_register(arithmetic_slot->second, destination.number));
        if (destination.width == 8) {
            if (value < -128 || value > 255)
                return fail("that value does not fit in a byte");
            bytes.push_back(static_cast<uint8_t>(value));
        } else if (destination.width == 16) {
            bytes.push_back(static_cast<uint8_t>(value));
            bytes.push_back(static_cast<uint8_t>(value >> 8));
        } else {
            push_u32(bytes, static_cast<uint32_t>(value));
        }
        return bytes_of(bytes);
    }

    if (m == "test") {
        if (ops.size() != 2)
            return wrong_operand_count(line, 2);
        Register a, b;
        if (!parse_register(ops[0], a) || !parse_register(ops[1], b))
            return fail("test is written between two registers");
        if (a.width != b.width)
            return fail("both registers have to be the same width");
        std::vector<uint8_t> bytes;
        width_prefix(bytes, a);
        if (sixty_four)
            add_rex(bytes, a.width == 64, b.number, a.number);
        bytes.push_back(a.width == 8 ? 0x84 : 0x85);
        bytes.push_back(modrm_register(b.number, a.number));
        return bytes_of(bytes);
    }

    if (m == "jmp" || m == "call") {
        if (ops.size() != 1)
            return wrong_operand_count(line, 1);
        int64_t target_address = 0;
        if (!parse_immediate(ops[0], target_address))
            return fail(m + " takes an address to go to");
        // The distance is measured from the end of the instruction, which is
        // five bytes long in the form that reaches the furthest.
        const int64_t distance = target_address - static_cast<int64_t>(address) - 5;
        if (distance < -(int64_t(1) << 31) || distance >= (int64_t(1) << 31))
            return fail("that is too far to reach with a single " + m);
        std::vector<uint8_t> bytes{static_cast<uint8_t>(m == "jmp" ? 0xe9 : 0xe8)};
        push_u32(bytes, static_cast<uint32_t>(distance));
        return bytes_of(bytes);
    }

    if (m.size() > 1 && m[0] == 'j') {
        const auto condition = conditions().find(m.substr(1));
        if (condition != conditions().end()) {
            if (ops.size() != 1)
                return wrong_operand_count(line, 1);
            int64_t target_address = 0;
            if (!parse_immediate(ops[0], target_address))
                return fail(m + " takes an address to go to");
            const int64_t distance = target_address - static_cast<int64_t>(address) - 6;
            if (distance < -(int64_t(1) << 31) || distance >= (int64_t(1) << 31))
                return fail("that is too far to reach with a conditional jump");
            std::vector<uint8_t> bytes{0x0f, static_cast<uint8_t>(0x80 + condition->second)};
            push_u32(bytes, static_cast<uint32_t>(distance));
            return bytes_of(bytes);
        }
    }

    return unknown_mnemonic(line, target);
}

} // namespace assembler
} // namespace astral_internal
