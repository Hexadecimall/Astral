// Nova's intermediate representation: what a function is between being read and
// being bytes.
//
// The tree a front end produces says what the source said. Machine code says
// where every value lives and how wide each operation is. Those are different
// questions, and a tree cannot answer the second one, so this sits between
// them: single assignment, basic blocks, and a place for every fact the machine
// needs that a tree has nowhere to put.
//
// Three things make this unlike the intermediate form a general compiler uses,
// and each of them is why Nova has one of its own.
//
// A type is optional. Nova's levels run from bytes to source, and level 1 says
// where a value lives while saying nothing about what it is. A representation
// that demanded a type could not hold a function that has just been recovered,
// which is the only kind of function there is until somebody reads it. So width
// and signedness are written on the operation, where the machine needs them,
// rather than being asked of a type that may not exist.
//
// Storage is an instruction, not a remark. `@x0` in the source means the value
// is in that register, and register allocation has to solve around it rather
// than compare against it afterwards. Patching a recovered function is exactly
// this: keep these registers, fit in these bytes, do not move. A representation
// that decides those for itself cannot be told.
//
// Nothing here names an instruction set. A target is read from its
// specification, because the specifications describe every processor the
// decompiler can already read, and a compiler that knows fewer machines than
// the decompiler is a compiler that stops where the reading got interesting.
#ifndef ASTRAL_NOVA_IR_HH
#define ASTRAL_NOVA_IR_HH

