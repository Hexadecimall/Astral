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

} // namespace pcode
} // namespace nova
} // namespace astral_internal

#endif
