#include "format.hh"

#include <cstdio>

namespace astral_internal {
namespace nova {
namespace {

std::string hex(uint64_t value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

// A string literal, escaped the way the tokeniser will read it back.
std::string quote(const std::string &bytes)
{
    std::string out = "\"";
    for (unsigned char c : bytes) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\0': out += "\\0"; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default:
            if (c < 0x20 || c >= 0x7f) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\x%02x", c);
                out += buffer;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

int precedence_of(BinaryOp op)
{
    switch (op) {
    case BinaryOp::LogicalOr: return 1;
    case BinaryOp::LogicalAnd: return 2;
    case BinaryOp::BitOr: return 3;
    case BinaryOp::BitXor: return 4;
    case BinaryOp::BitAnd: return 5;
    case BinaryOp::Equal:
    case BinaryOp::NotEqual: return 6;
    case BinaryOp::Less:
    case BinaryOp::LessEqual:
    case BinaryOp::Greater:
    case BinaryOp::GreaterEqual: return 7;
    case BinaryOp::ShiftLeft:
    case BinaryOp::ShiftRight: return 8;
    case BinaryOp::Add:
    case BinaryOp::Subtract: return 9;
    case BinaryOp::Multiply:
    case BinaryOp::Divide:
    case BinaryOp::Modulo: return 10;
    case BinaryOp::Comma: return 0;
    }
    return 0;
}

const char *spelling_of(BinaryOp op)
{
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Subtract: return "-";
    case BinaryOp::Multiply: return "*";
    case BinaryOp::Divide: return "/";
    case BinaryOp::Modulo: return "%";
    case BinaryOp::ShiftLeft: return "<<";
    case BinaryOp::ShiftRight: return ">>";
    case BinaryOp::Less: return "<";
    case BinaryOp::LessEqual: return "<=";
    case BinaryOp::Greater: return ">";
    case BinaryOp::GreaterEqual: return ">=";
    case BinaryOp::Equal: return "==";
    case BinaryOp::NotEqual: return "!=";
    case BinaryOp::BitAnd: return "&";
    case BinaryOp::BitOr: return "|";
    case BinaryOp::BitXor: return "^";
    case BinaryOp::LogicalAnd: return "&&";
    case BinaryOp::LogicalOr: return "||";
    case BinaryOp::Comma: return ",";
    }
    return "?";
}

const char *spelling_of(UnaryOp op)
{
    switch (op) {
    case UnaryOp::Plus: return "+";
    case UnaryOp::Minus: return "-";
    case UnaryOp::Not: return "!";
    case UnaryOp::BitNot: return "~";
    case UnaryOp::Dereference: return "*";
    case UnaryOp::AddressOf: return "&";
    case UnaryOp::PreIncrement:
    case UnaryOp::PostIncrement: return "++";
    case UnaryOp::PreDecrement:
    case UnaryOp::PostDecrement: return "--";
    }
    return "?";
}

// Whether an expression yields a truth value. Those get parentheses whenever
// they stand as a whole value, which is the rule that makes
// `return (strcmp(string, "astral") == 0);` the only way to write it.
bool is_truth(const Expression &expression)
{
    if (expression.kind == Expression::Kind::Binary) {
        switch (expression.binary_op) {
        case BinaryOp::Less:
        case BinaryOp::LessEqual:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEqual:
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
        case BinaryOp::LogicalAnd:
        case BinaryOp::LogicalOr:
            return true;
        default:
            return false;
        }
    }
    return false;
}

class Writer {
public:
    Writer(const Types &types) : types_(types) {}

    void unit(const Unit &unit);
    void function(const Function &function);
    void expression(const Expression &expression, int parent_precedence = 0);
    std::string take() { return std::move(out_); }

private:
    void line(const std::string &text);
    void indent() { out_ += std::string(static_cast<size_t>(depth_) * 4, ' '); }
    void documentation(const std::string &text);
    void variable(const Variable &variable, const char *word);
    void statement(const Statement &statement);
    void block(const Statement &statement);
    // A whole value: an initialiser, a return, the right side of an
    // assignment. Truth values are wrapped here.
    void value(const Expression &expression);

    const Types &types_;
    std::string out_;
    int depth_ = 0;
};

void Writer::line(const std::string &text)
{
    indent();
    out_ += text;
    out_ += "\n";
}

void Writer::documentation(const std::string &text)
{
    if (text.empty())
        return;
    indent();
    out_ += "#/" + text + "#\\\n";
}

void Writer::unit(const Unit &unit)
{
    for (const std::string &import : unit.imports)
        line("import " + import + ";");
    if (!unit.imports.empty())
        out_ += "\n";

    for (const Enumeration &enumeration : unit.enumerations) {
        documentation(enumeration.documentation);
        line("enum " + enumeration.name + " {");
        ++depth_;
        uint64_t expected = 0;
        for (const Enumeration::Constant &constant : enumeration.constants) {
            std::string text = constant.name;
            if (constant.value != expected)
                text += " = " + std::to_string(constant.value);
            expected = constant.value + 1;
            line(text + ",");
        }
        --depth_;
        line("}");
        out_ += "\n";
    }

    for (const Record &record : unit.records) {
        documentation(record.documentation);
        std::string head = "struct " + record.name;
        if (record.has_address)
            head += " @ " + hex(record.address);
        line(head + " {");
        ++depth_;
        for (const Record::Member &member : record.members) {
            std::string text = "var " + member.name + ": " + spell(member.type, types_);
            // Offsets are always written out. A recovered layout is a fact
            // about the binary, and the reader should not have to add up
            // sizes to learn it.
            text += " @ " + hex(member.offset);
            line(text + ";");
        }
        --depth_;
        line("}");
        out_ += "\n";
    }

    for (const Variable &global : unit.globals) {
        indent();
        variable(global, global.is_constant ? "val" : "var");
        out_ += ";\n";
    }
    if (!unit.globals.empty())
        out_ += "\n";

    bool first = true;
    for (const Function &function : unit.functions) {
        if (!first)
            out_ += "\n";
        first = false;
        this->function(function);
    }
}

void Writer::variable(const Variable &variable, const char *word)
{
    out_ += word;
    out_ += " " + variable.name;
    if (variable.type)
        out_ += ": " + spell(variable.type, types_);
    if (!variable.storage.is_none())
        out_ += " @ " + spell(variable.storage);
    if (variable.initialiser) {
        out_ += " = ";
        value(*variable.initialiser);
    }
}

void Writer::function(const Function &function)
{
    documentation(function.documentation);
    indent();
    out_ += "func " + function.name;
    if (function.has_address)
        out_ += " @ " + hex(function.address);
    out_ += "(";
    bool first = true;
    for (const Variable &parameter : function.parameters) {
        if (!first)
            out_ += ", ";
        first = false;
        if (!parameter.type && parameter.storage.kind == Storage::Kind::Register
            && parameter.name == parameter.storage.register_name) {
            out_ += "@" + parameter.storage.register_name;
            continue;
        }
        out_ += parameter.name;
        if (parameter.type)
            out_ += ": " + spell(parameter.type, types_);
        if (!parameter.storage.is_none())
            out_ += " @ " + spell(parameter.storage);
    }
    if (function.variadic)
        out_ += first ? "..." : ", ...";
    out_ += ")";

    bool is_void = function.result && function.result->kind == compiler::Type::Kind::Void;
    if (!is_void || !function.result_storage.is_none()) {
        out_ += ":";
        if (function.result && !is_void)
            out_ += " " + spell(function.result, types_);
        if (!function.result_storage.is_none())
            out_ += (function.result && !is_void ? " @ " : " @") + spell(function.result_storage);
    }

    if (!function.body) {
        out_ += ";\n";
        return;
    }
    out_ += " ";
    block(*function.body);
}

void Writer::block(const Statement &statement)
{
    if (statement.kind != Statement::Kind::Compound) {
        out_ += "{\n";
        ++depth_;
        this->statement(statement);
        --depth_;
        line("}");
        return;
    }
    out_ += "{\n";
    ++depth_;
    for (const StatementPtr &child : statement.body)
        if (child)
            this->statement(*child);
    --depth_;
    line("}");
}

void Writer::value(const Expression &expression)
{
    if (is_truth(expression)) {
        out_ += "(";
        this->expression(expression, 0);
        out_ += ")";
        return;
    }
    this->expression(expression, 0);
}

void Writer::statement(const Statement &statement)
{
    switch (statement.kind) {
    case Statement::Kind::Compound:
        indent();
        block(statement);
        return;
    case Statement::Kind::Empty:
        return;
    case Statement::Kind::Expression:
        indent();
        if (statement.value)
            expression(*statement.value, 0);
        out_ += ";\n";
        return;
    case Statement::Kind::Declaration: {
        for (const Variable &variable : statement.variables) {
            indent();
            const char *word = variable.is_stack ? "stack" : variable.is_constant ? "val" : "var";
            this->variable(variable, word);
            out_ += ";\n";
        }
        return;
    }
    case Statement::Kind::If: {
        indent();
        const Statement *current = &statement;
        for (;;) {
            out_ += "if (";
            expression(*current->value, 0);
            out_ += ") ";
            block(*current->then_branch);
            if (!current->else_branch)
                return;
            // Take the newline back so else sits after the brace.
            out_.pop_back();
            out_ += " else ";
            if (current->else_branch->kind == Statement::Kind::If) {
                current = current->else_branch.get();
                continue;
            }
            block(*current->else_branch);
            return;
        }
    }
    case Statement::Kind::While:
        indent();
        out_ += "while (";
        expression(*statement.value, 0);
        out_ += ") ";
        block(*statement.then_branch);
        return;
    case Statement::Kind::DoWhile:
        indent();
        out_ += "do ";
        block(*statement.then_branch);
        out_.pop_back();
        out_ += " while (";
        expression(*statement.value, 0);
        out_ += ");\n";
        return;
    case Statement::Kind::Loop:
        indent();
        out_ += "loop ";
        block(*statement.then_branch);
        return;
    case Statement::Kind::ForIn:
        indent();
        out_ += "for (" + statement.name + " in ";
        expression(*statement.subject, 0);
        out_ += ") ";
        block(*statement.then_branch);
        return;
    case Statement::Kind::Match:
        indent();
        out_ += "match (";
        expression(*statement.value, 0);
        out_ += ") {\n";
        ++depth_;
        for (const MatchArm &arm : statement.arms) {
            indent();
            if (arm.values.empty()) {
                out_ += "else ";
            } else {
                bool first = true;
                for (const ExpressionPtr &value : arm.values) {
                    if (!first)
                        out_ += " or ";
                    first = false;
                    expression(*value, 0);
                }
                out_ += " ";
            }
            block(*arm.body);
        }
        --depth_;
        line("}");
        return;
    case Statement::Kind::Label:
        // A label sits one level out, the way C programmers write them, so
        // the eye finds it when following a goto.
        --depth_;
        if (depth_ < 0)
            depth_ = 0;
        line(statement.name + ":");
        ++depth_;
        return;
    case Statement::Kind::Goto:
        line("goto " + statement.name + ";");
        return;
    case Statement::Kind::Break:
        line("break;");
        return;
    case Statement::Kind::Continue:
        line("continue;");
        return;
    case Statement::Kind::Return:
        indent();
        out_ += "return";
        if (statement.value) {
            out_ += " ";
            value(*statement.value);
        }
        out_ += ";\n";
        return;
    case Statement::Kind::Asm: {
        line("asm {");
        ++depth_;
        size_t start = 0;
        while (start < statement.assembly.size()) {
            size_t end = statement.assembly.find('\n', start);
            if (end == std::string::npos)
                end = statement.assembly.size();
            std::string one = statement.assembly.substr(start, end - start);
            if (!one.empty())
                line(one);
            start = end + 1;
        }
        --depth_;
        line("}");
        return;
    }
    }
}

void Writer::expression(const Expression &expression, int parent_precedence)
{
    switch (expression.kind) {
    case Expression::Kind::IntegerLiteral: {
        // Small values in decimal, everything that looks like an address or a
        // mask in hexadecimal. The line is drawn where a person stops being
        // able to see the value at a glance.
        uint64_t v = expression.integer_value;
        if (v < 1024)
            out_ += std::to_string(v);
        else
            out_ += hex(v);
        return;
    }
    case Expression::Kind::FloatLiteral: {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%g", expression.float_value);
        std::string text = buffer;
        if (text.find('.') == std::string::npos && text.find('e') == std::string::npos)
            text += ".0";
        out_ += text;
        return;
    }
    case Expression::Kind::StringLiteral:
        out_ += quote(expression.text);
        return;
    case Expression::Kind::BooleanLiteral:
        out_ += expression.boolean_value ? "true" : "false";
        return;
    case Expression::Kind::NullLiteral:
        out_ += "null";
        return;
    case Expression::Kind::Name:
        out_ += expression.name;
        return;
    case Expression::Kind::Register:
        out_ += expression.name;
        if (expression.entry_value)
            out_ += "@entry";
        return;
    case Expression::Kind::AddressOfName:
        out_ += "&" + expression.name;
        return;
    case Expression::Kind::Call: {
        this->expression(*expression.callee, 100);
        out_ += "(";
        bool first = true;
        for (const ExpressionPtr &argument : expression.arguments) {
            if (!first)
                out_ += ", ";
            first = false;
            this->expression(*argument, 0);
        }
        out_ += ")";
        return;
    }
    case Expression::Kind::Unary: {
        bool postfix = expression.unary_op == UnaryOp::PostIncrement
                       || expression.unary_op == UnaryOp::PostDecrement;
        if (!postfix)
            out_ += spelling_of(expression.unary_op);
        // A unary applied to a binary needs the binary bracketed, and a unary
        // applied to a unary of the same sign needs a space: `- -x`.
        bool bracket = expression.left->kind == Expression::Kind::Binary
                       || expression.left->kind == Expression::Kind::Conditional
                       || expression.left->kind == Expression::Kind::Assign
                       || expression.left->kind == Expression::Kind::As;
        if (bracket)
            out_ += "(";
        this->expression(*expression.left, 11);
        if (bracket)
            out_ += ")";
        if (postfix)
            out_ += spelling_of(expression.unary_op);
        return;
    }
    case Expression::Kind::Binary: {
        int mine = precedence_of(expression.binary_op);
        bool bracket = mine < parent_precedence || (mine == parent_precedence && parent_precedence != 0);
        if (bracket)
            out_ += "(";
        this->expression(*expression.left, mine);
        out_ += std::string(" ") + spelling_of(expression.binary_op) + " ";
        // The right operand brackets at equal precedence so a - (b - c) keeps
        // its meaning; the left does not need to.
        this->expression(*expression.right, mine + 1);
        if (bracket)
            out_ += ")";
        return;
    }
    case Expression::Kind::Assign: {
        bool bracket = parent_precedence > 0;
        if (bracket)
            out_ += "(";
        this->expression(*expression.left, 0);
        out_ += " ";
        if (expression.binary_op != BinaryOp::Comma)
            out_ += spelling_of(expression.binary_op);
        out_ += "= ";
        value(*expression.right);
        if (bracket)
            out_ += ")";
        return;
    }
    case Expression::Kind::Index:
        this->expression(*expression.left, 100);
        out_ += "[";
        this->expression(*expression.right, 0);
        out_ += "]";
        return;
    case Expression::Kind::Member:
        this->expression(*expression.left, 100);
        out_ += expression.through_pointer ? "->" : ".";
        out_ += expression.name;
        return;
    case Expression::Kind::Conditional: {
        bool bracket = parent_precedence > 0;
        if (bracket)
            out_ += "(";
        this->expression(*expression.left, 1);
        out_ += " ? ";
        this->expression(*expression.right, 0);
        out_ += " : ";
        this->expression(*expression.third, 0);
        if (bracket)
            out_ += ")";
        return;
    }
    case Expression::Kind::As: {
        bool bracket = parent_precedence > 10;
        if (bracket)
            out_ += "(";
        this->expression(*expression.left, 11);
        out_ += " as " + spell(expression.named_type, types_);
        if (bracket)
            out_ += ")";
        return;
    }
    case Expression::Kind::SizeOf:
        out_ += "sizeof(";
        if (expression.named_type)
            out_ += spell(expression.named_type, types_);
        else
            this->expression(*expression.left, 0);
        out_ += ")";
        return;
    case Expression::Kind::Range:
        this->expression(*expression.left, 1);
        out_ += expression.inclusive ? "..=" : "..";
        this->expression(*expression.right, 1);
        return;
    case Expression::Kind::List: {
        out_ += "[";
        bool first = true;
        for (const ExpressionPtr &element : expression.arguments) {
            if (!first)
                out_ += ", ";
            first = false;
            this->expression(*element, 0);
        }
        out_ += "]";
        return;
    }
    }
}

} // namespace

std::string spell(TypePtr type, const Types &types)
{
    if (!type)
        return "";
    using Kind = compiler::Type::Kind;
    switch (type->kind) {
    case Kind::Void:
        return "void";
    case Kind::Integer:
        if (types.is_unknown(type))
            return "unknown" + std::to_string(type->width * 8);
        if (type == types.boolean())
            return "bool";
        return std::string(type->is_signed ? "i" : "u") + std::to_string(type->width * 8);
    case Kind::Floating:
        return "f" + std::to_string(type->width * 8);
    case Kind::Pointer:
        return "*" + spell(type->target, types);
    case Kind::Array:
        if (type->count == 0)
            return "[" + spell(type->target, types) + "]";
        return "[" + spell(type->target, types) + "; " + std::to_string(type->count) + "]";
    case Kind::Function: {
        std::string text = "func(";
        bool first = true;
        for (TypePtr parameter : type->parameters) {
            if (!first)
                text += ", ";
            first = false;
            text += spell(parameter, types);
        }
        if (type->variadic)
            text += first ? "..." : ", ...";
        text += ")";
        if (type->result && type->result->kind != Kind::Void)
            text += ": " + spell(type->result, types);
        return text;
    }
    case Kind::Struct:
        return type->name;
    }
    return "?";
}

std::string spell(const Storage &storage)
{
    switch (storage.kind) {
    case Storage::Kind::None:
        return "";
    case Storage::Kind::Register:
        return storage.register_name;
    case Storage::Kind::EntryRegister:
        return storage.register_name + "@entry";
    case Storage::Kind::Address:
        return hex(storage.address);
    case Storage::Kind::Frame: {
        int64_t offset = storage.offset;
        if (offset < 0)
            return "-" + hex(static_cast<uint64_t>(-offset));
        return "+" + hex(static_cast<uint64_t>(offset));
    }
    }
    return "";
}

std::string format(const Unit &unit, const Types &types)
{
    Writer writer(types);
    writer.unit(unit);
    return writer.take();
}

std::string format(const Function &function, const Types &types)
{
    Writer writer(types);
    writer.function(function);
    return writer.take();
}

std::string format(const Expression &expression, const Types &types)
{
    Writer writer(types);
    writer.expression(expression);
    return writer.take();
}

} // namespace nova
} // namespace astral_internal
