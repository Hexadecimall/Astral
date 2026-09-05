#include "sema.hh"

#include <cstdlib>
#include <map>
#include <set>
#include <string>

namespace astral_internal {
namespace compiler {

namespace {

std::string spell(TypePtr type)
{
    if (type == nullptr)
        return "an unknown type";
    switch (type->kind) {
    case Type::Kind::Void:
        return "void";
    case Type::Kind::Integer: {
        std::string name = type->is_signed ? "a signed " : "an unsigned ";
        return name + std::to_string(type->width * 8) + "-bit integer";
    }
    case Type::Kind::Floating:
        return type->width == 4 ? "float" : "double";
    case Type::Kind::Pointer:
        return "a pointer to " + spell(type->target);
    case Type::Kind::Array:
        return "an array of " + spell(type->target);
    case Type::Kind::Function:
        return "a function returning " + spell(type->result);
    case Type::Kind::Struct:
        return type->name.empty() ? "a struct" : "struct " + type->name;
    }
    return "an unknown type";
}

// `_2_6_` names six bytes starting two bytes in. Returns how wide the piece
// is, rounded up to a width a register holds.
bool piece_width(const std::string &name, int &width)
{
    if (name.size() < 5 || name.front() != '_' || name.back() != '_')
        return false;
    size_t middle = name.find('_', 1);
    if (middle == std::string::npos || middle + 1 >= name.size() - 1)
        return false;
    for (size_t i = 1; i < name.size() - 1; ++i)
        if (i != middle && (name[i] < '0' || name[i] > '9'))
            return false;
    unsigned long bytes = std::strtoul(name.c_str() + middle + 1, nullptr, 10);
    if (bytes == 0)
        return false;
    width = bytes <= 1 ? 1 : bytes <= 2 ? 2 : bytes <= 4 ? 4 : 8;
    return true;
}

struct Symbol {
    TypePtr type = nullptr;
    bool is_function = false;
    bool is_defined = false;
};

class Checker {
public:
    Checker(TypeStore &types, std::vector<Diagnostic> &diagnostics)
        : types_(types), diagnostics_(diagnostics)
    {
    }

    bool run(Unit &unit);

private:
    void fail(const Where &where, const std::string &message)
    {
        Diagnostic diagnostic;
        diagnostic.line = where.line;
        diagnostic.column = where.column;
        diagnostic.message = message;
        diagnostics_.push_back(diagnostic);
        ++errors_;
    }
    void warn(const Where &where, const std::string &message)
    {
        Diagnostic diagnostic;
        diagnostic.line = where.line;
        diagnostic.column = where.column;
        diagnostic.message = "warning: " + message;
        diagnostics_.push_back(diagnostic);
    }

    // -------------------------------------------------------------- scopes

    void push() { scopes_.push_back(std::map<std::string, Symbol>()); }
    void pop() { scopes_.pop_back(); }
    void declare(const std::string &name, const Symbol &symbol)
    {
        if (!name.empty())
            scopes_.back()[name] = symbol;
    }
    const Symbol *lookup(const std::string &name) const
    {
        for (size_t i = scopes_.size(); i-- > 0;) {
            std::map<std::string, Symbol>::const_iterator found = scopes_[i].find(name);
            if (found != scopes_[i].end())
                return &found->second;
        }
        return nullptr;
    }

    // -------------------------------------------------------------- types

    // What a value of this type turns into when it is used: an array becomes a
    // pointer to its first element, a function becomes a pointer to itself.
    TypePtr decay(TypePtr type)
    {
        if (type == nullptr)
            return nullptr;
        if (type->kind == Type::Kind::Array)
            return types_.pointer_to(type->target);
        if (type->kind == Type::Kind::Function)
            return types_.pointer_to(type);
        return type;
    }

    // Anything narrower than an int is worked on as an int.
    TypePtr promote(TypePtr type)
    {
        if (type != nullptr && type->kind == Type::Kind::Integer && type->width < 4)
            return types_.integer(4, true);
        return type;
    }

