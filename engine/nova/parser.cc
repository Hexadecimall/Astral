#include "parser.hh"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace astral_internal {
namespace nova {
namespace {

class Parser {
public:
    Parser(const std::vector<Token> &tokens, Types &types, Unit &unit,
           std::vector<Diagnostic> &diagnostics)
        : tokens_(tokens), types_(types), unit_(unit), diagnostics_(diagnostics)
    {
    }

    bool run();

private:
    // ------------------------------------------------------------ cursor
    const Token &peek(size_t ahead = 0) const
    {
        size_t index = at_ + ahead;
        return index < tokens_.size() ? tokens_[index] : tokens_.back();
    }
    const Token &take() { return tokens_[at_ < tokens_.size() - 1 ? at_++ : at_]; }
    bool at_end() const { return peek().is(Token::Kind::End); }
    bool accept(const char *punctuation);
    bool accept_word(const char *word);
    bool expect(const char *punctuation, const char *doing);
    bool expect_word(const char *word, const char *doing);
    void complain(const Where &where, const std::string &message);
    void warn(const Where &where, const std::string &message);
    // Skips to the next likely statement or item start after a mistake, so
    // one bad line does not turn the rest of the file into noise.
    void recover();
    std::string take_documentation();

    // ------------------------------------------------------------ items
    bool parse_item();
    bool parse_import();
    bool parse_function(const std::string &documentation);
    bool parse_global(const std::string &documentation);
    bool parse_record(const std::string &documentation);
    bool parse_enumeration(const std::string &documentation);
    bool parse_parameters(Function &function);

    // ------------------------------------------------------------ types
    // Reads a type, or returns null with a complaint. `optional` allows the
    // absence of one, which is how level 1 leaves a parameter untyped.
    TypePtr parse_type(bool optional = false);
    bool parse_storage(Storage &storage);
    bool at_type_start() const;

    // ------------------------------------------------------------ statements
    StatementPtr parse_statement();
    StatementPtr parse_statement_body();
    StatementPtr parse_block();
    // The body of an if, a loop or an arm. Braces are how Nova is written and
    // what the formatter puts back, but a single statement is accepted so that
    // `if (done) goto end;` reads, which is how a recovered branch out of a
    // structure comes across.
    StatementPtr parse_body();
    StatementPtr parse_declaration(bool is_stack);
    StatementPtr parse_if();
    StatementPtr parse_while();
    StatementPtr parse_do_while();
    StatementPtr parse_loop();
    StatementPtr parse_for();
    StatementPtr parse_match();
    StatementPtr parse_asm();
    StatementPtr parse_return();
    StatementPtr parse_call();
    bool parse_variable(Variable &variable, bool is_stack, bool allow_untyped);

    // ------------------------------------------------------------ expressions
    ExpressionPtr parse_expression();
    ExpressionPtr parse_assignment();
    ExpressionPtr parse_conditional();
    ExpressionPtr parse_binary(int precedence);
    ExpressionPtr parse_as();
    ExpressionPtr parse_unary();
    ExpressionPtr parse_postfix();
    ExpressionPtr parse_primary();
    ExpressionPtr parse_list();
    ExpressionPtr make(Expression::Kind kind, const Where &where);

