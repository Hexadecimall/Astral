#include "parser.hh"

#include <functional>
#include <map>

namespace astral_internal {
namespace compiler {

namespace {

// Which node field holds what, since the tree keeps one set of children for
// every shape of expression:
//
//   Unary        left is the operand
//   Binary       left and right are the operands
//   Assign       left is the target, right is the value
//   Index        left is the base, right is the subscript
//   Member       left is the object, name is the member
//   Conditional  left is the test, right and third are the answers
//   Cast         left is the operand, named_type is what it becomes
//   SizeOf       named_type when a type was written, otherwise left
//   Call         callee and arguments

ExpressionPtr make_expression(Expression::Kind kind, const Where &where)
{
    ExpressionPtr expression(new Expression());
    expression->kind = kind;
    expression->where = where;
    return expression;
}

StatementPtr make_statement(Statement::Kind kind, const Where &where)
{
    StatementPtr statement(new Statement());
    statement->kind = kind;
    statement->where = where;
    return statement;
}

// Identifiers that carry no meaning for code generation. The first two come
// from the runtime header's macros: the `#define` line is dropped with every
// other preprocessor line, so the name survives into the token stream with
// nothing behind it.
bool is_ignorable_specifier(const std::string &name)
{
    return name == "astralInline" || name == "astralNoreturn" || name == "__inline" ||
           name == "__inline__" || name == "__forceinline" || name == "__extension__" ||
           name == "__signed__" || name == "__const" || name == "__volatile__" ||
           name == "__restrict" || name == "__restrict__" || name == "_Nullable" ||
           name == "_Nonnull" || name == "_Null_unspecified";
}

// What the type keywords add up to, collected before any of them is turned
// into a type: `unsigned long int` is three tokens naming one thing.
struct BaseSpelling {
    int longs = 0;
    int shorts = 0;
    bool is_signed = false;
    bool is_unsigned = false;
    enum class Core { None, Void, Char, Int, Float, Double, Bool } core = Core::None;
};

class Parser {
public:
    Parser(const std::vector<Token> &tokens, TypeStore &types,
           std::vector<Diagnostic> &diagnostics)
        : tokens_(tokens), types_(types), diagnostics_(diagnostics)
    {
    }

    bool run(Unit &unit);

private:
    // ------------------------------------------------------------ the stream

    const Token &peek(size_t ahead = 0) const
    {
        size_t at = at_ + ahead;
        return at < tokens_.size() ? tokens_[at] : tokens_.back();
    }
    const Token &take() { return tokens_[at_ < tokens_.size() - 1 ? at_++ : at_]; }
    bool at_end() const { return peek().kind == Token::Kind::End; }

    bool eat_punctuation(const char *what)
    {
        if (!peek().is_punctuation(what))
            return false;
        take();
        return true;
    }
    bool eat_name(const char *what)
    {
        if (!peek().is_name(what))
            return false;
        take();
        return true;
    }
    bool expect_punctuation(const char *what)
    {
        if (eat_punctuation(what))
            return true;
        fail(peek().where, std::string("error: expected '") + what + "' but found " +
                               describe(peek()));
        return false;
    }

    static std::string describe(const Token &token)
    {
        switch (token.kind) {
        case Token::Kind::End:
            return "the end of the file";
        case Token::Kind::Name:
            return "'" + token.text + "'";
        case Token::Kind::Integer:
        case Token::Kind::Float:
            return "the number " + (token.text.empty() ? std::string("0") : token.text);
        case Token::Kind::Character:
            return "a character literal";
        case Token::Kind::String:
            return "a string literal";
        case Token::Kind::Punctuation:
            return "'" + token.text + "'";
        }
        return "something unreadable";
    }

