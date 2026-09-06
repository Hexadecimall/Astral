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
#include <map>
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

    // Which compiler's conventions to follow, when the id said. The same
    // instruction set passes arguments one way under one compiler and another
    // way under another - x86-64 is the loud example - so this is not a detail
    // that can be dropped when an id is read. Empty means the specification's
    // own default.
    std::string compiler;

    int address_bits = 0;   // 16 on a z80, 24 on a PIC-24, 32, 64
    int pointer_bytes = 0;  // four on a sixty-four bit machine, under ilp32

    // The width arithmetic is natural in. Taken from the address bus, which is
    // the same answer on most processors and the wrong one on an eight-bit
    // machine that addresses sixteen bits: an 8051 adds a byte at a time. The
    // registers say which it is, and the registers are in the compiled
    // specification rather than in the description read here.
    int word_bytes = 0;

    bool data_big_endian = false;
    bool instruction_big_endian = false;  // not always the same answer

    // How many bytes one address counts, and whether code and data are separate
    // memories. Both are properties of the address spaces, which live in the
    // compiled specification and not in the description, so neither is answered
    // by reading a language id alone.
    //
    // `spaces_read` says whether they were answered at all. A zero unit and a
    // false Harvard are what an unread target looks like, not claims about the
    // processor: nineteen of the languages here are word-addressed and
    // twenty-three are Harvard, and saying otherwise would be confidently
    // wrong about all of them rather than honestly silent.
    bool spaces_read = false;
    int address_unit_bytes = 0;
    bool harvard = false;

    compiler::Abi abi = compiler::Abi::SystemV;

    // Reads a target out of the specifications, so what this knows about a
    // processor is what the specification says rather than what anyone wrote
    // down here. Accepts a language id with or without a compiler on the end.
    // Returns false and says why when no specification describes it.
    static bool from_language_id(const std::string &language_id, Target &target,
                                 std::string &error);

    // Reads what only the compiled specification knows: how wide each register
    // is, how many bytes an address counts, and whether code and data are
    // separate memories. This loads the processor's specification, which is a
    // heavier thing than reading a description, so it is asked for rather than
    // done on the way past.
    //
    // Sets `spaces_read` when it succeeds. Everything filled in here is read
    // from the specification, so a processor nobody has written support for
    // still answers correctly about itself.
    bool read_specification(std::string &error);

    // How wide the named register is, in bytes, or zero when the specification
    // has no register by that name or has not been read.
    //
    // This is what a level-1 function needs. `@w0` says where a value lives and
    // nothing about its type, and the register is the only thing that says how
    // wide the value is: on AARCH64 `w0` is four bytes and `x0` is eight, and
    // guessing the machine word gets one of them wrong every time.
    int register_width(const std::string &name) const;

    // Where a register sits in the register file and how wide it is. p-code
    // addresses a register by an offset into that file rather than by name, so
    // both are kept: the name is what the source wrote, the offset is what the
    // machine means by it, and two names can overlap the same bytes.
    struct RegisterPlace {
        uint64_t offset = 0;
        int width = 0;
    };

    // Register widths by name, filled in by read_specification.
    std::map<std::string, int> registers;
    std::map<std::string, RegisterPlace> register_places;

    // Where the named register sits, or nothing when the specification has no
    // register by that name or has not been read.
    const RegisterPlace *register_place(const std::string &name) const;

    // The register a frame is measured from, and which way the frame grows.
    //
    // A frame offset is not an address. `@-0x70` means seventy bytes below
    // wherever this function's frame begins, and where that is, is in a
    // register the processor names. Without that, a frame slot is a number and
    // nothing can be read out of it.
    struct Frame {
        std::string pointer;         // the register the frame is measured from
        int pointer_width = 0;
        bool grows_downward = true;  // which way a frame is pushed
        bool known = false;
    };

    // Reads how a frame works on this processor, from the same specification
    // everything else here is read from.
    bool frame(Frame &out, std::string &error) const;

    // Where a call puts its arguments and its answer, according to this
    // processor's own compiler specification.
    //
    // This is the other half of what a pin is. A pinned parameter says where a
    // value already is because a recovered function put it there; this says
    // where one goes when nothing has said. Both end up as the same kind of
    // storage, and neither is guessed: the specification that describes the
    // calling convention is the one shipped with the processor, so a machine
    // nobody has written support for still passes arguments correctly.
    //
    // `widths` is how many bytes each argument takes and `result_width` how
    // many the answer does; zero means it answers with nothing. Returns false
    // and says why when the specification cannot be read.
    bool calling_convention(const std::vector<int> &widths, int result_width,
                            std::vector<Storage> &parameters, Storage &result,
                            std::string &error) const;
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

