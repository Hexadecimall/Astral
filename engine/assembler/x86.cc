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
#include <cstdint>
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

// ------------------------------------------------------------------ operands

// A place in memory: `[rbp - 0x10]`, `[rsp + rax*4 + 8]`, `[rip + 0x1234]`.
// Everything a code generator addresses is one of these, so the encoder takes
// them wherever a register would do.
struct Memory {
    bool has_base = false;
    Register base;
    bool has_index = false;
    Register index;
    int scale = 1;
    int64_t displacement = 0;
    // 64-bit only: the displacement is measured from the end of the
    // instruction rather than from any register.
    bool rip_relative = false;
    // What "dword ptr" said, or 0 when nothing said.
    int width = 0;
};

struct Operand {
    enum class Kind { None, Reg, Mem, Imm };
    Kind kind = Kind::None;
    Register reg;
    Memory mem;
    int64_t immediate = 0;

    bool is_register() const { return kind == Kind::Reg; }
    bool is_memory() const { return kind == Kind::Mem; }
    bool is_immediate() const { return kind == Kind::Imm; }
};

// "qword ptr [rbp-8]" says how wide the access is when neither operand is a
// register that could have said it.
int width_word(const std::string &word)
{
    if (word == "byte") return 8;
    if (word == "word") return 16;
    if (word == "dword") return 32;
    if (word == "qword") return 64;
    return 0;
}

// Splits `a + b*4 - 8` into its terms, keeping the sign with each.
std::vector<std::string> address_terms(const std::string &inside)
{
    std::vector<std::string> terms;
    std::string current;
    for (size_t i = 0; i < inside.size(); ++i) {
        const char c = inside[i];
        if ((c == '+' || c == '-') && !trim(current).empty()) {
            terms.push_back(trim(current));
            current = (c == '-') ? "-" : "";
            continue;
        }
        current.push_back(c);
    }
    if (!trim(current).empty())
        terms.push_back(trim(current));
    return terms;
}

bool parse_memory(const std::string &text, Memory &out)
{
    std::string rest = trim(text);
    // Any number of size words in front of the brackets.
    for (;;) {
        const size_t space = rest.find(' ');
        if (space == std::string::npos)
            break;
        const int width = width_word(lower(rest.substr(0, space)));
        if (width == 0)
            break;
        out.width = width;
        rest = trim(rest.substr(space + 1));
        if (rest.compare(0, 3, "ptr") == 0)
            rest = trim(rest.substr(3));
    }
    if (rest.size() < 2 || rest.front() != '[' || rest.back() != ']')
        return false;
    const std::string inside = trim(rest.substr(1, rest.size() - 2));
    if (inside.empty())
        return false;

    for (const std::string &term : address_terms(inside)) {
        bool negative = !term.empty() && term[0] == '-';
        std::string body = trim(negative ? term.substr(1) : term);
        if (body.empty())
            return false;
        // A scaled index: `rax*4`.
        const size_t star = body.find('*');
        if (star != std::string::npos) {
            Register index;
            int64_t scale = 0;
            if (!parse_register(trim(body.substr(0, star)), index))
                return false;
            if (!parse_immediate(trim(body.substr(star + 1)), scale))
                return false;
            if (scale != 1 && scale != 2 && scale != 4 && scale != 8)
                return false;
            if (out.has_index || negative)
                return false;
            out.has_index = true;
            out.index = index;
            out.scale = static_cast<int>(scale);
            continue;
        }
        if (lower(body) == "rip") {
            if (negative)
                return false;
            out.rip_relative = true;
            continue;
        }
        Register reg;
        if (parse_register(body, reg)) {
            if (negative)
                return false;
            if (!out.has_base) {
                out.has_base = true;
                out.base = reg;
            } else if (!out.has_index) {
                out.has_index = true;
                out.index = reg;
                out.scale = 1;
            } else {
                return false;
            }
            continue;
        }
        int64_t value = 0;
        if (!parse_immediate(body, value))
            return false;
        out.displacement += negative ? -value : value;
    }
    return out.has_base || out.has_index || out.rip_relative;
}

bool parse_operand(const std::string &text, Operand &out)
{
    if (parse_register(text, out.reg)) {
        out.kind = Operand::Kind::Reg;
        return true;
    }
    if (parse_memory(text, out.mem)) {
        out.kind = Operand::Kind::Mem;
        return true;
    }
    if (parse_immediate(trim(text), out.immediate)) {
        out.kind = Operand::Kind::Imm;
        return true;
    }
    return false;
}