    TypePtr usual_arithmetic(TypePtr left, TypePtr right)
    {
        if (left == nullptr)
            return right;
        if (right == nullptr)
            return left;
        if (left->kind == Type::Kind::Floating || right->kind == Type::Kind::Floating) {
            int width = 4;
            if (left->kind == Type::Kind::Floating)
                width = left->width > width ? left->width : width;
            if (right->kind == Type::Kind::Floating)
                width = right->width > width ? right->width : width;
            return types_.floating(width);
        }
        left = promote(left);
        right = promote(right);
        if (left->kind != Type::Kind::Integer || right->kind != Type::Kind::Integer)
            return left;
        if (left->is_signed == right->is_signed)
            return left->width >= right->width ? left : right;
        TypePtr unsigned_side = left->is_signed ? right : left;
        TypePtr signed_side = left->is_signed ? left : right;
        if (unsigned_side->width >= signed_side->width)
            return unsigned_side;
        // The signed type is wider, so it holds every value the unsigned one
        // can take and the result stays signed.
        return signed_side;
    }

    // Whether a value of `from` can stand in for one of `to`, and what to say
    // when it only nearly can. Nothing here is ever an error: the code this
    // compiles came out of a decompiler and mixes the two constantly.
    void check_conversion(TypePtr to, TypePtr from, const Where &where, const std::string &what)
    {
        to = decay(to);
        from = decay(from);
        if (to == nullptr || from == nullptr || to == from)
            return;
        if (to->kind == Type::Kind::Void || from->kind == Type::Kind::Void)
            return;
        bool to_pointer = to->kind == Type::Kind::Pointer;
        bool from_pointer = from->kind == Type::Kind::Pointer;
        if (to_pointer != from_pointer) {
            if ((to_pointer && from->kind == Type::Kind::Integer) ||
                (from_pointer && to->kind == Type::Kind::Integer)) {
                warn(where, what + " turns " + spell(from) + " into " + spell(to));
                return;
            }
            warn(where, what + " turns " + spell(from) + " into " + spell(to));
            return;
        }
        if (to_pointer && from_pointer) {
            TypePtr a = to->target;
            TypePtr b = from->target;
            if (a == b || (a && a->kind == Type::Kind::Void) ||
                (b && b->kind == Type::Kind::Void))
                return;
            if (a && b && a->kind == Type::Kind::Integer && b->kind == Type::Kind::Integer &&
                a->width == b->width)
                return;
            warn(where, what + " turns " + spell(from) + " into " + spell(to));
            return;
        }
        if (to->kind == Type::Kind::Struct || from->kind == Type::Kind::Struct) {
            if (to != from)
                warn(where, what + " turns " + spell(from) + " into " + spell(to));
        }
    }

    bool is_scalar(TypePtr type)
    {
        type = decay(type);
        return type != nullptr && (type->kind == Type::Kind::Integer ||
                                   type->kind == Type::Kind::Floating ||
                                   type->kind == Type::Kind::Pointer);
    }

    // -------------------------------------------------------------- walking

    void check_function(Function &function);
    void check_statement(Statement *statement, TypePtr result);
    void check_variable(Variable &variable);
    void check_initialiser(Expression *expression, TypePtr type);
    TypePtr check_expression(Expression *expression);
    TypePtr check_call(Expression *expression);
    void collect_labels(const Statement *statement, std::set<std::string> &labels);
    void check_gotos(const Statement *statement, const std::set<std::string> &labels);

    void need_external(const std::string &name, TypePtr type, bool is_function);