    void fail(const Where &where, const std::string &message)
    {
        // One complaint per position: recovery would otherwise turn a single
        // mistake into a wall of noise.
        if (!diagnostics_.empty() && diagnostics_.back().line == where.line &&
            diagnostics_.back().column == where.column)
            return;
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

    // ------------------------------------------------------------ specifiers

    bool skip_ignorable();
    bool is_declaration_start(size_t ahead = 0) const;
    bool is_type_name_start(size_t ahead = 0) const;

    struct Specifiers {
        TypePtr type = nullptr;
        bool is_typedef = false;
        bool is_extern = false;
        bool is_static = false;
        bool present = false;
        Where where;
    };
    Specifiers parse_specifiers();
    TypePtr build_base(const BaseSpelling &spelling, const Where &where);
    TypePtr parse_struct(bool is_union);
    TypePtr parse_enum();

    // ------------------------------------------------------------ declarators

    using Build = std::function<TypePtr(TypePtr)>;
    Build parse_declarator(std::string &name, std::vector<Variable> *parameters);
    Build parse_direct_declarator(std::string &name, std::vector<Variable> *parameters);
    void parse_parameters(std::vector<TypePtr> &types, std::vector<Variable> &named,
                          bool &variadic);
    TypePtr parse_type_name();
    void skip_trailing_decoration();

    // ------------------------------------------------------------ the pieces

    bool parse_external_declaration(Unit &unit);
    StatementPtr parse_statement();
    StatementPtr parse_compound();
    StatementPtr parse_declaration_statement();
    ExpressionPtr parse_initialiser();

    ExpressionPtr parse_expression();
    ExpressionPtr parse_assignment();
    ExpressionPtr parse_conditional();
    ExpressionPtr parse_binary(int level);
    ExpressionPtr parse_cast_expression();
    ExpressionPtr parse_unary();
    ExpressionPtr parse_postfix(ExpressionPtr base);
    ExpressionPtr parse_primary();

    bool fold_constant(const Expression *expression, uint64_t &value);
    void recover_to_semicolon();

    const std::vector<Token> &tokens_;
    TypeStore &types_;
    std::vector<Diagnostic> &diagnostics_;
    size_t at_ = 0;
    int errors_ = 0;
    // Enumerators are plain constants once read; nothing later needs to know
    // which enumeration they came from.
    std::map<std::string, uint64_t> enumerators_;
};

// ---------------------------------------------------------------- specifiers

bool Parser::skip_ignorable()
{
    bool skipped = false;
    for (;;) {
        const Token &token = peek();
        if (token.kind != Token::Kind::Name)
            break;
        if (token.text == "__attribute__" || token.text == "__declspec") {
            take();
            // Whatever is inside is advice to a compiler that is not this one.
            if (peek().is_punctuation("(")) {
                int depth = 0;
                do {
                    if (peek().is_punctuation("("))
                        ++depth;
                    else if (peek().is_punctuation(")"))
                        --depth;
                    else if (at_end())
                        break;
                    take();
                } while (depth > 0);
            }
            skipped = true;
            continue;
        }
        if (token.text == "__asm__" || token.text == "__asm" || token.text == "asm") {
            take();
            if (peek().is_punctuation("(")) {
                int depth = 0;
                do {
                    if (peek().is_punctuation("("))
                        ++depth;
                    else if (peek().is_punctuation(")"))
                        --depth;
                    else if (at_end())
                        break;
                    take();
                } while (depth > 0);
            }
            skipped = true;
            continue;
        }
        if (is_ignorable_specifier(token.text)) {
            take();
            skipped = true;
            continue;
        }
        break;
    }
    return skipped;
}

bool Parser::is_type_name_start(size_t ahead) const
{
    const Token &token = peek(ahead);
    if (token.kind != Token::Kind::Name)
        return false;
    const std::string &text = token.text;
    if (text == "void" || text == "char" || text == "short" || text == "int" || text == "long" ||
        text == "signed" || text == "unsigned" || text == "float" || text == "double" ||
        text == "_Bool" || text == "bool" || text == "struct" || text == "union" ||
        text == "enum" || text == "const" || text == "volatile" || text == "restrict" ||
        text == "_Atomic")
        return true;
    if (is_ignorable_specifier(text))
        return true;
    return !is_keyword(text) && types_.named(text) != nullptr;
}

bool Parser::is_declaration_start(size_t ahead) const
{
    const Token &token = peek(ahead);
    if (token.kind != Token::Kind::Name)
        return false;
    const std::string &text = token.text;
    if (text == "typedef" || text == "extern" || text == "static" || text == "auto" ||
        text == "register" || text == "inline" || text == "_Noreturn" ||
        text == "_Thread_local" || text == "_Static_assert")
        return true;
    if (!is_type_name_start(ahead))
        return false;
    if (is_keyword(text) || is_ignorable_specifier(text))
        return true;
    // A typedef name only begins a declaration when what follows could be a
    // declarator. `count x;` declares; `count = 1;` does not, even where
    // `count` also names a type.
    const Token &next = peek(ahead + 1);
    if (next.kind == Token::Kind::Name && !is_keyword(next.text))
        return true;
    if (next.is_punctuation("*"))
        return true;
    if (next.is_punctuation("(") && peek(ahead + 2).is_punctuation("*"))
        return true;
    return false;
}

TypePtr Parser::build_base(const BaseSpelling &spelling, const Where &where)
{
    if (spelling.core == BaseSpelling::Core::Void)
        return types_.void_type();
    if (spelling.core == BaseSpelling::Core::Bool)
        return types_.integer(1, false);
    if (spelling.core == BaseSpelling::Core::Float)
        return types_.floating(4);
    if (spelling.core == BaseSpelling::Core::Double)
        // A long double is compiled as a double: no target here carries a
        // wider one that the rest of the compiler could work with.
        return types_.floating(8);
    if (spelling.core == BaseSpelling::Core::Char) {
        // Plain `char` is signed here, matching the platforms this compiler
        // targets. `signed char` and `unsigned char` say so themselves.
        bool is_signed = !spelling.is_unsigned;
        return types_.integer(1, is_signed);
    }
    int width = 4;
    if (spelling.shorts > 0)
        width = 2;
    if (spelling.longs > 0)
        width = 8;
    if (spelling.shorts > 0 && spelling.longs > 0)
        fail(where, "error: a type cannot be both short and long");
    return types_.integer(width, !spelling.is_unsigned);
}

TypePtr Parser::parse_struct(bool is_union)
{
    Where where = peek().where;
    take();
    skip_ignorable();
    std::string tag;
    if (peek().kind == Token::Kind::Name && !is_keyword(peek().text))
        tag = take().text;
    std::string key = (is_union ? "union " : "struct ") + tag;

    if (!peek().is_punctuation("{")) {
        if (tag.empty()) {
            fail(where, "error: a struct written without a name has to have a body");
            return types_.integer(4, true);
        }
        TypePtr existing = types_.named(key);
        if (existing != nullptr)
            return existing;
        // A reference to a struct that has not been defined yet. It gets an
        // empty body so the name resolves; a later definition replaces it.
        TypePtr placeholder = types_.structure(tag, std::vector<Type::Member>());
        types_.define_name(key, placeholder);
        return placeholder;
    }

    if (is_union) {
        fail(where, "error: unions are not supported: every member would have to share an "
                    "offset, which this compiler's layout does not express");
    }

    take();
    std::vector<Type::Member> members;
    while (!peek().is_punctuation("}") && !at_end()) {
        Specifiers specifiers = parse_specifiers();
        if (!specifiers.present) {
            fail(peek().where, "error: expected a member declaration but found " +
                                   describe(peek()));
            recover_to_semicolon();
            continue;
        }
        if (eat_punctuation(";"))
            continue;
        for (;;) {
            std::string name;
            Build build = parse_declarator(name, nullptr);
            Type::Member member;
            member.name = name;
            member.type = build(specifiers.type);
            if (eat_punctuation(":")) {
                ExpressionPtr width = parse_conditional();
                (void)width;
                warn(where, "a bitfield is laid out as a whole member, which is not what "
                            "the source asks for");
            }
            skip_trailing_decoration();
            if (name.empty())
                fail(peek().where, "error: a struct member has to be named");
            members.push_back(member);
            if (!eat_punctuation(","))
                break;
        }
        expect_punctuation(";");
    }
    expect_punctuation("}");

    TypePtr type = types_.structure(tag, std::move(members));
    if (!tag.empty())
        types_.define_name(key, type);
    return type;
}

TypePtr Parser::parse_enum()
{
    take();
    skip_ignorable();
    std::string tag;
    if (peek().kind == Token::Kind::Name && !is_keyword(peek().text))
        tag = take().text;
    if (eat_punctuation("{")) {
        uint64_t next_value = 0;
        while (!peek().is_punctuation("}") && !at_end()) {
            if (peek().kind != Token::Kind::Name) {
                fail(peek().where, "error: expected an enumeration constant but found " +
                                       describe(peek()));
                break;
            }
            std::string name = take().text;
            if (eat_punctuation("=")) {
                ExpressionPtr value = parse_conditional();
                uint64_t folded = 0;
                if (!fold_constant(value.get(), folded))
                    fail(value->where, "error: an enumeration constant has to be a constant");
                next_value = folded;
            }
            enumerators_[name] = next_value;
            ++next_value;
            if (!eat_punctuation(","))
                break;
        }
        expect_punctuation("}");
    }
    (void)tag;
    return types_.integer(4, true);
}

Parser::Specifiers Parser::parse_specifiers()
{
    Specifiers specifiers;
    specifiers.where = peek().where;
    BaseSpelling spelling;
    bool saw_base_keyword = false;
    TypePtr named_type = nullptr;

    for (;;) {
        if (skip_ignorable()) {
            specifiers.present = true;
            continue;
        }
        const Token &token = peek();
        if (token.kind != Token::Kind::Name)
            break;
        const std::string &text = token.text;

        if (text == "typedef") {
            specifiers.is_typedef = true;
        } else if (text == "extern") {
            specifiers.is_extern = true;
        } else if (text == "static") {
            specifiers.is_static = true;
        } else if (text == "auto" || text == "register" || text == "inline" ||
                   text == "_Noreturn" || text == "_Thread_local" || text == "const" ||
                   text == "volatile" || text == "restrict") {
            // Accepted and dropped: none of them changes the bytes produced.
        } else if (text == "_Atomic") {
            warn(token.where, "_Atomic is accepted but the access it asks for is ordinary");
        } else if (text == "void") {
            spelling.core = BaseSpelling::Core::Void;
            saw_base_keyword = true;
        } else if (text == "char") {
            spelling.core = BaseSpelling::Core::Char;
            saw_base_keyword = true;
        } else if (text == "int") {
            if (spelling.core == BaseSpelling::Core::None)
                spelling.core = BaseSpelling::Core::Int;
            saw_base_keyword = true;
        } else if (text == "float") {
            spelling.core = BaseSpelling::Core::Float;
            saw_base_keyword = true;
        } else if (text == "double") {
            spelling.core = BaseSpelling::Core::Double;
            saw_base_keyword = true;
        } else if (text == "_Bool" || (text == "bool" && named_type == nullptr &&
                                       types_.named("bool") == nullptr)) {
            spelling.core = BaseSpelling::Core::Bool;
            saw_base_keyword = true;
        } else if (text == "short") {
            ++spelling.shorts;
            saw_base_keyword = true;
        } else if (text == "long") {
            ++spelling.longs;
            saw_base_keyword = true;
        } else if (text == "signed") {
            spelling.is_signed = true;
            saw_base_keyword = true;
        } else if (text == "unsigned") {
            spelling.is_unsigned = true;
            saw_base_keyword = true;
        } else if (text == "struct" || text == "union") {
            if (named_type != nullptr || saw_base_keyword)
                break;
            named_type = parse_struct(text == "union");
            specifiers.present = true;
            continue;
        } else if (text == "enum") {
            if (named_type != nullptr || saw_base_keyword)
                break;
            named_type = parse_enum();
            specifiers.present = true;
            continue;
        } else if (!is_keyword(text) && named_type == nullptr && !saw_base_keyword &&
                   types_.named(text) != nullptr) {
            named_type = types_.named(text);
        } else {
            break;
        }
        take();
        specifiers.present = true;
    }

    if (named_type != nullptr)
        specifiers.type = named_type;
    else if (saw_base_keyword)
        specifiers.type = build_base(spelling, specifiers.where);
    else if (specifiers.present)
        // A declaration with storage but no type: C89 says int, and code that
        // relies on it is old but real.
        specifiers.type = types_.integer(4, true);

    return specifiers;
}

// ---------------------------------------------------------------- declarators

void Parser::skip_trailing_decoration()
{
    // `__asm__("name")` and attributes may follow a declarator. They say where
    // a symbol lives or how a compiler should treat it, neither of which
    // changes what the declaration means here.
    skip_ignorable();
}

Parser::Build Parser::parse_declarator(std::string &name, std::vector<Variable> *parameters)
{
    if (eat_punctuation("*")) {
        // Qualifiers on the pointer itself, as in `char *const p`.
        for (;;) {
            if (skip_ignorable())
                continue;
            if (eat_name("const") || eat_name("volatile") || eat_name("restrict") ||
                eat_name("_Atomic"))
                continue;
            break;
        }
        Build inner = parse_declarator(name, parameters);
        TypeStore *types = &types_;
        return [inner, types](TypePtr base) { return inner(types->pointer_to(base)); };
    }
    return parse_direct_declarator(name, parameters);
}

Parser::Build Parser::parse_direct_declarator(std::string &name,
                                              std::vector<Variable> *parameters)
{
    TypeStore *types = &types_;
    Build core = [](TypePtr type) { return type; };

    skip_ignorable();
    // A '(' here is either a declarator in parentheses or the parameter list of
    // an abstract function type. Only the first can start with '*' or with a
    // name that does not name a type.
    bool parenthesised_declarator = false;
    if (peek().is_punctuation("(")) {
        const Token &next = peek(1);
        if (next.is_punctuation("*") || next.is_punctuation("("))
            parenthesised_declarator = true;
        else if (next.kind == Token::Kind::Name && !is_type_name_start(1))
            parenthesised_declarator = true;
    }

    if (parenthesised_declarator) {
        take();
        core = parse_declarator(name, parameters);
        expect_punctuation(")");
    } else if (peek().kind == Token::Kind::Name && !is_keyword(peek().text) &&
               !is_ignorable_specifier(peek().text)) {
        name = take().text;
    }

    std::vector<Build> postfixes;
    for (;;) {
        if (peek().is_punctuation("[")) {
            take();
            // `const` inside a parameter's array bounds is allowed and ignored.
            while (eat_name("const") || eat_name("volatile") || eat_name("restrict") ||
                   eat_name("static"))
                ;
            uint64_t count = 0;
            if (!peek().is_punctuation("]")) {
                ExpressionPtr size = parse_conditional();
                if (!fold_constant(size.get(), count)) {
                    fail(size->where, "error: an array length has to be a constant");
                    count = 0;
                }
            }
            expect_punctuation("]");
            postfixes.push_back(
                [types, count](TypePtr type) { return types->array_of(type, count); });
            continue;
        }
        if (peek().is_punctuation("(")) {
            std::vector<TypePtr> parameter_types;
            std::vector<Variable> named;
            bool variadic = false;
            parse_parameters(parameter_types, named, variadic);
            if (parameters != nullptr && parameters->empty())
                *parameters = named;
            postfixes.push_back([types, parameter_types, variadic](TypePtr type) {
                return types->function(type, parameter_types, variadic);
            });
            continue;
        }
        break;
    }

    skip_trailing_decoration();

    // The postfixes bind tighter the further left they are, so the rightmost
    // one wraps the base first: `int a[3][4]` is three arrays of four ints.
    Build wrapped = [postfixes](TypePtr base) {
        TypePtr type = base;
        for (size_t i = postfixes.size(); i-- > 0;)
            type = postfixes[i](type);
        return type;
    };
    return [core, wrapped](TypePtr base) { return core(wrapped(base)); };
}

void Parser::parse_parameters(std::vector<TypePtr> &parameter_types,
                              std::vector<Variable> &named, bool &variadic)
{
    expect_punctuation("(");
    if (eat_punctuation(")")) {
        // An empty list says nothing about the parameters rather than saying
        // there are none, so calls through it are not checked. The emitter
        // writes this for anything whose signature it could not recover.
        variadic = true;
        return;
    }
    if (peek().is_name("void") && peek(1).is_punctuation(")")) {
        take();
        take();
        return;
    }
    for (;;) {
        if (eat_punctuation("...")) {
            variadic = true;
            break;
        }
        Specifiers specifiers = parse_specifiers();
        if (!specifiers.present) {
            fail(peek().where,
                 "error: expected a parameter type but found " + describe(peek()));
            break;
        }
        Variable variable;
        variable.where = peek().where;
        std::string name;
        Build build = parse_declarator(name, nullptr);
        TypePtr type = build(specifiers.type);
        // A parameter declared as an array or a function is really a pointer.
        if (type != nullptr && type->kind == Type::Kind::Array)
            type = types_.pointer_to(type->target);
        else if (type != nullptr && type->kind == Type::Kind::Function)
            type = types_.pointer_to(type);
        variable.name = name;
        variable.type = type;
        parameter_types.push_back(type);
        named.push_back(std::move(variable));
        if (!eat_punctuation(","))
            break;
    }
    expect_punctuation(")");
}

TypePtr Parser::parse_type_name()
{
    Specifiers specifiers = parse_specifiers();
    if (!specifiers.present)
        return nullptr;
    std::string name;
    Build build = parse_declarator(name, nullptr);
    if (!name.empty())
        fail(specifiers.where, "error: a type name cannot declare '" + name + "'");
    return build(specifiers.type);
}

// ---------------------------------------------------------------- constants

bool Parser::fold_constant(const Expression *expression, uint64_t &value)
{
    if (expression == nullptr)
        return false;
    switch (expression->kind) {
    case Expression::Kind::IntegerLiteral:
        value = expression->integer_value;
        return true;
    case Expression::Kind::Cast:
        return fold_constant(expression->left.get(), value);
    case Expression::Kind::Unary: {
        uint64_t operand = 0;
        if (!fold_constant(expression->left.get(), operand))
            return false;
        switch (expression->unary_op) {
        case UnaryOp::Plus: value = operand; return true;
        case UnaryOp::Minus: value = static_cast<uint64_t>(-static_cast<int64_t>(operand));
            return true;
        case UnaryOp::Not: value = operand == 0 ? 1 : 0; return true;
        case UnaryOp::BitNot: value = ~operand; return true;
        default: return false;
        }
    }
    case Expression::Kind::SizeOf:
        if (expression->named_type != nullptr) {
            value = TypeStore::size_of(expression->named_type);
            return true;
        }
        return false;
    case Expression::Kind::Conditional: {
        uint64_t test = 0;
        if (!fold_constant(expression->left.get(), test))
            return false;
        return fold_constant(test != 0 ? expression->right.get() : expression->third.get(),
                             value);
    }
    case Expression::Kind::Binary: {
        uint64_t left = 0;
        uint64_t right = 0;
        if (!fold_constant(expression->left.get(), left) ||
            !fold_constant(expression->right.get(), right))
            return false;
        int64_t signed_left = static_cast<int64_t>(left);
        int64_t signed_right = static_cast<int64_t>(right);
        switch (expression->binary_op) {
        case BinaryOp::Add: value = left + right; return true;
        case BinaryOp::Subtract: value = left - right; return true;
        case BinaryOp::Multiply: value = left * right; return true;
        case BinaryOp::Divide:
            if (right == 0)
                return false;
            value = static_cast<uint64_t>(signed_left / signed_right);
            return true;
        case BinaryOp::Modulo:
            if (right == 0)
                return false;
            value = static_cast<uint64_t>(signed_left % signed_right);
            return true;
        case BinaryOp::ShiftLeft: value = right >= 64 ? 0 : left << right; return true;
        case BinaryOp::ShiftRight: value = right >= 64 ? 0 : left >> right; return true;
        case BinaryOp::BitAnd: value = left & right; return true;
        case BinaryOp::BitOr: value = left | right; return true;
        case BinaryOp::BitXor: value = left ^ right; return true;
        case BinaryOp::Less: value = signed_left < signed_right; return true;
        case BinaryOp::LessEqual: value = signed_left <= signed_right; return true;
        case BinaryOp::Greater: value = signed_left > signed_right; return true;
        case BinaryOp::GreaterEqual: value = signed_left >= signed_right; return true;
        case BinaryOp::Equal: value = left == right; return true;
        case BinaryOp::NotEqual: value = left != right; return true;
        case BinaryOp::LogicalAnd: value = (left != 0 && right != 0) ? 1 : 0; return true;
        case BinaryOp::LogicalOr: value = (left != 0 || right != 0) ? 1 : 0; return true;
        case BinaryOp::Comma: value = right; return true;
        }
        return false;
    }
    default:
        return false;
    }
}

// ---------------------------------------------------------------- expressions

ExpressionPtr Parser::parse_primary()
{
    Where where = peek().where;
    const Token &token = peek();

    switch (token.kind) {
    case Token::Kind::Integer: {
        take();
        ExpressionPtr expression = make_expression(Expression::Kind::IntegerLiteral, where);
        expression->integer_value = token.integer_value;
        expression->type = types_.integer(token.width, !token.is_unsigned);
        return expression;
    }
    case Token::Kind::Character: {
        take();
        ExpressionPtr expression = make_expression(Expression::Kind::IntegerLiteral, where);
        expression->integer_value = token.integer_value;
        expression->type = types_.integer(4, true);
        return expression;
    }
    case Token::Kind::Float: {
        take();
        ExpressionPtr expression = make_expression(Expression::Kind::FloatLiteral, where);
        expression->float_value = token.float_value;
        expression->type = types_.floating(token.width);
        return expression;
    }
    case Token::Kind::String: {
        ExpressionPtr expression = make_expression(Expression::Kind::StringLiteral, where);
        // Literals written next to each other are one literal.
        while (peek().kind == Token::Kind::String)
            expression->text += take().bytes;
        expression->type =
            types_.array_of(types_.integer(1, true), expression->text.size() + 1);
        return expression;
    }
    default:
        break;
    }

    if (token.kind == Token::Kind::Name && !is_keyword(token.text)) {
        std::map<std::string, uint64_t>::const_iterator found = enumerators_.find(token.text);
        if (found != enumerators_.end()) {
            take();
            ExpressionPtr expression = make_expression(Expression::Kind::IntegerLiteral, where);
            expression->integer_value = found->second;
            expression->type = types_.integer(4, true);
            return expression;
        }
        // These are macros in the headers the emitted file includes, and the
        // preprocessor lines that define them were dropped.
        if (token.text == "true" || token.text == "false" || token.text == "NULL") {
            take();
            ExpressionPtr expression = make_expression(Expression::Kind::IntegerLiteral, where);
            expression->integer_value = token.text == "true" ? 1 : 0;
            expression->type = token.text == "NULL" ? types_.pointer_to(types_.void_type())
                                                    : types_.integer(4, true);
            return expression;
        }
        take();
        ExpressionPtr expression = make_expression(Expression::Kind::Name, where);
        expression->name = token.text;
        return expression;
    }

    if (eat_punctuation("(")) {
        ExpressionPtr inner = parse_expression();
        expect_punctuation(")");
        return inner;
    }

    fail(where, "error: expected a value but found " + describe(token));
    if (!at_end())
        take();
    ExpressionPtr broken = make_expression(Expression::Kind::IntegerLiteral, where);
    broken->type = types_.integer(4, true);
    return broken;
}

ExpressionPtr Parser::parse_postfix(ExpressionPtr base)
{
    for (;;) {
        Where where = peek().where;
        if (eat_punctuation("[")) {
            ExpressionPtr index = make_expression(Expression::Kind::Index, where);
            index->left = std::move(base);
            index->right = parse_expression();
            expect_punctuation("]");
            base = std::move(index);
            continue;
        }
        if (peek().is_punctuation("(")) {
            take();
            ExpressionPtr call = make_expression(Expression::Kind::Call, where);
            call->callee = std::move(base);
            if (!peek().is_punctuation(")")) {
                for (;;) {
                    call->arguments.push_back(parse_assignment());
                    if (!eat_punctuation(","))
                        break;
                }
            }
            expect_punctuation(")");
            base = std::move(call);
            continue;
        }
        if (peek().is_punctuation(".") || peek().is_punctuation("->")) {
            bool through_pointer = peek().text == "->";
            take();
            ExpressionPtr member = make_expression(Expression::Kind::Member, where);
            member->through_pointer = through_pointer;
            member->left = std::move(base);
            if (peek().kind != Token::Kind::Name)
                fail(peek().where, "error: expected a member name but found " + describe(peek()));
            else
                member->name = take().text;
            base = std::move(member);
            continue;
        }
        if (peek().is_punctuation("++") || peek().is_punctuation("--")) {
            bool increment = peek().text == "++";
            take();
            ExpressionPtr unary = make_expression(Expression::Kind::Unary, where);
            unary->unary_op = increment ? UnaryOp::PostIncrement : UnaryOp::PostDecrement;
            unary->left = std::move(base);
            base = std::move(unary);
            continue;
        }
        break;
    }
    return base;
}

ExpressionPtr Parser::parse_unary()
{
    Where where = peek().where;

    if (peek().is_punctuation("++") || peek().is_punctuation("--")) {
        bool increment = peek().text == "++";
        take();
        ExpressionPtr unary = make_expression(Expression::Kind::Unary, where);
        unary->unary_op = increment ? UnaryOp::PreIncrement : UnaryOp::PreDecrement;
        unary->left = parse_unary();
        return unary;
    }

    static const struct {
        const char *text;
        UnaryOp op;
    } PREFIXES[] = {
        {"+", UnaryOp::Plus},        {"-", UnaryOp::Minus},
        {"!", UnaryOp::Not},         {"~", UnaryOp::BitNot},
        {"*", UnaryOp::Dereference}, {"&", UnaryOp::AddressOf},
    };
    for (const auto &prefix : PREFIXES) {
        if (!peek().is_punctuation(prefix.text))
            continue;
        take();
        ExpressionPtr unary = make_expression(Expression::Kind::Unary, where);
        unary->unary_op = prefix.op;
        unary->left = parse_cast_expression();
        return unary;
    }

    if (peek().is_name("sizeof") || peek().is_name("_Alignof")) {
        bool alignment = peek().text == "_Alignof";
        take();
        ExpressionPtr size = make_expression(Expression::Kind::SizeOf, where);
        if (peek().is_punctuation("(") && is_type_name_start(1)) {
            take();
            size->named_type = parse_type_name();
            expect_punctuation(")");
        } else {
            size->left = parse_unary();
        }
        size->type = types_.integer(8, false);
        if (alignment)
            warn(where, "_Alignof is read as sizeof, which is only the same for the types "
                        "this compiler knows about");
        return size;
    }

    return parse_postfix(parse_primary());
}

ExpressionPtr Parser::parse_cast_expression()
{
    if (peek().is_punctuation("(") && is_type_name_start(1)) {
        Where where = peek().where;
        size_t saved = at_;
        take();
        TypePtr type = parse_type_name();
        if (type != nullptr && eat_punctuation(")")) {
            // `(int){1}` is a compound literal, not a cast. Nothing here
            // produces one, so the token stream is rewound and it is read as
            // whatever it looks like instead.
            if (!peek().is_punctuation("{")) {
                ExpressionPtr cast = make_expression(Expression::Kind::Cast, where);
                cast->named_type = type;
                cast->left = parse_cast_expression();
                return cast;
            }
        }
        at_ = saved;
    }
    return parse_unary();
}

// Every binary level, loosest first. Each entry is one precedence step, and
// they are all left-associative, which is what makes one loop enough.
struct BinaryLevel {
    const char *text;
    BinaryOp op;
    int level;
};

static const BinaryLevel BINARY_LEVELS[] = {
    {"||", BinaryOp::LogicalOr, 0},
    {"&&", BinaryOp::LogicalAnd, 1},
    {"|", BinaryOp::BitOr, 2},
    {"^", BinaryOp::BitXor, 3},
    {"&", BinaryOp::BitAnd, 4},
    {"==", BinaryOp::Equal, 5},
    {"!=", BinaryOp::NotEqual, 5},
    {"<", BinaryOp::Less, 6},
    {">", BinaryOp::Greater, 6},
    {"<=", BinaryOp::LessEqual, 6},
    {">=", BinaryOp::GreaterEqual, 6},
    {"<<", BinaryOp::ShiftLeft, 7},
    {">>", BinaryOp::ShiftRight, 7},
    {"+", BinaryOp::Add, 8},
    {"-", BinaryOp::Subtract, 8},
    {"*", BinaryOp::Multiply, 9},
    {"/", BinaryOp::Divide, 9},
    {"%", BinaryOp::Modulo, 9},
};
static const int BINARY_LEVEL_COUNT = 10;

ExpressionPtr Parser::parse_binary(int level)
{
    if (level >= BINARY_LEVEL_COUNT)
        return parse_cast_expression();

    ExpressionPtr left = parse_binary(level + 1);
    for (;;) {
        const BinaryLevel *found = nullptr;
        for (const BinaryLevel &candidate : BINARY_LEVELS)
            if (candidate.level == level && peek().is_punctuation(candidate.text)) {
                found = &candidate;
                break;
            }
        if (found == nullptr)
            break;
        Where where = peek().where;
        take();
        ExpressionPtr binary = make_expression(Expression::Kind::Binary, where);
        binary->binary_op = found->op;
        binary->left = std::move(left);
        binary->right = parse_binary(level + 1);
        left = std::move(binary);
    }
    return left;
}

ExpressionPtr Parser::parse_conditional()
{
    ExpressionPtr test = parse_binary(0);
    if (!peek().is_punctuation("?"))
        return test;
    Where where = peek().where;
    take();
    ExpressionPtr conditional = make_expression(Expression::Kind::Conditional, where);
    conditional->left = std::move(test);
    conditional->right = parse_expression();
    expect_punctuation(":");
    conditional->third = parse_conditional();
    return conditional;
}

ExpressionPtr Parser::parse_assignment()
{
    ExpressionPtr left = parse_conditional();

    static const struct {
        const char *text;
        BinaryOp op;
    } ASSIGNMENTS[] = {
        {"=", BinaryOp::Comma},        {"+=", BinaryOp::Add},
        {"-=", BinaryOp::Subtract},    {"*=", BinaryOp::Multiply},
        {"/=", BinaryOp::Divide},      {"%=", BinaryOp::Modulo},
        {"<<=", BinaryOp::ShiftLeft},  {">>=", BinaryOp::ShiftRight},
        {"&=", BinaryOp::BitAnd},      {"|=", BinaryOp::BitOr},
        {"^=", BinaryOp::BitXor},
    };
    for (const auto &candidate : ASSIGNMENTS) {
        if (!peek().is_punctuation(candidate.text))
            continue;
        Where where = peek().where;
        take();
        ExpressionPtr assign = make_expression(Expression::Kind::Assign, where);
        assign->assign_op = candidate.op;
        assign->left = std::move(left);
        // Assignment groups to the right: `a = b = c` sets b then a.
        assign->right = parse_assignment();
        return assign;
    }
    return left;
}

ExpressionPtr Parser::parse_expression()
{
    ExpressionPtr left = parse_assignment();
    while (peek().is_punctuation(",")) {
        Where where = peek().where;
        take();
        ExpressionPtr comma = make_expression(Expression::Kind::Binary, where);
        comma->binary_op = BinaryOp::Comma;
        comma->left = std::move(left);
        comma->right = parse_assignment();
        left = std::move(comma);
    }
    return left;
}

ExpressionPtr Parser::parse_initialiser()
{
    if (!peek().is_punctuation("{"))
        return parse_assignment();

    Where where = peek().where;
    take();
    ExpressionPtr list = make_expression(Expression::Kind::InitialiserList, where);
    while (!peek().is_punctuation("}") && !at_end()) {
        std::string designator;
        // A designator names which element is being set. It is read so the
        // source is accepted, and kept on the element for whoever wants it.
        while (peek().is_punctuation(".") || peek().is_punctuation("[")) {
            if (eat_punctuation(".")) {
                if (peek().kind == Token::Kind::Name)
                    designator = take().text;
            } else {
                take();
                ExpressionPtr index = parse_conditional();
                uint64_t value = 0;
                if (fold_constant(index.get(), value))
                    designator = std::to_string(value);
                expect_punctuation("]");
            }
        }
        if (!designator.empty())
            expect_punctuation("=");
        ExpressionPtr element = parse_initialiser();
        element->name = designator;
        list->arguments.push_back(std::move(element));
        if (!eat_punctuation(","))
            break;
    }
    expect_punctuation("}");
    return list;
}

// ---------------------------------------------------------------- statements

void Parser::recover_to_semicolon()
{
    // Step past whatever could not be read, so one mistake costs one
    // declaration rather than the rest of the file.
    int depth = 0;
    while (!at_end()) {
        if (peek().is_punctuation("{"))
            ++depth;
        else if (peek().is_punctuation("}")) {
            if (depth == 0)
                return;
            --depth;
        } else if (peek().is_punctuation(";") && depth == 0) {
            take();
            return;
        }
        take();
    }
}

StatementPtr Parser::parse_declaration_statement()
{
    Where where = peek().where;
    Specifiers specifiers = parse_specifiers();
    StatementPtr statement = make_statement(Statement::Kind::Declaration, where);

    if (eat_punctuation(";"))
        return statement;

    for (;;) {
        Variable variable;
        variable.where = peek().where;
        std::string name;
        Build build = parse_declarator(name, nullptr);
        variable.name = name;
        variable.type = build(specifiers.type);
        if (eat_punctuation("="))
            variable.initialiser = parse_initialiser();
        // `char text[] = "..."` takes its length from what it holds.
        if (variable.type != nullptr && variable.type->kind == Type::Kind::Array &&
            variable.type->count == 0 && variable.initialiser) {
            if (variable.initialiser->kind == Expression::Kind::StringLiteral)
                variable.type = types_.array_of(variable.type->target,
                                                variable.initialiser->text.size() + 1);
            else if (variable.initialiser->kind == Expression::Kind::InitialiserList)
                variable.type = types_.array_of(variable.type->target,
                                                variable.initialiser->arguments.size());
        }
        if (specifiers.is_typedef)
            types_.define_name(name, variable.type);
        else
            statement->variables.push_back(std::move(variable));
        if (!eat_punctuation(","))
            break;
    }
    expect_punctuation(";");
    return statement;
}

StatementPtr Parser::parse_compound()
{
    Where where = peek().where;
    StatementPtr compound = make_statement(Statement::Kind::Compound, where);
    if (!expect_punctuation("{"))
        return compound;
    while (!peek().is_punctuation("}") && !at_end()) {
        size_t before = at_;
        StatementPtr statement = parse_statement();
        if (statement)
            compound->body.push_back(std::move(statement));
        if (at_ == before) {
            // Nothing was consumed, which would otherwise spin here forever.
            take();
        }
    }
    expect_punctuation("}");
    return compound;
}

StatementPtr Parser::parse_statement()
{
    Where where = peek().where;

    if (peek().is_punctuation("{"))
        return parse_compound();

    if (eat_punctuation(";"))
        return make_statement(Statement::Kind::Empty, where);

    if (peek().kind == Token::Kind::Name) {
        const std::string &text = peek().text;

        if (text == "if") {
            take();
            StatementPtr statement = make_statement(Statement::Kind::If, where);
            expect_punctuation("(");
            statement->value = parse_expression();
            expect_punctuation(")");
            statement->then_branch = parse_statement();
            if (eat_name("else"))
                statement->else_branch = parse_statement();
            return statement;
        }
        if (text == "while") {
            take();
            StatementPtr statement = make_statement(Statement::Kind::While, where);
            expect_punctuation("(");
            statement->value = parse_expression();
            expect_punctuation(")");
            statement->then_branch = parse_statement();
            return statement;
        }
        if (text == "do") {
            take();
            StatementPtr statement = make_statement(Statement::Kind::DoWhile, where);
            statement->then_branch = parse_statement();
            if (!eat_name("while"))
                fail(peek().where, "error: a do body has to be followed by while");
            expect_punctuation("(");
            statement->value = parse_expression();
            expect_punctuation(")");
            expect_punctuation(";");
            return statement;
        }
        if (text == "for") {
            take();
            StatementPtr statement = make_statement(Statement::Kind::For, where);
            expect_punctuation("(");
            if (peek().is_punctuation(";")) {
                take();
            } else if (is_declaration_start()) {
                statement->initialiser = parse_declaration_statement();
            } else {
                StatementPtr first = make_statement(Statement::Kind::Expression, peek().where);
                first->value = parse_expression();
                expect_punctuation(";");
                statement->initialiser = std::move(first);
            }
            if (!peek().is_punctuation(";"))
                statement->condition = parse_expression();
            expect_punctuation(";");
            if (!peek().is_punctuation(")"))
                statement->step = parse_expression();
            expect_punctuation(")");
            statement->then_branch = parse_statement();
            return statement;
        }
        if (text == "switch") {
            take();
            StatementPtr statement = make_statement(Statement::Kind::Switch, where);
            expect_punctuation("(");
            statement->value = parse_expression();
            expect_punctuation(")");
            statement->then_branch = parse_statement();
            return statement;
        }
        if (text == "case") {
            take();
            StatementPtr statement = make_statement(Statement::Kind::Case, where);
            ExpressionPtr value = parse_conditional();
            uint64_t folded = 0;
            if (!fold_constant(value.get(), folded))
                fail(value->where, "error: a case label has to be a constant");
            statement->case_value = folded;
            expect_punctuation(":");
            statement->then_branch = parse_statement();
            return statement;
        }
        if (text == "default") {
            take();
            StatementPtr statement = make_statement(Statement::Kind::Default, where);
            expect_punctuation(":");
            statement->then_branch = parse_statement();
            return statement;
        }
        if (text == "goto") {
            take();
            StatementPtr statement = make_statement(Statement::Kind::Goto, where);
            if (peek().kind != Token::Kind::Name)
                fail(peek().where, "error: goto has to name a label");
            else
                statement->name = take().text;
            expect_punctuation(";");
            return statement;
        }
        if (text == "break") {
            take();
            expect_punctuation(";");
            return make_statement(Statement::Kind::Break, where);
        }
        if (text == "continue") {
            take();
            expect_punctuation(";");
            return make_statement(Statement::Kind::Continue, where);
        }
        if (text == "return") {
            take();
            StatementPtr statement = make_statement(Statement::Kind::Return, where);
            if (!peek().is_punctuation(";"))
                statement->value = parse_expression();
            expect_punctuation(";");
            return statement;
        }
        // A label, which is the one place a bare name is followed by a colon.
        if (!is_keyword(text) && peek(1).is_punctuation(":")) {
            StatementPtr statement = make_statement(Statement::Kind::Label, where);
            statement->name = take().text;
            take();
            // A label at the end of a block has nothing after it, which C23
            // allows and the emitter writes.
            if (peek().is_punctuation("}"))
                statement->then_branch = make_statement(Statement::Kind::Empty, peek().where);
            else
                statement->then_branch = parse_statement();
            return statement;
        }
    }

    if (is_declaration_start())
        return parse_declaration_statement();

    StatementPtr statement = make_statement(Statement::Kind::Expression, where);
    statement->value = parse_expression();
    expect_punctuation(";");
    return statement;
}

// ---------------------------------------------------------------- top level

bool Parser::parse_external_declaration(Unit &unit)
{
    Where where = peek().where;
    Specifiers specifiers = parse_specifiers();
    if (!specifiers.present) {
        fail(where, "error: expected a declaration but found " + describe(peek()));
        recover_to_semicolon();
        return false;
    }
    if (eat_punctuation(";"))
        return true;

    bool first = true;
    for (;;) {
        std::string name;
        std::vector<Variable> parameters;
        Build build = parse_declarator(name, &parameters);
        TypePtr type = build(specifiers.type);

        if (name.empty()) {
            fail(where, "error: this declaration does not name anything");
            recover_to_semicolon();
            return false;
        }

        if (specifiers.is_typedef) {
            types_.define_name(name, type);
        } else if (type != nullptr && type->kind == Type::Kind::Function) {
            Function function;
            function.name = name;
            function.type = type;
            function.parameters = std::move(parameters);
            function.where = where;
            if (first && peek().is_punctuation("{")) {
                function.body = parse_compound();
                unit.functions.push_back(std::move(function));
                return true;
            }
            unit.functions.push_back(std::move(function));
        } else {
            Variable variable;
            variable.name = name;
            variable.type = type;
            variable.where = where;
            if (eat_punctuation("="))
                variable.initialiser = parse_initialiser();
            if (variable.type != nullptr && variable.type->kind == Type::Kind::Array &&
                variable.type->count == 0 && variable.initialiser) {
                if (variable.initialiser->kind == Expression::Kind::StringLiteral)
                    variable.type = types_.array_of(variable.type->target,
                                                    variable.initialiser->text.size() + 1);
                else if (variable.initialiser->kind == Expression::Kind::InitialiserList)
                    variable.type = types_.array_of(variable.type->target,
                                                    variable.initialiser->arguments.size());
            }
            if (specifiers.is_extern && !variable.initialiser) {
                // Declared, not defined: it belongs to the program already.
                External external;
                external.name = variable.name;
                external.type = variable.type;
                external.is_function = false;
                unit.externals.push_back(external);
            } else {
                unit.globals.push_back(std::move(variable));
            }
        }

        first = false;
        if (!eat_punctuation(","))
            break;
    }
    expect_punctuation(";");
    return true;
}

bool Parser::run(Unit &unit)
{
    while (!at_end()) {
        size_t before = at_;
        parse_external_declaration(unit);
        if (at_ == before)
            take();
    }
    return errors_ == 0;
}

} // namespace

bool parse(const std::vector<Token> &tokens, TypeStore &types, Unit &unit,
           std::vector<Diagnostic> &diagnostics)
{
    Parser parser(tokens, types, diagnostics);
    return parser.run(unit);
}

} // namespace compiler
} // namespace astral_internal
