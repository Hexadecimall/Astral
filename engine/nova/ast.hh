// What a piece of Nova means, once it has been read.
//
// Nova is the language Astral writes for recovered code. It borrows its shape
// from Fusion and its semantics from the hardware: storage is a thing you can
// name, a type is a view over storage rather than a promise about it, and
// nothing is undefined. Where C forces a commitment the binary does not
// support, Nova lets the commitment be missing and still compiles.
//
// The tree here is deliberately close to the C compiler's tree, because the
// back end that turns a tree into bytes already exists and is worth keeping.
// What Nova adds is storage: where a value lives, said out loud.
#ifndef ASTRAL_NOVA_AST_HH
#define ASTRAL_NOVA_AST_HH

#include "compiler/ast.hh"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace astral_internal {
namespace nova {

using compiler::Where;

// Where a value lives. This is what `@` writes.
//
// A recovered function knows more about storage than C can say: that a
// parameter arrives in x0, that a local is the stack slot at -0x70, that a
// global is the four bytes at 0x10000c0d0. Saying it removes the guessing that
// otherwise has to happen twice, once when reading and once when writing.
struct Storage {
    enum class Kind {
        // Nothing was said. The ABI decides, exactly as it does in C.
        None,
        // A machine register, by the name the architecture uses: x0, w22, d3.
        Register,
        // A fixed address in the image.
        Address,
        // An offset from the frame pointer, signed, so -0x70 is a local.
        Frame,
        // The value a register held on entry, which is not a place code can
        // write to. This is how `x29@entry` stops being a variable named after
        // a mistake.
        EntryRegister,
    };

    Kind kind = Kind::None;
    std::string register_name;  // Register and EntryRegister
    uint64_t address = 0;       // Address
    int64_t offset = 0;         // Frame

    bool is_none() const { return kind == Kind::None; }
};

// How much of a function has been recovered.
//
// Every level compiles. The level is not a mode the reader picks, it is a
// property of the text: it falls out of how much the source actually says.
// Raising it is the work of understanding the program, and the file keeps
// building the whole way up.
enum class Level {
    // Bytes. The body is one or more `asm` blocks and nothing else.
    Machine = 0,
    // Storage is known, types are not. Assignments name registers directly.
    Storage = 1,
    // Types are known and storage is still pinned where it matters.
    Typed = 2,
    // Storage is entirely implied by the ABI. This reads like a program.
    Source = 3,
};


// ---------------------------------------------------------------- expressions

struct Expression;
using ExpressionPtr = std::unique_ptr<Expression>;

using compiler::BinaryOp;
using compiler::UnaryOp;
using compiler::TypePtr;
using compiler::TypeStore;

struct Expression {
    enum class Kind {
        IntegerLiteral,
        FloatLiteral,
        StringLiteral,
        BooleanLiteral,
        NullLiteral,
        Name,
        // A register named where a value is expected, which is what level 1
        // reads like: `w0 = (w0 == 0);`.
        Register,
        Call,
        Unary,
        Binary,
        Assign,
        Index,
        Member,
        Conditional,
        // `value as u32`, which is Nova's cast. It reinterprets a view over
        // storage and never changes the bits unless the widths differ.
        As,
        SizeOf,
        AddressOfName,
        // `0..10` and `0..=10`, which only appear as the subject of a for.
        Range,
        // A braced list, for an array or a struct value.
        List,
    };

    Kind kind = Kind::IntegerLiteral;
    Where where;
    TypePtr type = nullptr;      // filled in while checking; null until then

    uint64_t integer_value = 0;
    double float_value = 0;
    bool boolean_value = false;
    std::string text;            // StringLiteral: the bytes, without a terminator
    std::string name;            // Name, Register, Member, AddressOfName

    ExpressionPtr callee;
    std::vector<ExpressionPtr> arguments;

    UnaryOp unary_op = UnaryOp::Plus;
    BinaryOp binary_op = BinaryOp::Add;
    ExpressionPtr left;
    ExpressionPtr right;
    ExpressionPtr third;

