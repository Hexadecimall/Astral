#include "lower.hh"

#include "format.hh"
#include "knowledge.hh"
#include "lexer.hh"

#include <cctype>
#include <cstdlib>
#include <set>
#include <utility>

namespace astral_internal {
namespace nova {
namespace {

namespace c = compiler;

// What a name means inside the function being lowered.
struct Binding {
    TypePtr type = nullptr;
    // The name the C tree uses. Usually the Nova name; for a parameter pinned
    // to a register it is also reachable by the register's name.
    std::string c_name;
    bool is_constant = false;
    bool is_function = false;
};

// `_2_6_` names six bytes starting two bytes in. The emitter writes these
// whenever the decompiler read part of a value without knowing what the whole
// was, so refusing them would mean refusing Astral's own output. Returns how
// wide the piece is, rounded up to a width a register holds.
bool byte_slice_width(const std::string &name, int &width)
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

// x5 and w5 are one register seen at two widths; d3 and s3 likewise. The
// canonical spelling is the wide one, and a narrow mention is a cast.
std::string canonical_register(const std::string &name)
{
    if (name.empty())
        return name;
    if (name[0] == 'w')
        return "x" + name.substr(1);
    if (name[0] == 's' && name.size() > 1 && std::isdigit(static_cast<unsigned char>(name[1])))
        return "d" + name.substr(1);
    return name;
}

class Lowerer {
public:
    Lowerer(const Unit &nova, Types &types, Lowered &out, std::vector<Diagnostic> &diagnostics)
        : nova_(nova), types_(types), out_(out), diagnostics_(diagnostics)
    {
    }

    bool run();

private:
    // ------------------------------------------------------------ reporting
    void say(const Where &where, const std::string &message)
    {
        ++errors_;
        diagnostics_.push_back(Diagnostic{where.line, where.column, message});
    }
    void warn(const Where &where, const std::string &message)
    {
        diagnostics_.push_back(Diagnostic{where.line, where.column, "warning: " + message});
    }
    std::string spell_type(TypePtr type) const { return spell(type, types_); }

    // ------------------------------------------------------------ scopes
    void push() { scopes_.emplace_back(); }
    void pop() { scopes_.pop_back(); }
    void bind(const std::string &name, const Binding &binding) { scopes_.back()[name] = binding; }
    const Binding *look_up(const std::string &name) const;

    // ------------------------------------------------------------ registers
    // The register type at the spelled width: x is u64, w is u32, d is f64,
    // s is f32.
    TypePtr register_type(const std::string &name);
    // The variable a register mention refers to in the current function,
    // declaring it if this is the first mention.
    const Binding *register_binding(const std::string &name, const Where &where);

    // ------------------------------------------------------------ items
    void global(const Variable &variable);
    void function(const Function &function);
    bool check_parameter_storage(const Function &function, size_t index, const Variable &parameter);

    // ------------------------------------------------------------ statements
    c::StatementPtr statement(const Statement &statement);
    c::StatementPtr block(const Statement &statement);
    c::StatementPtr declaration(const Statement &statement);
    c::StatementPtr for_in(const Statement &statement);
    c::StatementPtr match(const Statement &statement);
    c::StatementPtr return_statement(const Statement &statement);
    bool fold_constant(const Expression &expression, uint64_t &value);

    // ------------------------------------------------------------ expressions
    c::ExpressionPtr expression(const Expression &expression, TypePtr &type);
    c::ExpressionPtr name(const Expression &expression, TypePtr &type);
    c::ExpressionPtr call(const Expression &expression, TypePtr &type);
    c::ExpressionPtr level_one_call(const Expression &callee, const Where &where);
    c::ExpressionPtr member(const Expression &expression, TypePtr &type);
    c::ExpressionPtr assign(const Expression &expression, TypePtr &type);
    c::ExpressionPtr cast(c::ExpressionPtr value, TypePtr to, const Where &where);
    c::ExpressionPtr make(c::Expression::Kind kind, const Where &where);
    c::ExpressionPtr integer(uint64_t value, TypePtr type, const Where &where);
    c::ExpressionPtr named(const std::string &name, const Where &where);
    TypePtr wider(TypePtr left, TypePtr right);

    const Unit &nova_;
    Types &types_;
    Lowered &out_;
    std::vector<Diagnostic> &diagnostics_;
    std::vector<std::map<std::string, Binding>> scopes_;
    std::map<std::string, const Function *> functions_;
    std::map<std::string, uint64_t> enum_constants_;
    std::set<std::string> enum_names_;
    int errors_ = 0;

    // Callees nothing declares, with the argument types they were called
    // with. Recorded as externals with a fixed signature before the C checker
    // runs, because its own guess for an unknown callee is C89's `int f()`,
    // and on Apple's arm64 a variadic call puts every argument on the stack.
    // Almost no real callee is variadic; a call written with two arguments
    // means two arguments in registers.
    std::map<std::string, std::vector<TypePtr>> unknown_callees_;

