// AArch64: the frame, the calling convention and one instruction per idea.
//
// The frame is fixed for the life of the call and addressed from the stack
// pointer, so nothing here has to track a moving offset:
//
//     sp + frame - 8    the return address
//     sp + frame - 16   the caller's frame pointer, which x29 then points at
//     ...               named variables and parameters
//     ...               the evaluation stack
//     sp + 0            arguments being passed to something else
//
// Arguments go out at the bottom because that is where a callee reads them.
// The evaluation stack sits above them, so filling in a call's arguments never
// disturbs a value waiting to be used.
//
// A frame slot is where a value definitely is. Between one slot and the next
// use of it, the value is usually somewhere better: in one of x9 to x15, or
// known outright as a constant or as an offset from the stack pointer, in
// which case no instruction is written for it at all. Everything that is only
// true of a straight run of instructions is given up at `settle` and
// `forget_registers`, which the generator calls wherever control can arrive
// from more than one direction.
#include "codegen.hh"

#include <algorithm>
#include <cstdint>
#include <sstream>

namespace astral_internal {
namespace compiler {
namespace {

// The registers a value may live in between two slots. All of them are the
// caller's to lose, so a call gives every one of them up.
const char *const kPool[] = {"x9", "x10", "x11", "x12", "x13", "x14", "x15"};
const int kPoolCount = 7;
// x16 is the register a long call goes through; the architecture reserves it
// for exactly that.
const char *const kCallee = "x16";

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

// The 32-bit name of a register written the 64-bit way.
std::string narrow(const std::string &wide)
{
    return "w" + wide.substr(1);
}

std::string sized(const std::string &wide, int width)
{
    return width >= 8 ? wide : narrow(wide);
}

// A value of `width` bytes, held in as many of the low bits of a doubleword as
// it takes and standing for the same number in all sixty-four.
uint64_t extend_to(uint64_t value, int width, bool is_signed)
{
    if (width >= 8)
        return value;
    const int bits = width * 8;
    const uint64_t mask = (uint64_t(1) << bits) - 1;
    value &= mask;
    if (is_signed && (value & (uint64_t(1) << (bits - 1))) != 0)
        value |= ~mask;
    return value;
}

bool is_power_of_two(uint64_t value, int &log)
{
    if (value == 0 || (value & (value - 1)) != 0)
        return false;
    log = 0;
    while ((value >> log) != 1)
        ++log;
    return true;
}

// What a comparison of this kind and sign answers to, and what it answers to
// when the two sides are written the other way round.
const char *condition_for(BinaryOp what, bool is_signed)
{
    switch (what) {
    case BinaryOp::Equal: return "eq";
    case BinaryOp::NotEqual: return "ne";
    case BinaryOp::Less: return is_signed ? "lt" : "lo";
    case BinaryOp::LessEqual: return is_signed ? "le" : "ls";
    case BinaryOp::Greater: return is_signed ? "gt" : "hi";
    case BinaryOp::GreaterEqual: return is_signed ? "ge" : "hs";
    default: return "al";
    }
}

BinaryOp reversed(BinaryOp what)
{
    switch (what) {
    case BinaryOp::Less: return BinaryOp::Greater;
    case BinaryOp::LessEqual: return BinaryOp::GreaterEqual;
    case BinaryOp::Greater: return BinaryOp::Less;
    case BinaryOp::GreaterEqual: return BinaryOp::LessEqual;
    default: return what;
    }
}

bool is_comparison(BinaryOp what)
{
    switch (what) {
    case BinaryOp::Less:
    case BinaryOp::LessEqual:
    case BinaryOp::Greater:
    case BinaryOp::GreaterEqual:
    case BinaryOp::Equal:
    case BinaryOp::NotEqual:
        return true;
    default:
        return false;
    }
}

// Everything known about one value on the evaluation stack.
//
// The invariant that makes the narrow forms safe: when `extended` is false the
// width is four, so the low thirty-two bits of the register are always the
// value and every operation narrower than a doubleword can use the w form
// without widening anything first.
struct Held {
    bool in_memory = false;      // the frame slot holds it
    std::string reg;             // where it is, or empty
    bool borrowed = false;       // the register is not one of the pool's
    bool is_constant = false;
    uint64_t constant = 0;
    bool is_frame_address = false;
    int64_t frame_offset = 0;
    int width = 8;               // what the value's type is worth in bytes
    bool width_signed = true;
    bool extended = true;        // the bits above `width` are its extension

    bool known() const { return in_memory || !reg.empty() || is_constant || is_frame_address; }
};

// What the frame is known to hold at one offset, so reading a variable back
// straight after writing it costs nothing.
struct Remembered {
    int64_t offset = 0;
    int type_width = 8;
    bool type_signed = true;
    std::string reg;
    bool borrowed = false;
    int width = 8;
    bool width_signed = true;
    bool extended = true;
};

class Arm64 : public Machine {
public:
    int slot_size() const override { return 8; }
    uint64_t frame_alignment() const override { return 16; }
    // A leaf keeps neither a return address nor a frame pointer, so the top of
    // the frame is free for what the body actually uses.
    uint64_t reserved_bytes() const override { return leaf_ ? 0 : 16; }
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

    void settle(int depth) override;
    void forget_registers() override;

    void jump(int depth, const std::string &label) override;
    void branch_if_zero(int depth, const std::string &label) override;
    void branch_if_nonzero(int depth, const std::string &label) override;
    void branch_if_equal(int depth, uint64_t value, const std::string &label) override;

    void call(int depth, const CallSite &site) override;
    void return_value(int depth) override;
    void return_nothing() override;

private:
    void say_line(const std::string &text) { out_->instruction(text); }
    int64_t slot_offset(int index) const { return eval_base_ + int64_t(index) * 8; }
    Held &at(int index);

    // --- registers ---
    std::string take_register();
    void release(const std::string &name);
    void release_slot(int index);
    // Puts the slot's value in a register and answers with its name.
    std::string in_register(int index);
    // Makes the whole register stand for the value, not just its low bytes.
    void make_extended(int index);
    // Puts the slot's value where the frame says it is.
    void spill(int index);
    void evict(int index);
    // What a call needs: everything in a register goes back to its slot, but a
    // constant and an offset from the stack pointer are neither in a register
    // nor lost by anything, so they cost nothing to keep.
    void hold_over(int depth);
    void clobber();
    void set_register(int index, const std::string &name, int width, bool width_signed,
                      bool extended);
    void set_constant(int index, uint64_t value, int width, bool width_signed);