#include "ast.hh"
#include "compiler/ast.hh"
#include "compiler/compiler.hh"

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {
namespace nova {
namespace ir {

using compiler::TypePtr;

// ------------------------------------------------------------------- targets

// Where bytes live. Taken from the specifications rather than assumed, because
// the assumption a flat byte-addressed memory makes is wrong on more processors
// than it is right: a Harvard machine cannot reach its own code, and a
// word-addressed one counts something other than bytes.
enum class Space {
    Data,      // where variables live
    Code,      // where instructions live; the same space as Data unless Harvard
    Register,  // the register file, addressed by offset the way p-code does
    Frame,     // an offset from the frame pointer, signed
    Constant,  // not a place at all: the value is the address
};

// A processor, described rather than listed. Every field here is already in the
// specification the decompiler reads, so adding a processor is adding its
// specification, not writing a back end.
//
// The awkward ones are the reason each field exists rather than a sensible
// default: instructions and data disagree about byte order on ARM:LEBE, a
// pointer is half a word on AARCH64 ilp32, an address counts two bytes on a
// wordSize2 machine, and a Harvard machine has two memories that both start at
// zero.
struct Target {
    std::string language_id;  // "AARCH64:LE:64:AppleSilicon", entire and exact

    int address_bits = 0;        // 16 on a z80, 24 on a PIC-24, 32, 64
    int pointer_bytes = 0;       // four on a sixty-four bit machine, under ilp32
    int word_bytes = 0;          // the width arithmetic is natural in
    int address_unit_bytes = 1;  // how many bytes one address counts

    bool data_big_endian = false;
    bool instruction_big_endian = false;  // not always the same answer
    bool harvard = false;                 // Code and Data are separate memories

    compiler::Abi abi = compiler::Abi::SystemV;

    // Reads a target out of the specifications, so what this knows about a
    // processor is what the specification says rather than what anyone wrote
    // down here. Accepts a language id with or without a compiler on the end.
    // Returns false and says why when no specification describes it.
    static bool from_language_id(const std::string &language_id, Target &target,
                                 std::string &error);
};

// ------------------------------------------------------------------- values

// A value, given once and never given again.
//
// What is known about it is what the source said. A type and no storage reads
// like a program; storage and no type is a function somebody has only just
// begun to understand. Both are ordinary, and neither is missing something this
// has to invent.
struct Value {
    uint32_t identifier = 0;
    TypePtr type = nullptr;  // null below level 2, and that is not a failure
    Storage storage;         // Nova's `@`, carried through rather than checked

    bool is_valid() const { return identifier != 0; }
};

// ---------------------------------------------------------------- operations

// What an operation does. Every one of these lowers to p-code, which is what
// the specifications describe every processor in, so this list is chosen to
// reach that vocabulary rather than to mirror any instruction set.
enum class Operation {
    // Values from nowhere.
    Constant,
    Copy,

    // Memory. The space says which memory, and the width says how much; neither
    // is read off a type, because at level 1 there is no type to read.
    Load,
    Store,

    // Arithmetic and logic. Signedness is on the instruction: the same bits are
    // added the same way whether or not anyone knows what they mean.
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Negate,
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    ShiftLeft,
    ShiftRight,

    // Comparison, which answers in one byte.
    Equal,
    NotEqual,
    Less,
    LessOrEqual,

    // Changing width, which is where signedness decides what fills the top.
    Extend,
    Truncate,

    // Addresses.
    FrameAddress,
    GlobalAddress,
    FunctionAddress,

    // Leaving and arriving.
    Call,
    Return,
    Jump,
    Branch,
    Switch,

    // Where control arrives from more than one place, the value it brings.
    Phi,

    // A body that was already instructions. Level 0 is bytes somebody has not
    // read yet, and refusing to hold it would mean the levels do not all
    // compile, which is the one promise Nova makes everywhere.
    Raw,
};

// One operation.
//
// `width` and `is_signed` are here rather than on a type because the machine
// asks for them and a level-1 function has no type to answer with. The existing
// generator already worked this way, deriving both from C types at the last
// moment; this only makes them the thing that was always being carried.
struct Instruction {
    Operation operation = Operation::Copy;
    Value result;
    std::vector<Value> arguments;

    uint64_t immediate = 0;
    Space space = Space::Data;
    int width = 0;           // bytes this works in; zero means the target's word
    bool is_signed = false;  // what the top bit is worth

    // Operation::Call, and the blocks a terminator can reach.
    std::string callee;
    std::vector<uint32_t> successors;

    // Operation::Raw, kept exactly as written so that reading it back gives
    // what was there before anybody compiled anything.
    std::vector<uint8_t> bytes;

    Where where;  // so a complaint still points at the line somebody wrote
};

// A run of instructions with one way in and one way out. The last instruction
// is the one that leaves.
struct Block {
    uint32_t identifier = 0;
    std::vector<Instruction> instructions;
};

// ----------------------------------------------------------------- functions

// A function, and the three things about it that are not negotiable.
//
// `address` is where it goes, `budget` is what it has to fit inside, and the
// storage on each parameter is where the caller has already agreed values will
// be. A recovered function being patched has all three fixed by the program it
// came out of, and none of them is the compiler's to choose. That is the whole
// reason this is not somebody else's intermediate form: every general compiler
// treats all three as its own decision and offers no way to be told otherwise.
struct Function {
    std::string name;
    Level level = Level::Source;

    std::vector<Value> parameters;  // each with the storage the ABI or the
                                    // source pinned it to
    Value result;

    std::vector<Block> blocks;
    uint32_t entry = 0;

    uint64_t address = 0;  // where it must sit; zero when it may go anywhere
    uint64_t budget = 0;   // bytes it must not exceed; zero when unbounded

    // What is already at `address`, so what comes out can be reduced to the
    // parts that differ. Empty asks for the whole function.
    std::vector<uint8_t> existing;
};

// Everything being compiled together, and the machine it is for.
struct Unit {
    Target target;
    std::vector<Function> functions;
};

// ---------------------------------------------------------------- inspection

// Whether an operation is a way out of a block. A block ends with exactly one
// of these and holds no others.
bool is_terminator(Operation operation);

// The operation's name, for anything a person reads.
const char *operation_name(Operation operation);

// How wide an instruction works, resolving the zero that means "the target's
// natural word" against the target actually being compiled for.
int width_of(const Instruction &instruction, const Target &target);

// Whether a function is put together the way everything downstream assumes:
// one definition per value, nothing read that is never given, every block
// ending in exactly one way out, and every block jumped to existing. Appends
// what is wrong to `problems` and returns whether it added nothing.
//
// This is checked rather than trusted because the failures it catches are the
// ones that otherwise become machine code that reads a register nothing wrote.
bool verify(const Function &function, std::vector<std::string> &problems);

} // namespace ir
} // namespace nova
} // namespace astral_internal

#endif
