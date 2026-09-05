// What a piece of C means, once it has been read.
//
// The input this describes is the C Astral itself emits, plus whatever a
// person changes in it. That is a small language: no preprocessor beyond what
// the emitter writes, no templates, no variable-length arrays, no bitfields.
// Keeping the tree small is what makes generating code for it tractable.
#ifndef ASTRAL_COMPILER_AST_HH
#define ASTRAL_COMPILER_AST_HH

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace astral_internal {
namespace compiler {

// Where something was written, so a complaint can point at it.
struct Where {
    int line = 0;
    int column = 0;
};

// ---------------------------------------------------------------- types

struct Type;
using TypePtr = const Type *;

struct Type {
    enum class Kind { Void, Integer, Floating, Pointer, Array, Function, Struct };

    Kind kind = Kind::Void;
    // Integer and Floating: how many bytes it occupies.
    int width = 0;
    // Integer: whether the top bit is a sign.
    bool is_signed = true;
    // Pointer and Array: what it points at or holds.
    TypePtr target = nullptr;
    // Array: how many, or zero when unknown.
    uint64_t count = 0;
    // Function: what it answers with and what it takes.
    TypePtr result = nullptr;
    std::vector<TypePtr> parameters;
    bool variadic = false;
    // Struct: its name and its members in order.
    std::string name;
    struct Member {
        std::string name;
        TypePtr type = nullptr;
        uint64_t offset = 0;
    };
    std::vector<Member> members;

    bool is_integer() const { return kind == Kind::Integer; }
    bool is_pointer() const { return kind == Kind::Pointer; }
    bool is_arithmetic() const { return kind == Kind::Integer || kind == Kind::Floating; }
};

// Types are shared and outlive the tree that uses them, so one place owns them
// and hands out pointers that stay valid.
class TypeStore {
public:
    TypeStore();

    TypePtr void_type() const { return void_; }
    // Widths are in bytes: 1, 2, 4 or 8.
    TypePtr integer(int width, bool is_signed);
    TypePtr floating(int width);
    TypePtr pointer_to(TypePtr target);
    TypePtr array_of(TypePtr target, uint64_t count);
    TypePtr function(TypePtr result, std::vector<TypePtr> parameters, bool variadic);
    TypePtr structure(const std::string &name, std::vector<Type::Member> members);

    // The type a name stands for, or null when the name is not a type.
    TypePtr named(const std::string &name) const;
    void define_name(const std::string &name, TypePtr type);

    // How many bytes a value of this type occupies.
    static uint64_t size_of(TypePtr type);
    // What it must be aligned to.
    static uint64_t align_of(TypePtr type);

private:
    TypePtr keep(Type type);

    std::vector<std::unique_ptr<Type>> owned_;
    std::vector<std::pair<std::string, TypePtr>> names_;
    TypePtr void_ = nullptr;
};

// ---------------------------------------------------------------- expressions

struct Expression;
using ExpressionPtr = std::unique_ptr<Expression>;

enum class UnaryOp { Plus, Minus, Not, BitNot, Dereference, AddressOf, PreIncrement, PreDecrement,
                     PostIncrement, PostDecrement };

enum class BinaryOp { Add, Subtract, Multiply, Divide, Modulo, ShiftLeft, ShiftRight, Less,
                      LessEqual, Greater, GreaterEqual, Equal, NotEqual, BitAnd, BitOr, BitXor,
                      LogicalAnd, LogicalOr, Comma };

struct Expression {
    enum class Kind { IntegerLiteral, FloatLiteral, StringLiteral, Name, Call, Unary, Binary,
                      Assign, Cast, Index, Member, Conditional, SizeOf,
                      // A braced initialiser, whose elements are in `arguments`.
                      // Needed because the emitter writes byte tables as one.
                      InitialiserList };

    Kind kind = Kind::IntegerLiteral;
    Where where;
    // Filled in while checking; null until then.
    TypePtr type = nullptr;

    // IntegerLiteral
    uint64_t integer_value = 0;
    // FloatLiteral
    double float_value = 0;
    // StringLiteral: the bytes, without a terminator.
    std::string text;
    // Name, Member: what is named.
    std::string name;
    // Call: what is called, then its arguments.
    ExpressionPtr callee;
    std::vector<ExpressionPtr> arguments;
    // Unary, Binary, Assign, Index, Conditional
    UnaryOp unary_op = UnaryOp::Plus;
    BinaryOp binary_op = BinaryOp::Add;
    // Assign carries the operator of a compound assignment, or Comma for plain.
    BinaryOp assign_op = BinaryOp::Comma;
    ExpressionPtr left;
    ExpressionPtr right;
    ExpressionPtr third;
    // Member: whether it was written with `->`.
    bool through_pointer = false;
    // Cast, SizeOf: the type named in the source.
    TypePtr named_type = nullptr;
};

// ---------------------------------------------------------------- statements

struct Statement;
using StatementPtr = std::unique_ptr<Statement>;

// One `int x = 3;` inside a function.
struct Variable {
    std::string name;
    TypePtr type = nullptr;
    ExpressionPtr initialiser;
    Where where;
};

struct Statement {
    enum class Kind { Compound, Expression, Declaration, If, While, DoWhile, For, Switch, Case,
                      Default, Label, Goto, Break, Continue, Return, Empty };

    Kind kind = Kind::Empty;
    Where where;

    // Compound
    std::vector<StatementPtr> body;
    // Declaration
    std::vector<Variable> variables;
    // Expression, If, While, DoWhile, For, Switch, Return
    ExpressionPtr value;
    // For
    StatementPtr initialiser;
    ExpressionPtr condition;
    ExpressionPtr step;
    // If, While, DoWhile, For, Switch, Case, Default, Label
    StatementPtr then_branch;
    StatementPtr else_branch;
    // Case
    uint64_t case_value = 0;
    // Label, Goto
    std::string name;
};

// ---------------------------------------------------------------- top level

struct Function {
    std::string name;
    TypePtr type = nullptr;              // a Function type
    std::vector<Variable> parameters;    // names matching type->parameters
    StatementPtr body;                   // null for a declaration with no body
    Where where;
};

// Something the code refers to but does not define: another function in the
// program, or a global. The address is filled in by whoever knows the program.
struct External {
    std::string name;
    TypePtr type = nullptr;
    bool is_function = false;
};

struct Unit {
    std::vector<Function> functions;
    std::vector<External> externals;
    std::vector<Variable> globals;
};

} // namespace compiler
} // namespace astral_internal

#endif
