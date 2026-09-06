// Writing x86-64, behind the same seam arm64 sits behind.
//
// Values live in frame slots and are addressed from the stack pointer, exactly
// as the generator above describes them. Nothing is kept in a register between
// one operation and the next: every step loads what it needs, works, and puts
// the answer back. That is more instructions than the machine needs, and it is
// the version whose every step can be checked against what the frame says is
// true. Holding values in registers is an optimisation to be added on top of
// something already correct, not the way to arrive at it.
#include "codegen.hh"

#include <sstream>

namespace astral_internal {
namespace compiler {
namespace {

std::string hex(uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string signed_offset(int64_t value)
{
    std::ostringstream out;
    if (value < 0)
        out << "-0x" << std::hex << static_cast<uint64_t>(-value);
    else
        out << "0x" << std::hex << static_cast<uint64_t>(value);
    return out.str();
}

// The name of a general register at a given width.
std::string reg(const char *base64, int width)
{
    const std::string name(base64);
    if (width == 8) {
        if (name == "rax") return "al";
        if (name == "rcx") return "cl";
        if (name == "rdx") return "dl";
        if (name == "rbx") return "bl";
        if (name == "rsi") return "sil";
        if (name == "rdi") return "dil";
        return name.substr(1) + "b";
    }
    if (width == 16) {
        if (name[0] == 'r' && name.size() == 3 && name[1] >= 'a' && name[1] <= 'z')
            return name.substr(1);
        return name + "w";
    }
    if (width == 32) {
        if (name[0] == 'r' && name.size() == 3 && name[1] >= 'a' && name[1] <= 'z')
            return "e" + name.substr(1);
        return name + "d";
    }
    return name;
}

const char *size_word(int width)
{
    switch (width) {
    case 1: return "byte";
    case 2: return "word";
    case 4: return "dword";
    default: return "qword";
    }
}

// What a call looks like, which is the whole of the difference between the two
// conventions this machine runs under.
struct CallRules {
    const char *const *argument_registers;
    int argument_register_count;
    // Windows makes the caller set aside room for the register arguments as
    // well, above anything passed on the stack, whether or not it is used.
    uint64_t shadow_bytes;
};

const char *const kSystemVArguments[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
const char *const kMicrosoftArguments[] = {"rcx", "rdx", "r8", "r9"};

CallRules rules_for(Abi abi)
{
    if (abi == Abi::Microsoft)
        return CallRules{kMicrosoftArguments, 4, 32};
    return CallRules{kSystemVArguments, 6, 0};
}

class X86_64 : public Machine {
public:
    explicit X86_64(Abi abi) : rules_(rules_for(abi)), abi_(abi) {}

    int slot_size() const override { return 8; }
    uint64_t frame_alignment() const override { return 16; }
    // The call that reached here pushed a return address, so the frame is
    // eight bytes out of step with what a call needs. Taking those eight back
    // is what keeps the stack aligned at every call this body makes.
    uint64_t reserved_bytes() const override { return 8; }
    uint64_t outgoing_bytes(const CallSite &site) const override;

    void prologue() override;
    void epilogue() override;
    void spill_parameters(const std::vector<Parameter> &parameters) override;

    void push_constant(int depth, uint64_t value) override;
    void push_frame_address(int depth, int64_t offset) override;
    void push_absolute(int depth, uint64_t address) override;
    void duplicate(int depth) override;
    void move_slot(int to, int from) override;
    void swap_slots(int first, int second) override;

    void load_indirect(int depth, const Operation &op) override;
    void store_indirect(int depth, const Operation &op) override;
    void unary(int depth, UnaryOp what, const Operation &op) override;
    void binary(int depth, BinaryOp what, const Operation &op) override;
    void convert(int depth, const Operation &from, const Operation &to) override;

    void jump(int depth, const std::string &label) override;
    void branch_if_zero(int depth, const std::string &label) override;
    void branch_if_nonzero(int depth, const std::string &label) override;
    void branch_if_equal(int depth, uint64_t value, const std::string &label) override;

    void call(int depth, const CallSite &site) override;
    void return_value(int depth) override;
    void return_nothing() override;

private:
    CallRules rules_;
    Abi abi_ = Abi::SystemV;

    void say_line(const std::string &text) { out_->instruction(text); }
    int64_t slot_offset(int index) const { return eval_base_ + int64_t(index) * 8; }
    // How the slot is written as an address.
    std::string slot(int index) const
    {
        return "[rsp+" + hex(static_cast<uint64_t>(slot_offset(index))) + "]";
    }
    // What the whole frame occupies, which is what the caller asked for plus
    // the eight bytes that put the stack back in step.
    uint64_t total() const { return frame_size_ + 8; }

    // A slot into a register, and a register back into a slot.
    void load(const std::string &into, int index) { say_line("mov " + into + ", " + slot(index)); }
    void store(int index, const std::string &from) { say_line("mov " + slot(index) + ", " + from); }
    // Puts a value of any size into a register.
    void constant(const std::string &into, uint64_t value);
    // Leaves the top of the stack compared against zero.
    void test_top(int index);
};

uint64_t X86_64::outgoing_bytes(const CallSite &site) const
{
    // Arguments past the sixth are handed over on the stack, and the machine
    // wants the stack aligned when the call is made.
    const size_t count = site.types.size();
    uint64_t extra = 0;
    if (count > static_cast<size_t>(rules_.argument_register_count))
        extra = (count - rules_.argument_register_count) * 8;
    return (extra + rules_.shadow_bytes + 15) / 16 * 16;
}

void X86_64::constant(const std::string &into, uint64_t value)
{
    // The short form only carries a signed thirty-two bit value; anything
    // wider is written out in full.
    const int64_t as_signed = static_cast<int64_t>(value);
    if (as_signed >= -(int64_t(1) << 31) && as_signed < (int64_t(1) << 31))
        say_line("mov " + into + ", " + hex(value));
    else
        say_line("mov " + into + ", " + hex(value));
}

void X86_64::prologue()
{
    if (total() > 0)
        say_line("sub rsp, " + hex(total()));
}

void X86_64::epilogue()
{
    if (total() > 0)
        say_line("add rsp, " + hex(total()));
    say_line("ret");
    out_->tighten();
}

void X86_64::spill_parameters(const std::vector<Parameter> &parameters)
{
    // What arrived in a register goes to the slot the body reads it from, and
    // what arrived above the frame is copied down to one.
    int in_register = 0;
    // The return address sits at the top of what the caller left, and stack
    // arguments start just above it. Under Windows the caller also set aside
    // room for the register arguments, which sits in between.
    uint64_t from_caller = total() + 8 + rules_.shadow_bytes;
    for (const Parameter &parameter : parameters) {
        const uint64_t size = TypeStore::size_of(parameter.type);
        const int width = size == 1 || size == 2 || size == 4 ? static_cast<int>(size) : 8;
        const std::string place =
            "[rsp+" + hex(static_cast<uint64_t>(parameter.frame_offset)) + "]";
        if (in_register < rules_.argument_register_count) {
            const char *source = rules_.argument_registers[in_register++];
            // The whole slot is written so nothing of an earlier value is left
            // in the bytes this one does not fill.
            if (width == 8) {
                say_line("mov " + place + ", " + source);
            } else {
                say_line("mov " + std::string(reg(source, 32)) + ", " + reg(source, 32));
                say_line("mov qword ptr " + place + ", 0");
                say_line("mov " + std::string(size_word(width)) + " ptr " + place + ", " +
                         reg(source, width * 8));
            }
            continue;
        }
        say_line("mov rax, [rsp+" + hex(from_caller) + "]");
        say_line("mov " + place + ", rax");
        from_caller += 8;
    }
}

void X86_64::push_constant(int depth, uint64_t value)
{
    const int64_t as_signed = static_cast<int64_t>(value);
    if (as_signed >= -(int64_t(1) << 31) && as_signed < (int64_t(1) << 31)) {
        say_line("mov qword ptr " + slot(depth) + ", " + hex(value));
        return;
    }
    constant("rax", value);
    store(depth, "rax");
}

void X86_64::push_frame_address(int depth, int64_t offset)
{
    say_line("lea rax, [rsp+" + hex(static_cast<uint64_t>(offset)) + "]");
    store(depth, "rax");
}

void X86_64::push_absolute(int depth, uint64_t address)
{
    constant("rax", address);
    store(depth, "rax");
}

void X86_64::duplicate(int depth)
{
    load("rax", depth - 1);
    store(depth, "rax");
}

void X86_64::move_slot(int to, int from)
{
    if (to == from)
        return;
    load("rax", from);
    store(to, "rax");
}

void X86_64::swap_slots(int first, int second)
{
    if (first == second)
        return;
    load("rax", first);
    load("rcx", second);
    store(first, "rcx");
    store(second, "rax");
}

void X86_64::load_indirect(int depth, const Operation &op)
{
    const int index = depth - 1;
    load("rcx", index);
    const int width = op.width;
    if (width == 8) {
        say_line("mov rax, [rcx]");
    } else if (width == 4) {
        if (op.is_signed)
            say_line("movsxd rax, dword ptr [rcx]");
        else
            say_line("mov eax, dword ptr [rcx]");
    } else {
        const std::string kind = op.is_signed ? "movsx" : "movzx";
        say_line(kind + " rax, " + size_word(width) + " ptr [rcx]");
    }
    store(index, "rax");
}

void X86_64::store_indirect(int depth, const Operation &op)
{
    // The value is on top and the address below it; what is stored stays as
    // the answer, in the slot the address occupied.
    const int value_index = depth - 1;
    const int address_index = depth - 2;
    load("rax", value_index);
    load("rcx", address_index);
    const int width = op.width;
    say_line("mov " + std::string(size_word(width)) + " ptr [rcx], " + reg("rax", width * 8));
    store(address_index, "rax");
}

void X86_64::unary(int depth, UnaryOp what, const Operation &op)
{
    const int index = depth - 1;
    load("rax", index);
    switch (what) {
    case UnaryOp::Minus:
        say_line("neg rax");
        break;
    case UnaryOp::BitNot:
        say_line("not rax");
        break;
    case UnaryOp::Not:
        say_line("test rax, rax");
        say_line("sete al");
        say_line("movzx rax, al");
        break;
    case UnaryOp::Plus:
        break;
    default:
        say("that operation cannot be written yet");
        return;
    }
    (void)op;
    store(index, "rax");
}

void X86_64::binary(int depth, BinaryOp what, const Operation &op)
{
    // The left operand is below the right; the answer takes the left's place.
    const int right = depth - 1;
    const int left = depth - 2;
    load("rax", left);
    load("rcx", right);
    const bool wide = op.width == 8;
    const std::string a = wide ? "rax" : "eax";
    const std::string c = wide ? "rcx" : "ecx";

    auto compare_into = [&](const char *set) {
        say_line("cmp " + a + ", " + c);
        say_line(std::string("set") + set + " al");
        say_line("movzx rax, al");
    };

    switch (what) {
    case BinaryOp::Add: say_line("add rax, rcx"); break;
    case BinaryOp::Subtract: say_line("sub rax, rcx"); break;
    case BinaryOp::Multiply: say_line("imul rax, rcx"); break;
    case BinaryOp::BitAnd: say_line("and rax, rcx"); break;
    case BinaryOp::BitOr: say_line("or rax, rcx"); break;
    case BinaryOp::BitXor: say_line("xor rax, rcx"); break;
    case BinaryOp::Divide:
    case BinaryOp::Modulo: {
        // The dividend is the whole of rdx:rax, so the top half has to say
        // what the sign is before the division runs.
        if (op.is_signed) {
            say_line(wide ? "cqo" : "cdq");
            say_line("idiv " + c);
        } else {
            say_line("xor rdx, rdx");
            say_line("div " + c);
        }
        if (what == BinaryOp::Modulo)
            say_line("mov rax, rdx");
        break;
    }
    case BinaryOp::ShiftLeft:
        say_line("shl rax, cl");
        break;
    case BinaryOp::ShiftRight:
        say_line(op.is_signed ? "sar rax, cl" : "shr rax, cl");
        break;
    case BinaryOp::Less: compare_into(op.is_signed ? "l" : "b"); break;
    case BinaryOp::LessEqual: compare_into(op.is_signed ? "le" : "be"); break;
    case BinaryOp::Greater: compare_into(op.is_signed ? "g" : "a"); break;
    case BinaryOp::GreaterEqual: compare_into(op.is_signed ? "ge" : "ae"); break;
    case BinaryOp::Equal: compare_into("e"); break;
    case BinaryOp::NotEqual: compare_into("ne"); break;
    case BinaryOp::LogicalAnd:
    case BinaryOp::LogicalOr: {
        // Both sides are already worked out by the time this runs, so the
        // answer is whether each is anything other than zero.
        say_line("test rax, rax");
        say_line("setne al");
        say_line("movzx rax, al");
        say_line("test rcx, rcx");
        say_line("setne cl");
        say_line("movzx rcx, cl");
        say_line(what == BinaryOp::LogicalAnd ? "and rax, rcx" : "or rax, rcx");
        break;
    }
    default:
        say("that operation cannot be written yet");
        return;
    }
    store(left, "rax");
}

void X86_64::convert(int depth, const Operation &from, const Operation &to)
{
    const int index = depth - 1;
    if (from.width == to.width && from.is_signed == to.is_signed)
        return;
    load("rax", index);
    // Narrowing keeps the low bytes; widening says what the top ones are.
    if (to.width >= from.width) {
        if (from.width == 8) {
            // Nothing to widen into.
        } else if (from.width == 4) {
            if (from.is_signed)
                say_line("movsxd rax, eax");
            else
                say_line("mov eax, eax");
        } else {
            const std::string kind = from.is_signed ? "movsx" : "movzx";
            say_line(kind + " rax, " + reg("rax", from.width * 8));
        }
    } else if (to.width == 4) {
        say_line("mov eax, eax");
    } else {
        say_line("movzx rax, " + reg("rax", to.width * 8));
    }
    store(index, "rax");
}

void X86_64::test_top(int index)
{
    load("rax", index);
    say_line("test rax, rax");
}

void X86_64::jump(int depth, const std::string &label)
{
    (void)depth;
    say_line("jmp %" + label + "%");
}

void X86_64::branch_if_zero(int depth, const std::string &label)
{
    test_top(depth - 1);
    say_line("je %" + label + "%");
}

void X86_64::branch_if_nonzero(int depth, const std::string &label)
{
    test_top(depth - 1);
    say_line("jne %" + label + "%");
}

void X86_64::branch_if_equal(int depth, uint64_t value, const std::string &label)
{
    load("rax", depth - 1);
    const int64_t as_signed = static_cast<int64_t>(value);
    if (as_signed >= -(int64_t(1) << 31) && as_signed < (int64_t(1) << 31)) {
        say_line("cmp rax, " + hex(value));
    } else {
        constant("rcx", value);
        say_line("cmp rax, rcx");
    }
    say_line("je %" + label + "%");
}

void X86_64::call(int depth, const CallSite &site)
{
    const size_t count = site.types.size();
    const int first = depth - static_cast<int>(count);
    // The arguments occupy the top slots, in order. Those past the sixth are
    // written to the bottom of the frame, where the callee looks for them.
    for (size_t i = 0; i < count; ++i) {
        const int index = first + static_cast<int>(i);
        if (i < static_cast<size_t>(rules_.argument_register_count)) {
            load(rules_.argument_registers[i], index);
            continue;
        }
        load("rax", index);
        const uint64_t at = rules_.shadow_bytes + (i - rules_.argument_register_count) * 8;
        say_line("mov [rsp+" + hex(at) + "], rax");
    }
    // A variadic callee under the common convention reads how many arguments
    // came in vector registers out of al, and none of them did. Windows has no
    // such rule.
    if (site.variadic && abi_ != Abi::Microsoft)
        say_line("mov eax, 0x0");

    if (site.through_pointer) {
        load("r11", first - 1);
        say_line("call r11");
    } else {
        say_line("call " + hex(site.address));
    }
    if (site.result != nullptr) {
        const int answer = site.through_pointer ? first - 1 : first;
        store(answer, "rax");
    }
}

void X86_64::return_value(int depth)
{
    load("rax", depth - 1);
    say_line("jmp %" + leave_ + "%");
}

void X86_64::return_nothing()
{
    say_line("jmp %" + leave_ + "%");
}

// ------------------------------------------------------------------ i386
//
// The same shape as above with the machine's own width taken away: every
// argument arrives on the stack, every value is four bytes, and the answer
// goes back in eax. What a 32-bit machine cannot do is work on eight bytes at
// once, so that is refused rather than written wrongly.
class X86 : public Machine {
public:
    int slot_size() const override { return 4; }
    uint64_t frame_alignment() const override { return 16; }
    // The call that reached here pushed four bytes, and the convention wants
    // the stack aligned to sixteen at the next one. Twelve puts it back.
    uint64_t reserved_bytes() const override { return 12; }
    uint64_t outgoing_bytes(const CallSite &site) const override
    {
        return (site.types.size() * 4 + 15) / 16 * 16;
    }

    void prologue() override
    {
        if (total() > 0)
            say_line("sub esp, " + hex(total()));
    }
    void epilogue() override
    {
        if (total() > 0)
            say_line("add esp, " + hex(total()));
        say_line("ret");
        out_->tighten();
    }

    void spill_parameters(const std::vector<Parameter> &parameters) override
    {
        // Everything arrived above the frame, in order, four bytes each.
        uint64_t from_caller = total() + 4;
        for (const Parameter &parameter : parameters) {
            if (TypeStore::size_of(parameter.type) > 4) {
                say("a value of more than four bytes cannot be passed in 32-bit code yet");
                return;
            }
            say_line("mov eax, [esp+" + hex(from_caller) + "]");
            say_line("mov [esp+" + hex(static_cast<uint64_t>(parameter.frame_offset)) + "], eax");
            from_caller += 4;
        }
    }

    void push_constant(int depth, uint64_t value) override
    {
        say_line("mov dword ptr " + slot(depth) + ", " + hex(value & 0xffffffffu));
    }
    void push_frame_address(int depth, int64_t offset) override
    {
        say_line("lea eax, [esp+" + hex(static_cast<uint64_t>(offset)) + "]");
        store(depth, "eax");
    }
    void push_absolute(int depth, uint64_t address) override
    {
        say_line("mov eax, " + hex(address & 0xffffffffu));
        store(depth, "eax");
    }
    void duplicate(int depth) override
    {
        load("eax", depth - 1);
        store(depth, "eax");
    }
    void move_slot(int to, int from) override
    {
        if (to == from)
            return;
        load("eax", from);
        store(to, "eax");
    }
    void swap_slots(int first, int second) override
    {
        if (first == second)
            return;
        load("eax", first);
        load("ecx", second);
        store(first, "ecx");
        store(second, "eax");
    }

    void load_indirect(int depth, const Operation &op) override
    {
        const int index = depth - 1;
        if (!fits(op.width))
            return;
        load("ecx", index);
        if (op.width == 4)
            say_line("mov eax, dword ptr [ecx]");
        else
            say_line(std::string(op.is_signed ? "movsx" : "movzx") + " eax, " +
                     size_word(op.width) + " ptr [ecx]");
        store(index, "eax");
    }
    void store_indirect(int depth, const Operation &op) override
    {
        if (!fits(op.width))
            return;
        const int value_index = depth - 1;
        const int address_index = depth - 2;
        load("eax", value_index);
        load("ecx", address_index);
        say_line("mov " + std::string(size_word(op.width)) + " ptr [ecx], " +
                 reg("rax", op.width * 8));
        store(address_index, "eax");
    }

    void unary(int depth, UnaryOp what, const Operation &op) override
    {
        (void)op;
        const int index = depth - 1;
        load("eax", index);
        switch (what) {
        case UnaryOp::Minus: say_line("neg eax"); break;
        case UnaryOp::BitNot: say_line("not eax"); break;
        case UnaryOp::Not:
            say_line("test eax, eax");
            say_line("sete al");
            say_line("movzx eax, al");
            break;
        case UnaryOp::Plus: break;
        default:
            say("that operation cannot be written yet");
            return;
        }
        store(index, "eax");
    }

    void binary(int depth, BinaryOp what, const Operation &op) override
    {
        if (!fits(op.width))
            return;
        const int right = depth - 1;
        const int left = depth - 2;
        load("eax", left);
        load("ecx", right);
        auto compare_into = [&](const char *set) {
            say_line("cmp eax, ecx");
            say_line(std::string("set") + set + " al");
            say_line("movzx eax, al");
        };
        switch (what) {
        case BinaryOp::Add: say_line("add eax, ecx"); break;
        case BinaryOp::Subtract: say_line("sub eax, ecx"); break;
        case BinaryOp::Multiply: say_line("imul eax, ecx"); break;
        case BinaryOp::BitAnd: say_line("and eax, ecx"); break;
        case BinaryOp::BitOr: say_line("or eax, ecx"); break;
        case BinaryOp::BitXor: say_line("xor eax, ecx"); break;
        case BinaryOp::Divide:
        case BinaryOp::Modulo:
            if (op.is_signed) {
                say_line("cdq");
                say_line("idiv ecx");
            } else {
                say_line("xor edx, edx");
                say_line("div ecx");
            }
            if (what == BinaryOp::Modulo)
                say_line("mov eax, edx");
            break;
        case BinaryOp::ShiftLeft: say_line("shl eax, cl"); break;
        case BinaryOp::ShiftRight: say_line(op.is_signed ? "sar eax, cl" : "shr eax, cl"); break;
        case BinaryOp::Less: compare_into(op.is_signed ? "l" : "b"); break;
        case BinaryOp::LessEqual: compare_into(op.is_signed ? "le" : "be"); break;
        case BinaryOp::Greater: compare_into(op.is_signed ? "g" : "a"); break;
        case BinaryOp::GreaterEqual: compare_into(op.is_signed ? "ge" : "ae"); break;
        case BinaryOp::Equal: compare_into("e"); break;
        case BinaryOp::NotEqual: compare_into("ne"); break;
        case BinaryOp::LogicalAnd:
        case BinaryOp::LogicalOr:
            say_line("test eax, eax");
            say_line("setne al");
            say_line("movzx eax, al");
            say_line("test ecx, ecx");
            say_line("setne cl");
            say_line("movzx ecx, cl");
            say_line(what == BinaryOp::LogicalAnd ? "and eax, ecx" : "or eax, ecx");
            break;
        default:
            say("that operation cannot be written yet");
            return;
        }
        store(left, "eax");
    }

    void convert(int depth, const Operation &from, const Operation &to) override
    {
        if (!fits(from.width) || !fits(to.width))
            return;
        const int index = depth - 1;
        if (from.width == to.width && from.is_signed == to.is_signed)
            return;
        load("eax", index);
        if (to.width >= from.width && from.width < 4)
            say_line(std::string(from.is_signed ? "movsx" : "movzx") + " eax, " +
                     reg("rax", from.width * 8));
        else if (to.width < 4)
            say_line("movzx eax, " + reg("rax", to.width * 8));
        store(index, "eax");
    }

    void jump(int depth, const std::string &label) override
    {
        (void)depth;
        say_line("jmp %" + label + "%");
    }
    void branch_if_zero(int depth, const std::string &label) override
    {
        load("eax", depth - 1);
        say_line("test eax, eax");
        say_line("je %" + label + "%");
    }
    void branch_if_nonzero(int depth, const std::string &label) override
    {
        load("eax", depth - 1);
        say_line("test eax, eax");
        say_line("jne %" + label + "%");
    }
    void branch_if_equal(int depth, uint64_t value, const std::string &label) override
    {
        load("eax", depth - 1);
        say_line("cmp eax, " + hex(value & 0xffffffffu));
        say_line("je %" + label + "%");
    }

    void call(int depth, const CallSite &site) override
    {
        // Everything goes on the stack, in order, starting at the bottom of
        // the frame where the callee looks for it.
        const size_t count = site.types.size();
        const int first = depth - static_cast<int>(count);
        for (size_t i = 0; i < count; ++i) {
            load("eax", first + static_cast<int>(i));
            say_line("mov [esp+" + hex(i * 4) + "], eax");
        }
        if (site.through_pointer) {
            load("edx", first - 1);
            say_line("call edx");
        } else {
            say_line("call " + hex(site.address));
        }
        if (site.result != nullptr)
            store(site.through_pointer ? first - 1 : first, "eax");
    }

    void return_value(int depth) override
    {
        load("eax", depth - 1);
        say_line("jmp %" + leave_ + "%");
    }
    void return_nothing() override { say_line("jmp %" + leave_ + "%"); }

private:
    void say_line(const std::string &text) { out_->instruction(text); }
    int64_t slot_offset(int index) const { return eval_base_ + int64_t(index) * 4; }
    std::string slot(int index) const
    {
        return "[esp+" + hex(static_cast<uint64_t>(slot_offset(index))) + "]";
    }
    uint64_t total() const { return frame_size_ + 12; }
    void load(const std::string &into, int index) { say_line("mov " + into + ", " + slot(index)); }
    void store(int index, const std::string &from) { say_line("mov " + slot(index) + ", " + from); }
    // Eight bytes at once is what this machine does not have.
    bool fits(int width)
    {
        if (width <= 4)
            return true;
        say("a value of more than four bytes cannot be worked on in 32-bit code yet");
        return false;
    }
};

} // namespace

std::unique_ptr<Machine> machine_for_x86(assembler::Target target, Abi abi)
{
    if (target == assembler::Target::X86_64)
        return std::unique_ptr<Machine>(new X86_64(abi));
    if (target == assembler::Target::X86)
        return std::unique_ptr<Machine>(new X86());
    return nullptr;
}

} // namespace compiler
} // namespace astral_internal
