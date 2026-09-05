// Turning the tree into instructions, with one seam for the architecture.
//
// Everything that is true of C wherever it runs lives here: which statement
// leads where, where a variable sits in the frame, which label a `break` means,
// how a switch chooses. Everything that is true only of one instruction set
// lives behind Machine. Adding a second architecture is writing another
// Machine, not another generator.
//
// Values are evaluated onto a stack whose slots are frame memory, addressed by
// depth. A frame slot is where a value definitely is; a register is where it
// may be while nothing can interrupt. Between two points control can reach from
// more than one direction, every live value is put back in its slot, so the
// slower model still describes what is true at every join.
#ifndef ASTRAL_COMPILER_CODEGEN_HH
#define ASTRAL_COMPILER_CODEGEN_HH

#include "asmbuffer.hh"
#include "ast.hh"
#include "compiler.hh"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace astral_internal {
namespace compiler {

// The shape of one machine operation, worked out from the C types before the
// architecture ever sees them. Every value on the evaluation stack is held
// widened to a full slot according to its own type, so an operation only has to
// say how wide it works and whether the top bit is a sign.
struct Operation {
    int width = 8;
    bool is_signed = true;
    int result_width = 8;
    bool result_signed = true;
};

// A parameter as the prologue has to deal with it: what it is and where the
// body will look for it.
struct Parameter {
    TypePtr type = nullptr;
    int64_t frame_offset = 0;
};

// Everything the architecture needs to know to make a call.
struct CallSite {
    // The arguments' types, in order. They occupy the top `types.size()` slots.
    std::vector<TypePtr> types;
    // How many of them the callee declared. The rest are variadic.
    size_t fixed_count = 0;
    bool variadic = false;
    // When true the value called sits in the slot below the first argument.
    bool through_pointer = false;
    uint64_t address = 0;
    // Null when the call answers with nothing.
    TypePtr result = nullptr;
    // For the comment beside the call.
    std::string name;
};

// What one instruction set has to answer for.
//
// Depth is the number of live values before the operation runs; slot `n` is the
// (n + 1)th value on the evaluation stack. An operation that consumes two and
// produces one is handed the depth it starts at and leaves its answer in the
// lower of the two slots; the generator does the counting.
class Machine {
public:
    virtual ~Machine() = default;

    // --- what the frame is made of ---
    virtual int slot_size() const = 0;
    virtual uint64_t frame_alignment() const = 0;
    // Bytes at the top of the frame the prologue keeps for itself.
    virtual uint64_t reserved_bytes() const = 0;
    // Bytes at the bottom of the frame this call needs for arguments it cannot
    // pass in registers.
    virtual uint64_t outgoing_bytes(const CallSite &call) const = 0;

    void attach(AsmBuffer *out) { out_ = out; }
    void set_frame(uint64_t size, int64_t eval_base, const std::string &leave)
    {
        frame_size_ = size;
        eval_base_ = eval_base;
        leave_ = leave;
    }
    // A function that calls nothing keeps no return address and needs no frame
    // pointer, which is most of a small patch's budget.
    void set_leaf(bool leaf) { leaf_ = leaf; }
    // Where the function will sit, which decides whether a call can be a direct
    // branch or has to go through a register.
    void set_origin(uint64_t address) { origin_ = address; }
    const std::string &error() const { return error_; }
    void forget_error() { error_.clear(); }

    // --- the function around the body ---
    virtual void prologue() = 0;
    virtual void epilogue() = 0;
    virtual void spill_parameters(const std::vector<Parameter> &parameters) = 0;

    // --- values ---
    virtual void push_constant(int depth, uint64_t value) = 0;
    virtual void push_frame_address(int depth, int64_t offset) = 0;
    virtual void push_absolute(int depth, uint64_t address) = 0;
    virtual void duplicate(int depth) = 0;
    virtual void move_slot(int to, int from) = 0;
    virtual void swap_slots(int first, int second) = 0;

    // top = what the address on top points at, widened per `op`.
    virtual void load_indirect(int depth, const Operation &op) = 0;
    // Stores the top at the address below it and leaves the value stored.
    virtual void store_indirect(int depth, const Operation &op) = 0;

    virtual void unary(int depth, UnaryOp what, const Operation &op) = 0;
    virtual void binary(int depth, BinaryOp what, const Operation &op) = 0;
    virtual void convert(int depth, const Operation &from, const Operation &to) = 0;

    // --- what a register may hold ---
    // Puts the lowest `depth` values back in their frame slots, so a point
    // control reaches from more than one direction finds them where the slower
    // model says they are.
    virtual void settle(int depth) { (void)depth; }
    // Control arrives here from somewhere else as well, so nothing held over
    // from the instructions just written is still known.
    virtual void forget_registers() {}

    // --- where to go next ---
    virtual void jump(int depth, const std::string &label) = 0;
    virtual void branch_if_zero(int depth, const std::string &label) = 0;
    virtual void branch_if_nonzero(int depth, const std::string &label) = 0;
    virtual void branch_if_equal(int depth, uint64_t value, const std::string &label) = 0;

    // --- calling and returning ---
    virtual void call(int depth, const CallSite &site) = 0;
    // Puts the top where a caller looks for the answer, then leaves.
    virtual void return_value(int depth) = 0;
    virtual void return_nothing() = 0;

protected:
    void say(const std::string &why)
    {
        if (error_.empty())
            error_ = why;
    }

    AsmBuffer *out_ = nullptr;
    uint64_t frame_size_ = 0;
    int64_t eval_base_ = 0;
    std::string leave_;
    std::string error_;
    uint64_t origin_ = 0;
    bool leaf_ = false;
};

// The architectures that have a Machine written for them.
std::unique_ptr<Machine> machine_for(assembler::Target target);

// Generates one function's body into `out`.
//
// `unit` is there for the names the body reaches for that it does not define.
// Returns false and fills `error` when something in the function cannot be
// written; `placed` collects literals that had to be given room.
bool generate_function(Machine &machine, AsmBuffer &out, assembler::Target target, uint64_t address,
                       const Unit &unit, const Function &function, const Environment &environment,
                       const Options &options, std::vector<Result::Datum> &placed,
                       std::vector<Diagnostic> &diagnostics, std::string &error);

} // namespace compiler
} // namespace astral_internal

#endif