    const std::vector<Token> &tokens_;
    Types &types_;
    Unit &unit_;
    std::vector<Diagnostic> &diagnostics_;
    size_t at_ = 0;
    bool failed_ = false;
};

// ------------------------------------------------------------------ cursor

bool Parser::accept(const char *punctuation)
{
    if (!peek().is_punctuation(punctuation))
        return false;
    take();
    return true;
}

bool Parser::accept_word(const char *word)
{
    if (!peek().is_name(word))
        return false;
    take();
    return true;
}

bool Parser::expect(const char *punctuation, const char *doing)
{
    if (accept(punctuation))
        return true;
    const Token &got = peek();
    std::string seen = got.is(Token::Kind::End) ? "the end of the file" : "'" + got.text + "'";
    complain(got.where, std::string("expected '") + punctuation + "' " + doing + ", found " + seen);
    return false;
}

bool Parser::expect_word(const char *word, const char *doing)
{
    if (accept_word(word))
        return true;
    const Token &got = peek();
    std::string seen = got.is(Token::Kind::End) ? "the end of the file" : "'" + got.text + "'";
    complain(got.where, std::string("expected '") + word + "' " + doing + ", found " + seen);
    return false;
}

void Parser::complain(const Where &where, const std::string &message)
{
    failed_ = true;
    diagnostics_.push_back(Diagnostic{where.line, where.column, message});
}

void Parser::warn(const Where &where, const std::string &message)
{
    diagnostics_.push_back(Diagnostic{where.line, where.column, "warning: " + message});
}

void Parser::recover()
{
    int depth = 0;
    while (!at_end()) {
        const Token &token = peek();
        if (token.is_punctuation("{"))
            ++depth;
        if (token.is_punctuation("}")) {
            if (depth == 0)
                return;
            --depth;
        }
        if (depth == 0 && token.is_punctuation(";")) {
            take();
            return;
        }
        if (depth == 0
            && (token.is_name("func") || token.is_name("struct") || token.is_name("enum")
                || token.is_name("import")))
            return;
        take();
    }
}

std::string Parser::take_documentation()
{
    std::string text;
    while (peek().is(Token::Kind::Documentation)) {
        if (!text.empty())
            text += "\n";
        text += take().text;
    }
    return text;
}

// ------------------------------------------------------------------- items

bool Parser::run()
{
    while (!at_end()) {
        size_t before = at_;
        if (!parse_item())
            recover();
        // A parse that consumed nothing would loop forever on the same token.
        if (at_ == before && !at_end())
            take();
    }
    return !failed_;
}

bool Parser::parse_item()
{
    std::string documentation = take_documentation();
    const Token &token = peek();
    if (token.is_name("import"))
        return parse_import();
    if (token.is_name("func"))
        return parse_function(documentation);
    if (token.is_name("var") || token.is_name("val"))
        return parse_global(documentation);
    if (token.is_name("struct") || token.is_name("class"))
        return parse_record(documentation);
    if (token.is_name("enum"))
        return parse_enumeration(documentation);
    if (token.is(Token::Kind::End))
        return true;
    if (token.is_punctuation(";")) {
        take();
        return true;
    }
    complain(token.where, "expected func, var, val, struct, enum or import at the top of the file, found '"
                              + token.text + "'");
    return false;
}

bool Parser::parse_import()
{
    take();
    if (!peek().is(Token::Kind::Name)) {
        complain(peek().where, "import needs a name");
        return false;
    }
    std::string name = take().text;
    // Fusion writes `dev-kit`; the tokeniser sees a minus. Join it back.
    while (accept("-")) {
        if (!peek().is(Token::Kind::Name))
            break;
        name += "-" + take().text;
    }
    unit_.imports.push_back(name);
    accept(";");
    return true;
}

// func name [@ address] ( parameters ) [: type [@ storage]] { body }
bool Parser::parse_function(const std::string &documentation)
{
    Function function;
    function.where = take().where;
    function.documentation = documentation;

    if (!peek().is(Token::Kind::Name) || is_keyword(peek().text)) {
        complain(peek().where, "func needs a name");
        return false;
    }
    function.name = take().text;

    if (peek().is_punctuation("@") && peek(1).is(Token::Kind::Integer)) {
        take();
        function.has_address = true;
        function.address = take().integer_value;
    }

    if (peek().is_punctuation("(")) {
        if (!parse_parameters(function))
            return false;
    }

    if (accept(":")) {
        // `: @w0` is a return whose storage is known and whose type is not.
        if (!peek().is_punctuation("@")) {
            function.result = parse_type();
            if (!function.result)
                return false;
        }
        if (peek().is_punctuation("@") && !parse_storage(function.result_storage))
            return false;
    } else {
        function.result = types_.void_type();
    }
    // A level-1 return with storage and no type is still a value of some
    // width; the checker sizes it from the register.
    if (!function.result && function.result_storage.is_none())
        function.result = types_.void_type();

    if (peek().is_punctuation("{")) {
        function.body = parse_block();
        if (!function.body)
            return false;
    } else {
        accept(";");
    }

    function.level = level_of(function);
    unit_.functions.push_back(std::move(function));
    return true;
}

// ( name: type @ storage, @ x1, name, ... )
bool Parser::parse_parameters(Function &function)
{
    expect("(", "to begin the parameters");
    if (accept(")"))
        return true;
    for (;;) {
        if (accept("..")) {
            // `...` reads as `..` `.`; either spelling means variadic.
            accept(".");
            function.variadic = true;
            break;
        }
        Variable parameter;
        parameter.where = peek().where;
        if (peek().is_punctuation("@")) {
            // Storage only. The parameter is named after its register, which
            // is what level 1 calls it anyway.
            if (!parse_storage(parameter.storage))
                return false;
            parameter.name = parameter.storage.register_name;
            if (parameter.name.empty()) {
                complain(parameter.where, "a parameter with only storage has to be in a register");
                return false;
            }
        } else {
            if (!peek().is(Token::Kind::Name)) {
                complain(peek().where, "expected a parameter name");
                return false;
            }
            parameter.name = take().text;
            if (accept(":")) {
                parameter.type = parse_type();
                if (!parameter.type)
                    return false;
            }
            if (peek().is_punctuation("@") && !parse_storage(parameter.storage))
                return false;
        }
        function.parameters.push_back(std::move(parameter));
        if (accept(","))
            continue;
        break;
    }
    return expect(")", "to end the parameters");
}

bool Parser::parse_global(const std::string &documentation)
{
    Variable global;
    global.is_constant = peek().is_name("val");
    global.where = take().where;
    (void)documentation;
    if (!parse_variable(global, false, false))
        return false;
    expect(";", "after the declaration");
    unit_.globals.push_back(std::move(global));
    return true;
}

// struct Name [@ address] { var member: type [@ offset]; ... }
bool Parser::parse_record(const std::string &documentation)
{
    Record record;
    record.is_class = peek().is_name("class");
    const std::string kind = record.is_class ? "class" : "struct";
    record.where = take().where;
    record.documentation = documentation;
    if (!peek().is(Token::Kind::Name)) {
        complain(peek().where, kind + " needs a name");
        return false;
    }
    record.name = take().text;
    if (peek().is_punctuation("@") && peek(1).is(Token::Kind::Integer)) {
        take();
        record.has_address = true;
        record.address = take().integer_value;
    }
    if (!expect("{", record.is_class ? "to begin the class" : "to begin the struct"))
        return false;

    // The name has to exist before the members are read, or a member that
    // points at its own struct cannot be spelled.
    std::vector<compiler::Type::Member> laid;
    TypePtr placeholder = types_.store().structure(record.name, {});
    types_.define_name(record.name, placeholder);

    uint64_t next_offset = 0;
    while (!accept("}")) {
        if (at_end()) {
            complain(record.where, kind + " " + record.name + " was never closed");
            return false;
        }
        const std::string inner = take_documentation();
        // A function written inside a class belongs to it. It is lifted out
        // under `Class::name` and compiled like any other function, so nothing
        // downstream has to know classes exist; and its first parameter, when
        // it is called self and given no type, is a pointer to the class.
        if (peek().is_name("func")) {
            if (!record.is_class) {
                complain(peek().where,
                         "a struct holds data; write it as a class to give it functions");
                return false;
            }
            const size_t before = unit_.functions.size();
            if (!parse_function(inner))
                return false;
            if (unit_.functions.size() != before + 1)
                return false;
            Function &method = unit_.functions.back();
            if (!method.parameters.empty() && method.parameters.front().name == "self" &&
                method.parameters.front().type == nullptr)
                method.parameters.front().type = types_.store().pointer_to(placeholder);
            method.name = record.name + "::" + method.name;
            record.methods.push_back(method.name);
            continue;
        }
        Record::Member member;
        member.where = peek().where;
        // `var` is what Fusion writes; a bare name is allowed too.
        accept_word("var");
        accept_word("val");
        if (!peek().is(Token::Kind::Name)) {
            complain(peek().where, "expected a member name in " + kind + " " + record.name);
            return false;
        }
        member.name = take().text;
        if (!expect(":", "after the member name"))
            return false;
        member.type = parse_type();
        if (!member.type)
            return false;
        if (accept("@")) {
            if (!peek().is(Token::Kind::Integer)) {
                complain(peek().where, "a member offset has to be a number");
                return false;
            }
            member.offset = take().integer_value;
            member.offset_given = true;
        } else {
            // Without an offset, members follow each other with natural
            // alignment, which is what a compiler would have done.
            uint64_t align = compiler::TypeStore::align_of(member.type);
            if (align > 1)
                next_offset = (next_offset + align - 1) / align * align;
            member.offset = next_offset;
        }
        uint64_t end = member.offset + compiler::TypeStore::size_of(member.type);
        if (end > next_offset)
            next_offset = end;
        accept(";");
        laid.push_back(compiler::Type::Member{member.name, member.type, member.offset});
        record.members.push_back(std::move(member));
    }

    // The store owns the placeholder; fill it in place so every pointer
    // handed out while reading the members stays right.
    const_cast<compiler::Type *>(placeholder)->members = std::move(laid);
    unit_.records.push_back(std::move(record));
    return true;
}

bool Parser::parse_enumeration(const std::string &documentation)
{
    Enumeration enumeration;
    enumeration.where = take().where;
    enumeration.documentation = documentation;
    if (!peek().is(Token::Kind::Name)) {
        complain(peek().where, "enum needs a name");
        return false;
    }
    enumeration.name = take().text;
    // An enumeration is a 32-bit integer with names for some of its values.
    types_.define_name(enumeration.name, types_.integer(4, true));
    if (!expect("{", "to begin the enum"))
        return false;
    uint64_t next = 0;
    while (!accept("}")) {
        if (at_end()) {
            complain(enumeration.where, "enum " + enumeration.name + " was never closed");
            return false;
        }
        Enumeration::Constant constant;
        constant.where = peek().where;
        if (!peek().is(Token::Kind::Name)) {
            complain(peek().where, "expected a name in enum " + enumeration.name);
            return false;
        }
        constant.name = take().text;
        if (accept("=")) {
            if (!peek().is(Token::Kind::Integer)) {
                complain(peek().where, "an enum value has to be a number");
                return false;
            }
            next = take().integer_value;
        }
        constant.value = next++;
        enumeration.constants.push_back(std::move(constant));
        if (!accept(","))
            accept(";");
    }
    unit_.enumerations.push_back(std::move(enumeration));
    return true;
}

// ------------------------------------------------------------------- types

bool Parser::at_type_start() const
{
    const Token &token = peek();
    if (token.is_punctuation("*") || token.is_punctuation("["))
        return true;
    return token.is(Token::Kind::Name) && types_.named(token.text) != nullptr;
}

// * type   [type; count]   name   func(types): type
TypePtr Parser::parse_type(bool optional)
{
    const Token &token = peek();
    if (token.is_punctuation("*")) {
        take();
        TypePtr target = parse_type();
        return target ? types_.pointer_to(target) : nullptr;
    }
    if (token.is_punctuation("[")) {
        take();
        TypePtr element = parse_type();
        if (!element)
            return nullptr;
        uint64_t count = 0;
        if (accept(";")) {
            if (!peek().is(Token::Kind::Integer)) {
                complain(peek().where, "an array length has to be a number");
                return nullptr;
            }
            count = take().integer_value;
        }
        if (!expect("]", "to end the array type"))
            return nullptr;
        return types_.array_of(element, count);
    }
    if (token.is_name("func")) {
        take();
        std::vector<TypePtr> parameters;
        bool variadic = false;
        if (accept("(")) {
            while (!accept(")")) {
                if (accept("..")) {
                    accept(".");
                    variadic = true;
                    continue;
                }
                TypePtr parameter = parse_type();
                if (!parameter)
                    return nullptr;
                parameters.push_back(parameter);
                accept(",");
            }
        }
        TypePtr result = types_.void_type();
        if (accept(":")) {
            result = parse_type();
            if (!result)
                return nullptr;
        }
        return types_.store().function(result, std::move(parameters), variadic);
    }
    if (token.is(Token::Kind::Name)) {
        TypePtr named = types_.named(token.text);
        if (named) {
            take();
            return named;
        }
        if (optional)
            return nullptr;
        complain(token.where, "'" + token.text + "' is not a type Nova knows");
        return nullptr;
    }
    if (optional)
        return nullptr;
    complain(token.where, "expected a type, found '" + token.text + "'");
    return nullptr;
}

// @ x0   @ 0x10000c0d0   @ -0x70   @ x29@entry
bool Parser::parse_storage(Storage &storage)
{
    Where where = peek().where;
    if (!expect("@", "before the storage"))
        return false;
    const Token &token = peek();
    if (token.is(Token::Kind::Integer)) {
        storage.kind = Storage::Kind::Address;
        storage.address = take().integer_value;
        return true;
    }
    if (token.is_punctuation("-") || token.is_punctuation("+")) {
        bool negative = take().text == "-";
        if (!peek().is(Token::Kind::Integer)) {
            complain(peek().where, "a frame offset has to be a number");
            return false;
        }
        int64_t offset = static_cast<int64_t>(take().integer_value);
        storage.kind = Storage::Kind::Frame;
        storage.offset = negative ? -offset : offset;
        return true;
    }
    if (token.is(Token::Kind::Name) && is_register_name(token.text)) {
        storage.kind = Storage::Kind::Register;
        storage.register_name = take().text;
        if (peek().is_punctuation("@") && peek(1).is_name("entry")) {
            take();
            take();
            storage.kind = Storage::Kind::EntryRegister;
        }
        return true;
    }
    complain(where, "after @ Nova expects a register, an address, or a frame offset like -0x70");
    return false;
}

// -------------------------------------------------------------- statements

StatementPtr Parser::parse_block()
{
    auto block = std::make_unique<Statement>();
    block->kind = Statement::Kind::Compound;
    block->where = peek().where;
    if (!expect("{", "to begin a block"))
        return nullptr;
    while (!accept("}")) {
        if (at_end()) {
            complain(block->where, "a block was never closed");
            return nullptr;
        }
        // A note with nothing after it belongs to the block itself.
        if (peek().is(Token::Kind::Documentation) && peek(1).is_punctuation("}")) {
            auto note = std::make_unique<Statement>();
            note->kind = Statement::Kind::Empty;
            note->where = peek().where;
            note->documentation = take().text;
            block->body.push_back(std::move(note));
            continue;
        }
        size_t before = at_;
        StatementPtr statement = parse_statement();
        if (statement)
            block->body.push_back(std::move(statement));
        else
            recover();
        if (at_ == before && !at_end())
            take();
    }
    return block;
}

StatementPtr Parser::parse_body()
{
    if (peek().is_punctuation("{"))
        return parse_block();
    StatementPtr one = parse_statement();
    if (!one)
        return nullptr;
    auto block = std::make_unique<Statement>();
    block->kind = Statement::Kind::Compound;
    block->where = one->where;
    block->body.push_back(std::move(one));
    return block;
}

StatementPtr Parser::parse_statement()
{
    std::string documentation = take_documentation();
    StatementPtr statement = parse_statement_body();
    if (statement && !documentation.empty())
        statement->documentation = documentation;
    return statement;
}

StatementPtr Parser::parse_statement_body()
{
    const Token &token = peek();
    if (token.is_punctuation("{"))
        return parse_block();
    if (token.is_name("var") || token.is_name("val"))
        return parse_declaration(false);
    if (token.is_name("stack"))
        return parse_declaration(true);
    if (token.is_name("if"))
        return parse_if();
    if (token.is_name("while"))
        return parse_while();
    if (token.is_name("do"))
        return parse_do_while();
    if (token.is_name("loop"))
        return parse_loop();
    if (token.is_name("for"))
        return parse_for();
    if (token.is_name("match"))
        return parse_match();
    if (token.is_name("asm"))
        return parse_asm();
    if (token.is_name("return"))
        return parse_return();
    if (token.is_name("call"))
        return parse_call();

    if (token.is_name("break") || token.is_name("continue")) {
        auto statement = std::make_unique<Statement>();
        statement->kind = token.is_name("break") ? Statement::Kind::Break : Statement::Kind::Continue;
        statement->where = take().where;
        expect(";", "after the statement");
        return statement;
    }
    if (token.is_name("goto")) {
        auto statement = std::make_unique<Statement>();
        statement->kind = Statement::Kind::Goto;
        statement->where = take().where;
        if (!peek().is(Token::Kind::Name)) {
            complain(peek().where, "goto needs a label");
            return nullptr;
        }
        statement->name = take().text;
        expect(";", "after the goto");
        return statement;
    }
    // `label name:` or `name:` both introduce a label.
    if (token.is_name("label") || (token.is(Token::Kind::Name) && peek(1).is_punctuation(":")
                                   && !is_keyword(token.text))) {
        auto statement = std::make_unique<Statement>();
        statement->kind = Statement::Kind::Label;
        statement->where = token.where;
        if (token.is_name("label"))
            take();
        statement->name = take().text;
        expect(":", "after the label");
        return statement;
    }
    if (accept(";")) {
        auto statement = std::make_unique<Statement>();
        statement->kind = Statement::Kind::Empty;
        statement->where = token.where;
        return statement;
    }

    auto statement = std::make_unique<Statement>();
    statement->kind = Statement::Kind::Expression;
    statement->where = token.where;
    statement->value = parse_expression();
    if (!statement->value)
        return nullptr;
    expect(";", "after the expression");
    return statement;
}

// name [: type] [@ storage] [= value]
bool Parser::parse_variable(Variable &variable, bool is_stack, bool allow_untyped)
{
    variable.is_stack = is_stack;
    if (!peek().is(Token::Kind::Name) || is_keyword(peek().text)) {
        complain(peek().where, "expected a name to declare");
        return false;
    }
    variable.name = take().text;
    if (accept(":")) {
        variable.type = parse_type();
        if (!variable.type)
            return false;
    }
    if (peek().is_punctuation("@") && !parse_storage(variable.storage))
        return false;
    if (accept("=")) {
        variable.initialiser = parse_expression();
        if (!variable.initialiser)
            return false;
    }
    if (!variable.type && !variable.initialiser && !allow_untyped
        && variable.storage.kind != Storage::Kind::Register) {
        complain(variable.where, variable.name + " needs a type or a value");
        return false;
    }
    return true;
}

StatementPtr Parser::parse_declaration(bool is_stack)
{
    auto statement = std::make_unique<Statement>();
    statement->kind = Statement::Kind::Declaration;
    const Token &word = take();
    statement->where = word.where;
    bool is_constant = word.is_name("val");
    for (;;) {
        Variable variable;
        variable.where = peek().where;
        variable.is_constant = is_constant;
        if (!parse_variable(variable, is_stack, false))
            return nullptr;
        statement->variables.push_back(std::move(variable));
        if (!accept(","))
            break;
    }
    expect(";", "after the declaration");
    return statement;
}

StatementPtr Parser::parse_if()
{
    auto statement = std::make_unique<Statement>();
    statement->kind = Statement::Kind::If;
    statement->where = take().where;
    if (!expect("(", "after if"))
        return nullptr;
    statement->value = parse_expression();
    if (!statement->value || !expect(")", "to close the condition"))
        return nullptr;
    statement->then_branch = parse_body();
    if (!statement->then_branch)
        return nullptr;
    if (accept_word("else")) {
        if (peek().is_name("if"))
            statement->else_branch = parse_if();
        else
            statement->else_branch = parse_body();
        if (!statement->else_branch)
            return nullptr;
    }
    return statement;
}

StatementPtr Parser::parse_while()
{
    auto statement = std::make_unique<Statement>();
    statement->kind = Statement::Kind::While;
    statement->where = take().where;
    if (!expect("(", "after while"))
        return nullptr;
    statement->value = parse_expression();
    if (!statement->value || !expect(")", "to close the condition"))
        return nullptr;
    statement->then_branch = parse_body();
    return statement->then_branch ? std::move(statement) : nullptr;
}

StatementPtr Parser::parse_do_while()
{
    auto statement = std::make_unique<Statement>();
    statement->kind = Statement::Kind::DoWhile;
    statement->where = take().where;
    statement->then_branch = parse_body();
    if (!statement->then_branch)
        return nullptr;
    if (!expect_word("while", "after the do block") || !expect("(", "after while"))
        return nullptr;
    statement->value = parse_expression();
    if (!statement->value || !expect(")", "to close the condition"))
        return nullptr;
    expect(";", "after the do-while");
    return statement;
}

StatementPtr Parser::parse_loop()
{
    auto statement = std::make_unique<Statement>();
    statement->kind = Statement::Kind::Loop;
    statement->where = take().where;
    statement->then_branch = parse_body();
    return statement->then_branch ? std::move(statement) : nullptr;
}

// for (name in subject) { }
StatementPtr Parser::parse_for()
{
    auto statement = std::make_unique<Statement>();
    statement->kind = Statement::Kind::ForIn;
    statement->where = take().where;
    if (!expect("(", "after for"))
        return nullptr;
    if (!peek().is(Token::Kind::Name)) {
        complain(peek().where, "for needs a name to bind");
        return nullptr;
    }
    statement->name = take().text;
    if (!expect_word("in", "after the loop variable"))
        return nullptr;
    statement->subject = parse_expression();
    if (!statement->subject || !expect(")", "to close the for"))
        return nullptr;
    statement->then_branch = parse_body();
    return statement->then_branch ? std::move(statement) : nullptr;
}

// match (subject) { value or value { } else { } }
StatementPtr Parser::parse_match()
{
    auto statement = std::make_unique<Statement>();
    statement->kind = Statement::Kind::Match;
    statement->where = take().where;
    if (!expect("(", "after match"))
        return nullptr;
    statement->value = parse_expression();
    if (!statement->value || !expect(")", "to close the subject"))
        return nullptr;
    if (!expect("{", "to begin the arms"))
        return nullptr;
    while (!accept("}")) {
        if (at_end()) {
            complain(statement->where, "a match was never closed");
            return nullptr;
        }
        MatchArm arm;
        arm.where = peek().where;
        if (!accept_word("else")) {
            for (;;) {
                ExpressionPtr value = parse_conditional();
                if (!value)
                    return nullptr;
                arm.values.push_back(std::move(value));
                if (accept_word("or") || accept("|") || accept(","))
                    continue;
                break;
            }
        }
        arm.body = parse_block();
        if (!arm.body)
            return nullptr;
        statement->arms.push_back(std::move(arm));
    }
    return statement;
}

// asm { instructions } - the tokeniser hands the body over as one token with
// the text exactly as written. Lines are trimmed and blank ones dropped, and
// that is all; the assembler is the one that reads it.
StatementPtr Parser::parse_asm()
{
    auto statement = std::make_unique<Statement>();
    statement->kind = Statement::Kind::Asm;
    statement->where = take().where;
    if (!peek().is(Token::Kind::Assembly)) {
        complain(peek().where, "asm needs a { } block after it");
        return nullptr;
    }
    const std::string &raw = take().text;
    std::string text;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t end = raw.find('\n', start);
        if (end == std::string::npos)
            end = raw.size();
        size_t first = start;
        while (first < end && std::isspace(static_cast<unsigned char>(raw[first])))
            ++first;
        size_t last = end;
        while (last > first && std::isspace(static_cast<unsigned char>(raw[last - 1])))
            --last;
        if (last > first)
            text += raw.substr(first, last - first) + "\n";
        if (end == raw.size())
            break;
        start = end + 1;
    }
    statement->assembly = text;
    return statement;
}

StatementPtr Parser::parse_return()
{
    auto statement = std::make_unique<Statement>();
    statement->kind = Statement::Kind::Return;
    statement->where = take().where;
    if (!accept(";")) {
        statement->value = parse_expression();
        if (!statement->value)
            return nullptr;
        expect(";", "after the return value");
    }
    return statement;
}

// call target;  - a call whose result nobody has named. Level 1 writes this
// when the return register is read directly afterwards.
StatementPtr Parser::parse_call()
{
    auto statement = std::make_unique<Statement>();
    statement->kind = Statement::Kind::Expression;
    statement->where = take().where;
    ExpressionPtr callee = parse_postfix();
    if (!callee)
        return nullptr;
    if (callee->kind == Expression::Kind::Call) {
        statement->value = std::move(callee);
    } else {
        ExpressionPtr call = make(Expression::Kind::Call, statement->where);
        call->callee = std::move(callee);
        statement->value = std::move(call);
    }
    expect(";", "after the call");
    return statement;
}

// ------------------------------------------------------------- expressions

ExpressionPtr Parser::make(Expression::Kind kind, const Where &where)
{
    auto expression = std::make_unique<Expression>();
    expression->kind = kind;
    expression->where = where;
    return expression;
}

// A comma joins two things done in order and answers with the second. The
// decompiler writes conditions like `(x = f(a), x == 0)` constantly, so this
// is not a corner of the language but most of what a recovered branch looks
// like. Call arguments are read by parse_assignment and never reach here, so
// a comma between them still separates rather than joins.
ExpressionPtr Parser::parse_expression()
{
    ExpressionPtr left = parse_assignment();
    if (!left)
        return nullptr;
    while (peek().is_punctuation(",")) {
        Where where = take().where;
        ExpressionPtr right = parse_assignment();
        if (!right)
            return nullptr;
        ExpressionPtr joined = make(Expression::Kind::Binary, where);
        joined->binary_op = BinaryOp::Comma;
        joined->left = std::move(left);
        joined->right = std::move(right);
        left = std::move(joined);
    }
    return left;
}

ExpressionPtr Parser::parse_assignment()
{
    ExpressionPtr left = parse_conditional();
    if (!left)
        return nullptr;
    const Token &token = peek();
    struct Compound {
        const char *spelling;
        BinaryOp op;
    };
    static const Compound compounds[] = {
        {"=", BinaryOp::Comma},       {"+=", BinaryOp::Add},       {"-=", BinaryOp::Subtract},
        {"*=", BinaryOp::Multiply},   {"/=", BinaryOp::Divide},    {"%=", BinaryOp::Modulo},
        {"&=", BinaryOp::BitAnd},     {"|=", BinaryOp::BitOr},     {"^=", BinaryOp::BitXor},
        {"<<=", BinaryOp::ShiftLeft}, {">>=", BinaryOp::ShiftRight},
    };
    for (const Compound &compound : compounds) {
        if (!token.is_punctuation(compound.spelling))
            continue;
        Where where = take().where;
        ExpressionPtr right = parse_assignment();
        if (!right)
            return nullptr;
        ExpressionPtr assign = make(Expression::Kind::Assign, where);
        assign->binary_op = compound.op;
        assign->left = std::move(left);
        assign->right = std::move(right);
        return assign;
    }
    // A range is only meaningful as the subject of a for, but it parses here
    // so the for does not need its own expression grammar.
    if (token.is_punctuation("..") || token.is_punctuation("..=")) {
        bool inclusive = token.is_punctuation("..=");
        Where where = take().where;
        ExpressionPtr right = parse_conditional();
        if (!right)
            return nullptr;
        ExpressionPtr range = make(Expression::Kind::Range, where);
        range->inclusive = inclusive;
        range->left = std::move(left);
        range->right = std::move(right);
        return range;
    }
    return left;
}

ExpressionPtr Parser::parse_conditional()
{
    ExpressionPtr condition = parse_binary(0);
    if (!condition)
        return nullptr;
    if (!peek().is_punctuation("?"))
        return condition;
    Where where = take().where;
    ExpressionPtr then = parse_expression();
    if (!then || !expect(":", "in the conditional"))
        return nullptr;
    ExpressionPtr otherwise = parse_conditional();
    if (!otherwise)
        return nullptr;
    ExpressionPtr result = make(Expression::Kind::Conditional, where);
    result->left = std::move(condition);
    result->right = std::move(then);
    result->third = std::move(otherwise);
    return result;
}

namespace {
struct Operator {
    const char *spelling;
    BinaryOp op;
    int precedence;
};
// Highest number binds tightest. The order is C's, which Fusion keeps.
const Operator kOperators[] = {
    {"||", BinaryOp::LogicalOr, 1},
    {"&&", BinaryOp::LogicalAnd, 2},
    {"|", BinaryOp::BitOr, 3},
    {"^", BinaryOp::BitXor, 4},
    {"&", BinaryOp::BitAnd, 5},
    {"==", BinaryOp::Equal, 6},
    {"!=", BinaryOp::NotEqual, 6},
    {"<", BinaryOp::Less, 7},
    {"<=", BinaryOp::LessEqual, 7},
    {">", BinaryOp::Greater, 7},
    {">=", BinaryOp::GreaterEqual, 7},
    {"<<", BinaryOp::ShiftLeft, 8},
    {">>", BinaryOp::ShiftRight, 8},
    {"+", BinaryOp::Add, 9},
    {"-", BinaryOp::Subtract, 9},
    {"*", BinaryOp::Multiply, 10},
    {"/", BinaryOp::Divide, 10},
    {"%", BinaryOp::Modulo, 10},
};
} // namespace

ExpressionPtr Parser::parse_binary(int precedence)
{
    ExpressionPtr left = parse_as();
    if (!left)
        return nullptr;
    for (;;) {
        const Operator *found = nullptr;
        for (const Operator &candidate : kOperators) {
            if (peek().is_punctuation(candidate.spelling) && candidate.precedence > precedence) {
                found = &candidate;
                break;
            }
        }
        if (!found)
            return left;
        Where where = take().where;
        ExpressionPtr right = parse_binary(found->precedence);
        if (!right)
            return nullptr;
        ExpressionPtr binary = make(Expression::Kind::Binary, where);
        binary->binary_op = found->op;
        binary->left = std::move(left);
        binary->right = std::move(right);
        left = std::move(binary);
    }
}

// value as type - binds tighter than any binary operator and looser than
// unary, which is what makes `-x as u32` mean negate first.
ExpressionPtr Parser::parse_as()
{
    ExpressionPtr value = parse_unary();
    if (!value)
        return nullptr;
    while (peek().is_name("as")) {
        Where where = take().where;
        TypePtr type = parse_type();
        if (!type)
            return nullptr;
        ExpressionPtr cast = make(Expression::Kind::As, where);
        cast->named_type = type;
        cast->left = std::move(value);
        value = std::move(cast);
    }
    return value;
}

ExpressionPtr Parser::parse_unary()
{
    const Token &token = peek();
    struct Prefix {
        const char *spelling;
        UnaryOp op;
    };
    static const Prefix prefixes[] = {
        {"!", UnaryOp::Not},        {"~", UnaryOp::BitNot},      {"-", UnaryOp::Minus},
        {"+", UnaryOp::Plus},       {"*", UnaryOp::Dereference}, {"&", UnaryOp::AddressOf},
    };
    for (const Prefix &prefix : prefixes) {
        if (!token.is_punctuation(prefix.spelling))
            continue;
        Where where = take().where;
        ExpressionPtr operand = parse_unary();
        if (!operand)
            return nullptr;
        ExpressionPtr unary = make(Expression::Kind::Unary, where);
        unary->unary_op = prefix.op;
        unary->left = std::move(operand);
        return unary;
    }
    if (token.is_name("sizeof")) {
        Where where = take().where;
        ExpressionPtr result = make(Expression::Kind::SizeOf, where);
        bool parenthesised = accept("(");
        if (at_type_start()) {
            result->named_type = parse_type();
            if (!result->named_type)
                return nullptr;
        } else {
            result->left = parse_unary();
            if (!result->left)
                return nullptr;
        }
        if (parenthesised && !expect(")", "to close sizeof"))
            return nullptr;
        return result;
    }
    return parse_postfix();
}

ExpressionPtr Parser::parse_postfix()
{
    ExpressionPtr value = parse_primary();
    if (!value)
        return nullptr;
    for (;;) {
        const Token &token = peek();
        if (token.is_punctuation("(")) {
            Where where = take().where;
            ExpressionPtr call = make(Expression::Kind::Call, where);
            call->callee = std::move(value);
            while (!accept(")")) {
                if (at_end()) {
                    complain(where, "a call was never closed");
                    return nullptr;
                }
                ExpressionPtr argument = parse_assignment();
                if (!argument)
                    return nullptr;
                call->arguments.push_back(std::move(argument));
                if (!accept(",") && !peek().is_punctuation(")")) {
                    complain(peek().where, "expected ',' or ')' in the call");
                    return nullptr;
                }
            }
            value = std::move(call);
            continue;
        }
        if (token.is_punctuation("[")) {
            Where where = take().where;
            ExpressionPtr index = parse_expression();
            if (!index || !expect("]", "to close the index"))
                return nullptr;
            ExpressionPtr result = make(Expression::Kind::Index, where);
            result->left = std::move(value);
            result->right = std::move(index);
            value = std::move(result);
            continue;
        }
        if (token.is_punctuation(".") || token.is_punctuation("->")) {
            bool through_pointer = token.is_punctuation("->");
            Where where = take().where;
            if (!peek().is(Token::Kind::Name)) {
                complain(peek().where, "expected a member name");
                return nullptr;
            }
            ExpressionPtr member = make(Expression::Kind::Member, where);
            member->name = take().text;
            member->through_pointer = through_pointer;
            member->left = std::move(value);
            value = std::move(member);
            continue;
        }
        // w22@entry: the register's value on entry.
        if (token.is_punctuation("@") && peek(1).is_name("entry")
            && (value->kind == Expression::Kind::Register || value->kind == Expression::Kind::Name)
            && is_register_name(value->name)) {
            take();
            take();
            value->kind = Expression::Kind::Register;
            value->entry_value = true;
            continue;
        }
        return value;
    }
}

ExpressionPtr Parser::parse_list()
{
    Where where = take().where;
    ExpressionPtr list = make(Expression::Kind::List, where);
    while (!accept("]") && !accept("}")) {
        if (at_end()) {
            complain(where, "a list was never closed");
            return nullptr;
        }
        ExpressionPtr element = parse_assignment();
        if (!element)
            return nullptr;
        list->arguments.push_back(std::move(element));
        accept(",");
    }
    return list;
}

ExpressionPtr Parser::parse_primary()
{
    const Token &token = peek();
    switch (token.kind) {
    case Token::Kind::Integer:
    case Token::Kind::Character: {
        ExpressionPtr literal = make(Expression::Kind::IntegerLiteral, token.where);
        literal->integer_value = token.integer_value;
        literal->type = types_.integer(token.width, !token.is_unsigned);
        take();
        return literal;
    }
    case Token::Kind::Float: {
        ExpressionPtr literal = make(Expression::Kind::FloatLiteral, token.where);
        literal->float_value = token.float_value;
        literal->type = types_.floating(8);
        take();
        return literal;
    }
    case Token::Kind::String: {
        ExpressionPtr literal = make(Expression::Kind::StringLiteral, token.where);
        literal->text = token.bytes;
        take();
        // Adjacent strings join, as they do in C and Fusion.
        while (peek().is(Token::Kind::String))
            literal->text += take().bytes;
        return literal;
    }
    case Token::Kind::Name: {
        if (token.is_name("true") || token.is_name("false")) {
            ExpressionPtr literal = make(Expression::Kind::BooleanLiteral, token.where);
            literal->boolean_value = token.is_name("true");
            literal->type = types_.boolean();
            take();
            return literal;
        }
        if (token.is_name("null")) {
            ExpressionPtr literal = make(Expression::Kind::NullLiteral, token.where);
            take();
            return literal;
        }
        if (is_keyword(token.text) && !token.is_name("call")) {
            complain(token.where, "'" + token.text + "' is a keyword and cannot be a value here");
            return nullptr;
        }
        ExpressionPtr name = make(Expression::Kind::Name, token.where);
        name->name = token.text;
        take();
        // `Parser::peek` is one name, spelled to say where it came from. The
        // function it refers to is the one the class wrote under that name.
        while (peek().is_punctuation("::") && peek(1).is(Token::Kind::Name)) {
            take();
            name->name += "::" + take().text;
        }
        return name;
    }
    case Token::Kind::Punctuation:
        if (token.is_punctuation("(")) {
            take();
            ExpressionPtr inner = parse_expression();
            if (!inner || !expect(")", "to close the parenthesis"))
                return nullptr;
            return inner;
        }
        if (token.is_punctuation("[") || token.is_punctuation("{"))
            return parse_list();
        complain(token.where, "expected a value, found '" + token.text + "'");
        return nullptr;
    case Token::Kind::Documentation:
        take();
        return parse_primary();
    case Token::Kind::Assembly:
        complain(token.where, "an asm block is a statement, not a value");
        return nullptr;
    case Token::Kind::End:
        complain(token.where, "expected a value, found the end of the file");
        return nullptr;
    }
    return nullptr;
}

// ------------------------------------------------------------------ levels

bool mentions_storage(const Statement &statement);

bool mentions_register(const Expression &expression)
{
    if (expression.kind == Expression::Kind::Register)
        return true;
    if (expression.kind == Expression::Kind::Name && is_register_name(expression.name))
        return true;
    if (expression.callee && mentions_register(*expression.callee))
        return true;
    for (const ExpressionPtr &argument : expression.arguments)
        if (argument && mentions_register(*argument))
            return true;
    if (expression.left && mentions_register(*expression.left))
        return true;
    if (expression.right && mentions_register(*expression.right))
        return true;
    if (expression.third && mentions_register(*expression.third))
        return true;
    return false;
}

bool mentions_storage(const Statement &statement)
{
    for (const Variable &variable : statement.variables)
        if (!variable.storage.is_none())
            return true;
    if (statement.value && mentions_register(*statement.value))
        return true;
    if (statement.subject && mentions_register(*statement.subject))
        return true;
    for (const StatementPtr &child : statement.body)
        if (child && mentions_storage(*child))
            return true;
    if (statement.then_branch && mentions_storage(*statement.then_branch))
        return true;
    if (statement.else_branch && mentions_storage(*statement.else_branch))
        return true;
    for (const MatchArm &arm : statement.arms)
        if (arm.body && mentions_storage(*arm.body))
            return true;
    return false;
}

bool only_assembly(const Statement &statement)
{
    if (statement.kind == Statement::Kind::Asm || statement.kind == Statement::Kind::Empty)
        return true;
    if (statement.kind != Statement::Kind::Compound)
        return false;
    bool any = false;
    for (const StatementPtr &child : statement.body) {
        if (!child || !only_assembly(*child))
            return false;
        if (child->kind == Statement::Kind::Asm)
            any = true;
    }
    return any;
}

} // namespace

Level level_of(const Function &function)
{
    if (function.body && only_assembly(*function.body))
        return Level::Machine;

    bool storage = !function.result_storage.is_none();
    bool untyped = false;
    for (const Variable &parameter : function.parameters) {
        if (!parameter.storage.is_none())
            storage = true;
        if (!parameter.type)
            untyped = true;
    }
    if (!function.result && !function.result_storage.is_none())
        untyped = true;
    if (function.body && mentions_storage(*function.body))
        storage = true;

    if (untyped)
        return Level::Storage;
    if (storage)
        return Level::Typed;
    return Level::Source;
}

bool parse(const std::vector<Token> &tokens, Types &types, Unit &unit,
           std::vector<Diagnostic> &diagnostics)
{
    Parser parser(tokens, types, unit, diagnostics);
    return parser.run();
}

} // namespace nova
} // namespace astral_internal