// ------------------------------------------------------------------ building

// Puts a function together, so that whatever is lowering into this has one
// thing to think about at a time.
//
// Two rules are kept here rather than left to whoever is building, because
// both are easy to break and expensive to find later: a value is numbered once
// and never reused, and a block is finished by exactly one way out. Emitting
// into a block that has already been left is refused at the point it happens,
// which is where the mistake is, instead of surfacing as a verification failure
// somewhere with no line attached.
class Builder {
public:
    explicit Builder(std::string name);

    // Numbers a fresh value. Storage is given here because at level 1 it is the
    // only thing that identifies the value at all.
    Value value(TypePtr type = nullptr, Storage storage = Storage());

    // A parameter, which is a value the caller has already put somewhere.
    Value parameter(TypePtr type = nullptr, Storage storage = Storage());

    // Opens a block and makes it the one being written into. The first block
    // opened becomes the entry.
    uint32_t block();
    // Writes into a block opened earlier.
    void resume(uint32_t identifier);
    uint32_t current() const { return current_; }

    // Whether the block being written into is still open. A lowering asks this
    // before adding a way out: a body whose last statement was a return has
    // already left, and writing a second one is how a function ends up leaving
    // twice. Blocks reached only by falling into them are the ones that need
    // the ending nobody wrote.
    bool block_is_open() const;

    // Adds an instruction to the block being written into, and answers with its
    // result. Refused, with `failed()` set, once that block has been left.
    Value emit(Instruction instruction);

    // The operations worth a shorthand, because a lowering writes them
    // constantly and spelling each one out obscures what it is doing.
    Value constant(uint64_t immediate, int width, TypePtr type = nullptr);
    Value binary(Operation operation, Value left, Value right, int width, bool is_signed,
                 TypePtr type = nullptr);
    Value load(Value address, int width, Space space = Space::Data, TypePtr type = nullptr);
    void store(Value address, Value held, int width, Space space = Space::Data);

    // The ways out. Each finishes the block being written into.
    void ret();
    void ret(Value held);
    void jump(uint32_t destination);
    void branch(Value condition, uint32_t when_true, uint32_t when_false);

    void set_level(Level level) { function_.level = level; }
    void set_address(uint64_t address) { function_.address = address; }
    void set_budget(uint64_t budget) { function_.budget = budget; }
    void set_result(Value result) { function_.result = result; }

    // Whether anything was refused along the way, and what.
    bool failed() const { return !problems_.empty(); }
    const std::vector<std::string> &problems() const { return problems_; }

    // The finished function. Verified on the way out, so a builder that was
    // misused says so before anything tries to generate code from it.
    bool finish(Function &function, std::vector<std::string> &problems);

private:
    Block *block_named(uint32_t identifier);
    void terminate(Instruction instruction);

    Function function_;
    uint32_t next_value_ = 1;
    uint32_t next_block_ = 1;
    uint32_t current_ = 0;
    std::vector<std::string> problems_;
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

// The function written out, one instruction to a line, in a form meant to be
// read by a person and compared by a test. Storage is shown where it was
// pinned, because a value that has to be somewhere is the thing most worth
// seeing, and a width is shown when it was said.
std::string to_text(const Function &function);

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
