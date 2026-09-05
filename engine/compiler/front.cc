#include "front.hh"

#include "lexer.hh"
#include "parser.hh"
#include "sema.hh"

#include <map>

namespace astral_internal {
namespace compiler {

namespace {

bool is_warning(const Diagnostic &diagnostic)
{
    return diagnostic.message.compare(0, 9, "warning: ") == 0;
}

// ---------------------------------------------------------------- comparing

// A type written out by what it is rather than by what it was called, so the
// same type reached through two typedefs compares the same.
std::string signature_of(TypePtr type)
{
    if (type == nullptr)
        return "?";
    switch (type->kind) {
    case Type::Kind::Void:
        return "v";
    case Type::Kind::Integer:
        return (type->is_signed ? "i" : "u") + std::to_string(type->width);
    case Type::Kind::Floating:
        return "f" + std::to_string(type->width);
    case Type::Kind::Pointer:
        return "p(" + signature_of(type->target) + ")";
    case Type::Kind::Array:
        return "a" + std::to_string(type->count) + "(" + signature_of(type->target) + ")";
    case Type::Kind::Function: {
        std::string text = "F(" + signature_of(type->result) + ":";
        for (TypePtr parameter : type->parameters)
            text += signature_of(parameter) + ",";
        if (type->variadic)
            text += "...";
        return text + ")";
    }
    case Type::Kind::Struct: {
        std::string text = "S" + type->name + "{";
        for (const Type::Member &member : type->members)
            text += member.name + ":" + signature_of(member.type) + ";";
        return text + "}";
    }
    }
    return "?";
}

// Walks a function and writes down what it means.
//
// Two strings come out. `full` is everything, literal values included, and is
// what tells whether two versions mean the same thing. `shape` is the same
// walk with each literal replaced by a marker, so two functions with the same
// shape differ at most in the values of their literals - the one change a
// patch can make by writing bytes rather than by generating code.
class Canoniser {
public:
    void visit(const Function &function)
    {
        full_ += "fn " + function.name + " " + signature_of(function.type) + "\n";
        shape_ += "fn " + function.name + " " + signature_of(function.type) + "\n";
        for (const Variable &parameter : function.parameters)
            slot(parameter.name);
        statement(function.body.get());
    }

    const std::string &full() const { return full_; }
    const std::string &shape() const { return shape_; }
    const std::vector<LiteralChange> &literals() const { return literals_; }

private:
    void put(const std::string &text)
    {
        full_ += text;
        shape_ += text;
    }

    // Locals and parameters are known by where they sit rather than by what
    // they are called, so renaming one changes nothing.
    void slot(const std::string &name)
    {
        if (!name.empty())
            slots_[name] = next_slot_++;
    }
    std::string reference(const std::string &name) const
    {
        std::map<std::string, int>::const_iterator found = slots_.find(name);
        if (found != slots_.end())
            return "$" + std::to_string(found->second);
        // Anything not local resolves to an address, so its name matters.
        return "@" + name;
    }

    void literal(const std::string &value, const Where &where, bool is_text)
    {
        full_ += value;
        shape_ += "<lit>";
        LiteralChange change;
        change.before = value;
        change.after = value;
        change.where = where;
        change.is_text = is_text;
        literals_.push_back(change);
    }

    void expression(const Expression *node)
    {
        if (node == nullptr) {
            put("_");
            return;
        }
        switch (node->kind) {
        case Expression::Kind::IntegerLiteral:
            put("int:" + signature_of(node->type) + ":");
            literal(std::to_string(node->integer_value), node->where, false);
            put(" ");
            return;
        case Expression::Kind::FloatLiteral:
            put("flt:" + signature_of(node->type) + ":");
            literal(std::to_string(node->float_value), node->where, false);
            put(" ");
            return;
        case Expression::Kind::StringLiteral:
            put("str:");
            literal(node->text, node->where, true);
            put(" ");
            return;
        case Expression::Kind::Name:
            put("name:" + reference(node->name) + " ");
            return;
        case Expression::Kind::Call:
            put("call(");
            expression(node->callee.get());
            for (const ExpressionPtr &argument : node->arguments)
                expression(argument.get());
            put(") ");
            return;
        case Expression::Kind::Unary:
            put("un" + std::to_string(static_cast<int>(node->unary_op)) + "(");
            expression(node->left.get());
            put(") ");
            return;
        case Expression::Kind::Binary:
            put("bin" + std::to_string(static_cast<int>(node->binary_op)) + "(");
            expression(node->left.get());
            expression(node->right.get());
            put(") ");
            return;
        case Expression::Kind::Assign:
            put("set" + std::to_string(static_cast<int>(node->assign_op)) + "(");
            expression(node->left.get());
            expression(node->right.get());
            put(") ");
            return;
        case Expression::Kind::Cast:
            put("cast" + signature_of(node->named_type) + "(");
            expression(node->left.get());
            put(") ");
            return;
        case Expression::Kind::Index:
            put("at(");
            expression(node->left.get());
            expression(node->right.get());
            put(") ");
            return;
        case Expression::Kind::Member:
            // A member name is part of a layout, not a local, so it counts.
            put(std::string(node->through_pointer ? "arrow." : "dot.") + node->name + "(");
            expression(node->left.get());
            put(") ");
            return;
        case Expression::Kind::Conditional:
            put("if(");
            expression(node->left.get());
            expression(node->right.get());
            expression(node->third.get());
            put(") ");
            return;
        case Expression::Kind::SizeOf:
            put("size" + signature_of(node->named_type) + "(");
            expression(node->left.get());
            put(") ");
            return;
        case Expression::Kind::InitialiserList:
            put("list(");
            for (const ExpressionPtr &element : node->arguments)
                expression(element.get());
            put(") ");
            return;
        }
    }