    TypeStore &types_;
    std::vector<Diagnostic> &diagnostics_;
    std::vector<std::map<std::string, Symbol>> scopes_;
    Unit *unit_ = nullptr;
    std::set<std::string> defined_;
    std::set<std::string> recorded_;
    // Names met before anything said what they are.
    std::set<std::string> guessed_;
    int errors_ = 0;
};

void Checker::need_external(const std::string &name, TypePtr type, bool is_function)
{
    if (name.empty() || defined_.count(name) != 0 || recorded_.count(name) != 0)
        return;
    recorded_.insert(name);
    External external;
    external.name = name;
    external.type = type;
    external.is_function = is_function;
    unit_->externals.push_back(external);
}

TypePtr Checker::check_call(Expression *expression)
{
    // A call to a name nothing declares is a call into the rest of the
    // program. C89 said such a function returns int and takes anything, which
    // is exactly the assumption that suits a patch.
    Expression *callee = expression->callee.get();
    // A name that was met before it was called was guessed at as data. Now it
    // is plainly a function, so the guess is corrected rather than complained
    // about: taking a function's address and then calling it through another
    // name is ordinary in decompiled code.
    if (callee != nullptr && callee->kind == Expression::Kind::Name &&
        guessed_.count(callee->name) != 0) {
        TypePtr corrected =
            types_.function(types_.integer(8, false), std::vector<TypePtr>(), true);
        scopes_.front()[callee->name].type = corrected;
        scopes_.front()[callee->name].is_function = true;
        for (External &external : unit_->externals)
            if (external.name == callee->name) {
                external.type = corrected;
                external.is_function = true;
            }
        guessed_.erase(callee->name);
    }
    if (callee != nullptr && callee->kind == Expression::Kind::Name &&
        lookup(callee->name) == nullptr) {
        TypePtr guessed =
            types_.function(types_.integer(4, true), std::vector<TypePtr>(), true);
        Symbol symbol;
        symbol.type = guessed;
        symbol.is_function = true;
        scopes_.front()[callee->name] = symbol;
        need_external(callee->name, guessed, true);
        warn(callee->where, "'" + callee->name +
                                "' is not declared here, so it is taken to be a function in "
                                "the program that answers with an int");
    }

    TypePtr callee_type = decay(check_expression(callee));
    const Type *signature = nullptr;
    if (callee_type != nullptr && callee_type->kind == Type::Kind::Pointer &&
        callee_type->target != nullptr && callee_type->target->kind == Type::Kind::Function)
        signature = callee_type->target;
    else if (callee_type != nullptr && callee_type->kind == Type::Kind::Function)
        signature = callee_type;

    for (ExpressionPtr &argument : expression->arguments)
        check_expression(argument.get());

    if (signature == nullptr) {
        if (callee_type != nullptr && callee_type->kind == Type::Kind::Pointer) {
            // Calling through a `void *` happens in decompiled code whenever
            // the shape of the target was never recovered.
            warn(expression->where, "this calls through " + spell(callee_type) +
                                        ", so nothing about the call can be checked");
            return types_.integer(8, false);
        }
        if (callee_type != nullptr && callee_type->kind == Type::Kind::Integer) {
            // An address the decompiler recovered as a number. Refusing this
            // would refuse whole files, so it is called and the answer is
            // taken to be a machine word.
            warn(expression->where, "this calls " + spell(callee_type) +
                                        " as if it were an address");
            return types_.integer(8, false);
        }
        fail(expression->where, "error: this calls " + spell(callee_type) +
                                    ", which is not something that can be called");
        return types_.integer(4, true);
    }

    // An empty parameter list means the signature was never established, so
    // there is nothing to check the arguments against.
    bool unspecified = signature->parameters.empty() && signature->variadic;
    if (!unspecified) {
        size_t given = expression->arguments.size();
        size_t wanted = signature->parameters.size();
        if (given < wanted)
            // Also only a warning. A prototype the decompiler recovered is a
            // guess, and its call sites are guesses made separately; the two
            // disagreeing says the guesses differ, not that the code is wrong.
            warn(expression->where, "this call passes " + std::to_string(given) +
                                        " argument" + (given == 1 ? "" : "s") + " but the "
                                        "function takes " + std::to_string(wanted));
        else if (given > wanted && !signature->variadic)
            // Extra arguments are placed and never read, which costs nothing.
            // The emitter writes calls like this whenever it recovered more
            // about a call site than about the function it reaches, so this
            // cannot be a refusal.
            warn(expression->where, "this call passes " + std::to_string(given) +
                                        " arguments but the function takes only " +
                                        std::to_string(wanted));
        size_t checked = given < wanted ? given : wanted;
        for (size_t i = 0; i < checked; ++i)
            check_conversion(signature->parameters[i], expression->arguments[i]->type,
                             expression->arguments[i]->where,
                             "argument " + std::to_string(i + 1));
    }

    expression->type = signature->result;
    return expression->type;
}

TypePtr Checker::check_expression(Expression *expression)
{
    if (expression == nullptr)
        return nullptr;

    switch (expression->kind) {
    case Expression::Kind::IntegerLiteral:
    case Expression::Kind::FloatLiteral:
        if (expression->type == nullptr)
            expression->type = types_.integer(4, true);
        return expression->type;

    case Expression::Kind::StringLiteral:
        if (expression->type == nullptr)
            expression->type =
                types_.array_of(types_.integer(1, true), expression->text.size() + 1);
        return expression->type;

    case Expression::Kind::Name: {
        const Symbol *symbol = lookup(expression->name);
        if (symbol == nullptr) {
            // A name the program supplies: a global in the image the patched
            // function shares an address space with.
            TypePtr guessed = types_.integer(8, false);
            Symbol fresh;
            fresh.type = guessed;
            scopes_.front()[expression->name] = fresh;
            need_external(expression->name, guessed, false);
            guessed_.insert(expression->name);
            warn(expression->where, "'" + expression->name +
                                        "' is not declared here, so it is taken to be "
                                        "something the program already holds");
            expression->type = guessed;
            return expression->type;
        }
        expression->type = symbol->type;
        return expression->type;
    }

    case Expression::Kind::Call:
        return check_call(expression);

    case Expression::Kind::Cast: {
        check_expression(expression->left.get());
        expression->type = expression->named_type;
        return expression->type;
    }

    case Expression::Kind::SizeOf: {
        if (expression->left)
            check_expression(expression->left.get());
        expression->type = types_.integer(8, false);
        return expression->type;
    }

    case Expression::Kind::Unary: {
        TypePtr operand = check_expression(expression->left.get());
        switch (expression->unary_op) {
        case UnaryOp::Not:
            if (!is_scalar(operand))
                fail(expression->where,
                     "error: '!' needs a number or a pointer, not " + spell(operand));
            expression->type = types_.integer(4, true);
            break;
        case UnaryOp::Plus:
        case UnaryOp::Minus:
        case UnaryOp::BitNot:
            if (operand != nullptr && !operand->is_arithmetic())
                warn(expression->where,
                     "this operator wants a number, and was given " + spell(operand));
            expression->type = promote(operand);
            break;
        case UnaryOp::Dereference: {
            TypePtr pointer = decay(operand);
            if (pointer != nullptr && pointer->kind == Type::Kind::Pointer) {
                expression->type = pointer->target;
            } else {
                // Reading through an integer is what decompiled code does
                // wherever an address was recovered as a number, so it is read
                // as a machine word rather than refused.
                warn(expression->where,
                     "this reads through " + spell(operand) + " as if it were an address");
                expression->type = types_.integer(8, false);
            }
            break;
        }
        case UnaryOp::AddressOf:
            expression->type = types_.pointer_to(operand);
            break;
        case UnaryOp::PreIncrement:
        case UnaryOp::PreDecrement:
        case UnaryOp::PostIncrement:
        case UnaryOp::PostDecrement:
            if (!is_scalar(operand))
                fail(expression->where, "error: this can only step a number or a pointer, "
                                        "and was given " + spell(operand));
            expression->type = operand;
            break;
        }
        return expression->type;
    }

    case Expression::Kind::Binary: {
        TypePtr left = check_expression(expression->left.get());
        TypePtr right = check_expression(expression->right.get());
        BinaryOp op = expression->binary_op;

        if (op == BinaryOp::Comma) {
            expression->type = right;
            return expression->type;
        }
        if (op == BinaryOp::LogicalAnd || op == BinaryOp::LogicalOr) {
            if (!is_scalar(left) || !is_scalar(right))
                warn(expression->where, "a condition should be a number or a pointer");
            expression->type = types_.integer(4, true);
            return expression->type;
        }
        if (op == BinaryOp::Less || op == BinaryOp::LessEqual || op == BinaryOp::Greater ||
            op == BinaryOp::GreaterEqual || op == BinaryOp::Equal || op == BinaryOp::NotEqual) {
            expression->type = types_.integer(4, true);
            return expression->type;
        }
        if (op == BinaryOp::ShiftLeft || op == BinaryOp::ShiftRight) {
            // Only the left side decides the result; the count is promoted on
            // its own and then forgotten.
            expression->type = promote(left);
            return expression->type;
        }

        TypePtr left_decayed = decay(left);
        TypePtr right_decayed = decay(right);
        bool left_pointer = left_decayed != nullptr && left_decayed->kind == Type::Kind::Pointer;
        bool right_pointer =
            right_decayed != nullptr && right_decayed->kind == Type::Kind::Pointer;

        if (op == BinaryOp::Add || op == BinaryOp::Subtract) {
            if (left_pointer && right_pointer) {
                if (op == BinaryOp::Subtract) {
                    // The difference counts elements, not bytes.
                    expression->type = types_.integer(8, true);
                    return expression->type;
                }
                fail(expression->where, "error: two pointers cannot be added");
                expression->type = left_decayed;
                return expression->type;
            }
            if (left_pointer) {
                expression->type = left_decayed;
                return expression->type;
            }
            if (right_pointer && op == BinaryOp::Add) {
                expression->type = right_decayed;
                return expression->type;
            }
            if (right_pointer) {
                fail(expression->where, "error: a pointer cannot be subtracted from a number");
                expression->type = right_decayed;
                return expression->type;
            }
        }

        if ((left_decayed != nullptr && !left_decayed->is_arithmetic()) ||
            (right_decayed != nullptr && !right_decayed->is_arithmetic()))
            warn(expression->where, "this operator wants numbers, and was given " +
                                        spell(left) + " and " + spell(right));
        expression->type = usual_arithmetic(left_decayed, right_decayed);
        return expression->type;
    }

    case Expression::Kind::Assign: {
        TypePtr left = check_expression(expression->left.get());
        TypePtr right = check_expression(expression->right.get());
        if (expression->assign_op == BinaryOp::Comma)
            check_conversion(left, right, expression->where, "this assignment");
        else if (!is_scalar(left))
            warn(expression->where,
                 "a compound assignment wants a number or a pointer on the left");
        (void)right;
        // The value of an assignment is what was stored, so it has the type of
        // the thing stored into.
        expression->type = left;
        return expression->type;
    }

    case Expression::Kind::Index: {
        TypePtr base = decay(check_expression(expression->left.get()));
        TypePtr index = check_expression(expression->right.get());
        if (index != nullptr && !index->is_integer())
            warn(expression->where, "a subscript should be a whole number");
        if (base != nullptr && base->kind == Type::Kind::Pointer) {
            expression->type = base->target;
        } else {
            warn(expression->where,
                 "this indexes " + spell(base) + ", which holds no elements, so it is read "
                 "as an address");
            expression->type = types_.integer(8, false);
        }
        return expression->type;
    }

    case Expression::Kind::Member: {
        TypePtr object = check_expression(expression->left.get());
        TypePtr structure = object;
        if (expression->through_pointer) {
            TypePtr pointer = decay(object);
            if (pointer == nullptr || pointer->kind != Type::Kind::Pointer) {
                fail(expression->where, "error: '->' needs a pointer to a struct, not " +
                                            spell(object));
                expression->type = types_.integer(4, true);
                return expression->type;
            }
            structure = pointer->target;
        }
        if (structure == nullptr || structure->kind != Type::Kind::Struct) {
            int width = 0;
            if (piece_width(expression->name, width)) {
                // `value._2_6_` is the decompiler's way of naming six bytes of
                // a value starting two bytes in. It is not C, but it is what
                // the emitter writes wherever a machine register was used at
                // more than one width.
                warn(expression->where, "'" + expression->name + "' names bytes inside " +
                                            spell(structure) + " rather than a member");
                expression->type = types_.integer(width, false);
                return expression->type;
            }
            fail(expression->where,
                 "error: '" + expression->name + "' was asked for from " + spell(structure) +
                     ", which has no members");
            expression->type = types_.integer(4, true);
            return expression->type;
        }
        for (const Type::Member &member : structure->members)
            if (member.name == expression->name) {
                expression->type = member.type;
                return expression->type;
            }
        fail(expression->where,
             "error: " + spell(structure) + " has no member called '" + expression->name + "'");
        expression->type = types_.integer(4, true);
        return expression->type;
    }

    case Expression::Kind::Conditional: {
        TypePtr test = check_expression(expression->left.get());
        if (!is_scalar(test))
            warn(expression->where, "a condition should be a number or a pointer");
        TypePtr yes = decay(check_expression(expression->right.get()));
        TypePtr no = decay(check_expression(expression->third.get()));
        if (yes != nullptr && no != nullptr && yes->is_arithmetic() && no->is_arithmetic())
            expression->type = usual_arithmetic(yes, no);
        else if (yes != nullptr && yes->kind == Type::Kind::Pointer)
            expression->type = yes;
        else if (no != nullptr && no->kind == Type::Kind::Pointer)
            expression->type = no;
        else
            expression->type = yes != nullptr ? yes : no;
        return expression->type;
    }

    case Expression::Kind::InitialiserList: {
        for (ExpressionPtr &element : expression->arguments)
            check_expression(element.get());
        return expression->type;
    }
    }
    return expression->type;
}

void Checker::check_initialiser(Expression *expression, TypePtr type)
{
    if (expression == nullptr)
        return;
    if (expression->kind == Expression::Kind::InitialiserList) {
        expression->type = type;
        TypePtr element = nullptr;
        if (type != nullptr && type->kind == Type::Kind::Array)
            element = type->target;
        size_t at = 0;
        for (ExpressionPtr &part : expression->arguments) {
            TypePtr wanted = element;
            if (type != nullptr && type->kind == Type::Kind::Struct && at < type->members.size())
                wanted = type->members[at].type;
            check_initialiser(part.get(), wanted);
            ++at;
        }
        if (type != nullptr && type->kind == Type::Kind::Array && type->count != 0 &&
            expression->arguments.size() > type->count)
            warn(expression->where, "this holds more values than the array has room for");
        return;
    }
    TypePtr value = check_expression(expression);
    // A string literal initialising an array fills it rather than pointing at
    // it, so there is nothing to convert.
    if (type != nullptr && type->kind == Type::Kind::Array &&
        expression->kind == Expression::Kind::StringLiteral)
        return;
    check_conversion(type, value, expression->where, "this initialiser");
}

void Checker::check_variable(Variable &variable)
{
    if (variable.type != nullptr && variable.type->kind == Type::Kind::Void)
        fail(variable.where, "error: '" + variable.name + "' cannot have type void");
    Symbol symbol;
    symbol.type = variable.type;
    declare(variable.name, symbol);
    if (variable.initialiser)
        check_initialiser(variable.initialiser.get(), variable.type);
}

void Checker::check_statement(Statement *statement, TypePtr result)
{
    if (statement == nullptr)
        return;
    switch (statement->kind) {
    case Statement::Kind::Compound:
        push();
        for (StatementPtr &part : statement->body)
            check_statement(part.get(), result);
        pop();
        break;
    case Statement::Kind::Declaration:
        for (Variable &variable : statement->variables)
            check_variable(variable);
        break;
    case Statement::Kind::Expression:
        check_expression(statement->value.get());
        break;
    case Statement::Kind::If:
    case Statement::Kind::While:
    case Statement::Kind::DoWhile: {
        TypePtr test = check_expression(statement->value.get());
        if (!is_scalar(test))
            warn(statement->where, "a condition should be a number or a pointer");
        check_statement(statement->then_branch.get(), result);
        check_statement(statement->else_branch.get(), result);
        break;
    }
    case Statement::Kind::For:
        // The first clause may declare, and what it declares lives as long as
        // the loop does.
        push();
        check_statement(statement->initialiser.get(), result);
        if (statement->condition) {
            TypePtr test = check_expression(statement->condition.get());
            if (!is_scalar(test))
                warn(statement->where, "a loop condition should be a number or a pointer");
        }
        check_expression(statement->step.get());
        check_statement(statement->then_branch.get(), result);
        pop();
        break;
    case Statement::Kind::Switch: {
        TypePtr chosen = check_expression(statement->value.get());
        if (chosen != nullptr && !chosen->is_integer())
            warn(statement->where, "a switch chooses on a whole number");
        check_statement(statement->then_branch.get(), result);
        break;
    }
    case Statement::Kind::Case:
    case Statement::Kind::Default:
    case Statement::Kind::Label:
        check_statement(statement->then_branch.get(), result);
        break;
    case Statement::Kind::Return:
        if (statement->value) {
            TypePtr given = check_expression(statement->value.get());
            if (result != nullptr && result->kind == Type::Kind::Void)
                warn(statement->where, "this returns a value from a function that answers "
                                       "with nothing");
            else
                check_conversion(result, given, statement->where, "this return");
        }
        break;
    case Statement::Kind::Goto:
    case Statement::Kind::Break:
    case Statement::Kind::Continue:
    case Statement::Kind::Empty:
        break;
    }
}

void Checker::collect_labels(const Statement *statement, std::set<std::string> &labels)
{
    if (statement == nullptr)
        return;
    if (statement->kind == Statement::Kind::Label)
        labels.insert(statement->name);
    for (const StatementPtr &part : statement->body)
        collect_labels(part.get(), labels);
    collect_labels(statement->initialiser.get(), labels);
    collect_labels(statement->then_branch.get(), labels);
    collect_labels(statement->else_branch.get(), labels);
}

void Checker::check_gotos(const Statement *statement, const std::set<std::string> &labels)
{
    if (statement == nullptr)
        return;
    if (statement->kind == Statement::Kind::Goto && labels.count(statement->name) == 0)
        fail(statement->where,
             "error: there is no label called '" + statement->name + "' in this function");
    for (const StatementPtr &part : statement->body)
        check_gotos(part.get(), labels);
    check_gotos(statement->initialiser.get(), labels);
    check_gotos(statement->then_branch.get(), labels);
    check_gotos(statement->else_branch.get(), labels);
}

void Checker::check_function(Function &function)
{
    push();
    const Type *signature = function.type;
    for (size_t i = 0; i < function.parameters.size(); ++i) {
        Variable &parameter = function.parameters[i];
        if (parameter.type == nullptr && signature != nullptr && i < signature->parameters.size())
            parameter.type = signature->parameters[i];
        if (parameter.name.empty())
            continue;
        if (parameter.type != nullptr && parameter.type->kind == Type::Kind::Void)
            fail(parameter.where, "error: a parameter cannot have type void");
        Symbol symbol;
        symbol.type = parameter.type;
        declare(parameter.name, symbol);
    }

    TypePtr result = signature != nullptr ? signature->result : nullptr;
    check_statement(function.body.get(), result);

    std::set<std::string> labels;
    collect_labels(function.body.get(), labels);
    check_gotos(function.body.get(), labels);
    pop();
}

bool Checker::run(Unit &unit)
{
    unit_ = &unit;
    push();

    for (const External &external : unit.externals)
        recorded_.insert(external.name);

    for (Variable &global : unit.globals) {
        Symbol symbol;
        symbol.type = global.type;
        symbol.is_defined = true;
        declare(global.name, symbol);
        defined_.insert(global.name);
    }
    for (const External &external : unit.externals) {
        Symbol symbol;
        symbol.type = external.type;
        symbol.is_function = external.is_function;
        declare(external.name, symbol);
    }
    // Every function is in scope everywhere, so calling one written further
    // down the file works whether or not it was also declared up top.
    for (const Function &function : unit.functions) {
        Symbol symbol;
        symbol.type = function.type;
        symbol.is_function = true;
        symbol.is_defined = function.body != nullptr;
        const Symbol *existing = lookup(function.name);
        if (existing != nullptr && existing->is_defined && !symbol.is_defined)
            continue;
        declare(function.name, symbol);
        if (function.body)
            defined_.insert(function.name);
    }
    // A function declared and never given a body is something the program
    // already contains.
    for (const Function &function : unit.functions)
        if (function.body == nullptr)
            need_external(function.name, function.type, true);

    for (Variable &global : unit.globals)
        if (global.initialiser)
            check_initialiser(global.initialiser.get(), global.type);

    for (Function &function : unit.functions)
        if (function.body)
            check_function(function);

    pop();
    return errors_ == 0;
}

} // namespace

bool check(TypeStore &types, Unit &unit, std::vector<Diagnostic> &diagnostics)
{
    Checker checker(types, diagnostics);
    return checker.run(unit);
}

} // namespace compiler
} // namespace astral_internal