    bool through_pointer = false;  // Member: written with `->`
    // Register: written `w22@entry`, meaning what the register held when
    // the function was entered. Not a place anything can write to.
    bool entry_value = false;
    bool inclusive = false;        // Range: written with `..=`
    TypePtr named_type = nullptr;  // As, SizeOf
};

// ---------------------------------------------------------------- statements

struct Statement;
using StatementPtr = std::unique_ptr<Statement>;

// One `var count: i32 = 0;`, or a `stack` slot, or a global.
struct Variable {
    std::string name;
    TypePtr type = nullptr;
    ExpressionPtr initialiser;
    Storage storage;
    // `val` rather than `var`. A constant is not merely advice: the compiler
    // refuses to generate a write to one.
    bool is_constant = false;
    // Declared with `stack`, so it lives in the frame whether or not an offset
    // was given.
    bool is_stack = false;
    Where where;
};

// One arm of a match.
struct MatchArm {
    // The values this arm answers to. Empty means `else`.
    std::vector<ExpressionPtr> values;
    StatementPtr body;
    Where where;
};

struct Statement {
    enum class Kind {
        Compound,
        Expression,
        Declaration,
        If,
        While,
        DoWhile,
        // `loop { }`, which runs until something breaks out of it.
        Loop,
        // `for (index in 0..10) { }` and `for (item in list) { }`.
        ForIn,
        Match,
        Label,
        Goto,
        Break,
        Continue,
        Return,
        // A block of instructions, kept as written. This is level 0, and it is
        // also the escape hatch at every other level.
        Asm,
        Empty,
    };

    Kind kind = Kind::Empty;
    Where where;

    std::vector<StatementPtr> body;   // Compound
    std::vector<Variable> variables;  // Declaration
    ExpressionPtr value;              // Expression, If, While, DoWhile, Return, Match
    ExpressionPtr subject;            // ForIn: what is iterated
    std::string name;                 // Label, Goto, ForIn: the bound name
    StatementPtr then_branch;
    StatementPtr else_branch;
    std::vector<MatchArm> arms;       // Match
    std::string assembly;             // Asm: the instructions, one per line
    // What was written about this statement, kept so the decompiler's
    // warnings and a person's notes survive a round trip.
    std::string documentation;
};

// ---------------------------------------------------------------- top level

struct Function {
    std::string name;
    std::vector<Variable> parameters;
    TypePtr result = nullptr;
    Storage result_storage;
    StatementPtr body;             // null for a declaration with no body
    // Where the function sits in the image, when the source says. This is what
    // makes `func check @ 0x100000500` name a place rather than invent one.
    bool has_address = false;
    uint64_t address = 0;
    bool variadic = false;
    // What the text says about itself, worked out rather than declared. See
    // level_of().
    Level level = Level::Source;
    std::string documentation;
    Where where;
};

// A type the program uses, recovered rather than designed. Members may overlap:
// the same four bytes are often read two ways, and refusing to write that down
// would only mean writing it down somewhere Nova cannot check.
struct Record {
    std::string name;
    struct Member {
        std::string name;
        TypePtr type = nullptr;
        uint64_t offset = 0;
        bool offset_given = false;
        Where where;
    };
    std::vector<Member> members;
    // A class is a struct that owns functions. Nothing else about it differs:
    // the members are laid out the same way, it is passed the same way, and
    // the functions it owns are functions. What it buys is that the code
    // belonging to a thing is written where the thing is.
    bool is_class = false;
    // The names of the functions written inside it, already lifted into the
    // unit under `Class::name`. Kept here so a reader of the tree can still
    // tell which functions the class owns.
    std::vector<std::string> methods;
    bool has_address = false;
    uint64_t address = 0;
    std::string documentation;
    Where where;
};

struct Enumeration {
    std::string name;
    struct Constant {
        std::string name;
        uint64_t value = 0;
        Where where;
    };
    std::vector<Constant> constants;
    std::string documentation;
    Where where;
};

struct Unit {
    std::vector<std::string> imports;
    std::vector<Function> functions;
    std::vector<Variable> globals;
    std::vector<Record> records;
    std::vector<Enumeration> enumerations;
};

// How much of a function the text actually says, which is what its level is.
// Nothing declares a level; it is read off the source, so it cannot drift from
// what is written.
Level level_of(const Function &function);

} // namespace nova
} // namespace astral_internal

#endif