    void statement(const Statement *node)
    {
        if (node == nullptr) {
            put(";none\n");
            return;
        }
        put(";" + std::to_string(static_cast<int>(node->kind)) + " ");
        switch (node->kind) {
        case Statement::Kind::Declaration:
            for (const Variable &variable : node->variables) {
                slot(variable.name);
                put(reference(variable.name) + ":" + signature_of(variable.type) + "=");
                if (variable.initialiser)
                    expression(variable.initialiser.get());
                put(" ");
            }
            break;
        case Statement::Kind::Case:
            put("case" + std::to_string(node->case_value) + " ");
            break;
        case Statement::Kind::Label:
        case Statement::Kind::Goto:
            // A label is internal to the function, so its name is not an
            // address; it is still what says which jump goes where, and there
            // is no position to put in its place.
            put("label:" + node->name + " ");
            break;
        default:
            break;
        }
        if (node->value)
            expression(node->value.get());
        put("\n");
        statement(node->initialiser.get());
        if (node->condition)
            expression(node->condition.get());
        if (node->step)
            expression(node->step.get());
        for (const StatementPtr &part : node->body)
            statement(part.get());
        statement(node->then_branch.get());
        statement(node->else_branch.get());
        put(";end\n");
    }

    std::string full_;
    std::string shape_;
    std::vector<LiteralChange> literals_;
    std::map<std::string, int> slots_;
    int next_slot_ = 0;
};

} // namespace

bool parse_unit(const std::string &source, TypeStore &types, Unit &unit,
                std::vector<Diagnostic> &diagnostics)
{
    size_t before = diagnostics.size();
    std::vector<Token> tokens = tokenise(source, diagnostics);
    bool ok = true;
    for (size_t i = before; i < diagnostics.size(); ++i)
        if (!is_warning(diagnostics[i]))
            ok = false;

    if (!parse(tokens, types, unit, diagnostics))
        ok = false;
    // Checking still runs after a parse error, since the tree it produced is
    // usually enough to find the rest of what is wrong in one pass.
    if (!check(types, unit, diagnostics))
        ok = false;
    return ok;
}

bool same_meaning(const Function &left, const Function &right)
{
    Canoniser a;
    a.visit(left);
    Canoniser b;
    b.visit(right);
    return a.full() == b.full();
}

uint64_t meaning_digest(const Function &function)
{
    Canoniser canoniser;
    canoniser.visit(function);
    // FNV-1a, which is enough to tell two versions apart and costs one pass.
    uint64_t hash = 1469598103934665603ull;
    for (char c : canoniser.full()) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool only_literals_differ(const Function &before, const Function &after,
                          std::vector<LiteralChange> &changes)
{
    changes.clear();
    Canoniser a;
    a.visit(before);
    Canoniser b;
    b.visit(after);
    if (a.shape() != b.shape())
        return false;
    // Equal shapes means the same walk with the same literals in the same
    // places, so the two lists line up one for one.
    if (a.literals().size() != b.literals().size())
        return false;
    for (size_t i = 0; i < a.literals().size(); ++i) {
        if (a.literals()[i].before == b.literals()[i].before)
            continue;
        LiteralChange change;
        change.before = a.literals()[i].before;
        change.after = b.literals()[i].before;
        change.where = b.literals()[i].where;
        change.is_text = b.literals()[i].is_text;
        changes.push_back(change);
    }
    return true;
}

} // namespace compiler
} // namespace astral_internal