// The REX bits a memory operand contributes: X from the index, B from the base.
void rex_bits_for(const Memory &mem, int &index_number, int &base_number)
{
    index_number = mem.has_index ? mem.index.number : 0;
    base_number = mem.has_base ? mem.base.number : 0;
}

// ModRM, and the SIB and displacement that go with it. `reg_field` is the
// three bits that name the other operand, or the opcode extension.
bool encode_memory(std::vector<uint8_t> &bytes, int reg_field, const Memory &mem, bool sixty_four)
{
    if (mem.rip_relative) {
        if (!sixty_four)
            return false;
        bytes.push_back(static_cast<uint8_t>(((reg_field & 7) << 3) | 5));
        push_u32(bytes, static_cast<uint32_t>(mem.displacement));
        return true;
    }
    const int base = mem.has_base ? mem.base.number : -1;
    const int index = mem.has_index ? mem.index.number : -1;
    // The stack pointer cannot be an index; its encoding is what says "no
    // index at all".
    if (index == 4)
        return false;

    // With no base, the address is a bare displacement, which needs a SIB
    // saying so.
    const bool need_sib = index >= 0 || base < 0 || (base & 7) == 4;
    int mod;
    if (base < 0) {
        mod = 0;
    } else if (mem.displacement == 0 && (base & 7) != 5) {
        mod = 0;
    } else if (mem.displacement >= -128 && mem.displacement <= 127) {
        mod = 1;
    } else {
        mod = 2;
    }

    const int rm = need_sib ? 4 : (base & 7);
    bytes.push_back(static_cast<uint8_t>((mod << 6) | ((reg_field & 7) << 3) | rm));
    if (need_sib) {
        int scale_bits = 0;
        switch (mem.scale) {
        case 1: scale_bits = 0; break;
        case 2: scale_bits = 1; break;
        case 4: scale_bits = 2; break;
        case 8: scale_bits = 3; break;
        default: return false;
        }
        const int sib_index = index >= 0 ? (index & 7) : 4;
        const int sib_base = base >= 0 ? (base & 7) : 5;
        bytes.push_back(static_cast<uint8_t>((scale_bits << 6) | (sib_index << 3) | sib_base));
    }
    if (mod == 1) {
        bytes.push_back(static_cast<uint8_t>(mem.displacement));
    } else if (mod == 2 || (base < 0 && need_sib)) {
        push_u32(bytes, static_cast<uint32_t>(mem.displacement));
    }
    return true;
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

    // ------------------------------------------------------- shared encoding
    // Every two-operand instruction has the same shape: a register on one
    // side, a register or a place in memory on the other. Writing that once
    // means memory works everywhere rather than in the few forms someone got
    // round to.
    auto operand_width = [&](const Operand &a, const Operand &b) -> int {
        if (a.is_register())
            return a.reg.width;
        if (b.is_register())
            return b.reg.width;
        if (a.is_memory() && a.mem.width != 0)
            return a.mem.width;
        if (b.is_memory() && b.mem.width != 0)
            return b.mem.width;
        return 0;
    };
    auto prefixes = [&](std::vector<uint8_t> &bytes, int width, int reg_number,
                        const Operand &rm) -> bool {
        if (width == 16)
            bytes.push_back(0x66);
        int index_number = 0;
        int base_number = 0;
        if (rm.is_memory())
            rex_bits_for(rm.mem, index_number, base_number);
        else
            base_number = rm.reg.number;
        if (!sixty_four) {
            if (width == 64 || reg_number >= 8 || index_number >= 8 || base_number >= 8)
                return false;
            return true;
        }
        uint8_t rex = 0x40;
        if (width == 64)
            rex |= 0x08;
        if (reg_number >= 8)
            rex |= 0x04;
        if (index_number >= 8)
            rex |= 0x02;
        if (base_number >= 8)
            rex |= 0x01;
        if (rex != 0x40)
            bytes.push_back(rex);
        return true;
    };
    // Writes the ModRM byte and whatever follows it for `rm`, which is either a
    // register or a place in memory.
    auto encode_rm = [&](std::vector<uint8_t> &bytes, int reg_field, const Operand &rm) -> bool {
        if (rm.is_register()) {
            bytes.push_back(modrm_register(reg_field, rm.reg.number));
            return true;
        }
        return encode_memory(bytes, reg_field, rm.mem, sixty_four);
    };
    auto push_immediate = [&](std::vector<uint8_t> &bytes, int width, int64_t value) {
        if (width == 8)
            bytes.push_back(static_cast<uint8_t>(value));
        else if (width == 16) {
            bytes.push_back(static_cast<uint8_t>(value));
            bytes.push_back(static_cast<uint8_t>(value >> 8));
        } else {
            push_u32(bytes, static_cast<uint32_t>(value));
        }
    };
    auto bad_for_mode = [&]() { return fail("that register only exists in 64-bit code"); };

    if (m == "push" || m == "pop") {
        if (ops.size() != 1)
            return wrong_operand_count(line, 1);
        Operand only;
        if (!parse_operand(ops[0], only))
            return fail(m + " takes a register, a place in memory" +
                        (m == "push" ? " or a value" : ""));
        if (only.is_register()) {
            const Result bad = check_width(only.reg);
            if (!bad.ok)
                return bad;
            if (sixty_four ? only.reg.width != 64 : only.reg.width != 32)
                return fail(m + " works on the machine's own width");
            std::vector<uint8_t> bytes;
            if (only.reg.number >= 8)
                bytes.push_back(0x41);
            bytes.push_back(static_cast<uint8_t>((m == "push" ? 0x50 : 0x58) + (only.reg.number & 7)));
            return bytes_of(bytes);
        }
        if (only.is_memory()) {
            std::vector<uint8_t> bytes;
            int index_number = 0, base_number = 0;
            rex_bits_for(only.mem, index_number, base_number);
            if (sixty_four && (index_number >= 8 || base_number >= 8)) {
                uint8_t rex = 0x40;
                if (index_number >= 8) rex |= 0x02;
                if (base_number >= 8) rex |= 0x01;
                bytes.push_back(rex);
            }
            bytes.push_back(m == "push" ? 0xff : 0x8f);
            if (!encode_rm(bytes, m == "push" ? 6 : 0, only))
                return fail("that address cannot be encoded");
            return bytes_of(bytes);
        }
        if (m != "push")
            return fail("pop takes a register or a place in memory");
        const int64_t value = only.immediate;
        if (value >= -128 && value <= 127)
            return bytes_of({0x6a, static_cast<uint8_t>(value)});
        std::vector<uint8_t> bytes{0x68};
        push_u32(bytes, static_cast<uint32_t>(value));
        return bytes_of(bytes);
    }

    if (m == "lea") {
        if (ops.size() != 2)
            return wrong_operand_count(line, 2);
        Register destination;
        Memory from;
        if (!parse_register(ops[0], destination))
            return fail("lea answers into a register");
        if (!parse_memory(ops[1], from))
            return fail("lea takes the address of a place in memory");
        const Result bad = check_width(destination);
        if (!bad.ok)
            return bad;
        Operand rm;
        rm.kind = Operand::Kind::Mem;
        rm.mem = from;
        std::vector<uint8_t> bytes;
        if (!prefixes(bytes, destination.width, destination.number, rm))
            return bad_for_mode();
        bytes.push_back(0x8d);
        if (!encode_rm(bytes, destination.number, rm))
            return fail("that address cannot be encoded");
        return bytes_of(bytes);
    }

    if (m == "mov" || m == "movzx" || m == "movsx" || m == "movsxd") {
        if (ops.size() != 2)
            return wrong_operand_count(line, 2);
        Operand destination, source;
        if (!parse_operand(ops[0], destination) || !parse_operand(ops[1], source))
            return fail(m + " takes a register, a place in memory, or a value");
        if (destination.is_register()) {
            const Result bad = check_width(destination.reg);
            if (!bad.ok)
                return bad;
        }

        if (m == "movzx" || m == "movsx" || m == "movsxd") {
            if (!destination.is_register())
                return fail(m + " answers into a register");
            int from_width = source.is_register() ? source.reg.width : source.mem.width;
            if (from_width == 0)
                return fail(m + " needs to know how wide the value it reads is");
            std::vector<uint8_t> bytes;
            if (!prefixes(bytes, destination.reg.width, destination.reg.number, source))
                return bad_for_mode();
            if (m == "movsxd" || (m == "movsx" && from_width == 32)) {
                if (!sixty_four)
                    return fail("movsxd is 64-bit only");
                bytes.push_back(0x63);
            } else {
                bytes.push_back(0x0f);
                const bool sign = m == "movsx";
                bytes.push_back(static_cast<uint8_t>((sign ? 0xbe : 0xb6) + (from_width == 16 ? 1 : 0)));
            }
            if (!encode_rm(bytes, destination.reg.number, source))
                return fail("that address cannot be encoded");
            return bytes_of(bytes);
        }

        if (destination.is_memory() && source.is_memory())
            return fail("one side of a mov has to be a register");

        if (source.is_immediate()) {
            const int width = destination.is_register() ? destination.reg.width
                                                        : destination.mem.width;
            if (width == 0)
                return fail("say how wide the write is: mov dword ptr [...], 1");
            // Into a register, the short form carries the value directly.
            if (destination.is_register() && width != 8 &&
                !(width == 64 && (source.immediate < -(int64_t(1) << 31) ||
                                  source.immediate >= (int64_t(1) << 31)))) {
                std::vector<uint8_t> bytes;
                if (width == 16)
                    bytes.push_back(0x66);
                if (sixty_four)
                    add_rex(bytes, width == 64, 0, destination.reg.number);
                else if (destination.reg.number >= 8 || width == 64)
                    return bad_for_mode();
                // A 64-bit register given a value that fits in 32 bits is
                // written with the sign-extending form, which is shorter.
                if (width == 64) {
                    bytes.push_back(0xc7);
                    bytes.push_back(modrm_register(0, destination.reg.number));
                    push_u32(bytes, static_cast<uint32_t>(source.immediate));
                    return bytes_of(bytes);
                }
                bytes.push_back(static_cast<uint8_t>(0xb8 + (destination.reg.number & 7)));
                push_immediate(bytes, width, source.immediate);
                return bytes_of(bytes);
            }
            if (destination.is_register() && width == 64) {
                std::vector<uint8_t> bytes;
                add_rex(bytes, true, 0, destination.reg.number);
                bytes.push_back(static_cast<uint8_t>(0xb8 + (destination.reg.number & 7)));
                for (int i = 0; i < 8; ++i)
                    bytes.push_back(static_cast<uint8_t>(static_cast<uint64_t>(source.immediate) >> (i * 8)));
                return bytes_of(bytes);
            }
            std::vector<uint8_t> bytes;
            if (!prefixes(bytes, width, 0, destination))
                return bad_for_mode();
            bytes.push_back(width == 8 ? 0xc6 : 0xc7);
            if (!encode_rm(bytes, 0, destination))
                return fail("that address cannot be encoded");
            push_immediate(bytes, width == 64 ? 32 : width, source.immediate);
            return bytes_of(bytes);
        }

        // One side is a register; the other names the ModRM operand.
        const bool store = destination.is_memory();
        const Operand &rm = store ? destination : source;
        const Operand &reg = store ? source : destination;
        if (!reg.is_register())
            return fail("one side of a mov has to be a register");
        const int width = operand_width(destination, source);
        if (rm.is_register() && rm.reg.width != reg.reg.width)
            return fail("both registers have to be the same width");
        std::vector<uint8_t> bytes;
        if (!prefixes(bytes, width, reg.reg.number, rm))
            return bad_for_mode();
        bytes.push_back(static_cast<uint8_t>((width == 8 ? 0x88 : 0x89) + (store ? 0 : 2)));
        if (!encode_rm(bytes, reg.reg.number, rm))
            return fail("that address cannot be encoded");
        return bytes_of(bytes);
    }

    const auto arithmetic_slot = arithmetic().find(m);
    if (arithmetic_slot != arithmetic().end()) {
        if (ops.size() != 2)
            return wrong_operand_count(line, 2);
        Operand destination, source;
        if (!parse_operand(ops[0], destination) || !parse_operand(ops[1], source))
            return fail(m + " takes a register, a place in memory, or a value");
        if (destination.is_register()) {
            const Result bad = check_width(destination.reg);
            if (!bad.ok)
                return bad;
        }
        if (destination.is_memory() && source.is_memory())
            return fail("one side of " + m + " has to be a register");

        if (source.is_immediate()) {
            const int width = destination.is_register() ? destination.reg.width
                                                        : destination.mem.width;
            if (width == 0)
                return fail("say how wide the operation is: " + m + " dword ptr [...], 1");
            std::vector<uint8_t> bytes;
            if (!prefixes(bytes, width, 0, destination))
                return bad_for_mode();
            if (width != 8 && source.immediate >= -128 && source.immediate <= 127) {
                bytes.push_back(0x83);
                if (!encode_rm(bytes, arithmetic_slot->second, destination))
                    return fail("that address cannot be encoded");
                bytes.push_back(static_cast<uint8_t>(source.immediate));
                return bytes_of(bytes);
            }
            bytes.push_back(width == 8 ? 0x80 : 0x81);
            if (!encode_rm(bytes, arithmetic_slot->second, destination))
                return fail("that address cannot be encoded");
            push_immediate(bytes, width == 64 ? 32 : width, source.immediate);
            return bytes_of(bytes);
        }

        const bool store = destination.is_memory();
        const Operand &rm = store ? destination : source;
        const Operand &reg = store ? source : destination;
        if (!reg.is_register())
            return fail("one side of " + m + " has to be a register");
        const int width = operand_width(destination, source);
        if (rm.is_register() && rm.reg.width != reg.reg.width)
            return fail("both registers have to be the same width");
        std::vector<uint8_t> bytes;
        if (!prefixes(bytes, width, reg.reg.number, rm))
            return bad_for_mode();
        bytes.push_back(static_cast<uint8_t>(arithmetic_slot->second * 8 +
                                             (width == 8 ? 0x00 : 0x01) + (store ? 0 : 2)));
        if (!encode_rm(bytes, reg.reg.number, rm))
            return fail("that address cannot be encoded");
        return bytes_of(bytes);
    }

    if (m == "imul") {
        if (ops.size() != 2)
            return wrong_operand_count(line, 2);
        Operand destination, source;
        if (!parse_operand(ops[0], destination) || !parse_operand(ops[1], source))
            return fail("imul answers into a register");
        if (!destination.is_register())
            return fail("imul answers into a register");
        std::vector<uint8_t> bytes;
        if (!prefixes(bytes, destination.reg.width, destination.reg.number, source))
            return bad_for_mode();
        bytes.push_back(0x0f);
        bytes.push_back(0xaf);
        if (!encode_rm(bytes, destination.reg.number, source))
            return fail("that address cannot be encoded");
        return bytes_of(bytes);
    }

    // One operand, an opcode extension in the ModRM byte.
    static const std::map<std::string, int> one_operand = {
        {"neg", 3}, {"not", 2}, {"mul", 4}, {"imul1", 5}, {"div", 6}, {"idiv", 7},
    };
    const auto single = one_operand.find(m);
    if (single != one_operand.end()) {
        if (ops.size() != 1)
            return wrong_operand_count(line, 1);
        Operand only;
        if (!parse_operand(ops[0], only) || only.is_immediate())
            return fail(m + " takes a register or a place in memory");
        const int width = only.is_register() ? only.reg.width : only.mem.width;
        if (width == 0)
            return fail("say how wide the operation is: " + m + " dword ptr [...]");
        std::vector<uint8_t> bytes;
        if (!prefixes(bytes, width, 0, only))
            return bad_for_mode();
        bytes.push_back(width == 8 ? 0xf6 : 0xf7);
        if (!encode_rm(bytes, single->second, only))
            return fail("that address cannot be encoded");
        return bytes_of(bytes);
    }

    static const std::map<std::string, int> shifts = {
        {"rol", 0}, {"ror", 1}, {"rcl", 2}, {"rcr", 3},
        {"shl", 4}, {"shr", 5}, {"sal", 4}, {"sar", 7},
    };
    const auto shift = shifts.find(m);
    if (shift != shifts.end()) {
        if (ops.size() != 2)
            return wrong_operand_count(line, 2);
        Operand destination;
        if (!parse_operand(ops[0], destination) || destination.is_immediate())
            return fail(m + " shifts a register or a place in memory");
        const int width = destination.is_register() ? destination.reg.width : destination.mem.width;
        if (width == 0)
            return fail("say how wide the shift is: " + m + " dword ptr [...], 1");
        std::vector<uint8_t> bytes;
        if (!prefixes(bytes, width, 0, destination))
            return bad_for_mode();
        // By cl, or by a fixed amount.
        if (lower(trim(ops[1])) == "cl") {
            bytes.push_back(width == 8 ? 0xd2 : 0xd3);
            if (!encode_rm(bytes, shift->second, destination))
                return fail("that address cannot be encoded");
            return bytes_of(bytes);
        }
        int64_t amount = 0;
        if (!parse_immediate(ops[1], amount))
            return fail(m + " shifts by cl or by a fixed amount");
        if (amount == 1) {
            bytes.push_back(width == 8 ? 0xd0 : 0xd1);
            if (!encode_rm(bytes, shift->second, destination))
                return fail("that address cannot be encoded");
            return bytes_of(bytes);
        }
        bytes.push_back(width == 8 ? 0xc0 : 0xc1);
        if (!encode_rm(bytes, shift->second, destination))
            return fail("that address cannot be encoded");
        bytes.push_back(static_cast<uint8_t>(amount));
        return bytes_of(bytes);
    }

    if (m == "test") {
        if (ops.size() != 2)
            return wrong_operand_count(line, 2);
        Operand destination, source;
        if (!parse_operand(ops[0], destination) || !parse_operand(ops[1], source))
            return fail("test takes a register or a place in memory");
        if (!source.is_register())
            return fail("the second operand of test has to be a register");
        const int width = operand_width(destination, source);
        std::vector<uint8_t> bytes;
        if (!prefixes(bytes, width, source.reg.number, destination))
            return bad_for_mode();
        bytes.push_back(width == 8 ? 0x84 : 0x85);
        if (!encode_rm(bytes, source.reg.number, destination))
            return fail("that address cannot be encoded");
        return bytes_of(bytes);
    }

    // setcc: a byte set to one or zero from the flags.
    if (m.size() > 3 && m.compare(0, 3, "set") == 0) {
        const auto condition = conditions().find(m.substr(3));
        if (condition != conditions().end()) {
            if (ops.size() != 1)
                return wrong_operand_count(line, 1);
            Operand only;
            if (!parse_operand(ops[0], only) || only.is_immediate())
                return fail(m + " writes to a byte register or a place in memory");
            std::vector<uint8_t> bytes;
            if (!prefixes(bytes, 8, 0, only))
                return bad_for_mode();
            bytes.push_back(0x0f);
            bytes.push_back(static_cast<uint8_t>(0x90 + condition->second));
            if (!encode_rm(bytes, 0, only))
                return fail("that address cannot be encoded");
            return bytes_of(bytes);
        }
    }

    // cmovcc: a move that only happens when the flags say so.
    if (m.size() > 4 && m.compare(0, 4, "cmov") == 0) {
        const auto condition = conditions().find(m.substr(4));
        if (condition != conditions().end()) {
            if (ops.size() != 2)
                return wrong_operand_count(line, 2);
            Operand destination, source;
            if (!parse_operand(ops[0], destination) || !parse_operand(ops[1], source))
                return fail(m + " answers into a register");
            if (!destination.is_register())
                return fail(m + " answers into a register");
            std::vector<uint8_t> bytes;
            if (!prefixes(bytes, destination.reg.width, destination.reg.number, source))
                return bad_for_mode();
            bytes.push_back(0x0f);
            bytes.push_back(static_cast<uint8_t>(0x40 + condition->second));
            if (!encode_rm(bytes, destination.reg.number, source))
                return fail("that address cannot be encoded");
            return bytes_of(bytes);
        }
    }

    if (m == "jmp" || m == "call") {
        if (ops.size() != 1)
            return wrong_operand_count(line, 1);
        // Through a register or a place in memory, when the target is not
        // known until it runs.
        Operand through;
        if (parse_operand(ops[0], through) && !through.is_immediate()) {
            std::vector<uint8_t> bytes;
            int index_number = 0, base_number = 0;
            if (through.is_memory())
                rex_bits_for(through.mem, index_number, base_number);
            else
                base_number = through.reg.number;
            if (sixty_four && (index_number >= 8 || base_number >= 8)) {
                uint8_t rex = 0x40;
                if (index_number >= 8) rex |= 0x02;
                if (base_number >= 8) rex |= 0x01;
                bytes.push_back(rex);
            }
            bytes.push_back(0xff);
            if (!encode_rm(bytes, m == "call" ? 2 : 4, through))
                return fail("that address cannot be encoded");
            return bytes_of(bytes);
        }
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
