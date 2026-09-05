#include "lexer.hh"

#include <cstdint>
#include <cstdlib>

namespace astral_internal {
namespace compiler {

namespace {

bool is_name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

bool is_name_part(char c) { return is_name_start(c) || (c >= '0' && c <= '9'); }

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_hex_digit(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

// Every punctuator, longest first, so a greedy match never stops early and
// reads `>>=` as `>>` followed by `=`.
const char *const PUNCTUATORS[] = {
    "...", "<<=", ">>=",
    "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##",
    "[", "]", "(", ")", "{", "}", ".", "&", "*", "+", "-", "~", "!", "/", "%",
    "<", ">", "^", "|", "?", ":", ";", "=", ",", "#",
};

const char *const KEYWORDS[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
    "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register",
    "restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch", "typedef",
    "union", "unsigned", "void", "volatile", "while",
    "_Bool", "_Complex", "_Noreturn", "_Alignof", "_Atomic", "_Thread_local", "_Static_assert",
};

// Reads text a character at a time while keeping track of where it came from,
// so a complaint can name a line and a column.
class Reader {
public:
    Reader(const std::string &source, std::vector<Diagnostic> &diagnostics)
        : source_(source), diagnostics_(diagnostics)
    {
    }

    bool done() const { return at_ >= source_.size(); }
    char peek(size_t ahead = 0) const
    {
        return at_ + ahead < source_.size() ? source_[at_ + ahead] : '\0';
    }
    char take()
    {
        char c = peek();
        advance();
        return c;
    }
    void advance()
    {
        if (at_ >= source_.size())
            return;
        if (source_[at_] == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        ++at_;
    }
    // A backslash at the end of a line joins it to the next one. Skipping them
    // here keeps every other rule from having to know about it.
    void skip_continuations()
    {
        while (peek() == '\\') {
            size_t ahead = 1;
            while (peek(ahead) == ' ' || peek(ahead) == '\t' || peek(ahead) == '\r')
                ++ahead;
            if (peek(ahead) != '\n')
                return;
            for (size_t i = 0; i <= ahead; ++i)
                advance();
        }
    }

    Where here() const
    {
        Where where;
        where.line = line_;
        where.column = column_;
        return where;
    }
    bool at_line_start() const { return column_ == 1; }

    void complain(const Where &where, const std::string &message)
    {
        Diagnostic diagnostic;
        diagnostic.line = where.line;
        diagnostic.column = where.column;
        diagnostic.message = message;
        diagnostics_.push_back(diagnostic);
    }

private:
    const std::string &source_;
    std::vector<Diagnostic> &diagnostics_;
    size_t at_ = 0;
    int line_ = 1;
    int column_ = 1;
};

// Whitespace, comments and the `#` lines the emitter writes. A preprocessor
// line is dropped whole: the emitted C uses them only for includes and pragmas,
// and a `#define` body that is dropped with them leaves behind identifiers the
// parser is told to ignore instead.
bool skip_trivia(Reader &reader)
{
    bool skipped = false;
    for (;;) {
        reader.skip_continuations();
        char c = reader.peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            reader.advance();
            skipped = true;
            continue;
        }
        if (c == '/' && reader.peek(1) == '/') {
            while (!reader.done() && reader.peek() != '\n') {
                reader.skip_continuations();
                if (reader.peek() == '\n')
                    break;
                reader.advance();
            }
            skipped = true;
            continue;
        }
        if (c == '/' && reader.peek(1) == '*') {
            Where where = reader.here();
            reader.advance();
            reader.advance();
            bool closed = false;
            while (!reader.done()) {
                if (reader.peek() == '*' && reader.peek(1) == '/') {
                    reader.advance();
                    reader.advance();
                    closed = true;
                    break;
                }
                reader.advance();
            }
            if (!closed)
                reader.complain(where, "error: a comment was opened and never closed");
            skipped = true;
            continue;
        }
        if (c == '#' && reader.at_line_start()) {
            while (!reader.done()) {
                reader.skip_continuations();
                if (reader.done() || reader.peek() == '\n')
                    break;
                reader.advance();
            }
            skipped = true;
            continue;
        }
        break;
    }
    return skipped;
}

// One escape sequence, already past the backslash.
uint64_t read_escape(Reader &reader)
{
    Where where = reader.here();
    char c = reader.take();
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
        uint64_t value = static_cast<uint64_t>(c - '0');
        for (int i = 0; i < 2 && reader.peek() >= '0' && reader.peek() <= '7'; ++i)
            value = value * 8 + static_cast<uint64_t>(reader.take() - '0');
        return value;
    }
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case 'e': return 27;
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case '?': return '?';
    case 'x': {
        if (!is_hex_digit(reader.peek())) {
            reader.complain(where, "error: \\x needs at least one hexadecimal digit");
            return 0;
        }
        uint64_t value = 0;
        while (is_hex_digit(reader.peek()))
            value = value * 16 + static_cast<uint64_t>(hex_value(reader.take()));
        return value;
    }
    case 'u': case 'U': {
        int digits = c == 'u' ? 4 : 8;
        uint64_t value = 0;
        for (int i = 0; i < digits && is_hex_digit(reader.peek()); ++i)
            value = value * 16 + static_cast<uint64_t>(hex_value(reader.take()));
        return value;
    }
    default:
        reader.complain(where, std::string("error: \\") + c + " is not an escape sequence");
        return static_cast<unsigned char>(c);
    }
}

void read_number(Reader &reader, Token &token)
{
    std::string spelling;
    bool hex = false;
    if (reader.peek() == '0' && (reader.peek(1) == 'x' || reader.peek(1) == 'X')) {
        hex = true;
        spelling += reader.take();
        spelling += reader.take();
        while (is_hex_digit(reader.peek()) || reader.peek() == '\'')
            if (reader.peek() == '\'')
                reader.advance();
            else
                spelling += reader.take();
    } else if (reader.peek() == '0' && (reader.peek(1) == 'b' || reader.peek(1) == 'B') &&
               (reader.peek(2) == '0' || reader.peek(2) == '1')) {
        reader.advance();
        reader.advance();
        uint64_t value = 0;
        while (reader.peek() == '0' || reader.peek() == '1' || reader.peek() == '\'')
            if (reader.peek() == '\'')
                reader.advance();
            else
                value = value * 2 + static_cast<uint64_t>(reader.take() - '0');
        token.kind = Token::Kind::Integer;
        token.integer_value = value;
        spelling = "0b";
    } else {
        while (is_digit(reader.peek()) || reader.peek() == '\'')
            if (reader.peek() == '\'')
                reader.advance();
            else
                spelling += reader.take();
    }

    bool floating = false;
    if (token.kind != Token::Kind::Integer) {
        if (reader.peek() == '.') {
            floating = true;
            spelling += reader.take();
            while (hex ? is_hex_digit(reader.peek()) : is_digit(reader.peek()))
                spelling += reader.take();
        }
        char exponent = hex ? 'p' : 'e';
        char exponent_upper = hex ? 'P' : 'E';
        if (reader.peek() == exponent || reader.peek() == exponent_upper) {
            floating = true;
            spelling += reader.take();
            if (reader.peek() == '+' || reader.peek() == '-')
                spelling += reader.take();
            while (is_digit(reader.peek()))
                spelling += reader.take();
        }
    }

    // Suffixes. Order is free: `ull` and `llu` mean the same thing.
    bool is_unsigned = false;
    int longs = 0;
    bool is_float_suffix = false;
    for (;;) {
        char c = reader.peek();
        if (c == 'u' || c == 'U') {
            is_unsigned = true;
            reader.advance();
        } else if (c == 'l' || c == 'L') {
            ++longs;
            reader.advance();
        } else if ((c == 'f' || c == 'F') && !hex) {
            is_float_suffix = true;
            reader.advance();
        } else {
            break;
        }
    }

    if (floating || (is_float_suffix && token.kind != Token::Kind::Integer)) {
        token.kind = Token::Kind::Float;
        token.float_value = std::strtod(spelling.c_str(), nullptr);
        token.width = is_float_suffix ? 4 : 8;
        token.text = spelling;
        return;
    }

    if (token.kind != Token::Kind::Integer) {
        token.kind = Token::Kind::Integer;
        if (hex)
            token.integer_value = std::strtoull(spelling.c_str() + 2, nullptr, 16);
        else if (spelling.size() > 1 && spelling[0] == '0')
            token.integer_value = std::strtoull(spelling.c_str() + 1, nullptr, 8);
        else
            token.integer_value = std::strtoull(spelling.c_str(), nullptr, 10);
    }
    token.text = spelling;
    token.is_unsigned = is_unsigned;
    // A literal is as wide as it has to be. Anything a 32-bit int cannot hold
    // is already 64-bit whether or not it was written with a suffix.
    if (longs > 0 || token.integer_value > 0x7fffffffull)
        token.width = 8;
    else
        token.width = 4;
    if (token.integer_value > 0xffffffffull)
        token.width = 8;
}

} // namespace

bool is_keyword(const std::string &name)
{
    for (const char *keyword : KEYWORDS)
        if (name == keyword)
            return true;
    return false;
}

std::vector<Token> tokenise(const std::string &source, std::vector<Diagnostic> &diagnostics)
{
    Reader reader(source, diagnostics);
    std::vector<Token> tokens;

    for (;;) {
        skip_trivia(reader);
        if (reader.done())
            break;

        Token token;
        token.where = reader.here();
        char c = reader.peek();

        // A prefix on a character or string literal says how wide its elements
        // are. Nothing downstream cares, so it is read and discarded.
        if ((c == 'L' || c == 'u' || c == 'U') &&
            (reader.peek(1) == '\'' || reader.peek(1) == '"')) {
            reader.advance();
            c = reader.peek();
        } else if (c == 'u' && reader.peek(1) == '8' && reader.peek(2) == '"') {
            reader.advance();
            reader.advance();
            c = reader.peek();
        }

        if (is_name_start(c)) {
            token.kind = Token::Kind::Name;
            while (is_name_part(reader.peek()))
                token.text += reader.take();
            tokens.push_back(token);
            continue;
        }

        if (is_digit(c) || (c == '.' && is_digit(reader.peek(1)))) {
            read_number(reader, token);
            tokens.push_back(token);
            continue;
        }

        if (c == '\'') {
            reader.advance();
            token.kind = Token::Kind::Character;
            uint64_t value = 0;
            int count = 0;
            while (!reader.done() && reader.peek() != '\'') {
                if (reader.peek() == '\n') {
                    reader.complain(token.where, "error: a character literal ran to the end "
                                                 "of the line without closing");
                    break;
                }
                uint64_t piece;
                if (reader.peek() == '\\') {
                    reader.advance();
                    piece = read_escape(reader);
                } else {
                    piece = static_cast<unsigned char>(reader.take());
                }
                // A multi-character literal packs its bytes, which is what the
                // compilers the decompiled code was built with do.
                value = count == 0 ? piece : (value << 8) | (piece & 0xff);
                ++count;
            }
            if (reader.peek() == '\'')
                reader.advance();
            else
                reader.complain(token.where, "error: a character literal was not closed");
            if (count == 0)
                reader.complain(token.where, "error: a character literal has to hold something");
            token.integer_value = value;
            // A plain character literal has type int in C.
            token.width = 4;
            tokens.push_back(token);
            continue;
        }

        if (c == '"') {
            reader.advance();
            token.kind = Token::Kind::String;
            bool closed = false;
            while (!reader.done()) {
                if (reader.peek() == '"') {
                    reader.advance();
                    closed = true;
                    break;
                }
                if (reader.peek() == '\n')
                    break;
                if (reader.peek() == '\\') {
                    reader.advance();
                    uint64_t value = read_escape(reader);
                    token.bytes += static_cast<char>(value & 0xff);
                } else {
                    token.bytes += reader.take();
                }
            }
            if (!closed)
                reader.complain(token.where, "error: a string literal was not closed");
            tokens.push_back(token);
            continue;
        }

        bool matched = false;
        for (const char *punctuator : PUNCTUATORS) {
            size_t length = 0;
            while (punctuator[length] != '\0')
                ++length;
            bool same = true;
            for (size_t i = 0; i < length; ++i)
                if (reader.peek(i) != punctuator[i]) {
                    same = false;
                    break;
                }
            if (!same)
                continue;
            for (size_t i = 0; i < length; ++i)
                reader.advance();
            token.kind = Token::Kind::Punctuation;
            token.text = punctuator;
            tokens.push_back(token);
            matched = true;
            break;
        }
        if (matched)
            continue;

        reader.complain(token.where,
                        std::string("error: '") + c + "' has no meaning in C");
        reader.advance();
    }

    Token end;
    end.kind = Token::Kind::End;
    end.where = reader.here();
    tokens.push_back(end);
    return tokens;
}

} // namespace compiler
} // namespace astral_internal