    // --- what the frame is known to hold ---
    void remember(int64_t offset, const Operation &op, const Held &value);
    const Remembered *recall(int64_t offset, const Operation &op) const;
    void forget_offset(int64_t offset);
    void forget_memory() { remembered_.clear(); }
    void forget_borrowed();

    // Writes any value at all into a register, however many instructions it takes.
    void constant(const std::string &into, uint64_t value);
    void frame_address(const std::string &into, int64_t offset);
    void widen(const std::string &reg, int width, bool is_signed);
    // Adds or takes away a constant with one instruction when the encoding
    // reaches; answers false when it does not.
    bool add_immediate(const std::string &into, const std::string &from, int64_t amount, int width);
    // Compares against a constant with one instruction when the encoding
    // reaches; answers false when it does not.
    bool compare_immediate(const std::string &value, uint64_t against, int width, bool is_signed);
    // Folds an operation on two known numbers, or answers false.
    bool fold(BinaryOp what, const Operation &op, uint64_t left, uint64_t right, uint64_t &out);
    // Writes the run of instructions for a load or a store at one width.
    void memory_access(bool loading, const Operation &op, const std::string &value,
                       const std::string &address);
    void tested_register(int index, std::string &reg, bool &is_wide);

    std::vector<Held> slots_;
    std::vector<Remembered> remembered_;
    // Which slot has each pool register, or -1.
    int owner_[kPoolCount] = {-1, -1, -1, -1, -1, -1, -1};
};

Held &Arm64::at(int index)
{
    if (static_cast<size_t>(index) >= slots_.size())
        slots_.resize(static_cast<size_t>(index) + 1);
    return slots_[static_cast<size_t>(index)];
}

uint64_t Arm64::outgoing_bytes(const CallSite &site) const
{
    // Named arguments take the first eight registers; anything past that, and
    // every variadic argument on this platform, is passed on the stack in an
    // eight-byte slot of its own.
    size_t in_registers = 0;
    uint64_t on_stack = 0;
    for (size_t i = 0; i < site.types.size(); ++i) {
        const bool variadic = site.variadic && i >= site.fixed_count;
        if (!variadic && in_registers < 8) {
            ++in_registers;
            continue;
        }
        on_stack += 8;
    }
    return (on_stack + 15) / 16 * 16;
}

// ------------------------------------------------------------------ registers

std::string Arm64::take_register()
{
    for (int i = 0; i < kPoolCount; ++i)
        if (owner_[i] < 0) {
            release(kPool[i]);
            return kPool[i];
        }
    // Under pressure the deepest value goes back to its slot, because the
    // deeper a value sits the longer it has to wait to be used.
    int victim = -1;
    for (int i = 0; i < kPoolCount; ++i)
        if (victim < 0 || owner_[i] < owner_[victim])
            victim = i;
    const int index = owner_[victim];
    evict(index);
    release(kPool[victim]);
    return kPool[victim];
}

void Arm64::release(const std::string &name)
{
    for (size_t i = 0; i < remembered_.size();) {
        if (remembered_[i].reg == name)
            remembered_.erase(remembered_.begin() + static_cast<long>(i));
        else
            ++i;
    }
    for (int i = 0; i < kPoolCount; ++i)
        if (kPool[i] == name && owner_[i] >= 0) {
            Held &held = at(owner_[i]);
            held.reg.clear();
            held.borrowed = false;
            owner_[i] = -1;
        }
}

void Arm64::release_slot(int index)
{
    Held &held = at(index);
    if (held.reg.empty())
        return;
    for (int i = 0; i < kPoolCount; ++i)
        if (kPool[i] == held.reg && owner_[i] == index)
            owner_[i] = -1;
    held.reg.clear();
    held.borrowed = false;
}

void Arm64::set_register(int index, const std::string &name, int width, bool width_signed,
                         bool extended)
{
    release_slot(index);
    release(name);
    Held &held = at(index);
    held.reg = name;
    held.borrowed = true;
    for (int i = 0; i < kPoolCount; ++i)
        if (kPool[i] == name) {
            owner_[i] = index;
            held.borrowed = false;
        }
    held.in_memory = false;
    held.is_constant = false;
    held.is_frame_address = false;
    held.width = width;
    held.width_signed = width_signed;
    held.extended = extended;
}

void Arm64::set_constant(int index, uint64_t value, int width, bool width_signed)
{
    release_slot(index);
    Held &held = at(index);
    held.in_memory = false;
    held.is_constant = true;
    held.constant = extend_to(value, width, width_signed);
    held.is_frame_address = false;
    held.width = width;
    held.width_signed = width_signed;
    held.extended = true;
}

std::string Arm64::in_register(int index)
{
    Held &held = at(index);
    if (!held.reg.empty())
        return held.reg;
    if (held.is_constant) {
        const uint64_t value = held.constant;
        const int width = held.width;
        const bool width_signed = held.width_signed;
        const std::string name = take_register();
        constant(name, value);
        set_register(index, name, width, width_signed, true);
        at(index).is_constant = true;
        at(index).constant = value;
        return name;
    }
    if (held.is_frame_address) {
        const int64_t offset = held.frame_offset;
        const std::string name = take_register();
        frame_address(name, offset);
        set_register(index, name, 8, false, true);
        at(index).is_frame_address = true;
        at(index).frame_offset = offset;
        return name;
    }
    const std::string name = take_register();
    say_line("ldr " + name + ", [sp, #" + signed_offset(slot_offset(index)) + "]");
    set_register(index, name, 8, true, true);
    at(index).in_memory = true;
    return name;
}

void Arm64::make_extended(int index)
{
    Held &held = at(index);
    if (held.extended)
        return;
    const std::string name = in_register(index);
    widen(name, held.width, held.width_signed);
    at(index).extended = true;
}

void Arm64::spill(int index)
{
    Held &held = at(index);
    if (held.in_memory || !held.known())
        return;
    // Nought is already a register, so a constant of nought needs nothing put
    // anywhere before it is written down.
    if (held.is_constant && held.constant == 0) {
        say_line("str xzr, [sp, #" + signed_offset(slot_offset(index)) + "]");
        at(index).in_memory = true;
        return;
    }
    make_extended(index);
    const std::string name = in_register(index);
    say_line("str " + name + ", [sp, #" + signed_offset(slot_offset(index)) + "]");
    at(index).in_memory = true;
}

void Arm64::evict(int index)
{
    spill(index);
    Held &held = at(index);
    release_slot(index);
    held.is_constant = false;
    held.is_frame_address = false;
    held.width = 8;
    held.width_signed = true;
    held.extended = true;
}

// ------------------------------------------------------------ what the frame holds

void Arm64::remember(int64_t offset, const Operation &op, const Held &value)
{
    forget_offset(offset);
    if (value.reg.empty())
        return;
    Remembered one;
    one.offset = offset;
    one.type_width = op.width;
    one.type_signed = op.is_signed;
    one.reg = value.reg;
    one.borrowed = value.borrowed;
    one.width = value.width;
    one.width_signed = value.width_signed;
    one.extended = value.extended;
    remembered_.push_back(one);
}

const Remembered *Arm64::recall(int64_t offset, const Operation &op) const
{
    for (const Remembered &one : remembered_)
        if (one.offset == offset && one.type_width == op.width && one.type_signed == op.is_signed)
            return &one;
    return nullptr;
}

void Arm64::forget_offset(int64_t offset)
{
    for (size_t i = 0; i < remembered_.size();) {
        // Anything that overlaps goes, whatever width either access was.
        if (remembered_[i].offset > offset - 8 && remembered_[i].offset < offset + 8)
            remembered_.erase(remembered_.begin() + static_cast<long>(i));
        else
            ++i;
    }
}

void Arm64::forget_borrowed()
{
    for (size_t i = 0; i < remembered_.size();) {
        if (remembered_[i].borrowed)
            remembered_.erase(remembered_.begin() + static_cast<long>(i));
        else
            ++i;
    }
    for (size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].borrowed && !slots_[i].reg.empty()) {
            spill(static_cast<int>(i));
            slots_[i].reg.clear();
            slots_[i].borrowed = false;
        }
}

// ------------------------------------------------------------------ pieces

void Arm64::constant(const std::string &into, uint64_t value)
{
    // mov builds any 64-bit value out of movz, movn and movk, so one line
    // covers every constant a patch can name.
    say_line("mov " + into + ", #" + hex(value));
}

void Arm64::frame_address(const std::string &into, int64_t offset)
{
    if (offset >= 0 && offset <= 0xfff) {
        say_line("add " + into + ", sp, #" + signed_offset(offset));
        return;
    }
    constant(into, static_cast<uint64_t>(offset));
    say_line("add " + into + ", " + into + ", sp");
}

void Arm64::widen(const std::string &reg, int width, bool is_signed)
{
    const std::string small = narrow(reg);
    switch (width) {
    case 1:
        say_line((is_signed ? "sxtb " + reg : "uxtb " + small) + ", " + small);
        return;
    case 2:
        say_line((is_signed ? "sxth " + reg : "uxth " + small) + ", " + small);
        return;
    case 4:
        if (is_signed)
            say_line("sxtw " + reg + ", " + small);
        else
            // Writing a w register clears the half above it, which is the
            // whole of what widening an unsigned word means.
            say_line("mov " + small + ", " + small);
        return;
    default:
        return;
    }
}

bool Arm64::add_immediate(const std::string &into, const std::string &from, int64_t amount,
                          int width)
{
    const std::string a = sized(into, width);
    const std::string b = sized(from, width);
    if (amount == 0) {
        if (a != b)
            say_line("mov " + a + ", " + b);
        return true;
    }
    const char *what = amount > 0 ? "add " : "sub ";
    const uint64_t size = static_cast<uint64_t>(amount > 0 ? amount : -amount);
    if (size <= 0xfff || (size <= 0xfff000 && (size & 0xfff) == 0)) {
        say_line(std::string(what) + a + ", " + b + ", #" + hex(size));
        return true;
    }
    return false;
}

bool Arm64::compare_immediate(const std::string &value, uint64_t against, int width, bool is_signed)
{
    const std::string a = sized(value, width);
    const int64_t number = static_cast<int64_t>(extend_to(against, width, true));
    const uint64_t size = static_cast<uint64_t>(number < 0 ? -number : number);
    if (size > 0xfff && !(size <= 0xfff000 && (size & 0xfff) == 0))
        return false;
    // Adding the other way round sets the same flags as taking away a negative
    // number would, which is how a comparison against a small negative fits.
    if (number < 0) {
        // Only when the value really is a negative number of this width; an
        // unsigned comparison against a large number is not the same thing.
        if (!is_signed && extend_to(against, width, false) != static_cast<uint64_t>(number))
            return false;
        say_line("cmn " + a + ", #" + hex(size));
        return true;
    }
    say_line("cmp " + a + ", #" + hex(size));
    return true;
}

bool Arm64::fold(BinaryOp what, const Operation &op, uint64_t left, uint64_t right, uint64_t &out)
{
    const int width = op.width;
    const bool is_signed = op.is_signed;
    const uint64_t a = extend_to(left, width, is_signed);
    const uint64_t b = extend_to(right, width, is_signed);
    const int64_t sa = static_cast<int64_t>(a);
    const int64_t sb = static_cast<int64_t>(b);
    uint64_t answer = 0;
    switch (what) {
    case BinaryOp::Add: answer = a + b; break;
    case BinaryOp::Subtract: answer = a - b; break;
    case BinaryOp::Multiply: answer = a * b; break;
    case BinaryOp::Divide:
        // A division by nothing is left to happen the way it would have.
        if (b == 0)
            return false;
        answer = is_signed ? static_cast<uint64_t>(sa / sb) : a / b;
        break;
    case BinaryOp::Modulo:
        if (b == 0)
            return false;
        answer = is_signed ? static_cast<uint64_t>(sa % sb) : a % b;
        break;
    case BinaryOp::ShiftLeft:
        if (b >= 64)
            return false;
        answer = a << b;
        break;
    case BinaryOp::ShiftRight:
        if (b >= 64)
            return false;
        answer = is_signed ? static_cast<uint64_t>(sa >> sb) : a >> b;
        break;
    case BinaryOp::BitAnd: answer = a & b; break;
    case BinaryOp::BitOr: answer = a | b; break;
    case BinaryOp::BitXor: answer = a ^ b; break;
    case BinaryOp::Equal: answer = a == b ? 1 : 0; break;
    case BinaryOp::NotEqual: answer = a != b ? 1 : 0; break;
    case BinaryOp::Less: answer = (is_signed ? sa < sb : a < b) ? 1 : 0; break;
    case BinaryOp::LessEqual: answer = (is_signed ? sa <= sb : a <= b) ? 1 : 0; break;
    case BinaryOp::Greater: answer = (is_signed ? sa > sb : a > b) ? 1 : 0; break;
    case BinaryOp::GreaterEqual: answer = (is_signed ? sa >= sb : a >= b) ? 1 : 0; break;
    default: return false;
    }
    out = extend_to(answer, op.result_width, op.result_signed);
    return true;
}

void Arm64::memory_access(bool loading, const Operation &op, const std::string &value,
                          const std::string &address)
{
    const std::string small = narrow(value);
    switch (op.width) {
    case 1:
        if (loading)
            say_line((op.is_signed ? "ldrsb " + value : "ldrb " + small) + ", " + address);
        else
            say_line("strb " + small + ", " + address);
        return;
    case 2:
        if (loading)
            say_line((op.is_signed ? "ldrsh " + value : "ldrh " + small) + ", " + address);
        else
            say_line("strh " + small + ", " + address);
        return;
    case 4:
        if (loading)
            say_line((op.is_signed ? "ldrsw " + value : "ldr " + small) + ", " + address);
        else
            say_line("str " + small + ", " + address);
        return;
    case 8:
        say_line((loading ? "ldr " : "str ") + value + ", " + address);
        return;
    default:
        say(loading ? "nothing here reads a value that many bytes wide"
                    : "nothing here writes a value that many bytes wide");
        return;
    }
}

// ------------------------------------------------------------------ frame

void Arm64::prologue()
{
    slots_.clear();
    remembered_.clear();
    for (int i = 0; i < kPoolCount; ++i)
        owner_[i] = -1;

    if (!leaf_ && frame_size_ < 16) {
        say("a frame has to be at least sixteen bytes for the return address to fit");
        return;
    }
    // A function that keeps nothing but the return address makes room for it
    // and writes it in one instruction.
    if (!leaf_ && frame_size_ == 16) {
        say_line("stp x29, x30, [sp, #-0x10]!");
        say_line("mov x29, sp");
        return;
    }
    uint64_t left = frame_size_;
    // The immediate only reaches 4095, so a very large frame is taken down in
    // steps rather than refused.
    while (left > 0) {
        const uint64_t step = left > 0xff0 ? 0xff0 : left;
        say_line("sub sp, sp, #" + hex(step));
        left -= step;
    }
    if (leaf_)
        return;
    const uint64_t saved = frame_size_ - 16;
    if (saved % 8 == 0 && saved / 8 <= 63)
        say_line("stp x29, x30, [sp, #" + hex(saved) + "]");
    else {
        say_line("str x29, [sp, #" + hex(saved) + "]");
        say_line("str x30, [sp, #" + hex(frame_size_ - 8) + "]");
    }
    if (saved <= 0xfff)
        say_line("add x29, sp, #" + hex(saved));
}

void Arm64::epilogue()
{
    if (!leaf_ && frame_size_ == 16) {
        say_line("ldp x29, x30, [sp], #0x10");
        say_line("ret");
        out_->drop_dead_frame_stores(eval_base_);
        out_->tighten();
        return;
    }
    if (!leaf_) {
        const uint64_t saved = frame_size_ - 16;
        if (saved % 8 == 0 && saved / 8 <= 63)
            say_line("ldp x29, x30, [sp, #" + hex(saved) + "]");
        else {
            say_line("ldr x29, [sp, #" + hex(saved) + "]");
            say_line("ldr x30, [sp, #" + hex(frame_size_ - 8) + "]");
        }
    }
    uint64_t left = frame_size_;
    while (left > 0) {
        const uint64_t step = left > 0xff0 ? 0xff0 : left;
        say_line("add sp, sp, #" + hex(step));
        left -= step;
    }
    say_line("ret");

    // Nothing more will be written, so the whole run can now be read at once.
    out_->drop_dead_frame_stores(eval_base_);
    out_->tighten();
}

void Arm64::spill_parameters(const std::vector<Parameter> &parameters)
{
    // Incoming arguments arrive in registers or just above the frame; the body
    // reads them from the frame like any other variable, so they are put there
    // once. What each one is still in is worth remembering, because the first
    // thing most bodies do is read a parameter back.
    int in_register = 0;
    uint64_t from_caller = 0;
    for (const Parameter &parameter : parameters) {
        const uint64_t size = TypeStore::size_of(parameter.type);
        const int bytes = size == 1 || size == 2 || size == 4 ? static_cast<int>(size) : 8;
        std::string source;
        bool from_a_register = false;
        if (in_register < 8) {
            std::ostringstream name;
            name << 'x' << in_register;
            source = name.str();
            from_a_register = true;
            ++in_register;
        } else {
            source = kPool[0];
            say_line("ldr " + std::string(kPool[0]) + ", [sp, #" +
                     hex(frame_size_ + from_caller) + "]");
            from_caller += 8;
        }
        Operation op;
        op.width = bytes;
        op.is_signed = parameter.type != nullptr && parameter.type->kind == Type::Kind::Integer
                           ? parameter.type->is_signed
                           : false;
        op.result_width = op.width;
        op.result_signed = op.is_signed;
        memory_access(false, op, source,
                      "[sp, #" + signed_offset(parameter.frame_offset) + "]");
        if (!from_a_register)
            continue;
        // Anything narrower than a word arrives widened to a word and no
        // further, so only the low half of the register can be relied on.
        Held shape;
        shape.reg = source;
        shape.borrowed = true;
        shape.width = bytes >= 8 ? 8 : 4;
        shape.width_signed = op.is_signed;
        shape.extended = bytes >= 8;
        remember(parameter.frame_offset, op, shape);
    }
}

// ------------------------------------------------------------------ values

void Arm64::push_constant(int depth, uint64_t value)
{
    set_constant(depth, value, 8, false);
}

void Arm64::push_frame_address(int depth, int64_t offset)
{
    release_slot(depth);
    Held &held = at(depth);
    held.in_memory = false;
    held.is_constant = false;
    held.is_frame_address = true;
    held.frame_offset = offset;
    held.width = 8;
    held.width_signed = false;
    held.extended = true;
}

void Arm64::push_absolute(int depth, uint64_t address)
{
    // Measured from the code rather than written down. An address held as a
    // number is only right where the program was written to load, and a
    // position-independent one rarely loads there, so a patch that named an
    // address outright would read whatever happened to be at that number
    // instead. adrp answers with the page this code is in plus a distance,
    // which stays true wherever the image lands.
    //
    // The pair reaches ±4 GB, which covers any address in the same image. A
    // target further away than that cannot be reached this way, and a bare
    // number is the only thing left; that is a program addressing something
    // outside itself, where a fixed address is what was meant anyway.
    const std::string name = take_register();
    say_line("adrp " + name + ", " + hex(address));
    const uint64_t low = address & 0xfff;
    if (low != 0)
        say_line("add " + name + ", " + name + ", #" + hex(low));
    set_register(depth, name, 8, false, true);
    Held &held = at(depth);
    held.is_constant = false;
}

void Arm64::duplicate(int depth)
{
    const Held source = at(depth - 1);
    release_slot(depth);
    if (source.reg.empty()) {
        Held &held = at(depth);
        held = source;
        held.in_memory = false;
        return;
    }
    // Two slots must never name one register, or writing either would spoil
    // the other; a copy is one instruction and keeps that impossible.
    const std::string name = take_register();
    say_line("mov " + name + ", " + source.reg);
    set_register(depth, name, source.width, source.width_signed, source.extended);
    Held &held = at(depth);
    held.is_constant = source.is_constant;
    held.constant = source.constant;
    held.is_frame_address = source.is_frame_address;
    held.frame_offset = source.frame_offset;
}

void Arm64::move_slot(int to, int from)
{
    if (to == from)
        return;
    release_slot(to);
    Held moved = at(from);
    at(from) = Held();
    at(from).in_memory = true;
    for (int i = 0; i < kPoolCount; ++i)
        if (!moved.reg.empty() && kPool[i] == moved.reg)
            owner_[i] = to;
    moved.in_memory = false;
    at(to) = moved;
}

void Arm64::swap_slots(int first, int second)
{
    if (first == second)
        return;
    Held a = at(first);
    Held b = at(second);
    at(first) = b;
    at(second) = a;
    for (int i = 0; i < kPoolCount; ++i) {
        if (owner_[i] == first)
            owner_[i] = second;
        else if (owner_[i] == second)
            owner_[i] = first;
    }
}

void Arm64::load_indirect(int depth, const Operation &op)
{
    const int index = depth - 1;
    Held address = at(index);
    if (address.is_frame_address) {
        const int64_t offset = address.frame_offset;
        const Remembered *known = recall(offset, op);
        if (known != nullptr) {
            const std::string reg = known->reg;
            const int width = known->width;
            const bool width_signed = known->width_signed;
            const bool extended = known->extended;
            const bool borrowed = known->borrowed;
            if (borrowed) {
                // A parameter still sitting in the register it arrived in is
                // read where it is; nothing here writes into a register a
                // value is read from, so two names for it are safe.
                release_slot(index);
                Held &held = at(index);
                held = Held();
                held.reg = reg;
                held.borrowed = true;
                held.width = width;
                held.width_signed = width_signed;
                held.extended = extended;
                return;
            }
            // One of the pool's registers can be taken for something else, so
            // the value read out gets a register of its own.
            const std::string name = take_register();
            say_line("mov " + name + ", " + reg);
            set_register(index, name, width, width_signed, extended);
            return;
        }
        const std::string name = take_register();
        memory_access(true, op, name, "[sp, #" + signed_offset(offset) + "]");
        set_register(index, name, op.width, op.is_signed, true);
        Held shape = at(index);
        remember(offset, op, shape);
        return;
    }
    const std::string pointer = in_register(index);
    const std::string name = take_register();
    memory_access(true, op, name, "[" + pointer + "]");
    set_register(index, name, op.width, op.is_signed, true);
}

void Arm64::store_indirect(int depth, const Operation &op)
{
    const int address_slot = depth - 2;
    const int value_slot = depth - 1;
    if (op.width == 8)
        make_extended(value_slot);
    Held address = at(address_slot);

    if (address.is_frame_address) {
        const int64_t offset = address.frame_offset;
        std::string value;
        if (at(value_slot).is_constant && at(value_slot).constant == 0)
            value = "xzr";
        else
            value = in_register(value_slot);
        memory_access(false, op, value, "[sp, #" + signed_offset(offset) + "]");
        const Held stored = at(value_slot);
        move_slot(address_slot, value_slot);
        remember(offset, op, at(address_slot));
        (void)stored;
        return;
    }

    const std::string pointer = in_register(address_slot);
    const std::string value = in_register(value_slot);
    memory_access(false, op, value, "[" + pointer + "]");
    // Where that went is not known, so nothing about the frame is either.
    forget_memory();
    // What is left behind is the value that was stored, which is what an
    // assignment answers with.
    move_slot(address_slot, value_slot);
}

void Arm64::unary(int depth, UnaryOp what, const Operation &op)
{
    const int index = depth - 1;
    Held &held = at(index);
    if (held.is_constant) {
        const uint64_t value = extend_to(held.constant, op.width, op.is_signed);
        uint64_t answer = 0;
        bool done = true;
        switch (what) {
        case UnaryOp::Minus: answer = 0 - value; break;
        case UnaryOp::BitNot: answer = ~value; break;
        case UnaryOp::Not: answer = value == 0 ? 1 : 0; break;
        case UnaryOp::Plus: answer = value; break;
        default: done = false; break;
        }
        if (done) {
            const int width = what == UnaryOp::Not ? 4 : op.result_width;
            const bool is_signed = what == UnaryOp::Not ? true : op.result_signed;
            set_constant(index, answer, width, is_signed);
            return;
        }
    }

    if (what == UnaryOp::Plus)
        return;
    if (what == UnaryOp::Not) {
        std::string reg;
        bool is_wide = false;
        tested_register(index, reg, is_wide);
        say_line("cmp " + std::string(is_wide ? reg : narrow(reg)) + ", #0");
        const std::string name = take_register();
        say_line("cset " + narrow(name) + ", eq");
        set_register(index, name, 4, true, true);
        return;
    }

    const bool wide = op.width >= 8;
    if (wide)
        make_extended(index);
    const std::string source = in_register(index);
    const std::string name = take_register();
    const std::string a = sized(name, op.width);
    const std::string b = sized(source, op.width);
    switch (what) {
    case UnaryOp::Minus:
        say_line("sub " + a + ", " + (wide ? "xzr" : "wzr") + ", " + b);
        break;
    case UnaryOp::BitNot:
        say_line("mvn " + a + ", " + b);
        break;
    default:
        say("this is not a unary operation the compiler writes directly");
        return;
    }
    if (op.result_width >= 8 || (op.result_width == 4 && op.width == 4)) {
        set_register(index, name, op.result_width, op.result_signed, op.result_width >= 8);
        return;
    }
    widen(name, op.result_width, op.result_signed);
    set_register(index, name, op.result_width, op.result_signed, true);
}

void Arm64::binary(int depth, BinaryOp what, const Operation &op)
{
    const int index = depth - 2;
    const int other = depth - 1;

    // Both sides known: the answer is known too, and nothing is written.
    if (at(index).is_constant && at(other).is_constant) {
        uint64_t answer = 0;
        if (fold(what, op, at(index).constant, at(other).constant, answer)) {
            release_slot(other);
            at(other) = Held();
            at(other).in_memory = true;
            set_constant(index, answer, is_comparison(what) ? 4 : op.result_width,
                         is_comparison(what) ? true : op.result_signed);
            return;
        }
    }

    // Where it makes no difference, the known side goes on the right, which is
    // the only side an immediate form can be written on.
    int left = index;
    int right = other;
    BinaryOp effective = what;
    if (at(index).is_constant && !at(other).is_constant) {
        const bool commutes = what == BinaryOp::Add || what == BinaryOp::Multiply ||
                              what == BinaryOp::BitAnd || what == BinaryOp::BitOr ||
                              what == BinaryOp::BitXor || is_comparison(what);
        if (commutes) {
            left = other;
            right = index;
            effective = reversed(what);
        }
    }

    const bool wide = op.width >= 8;
    if (wide) {
        make_extended(left);
        if (!at(right).is_constant)
            make_extended(right);
    }

    // What the answer looks like once it is worked out.
    auto finish = [&](const std::string &name, bool comparison) {
        if (comparison) {
            release_slot(other);
            at(other) = Held();
            at(other).in_memory = true;
            set_register(index, name, op.result_width >= 8 ? 8 : 4,
                         op.result_width >= 8 ? op.result_signed : true, true);
            return;
        }
        // A word-wide answer is left as it is: the low half is the value, and
        // whatever wants the whole register says so and widens it then.
        const bool narrow_result = op.result_width == 4 && op.width == 4;
        if (!narrow_result && op.result_width < 8)
            widen(name, op.result_width, op.result_signed);
        release_slot(other);
        at(other) = Held();
        at(other).in_memory = true;
        set_register(index, name, op.result_width, op.result_signed,
                     narrow_result ? false : true);
    };

    uint64_t known = 0;
    const bool right_known = at(right).is_constant;
    if (right_known)
        known = extend_to(at(right).constant, op.width, op.is_signed);

    if (is_comparison(effective)) {
        std::string a;
        if (right_known) {
            a = in_register(left);
            if (!compare_immediate(a, at(right).constant, op.width, op.is_signed)) {
                const std::string b = in_register(right);
                say_line("cmp " + sized(a, op.width) + ", " + sized(b, op.width));
            }
        } else {
            a = in_register(left);
            const std::string b = in_register(right);
            say_line("cmp " + sized(a, op.width) + ", " + sized(b, op.width));
        }
        const std::string name = take_register();
        say_line("cset " + std::string(op.result_width >= 8 ? name : narrow(name)) + ", " +
                 condition_for(effective, op.is_signed));
        finish(name, true);
        return;
    }

    const std::string a = in_register(left);
    const std::string name = take_register();
    const std::string d = sized(name, op.width);
    const std::string s = sized(a, op.width);

    if (right_known) {
        int log = 0;
        switch (effective) {
        case BinaryOp::Add:
            if (add_immediate(name, a, static_cast<int64_t>(known), op.width)) {
                finish(name, false);
                return;
            }
            break;
        case BinaryOp::Subtract:
            if (add_immediate(name, a, -static_cast<int64_t>(known), op.width)) {
                finish(name, false);
                return;
            }
            break;
        case BinaryOp::Multiply:
            // Multiplying by a power of two is a shift, whatever the sign.
            if (is_power_of_two(known, log)) {
                say_line("lsl " + d + ", " + s + ", #" + hex(static_cast<uint64_t>(log)));
                finish(name, false);
                return;
            }
            break;
        case BinaryOp::Divide:
            // Only the unsigned case is a shift: rounding towards nought needs
            // more instructions than the divide it would replace.
            if (!op.is_signed && is_power_of_two(known, log)) {
                say_line("lsr " + d + ", " + s + ", #" + hex(static_cast<uint64_t>(log)));
                finish(name, false);
                return;
            }
            break;
        case BinaryOp::ShiftLeft:
        case BinaryOp::ShiftRight: {
            const uint64_t amount = at(right).constant;
            if (amount < static_cast<uint64_t>(op.width >= 8 ? 64 : 32)) {
                const char *how = effective == BinaryOp::ShiftLeft
                                      ? "lsl "
                                      : (op.is_signed ? "asr " : "lsr ");
                say_line(std::string(how) + d + ", " + s + ", #" + hex(amount));
                finish(name, false);
                return;
            }
            break;
        }
        default:
            break;
        }
    }

    const std::string b = in_register(right);
    const std::string t = sized(b, op.width);
    switch (effective) {
    case BinaryOp::Add: say_line("add " + d + ", " + s + ", " + t); break;
    case BinaryOp::Subtract: say_line("sub " + d + ", " + s + ", " + t); break;
    case BinaryOp::Multiply: say_line("mul " + d + ", " + s + ", " + t); break;
    case BinaryOp::Divide:
        say_line((op.is_signed ? "sdiv " : "udiv ") + d + ", " + s + ", " + t);
        break;
    case BinaryOp::Modulo: {
        const std::string scratch = take_register();
        const std::string q = sized(scratch, op.width);
        say_line((op.is_signed ? "sdiv " : "udiv ") + q + ", " + s + ", " + t);
        say_line("msub " + d + ", " + q + ", " + t + ", " + s);
        release(scratch);
        break;
    }
    case BinaryOp::ShiftLeft: say_line("lsl " + d + ", " + s + ", " + t); break;
    case BinaryOp::ShiftRight:
        say_line((op.is_signed ? "asr " : "lsr ") + d + ", " + s + ", " + t);
        break;
    case BinaryOp::BitAnd: say_line("and " + d + ", " + s + ", " + t); break;
    case BinaryOp::BitOr: say_line("orr " + d + ", " + s + ", " + t); break;
    case BinaryOp::BitXor: say_line("eor " + d + ", " + s + ", " + t); break;
    default:
        say("this is not a binary operation the compiler writes directly");
        return;
    }
    finish(name, false);
}

void Arm64::convert(int depth, const Operation &from, const Operation &to)
{
    (void)from;
    const int index = depth - 1;
    if (to.width >= 8)
        return;
    Held &held = at(index);
    if (held.is_constant) {
        set_constant(index, held.constant, to.width, to.is_signed);
        return;
    }
    if (to.width == 4) {
        // The low half of the register is the value cut down to a word however
        // it got there, so a cast to a word is only a change of mind.
        in_register(index);
        Held &value = at(index);
        value.width = 4;
        value.width_signed = to.is_signed;
        value.extended = false;
        value.is_frame_address = false;
        return;
    }
    if (held.extended && held.width <= to.width &&
        (held.width < to.width ? (!held.width_signed || held.width_signed == to.is_signed)
                               : held.width_signed == to.is_signed))
        return;
    const std::string name = in_register(index);
    widen(name, to.width, to.is_signed);
    Held &value = at(index);
    value.width = to.width;
    value.width_signed = to.is_signed;
    value.extended = true;
    value.is_frame_address = false;
    value.is_constant = false;
}

// ------------------------------------------------------- what a register holds

void Arm64::settle(int depth)
{
    for (int i = 0; i < depth; ++i)
        if (static_cast<size_t>(i) < slots_.size())
            spill(i);
}

void Arm64::hold_over(int depth)
{
    for (int i = 0; i < depth && static_cast<size_t>(i) < slots_.size(); ++i) {
        Held &held = slots_[static_cast<size_t>(i)];
        if (held.is_constant || held.is_frame_address || held.reg.empty())
            continue;
        spill(i);
    }
}

void Arm64::clobber()
{
    remembered_.clear();
    for (int i = 0; i < kPoolCount; ++i)
        owner_[i] = -1;
    for (Held &held : slots_) {
        held.reg.clear();
        held.borrowed = false;
        if (held.is_constant || held.is_frame_address) {
            held.extended = true;
            continue;
        }
        held.in_memory = true;
        held.width = 8;
        held.width_signed = true;
        held.extended = true;
    }
}

void Arm64::forget_registers()
{
    remembered_.clear();
    for (int i = 0; i < kPoolCount; ++i)
        owner_[i] = -1;
    for (Held &held : slots_) {
        held = Held();
        held.in_memory = true;
    }
}

// ------------------------------------------------------------------ control

void Arm64::jump(int depth, const std::string &label)
{
    settle(depth);
    say_line("b %" + label + "%");
    forget_registers();
}

// The register a truth is tested in, and whether the whole of it counts.
void Arm64::tested_register(int index, std::string &reg, bool &is_wide)
{
    Held &held = at(index);
    is_wide = held.width >= 8;
    if (is_wide)
        make_extended(index);
    reg = in_register(index);
}

void Arm64::branch_if_zero(int depth, const std::string &label)
{
    const int index = depth - 1;
    if (at(index).is_constant) {
        const uint64_t value = at(index).constant;
        settle(index);
        if (value == 0) {
            say_line("b %" + label + "%");
            forget_registers();
        }
        return;
    }
    settle(index);
    std::string reg;
    bool is_wide = false;
    tested_register(index, reg, is_wide);
    say_line("cbz " + (is_wide ? reg : narrow(reg)) + ", %" + label + "%");
}

void Arm64::branch_if_nonzero(int depth, const std::string &label)
{
    const int index = depth - 1;
    if (at(index).is_constant) {
        const uint64_t value = at(index).constant;
        settle(index);
        if (value != 0) {
            say_line("b %" + label + "%");
            forget_registers();
        }
        return;
    }
    settle(index);
    std::string reg;
    bool is_wide = false;
    tested_register(index, reg, is_wide);
    say_line("cbnz " + (is_wide ? reg : narrow(reg)) + ", %" + label + "%");
}

void Arm64::branch_if_equal(int depth, uint64_t value, const std::string &label)
{
    const int index = depth - 1;
    settle(index);
    if (at(index).is_constant) {
        if (at(index).constant == value) {
            say_line("b %" + label + "%");
            forget_registers();
        }
        return;
    }
    // A case value that needs more than a word has to be compared against the
    // whole register, so the whole register has to mean the value.
    const bool wide = at(index).width >= 8 || extend_to(value, 4, true) != value;
    if (wide)
        make_extended(index);
    const std::string reg = in_register(index);
    const int width = wide ? 8 : 4;
    if (!compare_immediate(reg, value, width, at(index).width_signed)) {
        const std::string scratch = take_register();
        constant(scratch, value);
        say_line("cmp " + sized(reg, width) + ", " + sized(scratch, width));
        release(scratch);
    }
    say_line("b.eq %" + label + "%");
}

// ------------------------------------------------------------------ calling

void Arm64::call(int depth, const CallSite &site)
{
    const int count = static_cast<int>(site.types.size());
    const int first = depth - count;
    const int callee_slot = first - 1;
    const int keep = site.through_pointer ? callee_slot : first;

    if (!site.name.empty())
        out_->comment("call " + site.name);

    // Everything a call does not consume has to be back in its slot, because
    // every register a value can live in is the callee's to lose.
    hold_over(keep);

    // Filling in the argument registers destroys what a parameter still in one
    // of them was worth, so anything reading one that is not its own goes
    // somewhere safe first.
    {
        int next = 0;
        for (int i = 0; i < count; ++i) {
            const bool variadic = site.variadic && static_cast<size_t>(i) >= site.fixed_count;
            std::string wanted;
            if (!variadic && next < 8) {
                std::ostringstream name;
                name << 'x' << next;
                wanted = name.str();
                ++next;
            }
            Held &held = at(first + i);
            if (!held.borrowed || held.reg.empty() || held.reg == wanted)
                continue;
            const std::string name = take_register();
            say_line("mov " + name + ", " + held.reg);
            const int width = held.width;
            const bool width_signed = held.width_signed;
            const bool extended = held.extended;
            set_register(first + i, name, width, width_signed, extended);
        }
        if (site.through_pointer) {
            Held &held = at(callee_slot);
            if (held.borrowed && !held.reg.empty()) {
                const std::string name = take_register();
                say_line("mov " + name + ", " + held.reg);
                const int width = held.width;
                const bool width_signed = held.width_signed;
                const bool extended = held.extended;
                set_register(callee_slot, name, width, width_signed, extended);
            }
        }
    }

    // Arguments that go on the stack are written first, because the evaluation
    // stack they come from sits above the outgoing area and is not disturbed
    // by writing to it.
    int in_registers = 0;
    uint64_t at_offset = 0;
    for (int i = 0; i < count; ++i) {
        const bool variadic = site.variadic && static_cast<size_t>(i) >= site.fixed_count;
        if (!variadic && in_registers < 8) {
            ++in_registers;
            continue;
        }
        make_extended(first + i);
        const std::string value = in_register(first + i);
        say_line("str " + value + ", [sp, #" + hex(at_offset) + "]");
        at_offset += 8;
    }

    // Then the register arguments, which nothing after this point touches.
    int next_register = 0;
    for (int i = 0; i < count; ++i) {
        const bool variadic = site.variadic && static_cast<size_t>(i) >= site.fixed_count;
        if (variadic || next_register >= 8)
            continue;
        std::ostringstream name;
        name << 'x' << next_register;
        make_extended(first + i);
        Held &held = at(first + i);
        if (held.is_constant)
            constant(name.str(), held.constant);
        else if (held.is_frame_address)
            frame_address(name.str(), held.frame_offset);
        else if (!held.reg.empty())
            say_line("mov " + name.str() + ", " + held.reg);
        else
            say_line("ldr " + name.str() + ", [sp, #" +
                     signed_offset(slot_offset(first + i)) + "]");
        ++next_register;
    }

    std::string callee;
    bool direct = false;
    if (site.through_pointer) {
        make_extended(callee_slot);
        Held &held = at(callee_slot);
        if (held.is_constant)
            constant(kCallee, held.constant);
        else if (!held.reg.empty())
            say_line("mov " + std::string(kCallee) + ", " + held.reg);
        else
            say_line("ldr " + std::string(kCallee) + ", [sp, #" +
                     signed_offset(slot_offset(callee_slot)) + "]");
        callee = kCallee;
    } else {
        // A direct branch reaches a hundred and twenty-eight megabytes, which
        // is further than any function is from the one being patched; only
        // when it is not does the address have to go through a register.
        const int64_t away =
            static_cast<int64_t>(site.address) - static_cast<int64_t>(origin_);
        direct = away >= -0x7000000 && away <= 0x7000000;
        if (!direct)
            constant(kCallee, site.address);
    }

    // No register survives a call, and neither does anything the frame was
    // known to hold, because the callee may have written to either. What a
    // constant is worth, and where in the frame a variable sits, are not
    // things a callee can change.
    clobber();

    if (direct)
        say_line("bl " + hex(site.address));
    else
        say_line("blr " + (site.through_pointer ? callee : std::string(kCallee)));

    const bool answers = site.result != nullptr && site.result->kind != Type::Kind::Void;
    if (!answers)
        return;
    const int destination = site.through_pointer ? callee_slot : first;
    const int width = static_cast<int>(TypeStore::size_of(site.result));
    const bool is_signed =
        site.result->kind == Type::Kind::Integer ? site.result->is_signed : false;
    if (width == 1 || width == 2) {
        widen("x0", width, is_signed);
        set_register(destination, "x0", width, is_signed, true);
        return;
    }
    // A word-wide answer only fills the low half of x0, and saying so is
    // cheaper than widening it for a use that may never come.
    set_register(destination, "x0", width == 4 ? 4 : 8, is_signed, width != 4);
}

void Arm64::return_value(int depth)
{
    const int index = depth - 1;
    make_extended(index);
    Held &held = at(index);
    if (held.is_constant)
        constant("x0", held.constant);
    else if (held.is_frame_address)
        frame_address("x0", held.frame_offset);
    else if (!held.reg.empty())
        say_line("mov x0, " + held.reg);
    else
        say_line("ldr x0, [sp, #" + signed_offset(slot_offset(index)) + "]");
    say_line("b %" + leave_ + "%");
    // Nothing after this is reached except through a label, which gives up
    // everything anyway; giving it up here saves writing values nobody reads.
    forget_registers();
}

void Arm64::return_nothing()
{
    say_line("b %" + leave_ + "%");
    forget_registers();
}

} // namespace

std::unique_ptr<Machine> machine_for(assembler::Target target)
{
    if (target == assembler::Target::Arm64)
        return std::unique_ptr<Machine>(new Arm64());
    return nullptr;
}

} // namespace compiler
} // namespace astral_internal
