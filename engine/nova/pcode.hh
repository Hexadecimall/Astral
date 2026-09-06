// From the representation to p-code, which is the one language every processor
// here is already described in.
//
// This is the step that makes one back end reach every processor rather than
// five. A specification says, for each instruction a machine has, both the bits
// it is written as and what it does in p-code. Reading that one way is how a
// program is taken apart. Written down as p-code, a function can be put back
// together the other way, against whichever specification is being compiled
// for, and there are seventy-four operations to cover rather than one
// instruction set per processor.
//
// Nothing is chosen here about which instructions a machine will use. That is
// the next question, and it is answered by matching against a specification;
// this only says what the function does, in the vocabulary that question is
// asked in.
#ifndef ASTRAL_NOVA_PCODE_HH
#define ASTRAL_NOVA_PCODE_HH

#include "ir.hh"
#include "opcodes.hh"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace astral_internal {
namespace nova {
namespace pcode {

// Which memory a value is in. p-code says every value is somewhere, including
// the ones that are only a number.
enum class Where {
    Constant,  // not a place: the offset is the value
    Register,  // the register file, at an offset the specification gives
    Unique,    // room for a value with no home, which is most of them
    Memory,    // what a program calls memory
    Frame,     // an offset from the frame pointer, resolved when a frame exists
};

// A value, in the only terms p-code has for one: where it is, how far in, and
// how many bytes of it there are.
struct Varnode {
    Where where = Where::Unique;
    uint64_t offset = 0;
    int size = 0;

    bool is_constant() const { return where == Where::Constant; }
};

// One operation. Every p-code operation takes some inputs and writes at most
// one output, which is what makes the whole language seventy-four things rather
// than an instruction set.
struct Operation {
    ghidra::OpCode opcode = ghidra::CPUI_COPY;
    bool writes = false;
    Varnode output;
    std::vector<Varnode> inputs;

    // Where control goes, for the operations that decide that. A p-code branch
    // names an instruction rather than a block, so these are kept as the blocks
    // they were and resolved when addresses exist.
    std::vector<uint32_t> successors;

    // Who is being called, for a call. A call names an address once there is
    // one; until then the name is what there is to go on.
    std::string callee;

    // The question a branch is asking, when the comparison that asks it was
    // folded into the branch itself. No processor computes a truth into a
    // register in one instruction and none needs to: the instruction that acts
    // on a comparison is the comparison, which is what `beq` is. CPUI_COPY
    // means nothing was folded and the branch reads a value already worked out.
    ghidra::OpCode compares = ghidra::CPUI_COPY;
};

// A block of operations, still in the order and shape the representation had.
struct Block {
    uint32_t identifier = 0;
    std::vector<Operation> operations;
};

struct Sequence {
    std::string name;
    std::vector<Block> blocks;
    uint32_t entry = 0;

    // Where this function leaves its answer.
    //
    // A processor's return instruction carries no value. It reads the register
    // the return address is in and goes there, and the answer is expected to be
    // sitting where the calling convention said it would be by the time that
    // happens. So leaving is two things - put the answer in that place, then go
    // - and this is the place, taken from the specification when the sequence
    // was written.
    //
    // Zero-sized when the function answers with nothing.
    Varnode answer;
};

// Writes `function` as p-code for `target`. Returns false and says what could
// not be written; an operation with no p-code spelling is refused rather than
// left out, because p-code that does less than the function did is worse than
// none.
bool to_pcode(const ir::Function &function, const ir::Target &target, Sequence &out,
              std::vector<std::string> &problems);

// The sequence written out, one operation to a line, for reading and comparing.
std::string to_text(const Sequence &sequence);

// The name a p-code operation goes by.
const char *opcode_name(ghidra::OpCode opcode);

// ------------------------------------------------------------------ running

// Running the p-code, on nothing but itself.
//
// This exists to answer one question that nothing else can: whether what was
// written means what the source said. Everything up to here is a translation,
// and a translation is only right if the answer comes out the same, so the
// operations are carried out and the answer is compared.
//
// It is also what the next step needs. Choosing which instructions a machine
// should use means proposing some and checking they do the same thing, and
// this is the thing they are checked against.
struct Machine {
    // What each register holds when it starts, by the offset the specification
    // gives for it. Registers not named here begin at zero.
    std::map<uint64_t, uint64_t> registers;
    // How many operations may run before it is called a loop that never ends.
    // A recovered function is not always a function that finishes.
    uint64_t budget = 100000;

    // The other functions a call can reach, by name. A call to something not
    // here stops rather than guessing what it would have done.
    std::map<std::string, const Sequence *> others;
};

struct Answer {
    bool ok = false;
    bool returned = false;
    uint64_t value = 0;      // what was returned, when something was
    std::string error;       // why it stopped, when it stopped badly
    uint64_t steps = 0;      // how many operations ran
    // What the registers held when it stopped, which is what a caller sees.
    std::map<uint64_t, uint64_t> registers;
};

// Runs `sequence` from its entry block and answers with what it returned.
Answer run(const Sequence &sequence, const Machine &machine);

} // namespace pcode
} // namespace nova
} // namespace astral_internal

#endif