    // Per function.
    const Function *current_ = nullptr;
    c::Function *current_c_ = nullptr;
    // Register variables the body mentioned, in the order first seen, and the
    // declarations they need at the top of the body.
    std::vector<std::string> registers_;
    std::vector<c::Variable> register_declarations_;
    bool saw_assembly_ = false;
};

// ---------------------------------------------------------------- helpers

const Binding *Lowerer::look_up(const std::string &name) const
{
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        auto found = scope->find(name);
        if (found != scope->end())
            return &found->second;
    }
    return nullptr;
}

TypePtr Lowerer::register_type(const std::string &name)
{
    switch (name.empty() ? '\0' : name[0]) {
    case 'x': return types_.integer(8, false);
    case 'w': return types_.integer(4, false);
    case 'd': return types_.floating(8);
    case 's': return types_.floating(4);
    default: return nullptr;
    }
}

const Binding *Lowerer::register_binding(const std::string &name, const Where &where)
{
    std::string wide = canonical_register(name);
    if (const Binding *existing = look_up(wide))
        return existing;
    if (!current_) {
        say(where, name + " is a register, and a register only means something inside a function");
        return nullptr;
    }
    TypePtr type = register_type(wide);
    if (!type) {
        say(where, name + " is not a register compiled code can hold a value in");
        return nullptr;
    }
    // Declared at the top of the function, so every path sees the same slot.
    c::Variable variable;
    variable.name = wide;
    variable.type = type;
    variable.where = where;
    register_declarations_.push_back(std::move(variable));
    registers_.push_back(wide);
    Binding binding;
    binding.type = type;
    binding.c_name = wide;
    scopes_[1][wide] = binding;  // the function's own scope, under the globals
    return look_up(wide);
}

c::ExpressionPtr Lowerer::make(c::Expression::Kind kind, const Where &where)
{
    auto expression = std::make_unique<c::Expression>();
    expression->kind = kind;
    expression->where = where;
    return expression;
}

c::ExpressionPtr Lowerer::integer(uint64_t value, TypePtr type, const Where &where)
{
    c::ExpressionPtr literal = make(c::Expression::Kind::IntegerLiteral, where);
    literal->integer_value = value;
    literal->type = type;
    return literal;
}

c::ExpressionPtr Lowerer::named(const std::string &name, const Where &where)
{
    c::ExpressionPtr expression = make(c::Expression::Kind::Name, where);
    expression->name = name;
    return expression;
}

c::ExpressionPtr Lowerer::cast(c::ExpressionPtr value, TypePtr to, const Where &where)
{
    if (!to || !value)
        return value;
    c::ExpressionPtr result = make(c::Expression::Kind::Cast, where);
    result->named_type = to;
    result->left = std::move(value);
    return result;
}

TypePtr Lowerer::wider(TypePtr left, TypePtr right)
{
    if (!left)
        return right;
    if (!right)
        return left;
    // Pointer arithmetic answers with the pointer.
    if (left->kind == c::Type::Kind::Pointer || left->kind == c::Type::Kind::Array)
        return left;
    if (right->kind == c::Type::Kind::Pointer || right->kind == c::Type::Kind::Array)
        return right;
    if (left->kind == c::Type::Kind::Floating)
        return left;
    if (right->kind == c::Type::Kind::Floating)
        return right;
    // Unknown storage stays unknown through arithmetic, so the refusal at a
    // divide reaches the reader with the original spelling.
    if (types_.is_unknown(left) && left->width >= right->width)
        return left;
    if (types_.is_unknown(right) && right->width >= left->width)
        return right;
    return left->width >= right->width ? left : right;
}

// ------------------------------------------------------------------- run

bool Lowerer::run()
{
    push();  // globals

    for (const Enumeration &enumeration : nova_.enumerations) {
        enum_names_.insert(enumeration.name);
        for (const Enumeration::Constant &constant : enumeration.constants) {
            enum_constants_[enumeration.name + "." + constant.name] = constant.value;
            enum_constants_[constant.name] = constant.value;
        }
    }
    for (const Function &function : nova_.functions)
        functions_[function.name] = &function;

    for (const Variable &variable : nova_.globals)
        global(variable);
    for (const Function &function : nova_.functions)
        this->function(function);

    for (const auto &[name, argument_types] : unknown_callees_) {
        c::External external;
        external.name = name;
        external.is_function = true;
        external.type = types_.store().function(types_.integer(4, true), argument_types, false);
        out_.unit.externals.push_back(external);
    }

    pop();
    return errors_ == 0;
}

void Lowerer::global(const Variable &variable)
{
    c::Variable lowered;
    lowered.name = variable.name;
    lowered.where = variable.where;
    lowered.type = variable.type;

    if (variable.storage.kind == Storage::Kind::Register
        || variable.storage.kind == Storage::Kind::EntryRegister) {
        say(variable.where, variable.name + " is a global, and a global does not live in a register");
        return;
    }
    if (variable.storage.kind == Storage::Kind::Frame) {
        say(variable.where, variable.name + " is a global, and a global does not live in a frame");
        return;
    }
    if (variable.initialiser) {
        TypePtr guessed = nullptr;
        lowered.initialiser = expression(*variable.initialiser, guessed);
        if (!lowered.type)
            lowered.type = guessed;
    }
    if (!lowered.type) {
        say(variable.where, variable.name + " needs a type; nothing here says what it is");
        return;
    }
    if (variable.storage.kind == Storage::Kind::Address)
        out_.addresses[variable.name] = variable.storage.address;

    Binding binding;
    binding.type = lowered.type;
    binding.c_name = variable.name;
    binding.is_constant = variable.is_constant;
    bind(variable.name, binding);
    out_.unit.globals.push_back(std::move(lowered));
}

// Whether a parameter's pinned register is where the ABI puts it. The ABI is
// not negotiable from inside a function, so a disagreement is a mistake in
// the text, not a request.
bool Lowerer::check_parameter_storage(const Function &function, size_t index,
                                       const Variable &parameter)
{
    const Storage &storage = parameter.storage;
    if (storage.kind == Storage::Kind::None)
        return true;
    if (storage.kind == Storage::Kind::Address) {
        say(parameter.where, parameter.name + " is a parameter; a parameter does not have an address");
        return false;
    }
    if (storage.kind == Storage::Kind::EntryRegister) {
        say(parameter.where, parameter.name + ": a parameter is already the value on entry; write @ "
                                 + storage.register_name);
        return false;
    }
    if (storage.kind == Storage::Kind::Frame) {
        if (index < 8)
            say(parameter.where, "parameter " + std::to_string(index) + " arrives in a register on arm64, not in the frame");
        return index >= 8;
    }
    bool floating = parameter.type && parameter.type->kind == c::Type::Kind::Floating;
    std::string wanted = std::string(floating ? "d" : "x") + std::to_string(index);
    if (canonical_register(storage.register_name) != wanted) {
        say(parameter.where, "parameter " + std::to_string(index) + " of " + function.name + " arrives in "
                                 + wanted + " on arm64, not in " + storage.register_name);
        return false;
    }
    return true;
}

void Lowerer::function(const Function &function)
{
    if (function.has_address)
        out_.addresses[function.name] = function.address;
    out_.levels[function.name] = function.level;

    if (function.level == Level::Machine) {
        std::string text;
        for (const StatementPtr &child : function.body->body)
            if (child && child->kind == Statement::Kind::Asm)
                text += child->assembly;
        out_.assembly[function.name] = text;
        return;
    }

    c::Function lowered;
    lowered.name = function.name;
    lowered.where = function.where;

    current_ = &function;
    current_c_ = &lowered;
    registers_.clear();
    register_declarations_.clear();
    saw_assembly_ = false;
    push();  // the function's scope, in which registers live

    std::vector<TypePtr> parameter_types;
    for (size_t index = 0; index < function.parameters.size(); ++index) {
        const Variable &parameter = function.parameters[index];
        check_parameter_storage(function, index, parameter);
        c::Variable one;
        one.where = parameter.where;
        one.type = parameter.type;
        one.name = parameter.name;
        if (!one.type) {
            if (parameter.storage.kind == Storage::Kind::Register) {
                one.type = register_type(parameter.storage.register_name);
                one.name = canonical_register(parameter.storage.register_name);
            }
            if (!one.type) {
                say(parameter.where, parameter.name + " needs a type or a register");
                one.type = types_.integer(8, false);
            }
        }
        Binding binding;
        binding.type = one.type;
        binding.c_name = one.name;
        bind(parameter.name, binding);
        if (parameter.storage.kind == Storage::Kind::Register) {
            std::string wide = canonical_register(parameter.storage.register_name);
            bind(wide, binding);
            registers_.push_back(wide);
        }
        parameter_types.push_back(one.type);
        lowered.parameters.push_back(std::move(one));
    }

    TypePtr result = function.result;
    if (!result && function.result_storage.kind == Storage::Kind::Register)
        result = register_type(function.result_storage.register_name);
    if (!result)
        result = types_.void_type();
    if (function.result_storage.kind == Storage::Kind::Register) {
        std::string wide = canonical_register(function.result_storage.register_name);
        bool floating = result->kind == c::Type::Kind::Floating;
        if (wide != (floating ? "d0" : "x0"))
            say(function.where, function.name + " answers in " + (floating ? "d0" : "x0")
                                    + " on arm64, not in " + function.result_storage.register_name);
    } else if (function.result_storage.kind != Storage::Kind::None) {
        say(function.where, "a result lives in a register, not at " + spell(function.result_storage));
    }
    lowered.type = types_.store().function(result, parameter_types, function.variadic);

    if (function.body) {
        c::StatementPtr body = block(*function.body);
        if (body) {
            // A function that answers through a register and whose body never
            // says `return` answers with whatever the register holds at the
            // end, which is what the text meant.
            if (function.result_storage.kind == Storage::Kind::Register
                && result->kind != c::Type::Kind::Void) {
                bool ends_with_return = !body->body.empty()
                                        && body->body.back()->kind == c::Statement::Kind::Return;
                if (!ends_with_return) {
                    std::string wide = canonical_register(function.result_storage.register_name);
                    if (const Binding *binding = look_up(wide)) {
                        auto ret = std::make_unique<c::Statement>();
                        ret->kind = c::Statement::Kind::Return;
                        ret->where = function.where;
                        ret->value = cast(named(binding->c_name, function.where), result, function.where);
                        body->body.push_back(std::move(ret));
                    }
                }
            }
            if (!register_declarations_.empty()) {
                auto declaration = std::make_unique<c::Statement>();
                declaration->kind = c::Statement::Kind::Declaration;
                declaration->where = function.where;
                declaration->variables = std::move(register_declarations_);
                body->body.insert(body->body.begin(), std::move(declaration));
            }
            lowered.body = std::move(body);
        }
    }

    pop();
    current_ = nullptr;
    current_c_ = nullptr;
    out_.unit.functions.push_back(std::move(lowered));
}

// -------------------------------------------------------------- statements

c::StatementPtr Lowerer::block(const Statement &statement)
{
    auto lowered = std::make_unique<c::Statement>();
    lowered->kind = c::Statement::Kind::Compound;
    lowered->where = statement.where;
    push();
    if (statement.kind == Statement::Kind::Compound) {
        for (const StatementPtr &child : statement.body)
            if (child)
                if (c::StatementPtr one = this->statement(*child))
                    lowered->body.push_back(std::move(one));
    } else if (c::StatementPtr one = this->statement(statement)) {
        lowered->body.push_back(std::move(one));
    }
    pop();
    return lowered;
}

c::StatementPtr Lowerer::statement(const Statement &statement)
{
    using Kind = Statement::Kind;
    switch (statement.kind) {
    case Kind::Compound:
        return block(statement);
    case Kind::Empty:
        return nullptr;
    case Kind::Expression: {
        auto lowered = std::make_unique<c::Statement>();
        lowered->kind = c::Statement::Kind::Expression;
        lowered->where = statement.where;
        TypePtr type = nullptr;
        lowered->value = expression(*statement.value, type);
        return lowered->value ? std::move(lowered) : nullptr;
    }
    case Kind::Declaration:
        return declaration(statement);
    case Kind::If:
    case Kind::While:
    case Kind::DoWhile: {
        auto lowered = std::make_unique<c::Statement>();
        lowered->kind = statement.kind == Kind::If      ? c::Statement::Kind::If
                        : statement.kind == Kind::While ? c::Statement::Kind::While
                                                        : c::Statement::Kind::DoWhile;
        lowered->where = statement.where;
        TypePtr type = nullptr;
        lowered->value = expression(*statement.value, type);
        if (statement.then_branch)
            lowered->then_branch = block(*statement.then_branch);
        if (statement.else_branch)
            lowered->else_branch = statement.else_branch->kind == Kind::If
                                       ? this->statement(*statement.else_branch)
                                       : block(*statement.else_branch);
        return lowered;
    }
    case Kind::Loop: {
        auto lowered = std::make_unique<c::Statement>();
        lowered->kind = c::Statement::Kind::While;
        lowered->where = statement.where;
        lowered->value = integer(1, types_.integer(4, true), statement.where);
        lowered->then_branch = block(*statement.then_branch);
        return lowered;
    }
    case Kind::ForIn:
        return for_in(statement);
    case Kind::Match:
        return match(statement);
    case Kind::Label:
    case Kind::Goto:
    case Kind::Break:
    case Kind::Continue: {
        auto lowered = std::make_unique<c::Statement>();
        lowered->kind = statement.kind == Kind::Label   ? c::Statement::Kind::Label
                        : statement.kind == Kind::Goto  ? c::Statement::Kind::Goto
                        : statement.kind == Kind::Break ? c::Statement::Kind::Break
                                                        : c::Statement::Kind::Continue;
        lowered->where = statement.where;
        lowered->name = statement.name;
        return lowered;
    }
    case Kind::Return:
        return return_statement(statement);
    case Kind::Asm:
        say(statement.where, "asm inside a function that also has statements is not supported yet; "
                             "make the whole body asm, or none of it");
        return nullptr;
    }
    return nullptr;
}

c::StatementPtr Lowerer::declaration(const Statement &statement)
{
    auto lowered = std::make_unique<c::Statement>();
    lowered->kind = c::Statement::Kind::Declaration;
    lowered->where = statement.where;
    for (const Variable &variable : statement.variables) {
        c::Variable one;
        one.name = variable.name;
        one.where = variable.where;
        one.type = variable.type;

        TypePtr guessed = nullptr;
        if (variable.initialiser)
            one.initialiser = expression(*variable.initialiser, guessed);
        if (!one.type && variable.storage.kind == Storage::Kind::Register)
            one.type = register_type(variable.storage.register_name);
        if (!one.type)
            one.type = guessed;
        if (!one.type) {
            if (variable.initialiser) {
                warn(variable.where, variable.name + " has no type and its value's type is not known here, "
                                         "so it is taken to be i64");
                one.type = types_.integer(8, true);
            } else {
                say(variable.where, variable.name + " needs a type");
                continue;
            }
        }
        if (variable.storage.kind == Storage::Kind::Address)
            say(variable.where, variable.name + " is a local; a local does not have an address. "
                                    "Declare it outside the function if it is a global");
        if (variable.storage.kind == Storage::Kind::EntryRegister)
            say(variable.where, variable.name + " cannot live in a register's entry value; that is read-only");

        Binding binding;
        binding.type = one.type;
        binding.c_name = variable.name;
        binding.is_constant = variable.is_constant;
        bind(variable.name, binding);
        if (variable.storage.kind == Storage::Kind::Register) {
            // The local is the register, so the register's name reaches it.
            std::string wide = canonical_register(variable.storage.register_name);
            bind(wide, binding);
            registers_.push_back(wide);
        }
        lowered->variables.push_back(std::move(one));
    }
    return lowered->variables.empty() ? nullptr : std::move(lowered);
}

// for (index in first..last) becomes the counted loop it is.
c::StatementPtr Lowerer::for_in(const Statement &statement)
{
    if (!statement.subject || statement.subject->kind != Expression::Kind::Range) {
        say(statement.where, "for over anything other than a range needs a length Nova cannot see; "
                             "write for (" + statement.name + " in 0..count)");
        return nullptr;
    }
    auto lowered = std::make_unique<c::Statement>();
    lowered->kind = c::Statement::Kind::For;
    lowered->where = statement.where;

    push();
    TypePtr first_type = nullptr;
    TypePtr last_type = nullptr;
    c::ExpressionPtr first = expression(*statement.subject->left, first_type);
    c::ExpressionPtr last = expression(*statement.subject->right, last_type);
    TypePtr counter = wider(first_type, last_type);
    if (!counter || counter->kind != c::Type::Kind::Integer)
        counter = types_.integer(8, true);

    auto initialiser = std::make_unique<c::Statement>();
    initialiser->kind = c::Statement::Kind::Declaration;
    initialiser->where = statement.where;
    c::Variable variable;
    variable.name = statement.name;
    variable.type = counter;
    variable.initialiser = std::move(first);
    variable.where = statement.where;
    initialiser->variables.push_back(std::move(variable));
    lowered->initialiser = std::move(initialiser);

    Binding binding;
    binding.type = counter;
    binding.c_name = statement.name;
    bind(statement.name, binding);

    c::ExpressionPtr condition = make(c::Expression::Kind::Binary, statement.where);
    condition->binary_op = statement.subject->inclusive ? c::BinaryOp::LessEqual : c::BinaryOp::Less;
    condition->left = named(statement.name, statement.where);
    condition->right = std::move(last);
    lowered->condition = std::move(condition);

    c::ExpressionPtr step = make(c::Expression::Kind::Assign, statement.where);
    step->assign_op = c::BinaryOp::Add;
    step->left = named(statement.name, statement.where);
    step->right = integer(1, counter, statement.where);
    lowered->step = std::move(step);

    lowered->then_branch = block(*statement.then_branch);
    pop();
    return lowered;
}

bool Lowerer::fold_constant(const Expression &expression, uint64_t &value)
{
    switch (expression.kind) {
    case Expression::Kind::IntegerLiteral:
        value = expression.integer_value;
        return true;
    case Expression::Kind::BooleanLiteral:
        value = expression.boolean_value ? 1 : 0;
        return true;
    case Expression::Kind::Name: {
        auto found = enum_constants_.find(expression.name);
        if (found != enum_constants_.end()) {
            value = found->second;
            return true;
        }
        // A constant the emitter named rather than left as a number. The
        // listing says TIOCGWINSZ or INT8_MAX because that is what the value
        // means; reading it back has to undo exactly that.
        return Knowledge::instance().constant_value(expression.name, value);
    }
    case Expression::Kind::Member: {
        if (!expression.left || expression.left->kind != Expression::Kind::Name)
            return false;
        auto found = enum_constants_.find(expression.left->name + "." + expression.name);
        if (found == enum_constants_.end())
            return false;
        value = found->second;
        return true;
    }
    case Expression::Kind::Unary:
        if (expression.unary_op == UnaryOp::Minus && fold_constant(*expression.left, value)) {
            value = static_cast<uint64_t>(-static_cast<int64_t>(value));
            return true;
        }
        return false;
    default:
        return false;
    }
}

// match becomes a switch. Each arm ends in a break, because a Nova arm does
// not fall into the next one; several values on one arm are several cases
// sharing a body, which is the one place fallthrough is what was meant.
c::StatementPtr Lowerer::match(const Statement &statement)
{
    auto lowered = std::make_unique<c::Statement>();
    lowered->kind = c::Statement::Kind::Switch;
    lowered->where = statement.where;
    TypePtr type = nullptr;
    lowered->value = expression(*statement.value, type);

    auto body = std::make_unique<c::Statement>();
    body->kind = c::Statement::Kind::Compound;
    body->where = statement.where;

    for (const MatchArm &arm : statement.arms) {
        c::StatementPtr arm_body = block(*arm.body);
        auto stop = std::make_unique<c::Statement>();
        stop->kind = c::Statement::Kind::Break;
        stop->where = arm.where;
        arm_body->body.push_back(std::move(stop));

        if (arm.values.empty()) {
            auto fallback = std::make_unique<c::Statement>();
            fallback->kind = c::Statement::Kind::Default;
            fallback->where = arm.where;
            fallback->then_branch = std::move(arm_body);
            body->body.push_back(std::move(fallback));
            continue;
        }
        for (size_t index = 0; index < arm.values.size(); ++index) {
            uint64_t value = 0;
            if (!fold_constant(*arm.values[index], value)) {
                say(arm.values[index]->where, "a match arm has to name a constant");
                continue;
            }
            auto label = std::make_unique<c::Statement>();
            label->kind = c::Statement::Kind::Case;
            label->where = arm.values[index]->where;
            label->case_value = value;
            if (index + 1 == arm.values.size())
                label->then_branch = std::move(arm_body);
            body->body.push_back(std::move(label));
        }
    }
    lowered->then_branch = std::move(body);
    return lowered;
}

c::StatementPtr Lowerer::return_statement(const Statement &statement)
{
    auto lowered = std::make_unique<c::Statement>();
    lowered->kind = c::Statement::Kind::Return;
    lowered->where = statement.where;
    if (statement.value) {
        TypePtr type = nullptr;
        lowered->value = expression(*statement.value, type);
        return lowered;
    }
    // `return;` in a function that answers through a register answers with
    // what the register holds.
    if (current_ && current_->result_storage.kind == Storage::Kind::Register) {
        std::string wide = canonical_register(current_->result_storage.register_name);
        if (const Binding *binding = look_up(wide)) {
            TypePtr result = current_c_->type->result;
            lowered->value = cast(named(binding->c_name, statement.where), result, statement.where);
        }
    }
    return lowered;
}

// ------------------------------------------------------------- expressions

c::ExpressionPtr Lowerer::expression(const Expression &e, TypePtr &type)
{
    using Kind = Expression::Kind;
    type = nullptr;
    switch (e.kind) {
    case Kind::IntegerLiteral:
        type = e.type;
        return integer(e.integer_value, e.type, e.where);
    case Kind::FloatLiteral: {
        c::ExpressionPtr literal = make(c::Expression::Kind::FloatLiteral, e.where);
        literal->float_value = e.float_value;
        type = types_.floating(8);
        return literal;
    }
    case Kind::StringLiteral: {
        c::ExpressionPtr literal = make(c::Expression::Kind::StringLiteral, e.where);
        literal->text = e.text;
        // Typed the way the C front end types its own literals, so the
        // generator treats the two identically.
        literal->type = types_.array_of(types_.integer(1, true), e.text.size() + 1);
        type = types_.pointer_to(types_.integer(1, true));
        return literal;
    }
    case Kind::BooleanLiteral:
        type = types_.boolean();
        return integer(e.boolean_value ? 1 : 0, types_.integer(4, true), e.where);
    case Kind::NullLiteral:
        type = types_.pointer_to(types_.void_type());
        return integer(0, types_.integer(8, false), e.where);
    case Kind::Name:
        return name(e, type);
    case Kind::Register: {
        if (e.entry_value) {
            say(e.where, "the value " + e.name + " held on entry is not something compiled code can produce; "
                             "give it a name, or take it from where it was saved");
            return nullptr;
        }
        return name(e, type);
    }
    case Kind::Call:
        return call(e, type);
    case Kind::Unary: {
        // &"text" is the text: a literal is already an address.
        if (e.unary_op == UnaryOp::AddressOf && e.left->kind == Kind::StringLiteral)
            return expression(*e.left, type);
        TypePtr inner = nullptr;
        c::ExpressionPtr operand = expression(*e.left, inner);
        if (!operand)
            return nullptr;
        c::ExpressionPtr unary = make(c::Expression::Kind::Unary, e.where);
        unary->unary_op = e.unary_op;
        unary->left = std::move(operand);
        switch (e.unary_op) {
        case UnaryOp::Dereference:
            type = inner && (inner->kind == c::Type::Kind::Pointer || inner->kind == c::Type::Kind::Array)
                       ? inner->target : nullptr;
            break;
        case UnaryOp::AddressOf:
            type = inner ? types_.pointer_to(inner) : nullptr;
            break;
        case UnaryOp::Not:
            type = types_.boolean();
            break;
        default:
            type = inner;
        }
        return unary;
    }
    case Kind::Binary: {
        TypePtr left_type = nullptr;
        TypePtr right_type = nullptr;
        c::ExpressionPtr left = expression(*e.left, left_type);
        c::ExpressionPtr right = expression(*e.right, right_type);
        if (!left || !right)
            return nullptr;
        if (Types::needs_signedness(e.binary_op)) {
            // This is the one refusal. Everything else works on storage whose
            // meaning is not known; these four need to know whether the top
            // bit is a sign, and guessing here is how wrong code gets written.
            const char *what = e.binary_op == c::BinaryOp::Divide       ? "dividing"
                               : e.binary_op == c::BinaryOp::Modulo     ? "taking the remainder of"
                               : e.binary_op == c::BinaryOp::ShiftRight ? "shifting right"
                                                                        : "ordering";
            if (left_type && types_.is_unknown(left_type)) {
                say(e.left->where, std::string(what) + " " + format(*e.left, types_) + " needs to know whether "
                                       + spell_type(left_type) + " is signed. Write it as i"
                                       + std::to_string(left_type->width * 8) + " or u"
                                       + std::to_string(left_type->width * 8));
                return nullptr;
            }
            if (right_type && types_.is_unknown(right_type)) {
                say(e.right->where, std::string(what) + " by " + format(*e.right, types_) + " needs to know whether "
                                        + spell_type(right_type) + " is signed. Write it as i"
                                        + std::to_string(right_type->width * 8) + " or u"
                                        + std::to_string(right_type->width * 8));
                return nullptr;
            }
        }
        c::ExpressionPtr binary = make(c::Expression::Kind::Binary, e.where);
        binary->binary_op = e.binary_op;
        binary->left = std::move(left);
        binary->right = std::move(right);
        switch (e.binary_op) {
        case c::BinaryOp::Less:
        case c::BinaryOp::LessEqual:
        case c::BinaryOp::Greater:
        case c::BinaryOp::GreaterEqual:
        case c::BinaryOp::Equal:
        case c::BinaryOp::NotEqual:
        case c::BinaryOp::LogicalAnd:
        case c::BinaryOp::LogicalOr:
            type = types_.boolean();
            break;
        default:
            type = wider(left_type, right_type);
        }
        return binary;
    }
    case Kind::Assign:
        return assign(e, type);
    case Kind::Index: {
        TypePtr base_type = nullptr;
        TypePtr index_type = nullptr;
        c::ExpressionPtr base = expression(*e.left, base_type);
        c::ExpressionPtr index = expression(*e.right, index_type);
        if (!base || !index)
            return nullptr;
        c::ExpressionPtr result = make(c::Expression::Kind::Index, e.where);
        result->left = std::move(base);
        result->right = std::move(index);
        type = base_type && (base_type->kind == c::Type::Kind::Pointer || base_type->kind == c::Type::Kind::Array)
                   ? base_type->target : nullptr;
        return result;
    }
    case Kind::Member:
        return member(e, type);
    case Kind::Conditional: {
        TypePtr test_type = nullptr;
        TypePtr then_type = nullptr;
        TypePtr else_type = nullptr;
        c::ExpressionPtr test = expression(*e.left, test_type);
        c::ExpressionPtr then = expression(*e.right, then_type);
        c::ExpressionPtr otherwise = expression(*e.third, else_type);
        if (!test || !then || !otherwise)
            return nullptr;
        c::ExpressionPtr result = make(c::Expression::Kind::Conditional, e.where);
        result->left = std::move(test);
        result->right = std::move(then);
        result->third = std::move(otherwise);
        type = wider(then_type, else_type);
        return result;
    }
    case Kind::As: {
        TypePtr inner = nullptr;
        c::ExpressionPtr value = expression(*e.left, inner);
        if (!value)
            return nullptr;
        type = e.named_type;
        return cast(std::move(value), e.named_type, e.where);
    }
    case Kind::SizeOf: {
        c::ExpressionPtr result = make(c::Expression::Kind::SizeOf, e.where);
        if (e.named_type) {
            result->named_type = e.named_type;
        } else {
            TypePtr inner = nullptr;
            result->left = expression(*e.left, inner);
            if (!result->left)
                return nullptr;
            // The C tree sizes a type, not a value, so the value's type is
            // what is handed over.
            result->named_type = inner;
            result->left.reset();
            if (!inner) {
                say(e.where, "sizeof needs a type, and this value's type is not known here");
                return nullptr;
            }
        }
        type = types_.integer(8, false);
        return result;
    }
    case Kind::AddressOfName: {
        c::ExpressionPtr unary = make(c::Expression::Kind::Unary, e.where);
        unary->unary_op = UnaryOp::AddressOf;
        unary->left = named(e.name, e.where);
        if (const Binding *binding = look_up(e.name)) {
            unary->left->name = binding->c_name;
            type = types_.pointer_to(binding->type);
        }
        return unary;
    }
    case Kind::Range:
        say(e.where, "a range only means something as the subject of a for");
        return nullptr;
    case Kind::List: {
        c::ExpressionPtr list = make(c::Expression::Kind::InitialiserList, e.where);
        for (const ExpressionPtr &element : e.arguments) {
            TypePtr inner = nullptr;
            c::ExpressionPtr one = expression(*element, inner);
            if (!one)
                return nullptr;
            list->arguments.push_back(std::move(one));
        }
        return list;
    }
    }
    return nullptr;
}

c::ExpressionPtr Lowerer::name(const Expression &e, TypePtr &type)
{
    auto constant = enum_constants_.find(e.name);
    if (constant != enum_constants_.end()) {
        type = types_.integer(4, true);
        return integer(constant->second, type, e.where);
    }
    if (const Binding *binding = look_up(e.name)) {
        type = binding->type;
        return named(binding->c_name, e.where);
    }
    auto function = functions_.find(e.name);
    if (function != functions_.end()) {
        type = nullptr;
        for (const c::Function &lowered : out_.unit.functions)
            if (lowered.name == e.name)
                type = lowered.type;
        return named(e.name, e.where);
    }
    if (is_register_name(e.name) && current_) {
        if (e.name == "sp" || e.name == "lr" || e.name == "pc" || e.name == "fp") {
            say(e.where, e.name + " is not a register a compiled function can treat as a value");
            return nullptr;
        }
        const Binding *binding = register_binding(e.name, e.where);
        if (!binding)
            return nullptr;
        TypePtr spelled = register_type(e.name);
        type = spelled;
        c::ExpressionPtr value = named(binding->c_name, e.where);
        // A narrow spelling of a wide register reads its low part.
        if (spelled && binding->type && spelled != binding->type && spelled->width < binding->type->width)
            return cast(std::move(value), spelled, e.where);
        return value;
    }
    // Not declared here: something the program around this code has. The C
    // checker records it as an external and the generator asks the program
    // for its address.
    type = nullptr;
    return named(e.name, e.where);
}

// `call strcmp;` at level 1: the arguments are the registers the function has
// set, in ABI order, up to the first it has not. The result lands in x0.
c::ExpressionPtr Lowerer::level_one_call(const Expression &callee, const Where &where)
{
    c::ExpressionPtr call = make(c::Expression::Kind::Call, where);
    TypePtr callee_type = nullptr;
    call->callee = expression(callee, callee_type);
    if (!call->callee)
        return nullptr;
    std::vector<TypePtr> argument_types;
    for (int index = 0; index < 8; ++index) {
        std::string wide = "x" + std::to_string(index);
        const Binding *binding = look_up(wide);
        if (!binding)
            break;
        argument_types.push_back(binding->type);
        call->arguments.push_back(named(binding->c_name, where));
    }
    if (callee.kind == Expression::Kind::Name && !look_up(callee.name) && !functions_.count(callee.name)
        && !unknown_callees_.count(callee.name))
        unknown_callees_[callee.name] = argument_types;
    const Binding *result = register_binding("x0", where);
    if (!result)
        return call;
    c::ExpressionPtr assign = make(c::Expression::Kind::Assign, where);
    assign->assign_op = c::BinaryOp::Comma;
    assign->left = named(result->c_name, where);
    assign->right = cast(std::move(call), result->type, where);
    return assign;
}

c::ExpressionPtr Lowerer::call(const Expression &e, TypePtr &type)
{
    // A bare `call f;` reaches here with no arguments and a callee that was
    // never given a list. That is the level-1 form.
    if (e.arguments.empty() && e.callee && e.callee->kind == Expression::Kind::Name && current_
        && current_->level == Level::Storage && !registers_.empty()) {
        type = types_.integer(8, false);
        return level_one_call(*e.callee, e.where);
    }
    if (e.callee && e.callee->kind == Expression::Kind::Name && enum_names_.count(e.callee->name)) {
        say(e.where, e.callee->name + " is an enum, not a function");
        return nullptr;
    }
    c::ExpressionPtr call = make(c::Expression::Kind::Call, e.where);
    TypePtr callee_type = nullptr;
    call->callee = expression(*e.callee, callee_type);
    if (!call->callee)
        return nullptr;
    std::vector<TypePtr> argument_types;
    for (const ExpressionPtr &argument : e.arguments) {
        TypePtr inner = nullptr;
        c::ExpressionPtr one = expression(*argument, inner);
        if (!one)
            return nullptr;
        argument_types.push_back(inner ? inner : types_.integer(8, false));
        call->arguments.push_back(std::move(one));
    }
    if (callee_type && callee_type->kind == c::Type::Kind::Pointer && callee_type->target)
        callee_type = callee_type->target;
    if (callee_type && callee_type->kind == c::Type::Kind::Function) {
        type = callee_type->result;
        return call;
    }
    if (e.callee->kind == Expression::Kind::Name && !look_up(e.callee->name)
        && !functions_.count(e.callee->name) && !unknown_callees_.count(e.callee->name))
        unknown_callees_[e.callee->name] = argument_types;
    type = nullptr;
    return call;
}

c::ExpressionPtr Lowerer::member(const Expression &e, TypePtr &type)
{
    // Kind.Minus is a constant, not a member.
    if (e.left->kind == Expression::Kind::Name) {
        auto constant = enum_constants_.find(e.left->name + "." + e.name);
        if (constant != enum_constants_.end()) {
            type = types_.integer(4, true);
            return integer(constant->second, type, e.where);
        }
    }
    TypePtr base_type = nullptr;
    c::ExpressionPtr base = expression(*e.left, base_type);
    if (!base)
        return nullptr;
    c::ExpressionPtr result = make(c::Expression::Kind::Member, e.where);
    result->name = e.name;
    result->left = std::move(base);

    TypePtr record = base_type;
    bool through_pointer = e.through_pointer;
    if (record && record->kind == c::Type::Kind::Pointer) {
        // `.` through a pointer is fine in Nova; the C tree wants the arrow.
        record = record->target;
        through_pointer = true;
    } else if (through_pointer && record) {
        warn(e.where, "-> on " + spell_type(record) + ", which is not a pointer; reading it as .");
        through_pointer = false;
    }
    result->through_pointer = through_pointer;
    int slice = 0;
    if (record && record->kind == c::Type::Kind::Struct) {
        for (const c::Type::Member &member : record->members)
            if (member.name == e.name) {
                type = member.type;
                return result;
            }
        if (byte_slice_width(e.name, slice)) {
            warn(e.where, e.name + " names bytes inside " + record->name
                              + " rather than a member of it");
            type = types_.unknown(slice);
            return result;
        }
        say(e.where, record->name + " has no member called " + e.name);
        return nullptr;
    }
    // Reached through something whose shape was never recovered. A slice still
    // says how wide it is, which is all the generator needs.
    if (byte_slice_width(e.name, slice)) {
        type = types_.unknown(slice);
        return result;
    }
    type = nullptr;
    return result;
}

c::ExpressionPtr Lowerer::assign(const Expression &e, TypePtr &type)
{
    if (e.left->kind == Expression::Kind::Name) {
        if (const Binding *binding = look_up(e.left->name))
            if (binding->is_constant) {
                say(e.where, e.left->name + " was declared with val and cannot be written");
                return nullptr;
            }
    }
    TypePtr left_type = nullptr;
    TypePtr right_type = nullptr;
    c::ExpressionPtr left = expression(*e.left, left_type);
    c::ExpressionPtr right = expression(*e.right, right_type);
    if (!left || !right)
        return nullptr;

    // Writing w5 sets the low half of x5 and clears the top; the C tree says
    // that by narrowing the value and letting the store widen it again.
    bool narrow_register = e.left->kind == Expression::Kind::Name && is_register_name(e.left->name)
                           && e.left->name[0] == 'w' && left->kind == c::Expression::Kind::Cast;
    if (narrow_register) {
        c::ExpressionPtr target = std::move(left->left);
        left = std::move(target);
        if (e.binary_op == c::BinaryOp::Comma)
            right = cast(std::move(right), types_.integer(4, false), e.where);
    }

    c::ExpressionPtr assign = make(c::Expression::Kind::Assign, e.where);
    assign->assign_op = e.binary_op;
    assign->left = std::move(left);
    assign->right = std::move(right);
    type = left_type;
    return assign;
}

} // namespace

bool lower(const Unit &nova, Types &types, Lowered &out, std::vector<Diagnostic> &diagnostics)
{
    Lowerer lowerer(nova, types, out, diagnostics);
    return lowerer.run();
}

} // namespace nova
} // namespace astral_internal
