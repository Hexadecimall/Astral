#include "lexer.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>

namespace astral_internal {
namespace nova {
namespace {

// Every word Nova reserves. Fusion's vocabulary, plus the words the machine
// needs: `stack` for a frame slot, `asm` for instructions, `call` for a call
// whose result nobody named yet.
const std::array<const char *, 33> kKeywords = {
    "func",  "var",    "val",    "return", "if",     "else",  "while", "for",
    "in",    "loop",   "break",  "continue", "match", "enum", "struct", "class", "stack",
    "asm",   "call",   "import", "true",   "false",  "null",  "goto",  "label",
    "sizeof", "cast",  "do",     "switch", "case",   "default", "extern", "as",
};

bool is_name_start(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_name_part(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The punctuators, longest first, so `..=` wins over `..` and `<<` over `<`.
const std::array<const char *, 40> kPunctuation = {
    "..=", "<<=", ">>=",
    "->",  "==",  "!=",  "<=",  ">=",  "&&",  "||",  "<<",  ">>",  "..",
    "+=",  "-=",  "*=",  "/=",  "%=",  "&=",  "|=",  "^=",  "::",
    "@",   ":",   ";",   ",",   ".",   "(",   ")",   "{",   "}",   "[",   "]",
    "=",   "<",   ">",   "+",   "-",   "*",   "/",
};

// The rest, which are single characters that did not need ordering.
const std::array<char, 6> kSinglePunctuation = {'%', '&', '|', '^', '~', '?'};

class Reader {
public:
    Reader(const std::string &source, std::vector<Diagnostic> &diagnostics)
        : source_(source), diagnostics_(diagnostics)
    {
    }

    std::vector<Token> run();

private:
    char peek(size_t ahead = 0) const
    {
        return at_ + ahead < source_.size() ? source_[at_ + ahead] : '\0';
    }
    bool done() const { return at_ >= source_.size(); }
    void advance();
    Where here() const { return Where{line_, column_}; }
    void complain(const Where &where, const std::string &message);

    void skip_blanks_and_comments(std::vector<Token> &out);
    Token read_number();
    Token read_name();
    Token read_string();
    Token read_character();
    Token read_punctuation();
    // Called after the name `asm` was read. If a brace follows, everything to
    // its match is one token, untouched.
    bool read_assembly(std::vector<Token> &out);

    // Reads one character of a string or character literal, following the
    // escape if there is one.
    bool read_escaped(char &value);

    const std::string &source_;
    std::vector<Diagnostic> &diagnostics_;
    size_t at_ = 0;
    int line_ = 1;
    int column_ = 1;
};

void Reader::advance()
{
    if (done())
        return;
    if (source_[at_] == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    ++at_;
}

void Reader::complain(const Where &where, const std::string &message)
{
    diagnostics_.push_back(Diagnostic{where.line, where.column, message});
}

// `#` starts a comment. `#/` starts a documentation block that runs to `#\`,
// and that one is kept: a person wrote it about code they were working out,
// and throwing it away would lose the only part of the file that is not
// recoverable from the binary.
void Reader::skip_blanks_and_comments(std::vector<Token> &out)
{
    for (;;) {
        while (!done() && std::isspace(static_cast<unsigned char>(peek())))
            advance();
        // C's comments are read too. A `//` comment is dropped like a `#`
        // one; a `/* */` comment is kept, because that is how the decompiler
        // writes its warnings, and a warning about the code should reach
        // whoever reads the code.
        if (peek() == '/' && peek(1) == '/') {
            while (!done() && peek() != '\n')
                advance();
            continue;
        }
        if (peek() == '/' && peek(1) == '*') {
            Where where = here();
            advance();
            advance();
            std::string text;
            for (;;) {
                if (done()) {
                    complain(where, "a comment was never closed with */");
                    break;
                }
                if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    break;
                }
                text.push_back(peek());
                advance();
            }
            Token token;
            token.kind = Token::Kind::Documentation;
            token.where = where;
            token.text = text;
            out.push_back(token);
            continue;
        }
        if (peek() != '#')
            return;

        Where where = here();
        if (peek(1) == '/') {
            advance();
            advance();
            std::string text;
            for (;;) {
                if (done()) {
                    complain(where, "a documentation block was never closed with #\\");
                    break;
                }
                if (peek() == '#' && peek(1) == '\\') {
                    advance();
                    advance();
                    break;
                }
                text.push_back(peek());
                advance();
            }
            Token token;
            token.kind = Token::Kind::Documentation;
            token.where = where;
            token.text = text;
            out.push_back(token);
            continue;
        }
        while (!done() && peek() != '\n')
            advance();
    }
}

Token Reader::read_number()
{
    Token token;
    token.kind = Token::Kind::Integer;
    token.where = here();

    std::string digits;
    int base = 10;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        base = 16;
        advance();
        advance();
    } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        base = 2;
        advance();
        advance();
    } else if (peek() == '0' && (peek(1) == 'o' || peek(1) == 'O')) {
        base = 8;
        advance();
        advance();
    }

    bool floating = false;
    for (;;) {
        char c = peek();
        // Underscores are separators for reading and mean nothing to the value.
        if (c == '_') {
            advance();
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            // Outside base 16, the letters are not digits, so `0b1and` reads as
            // a number followed by a name rather than as one broken number.
            if (base != 16 && !std::isdigit(static_cast<unsigned char>(c)))
                break;
            digits.push_back(c);
            advance();
            continue;
        }
        if (base == 10 && c == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            floating = true;
            digits.push_back(c);
            advance();
            continue;
        }
        break;
    }

    if (digits.empty()) {
        complain(token.where, "a number with no digits");
        digits = "0";
    }

    if (floating) {
        token.kind = Token::Kind::Float;
        token.float_value = std::strtod(digits.c_str(), nullptr);
        return token;
    }

    token.integer_value = std::strtoull(digits.c_str(), nullptr, base);
    // A value that does not fit in 32 bits is 64 bits wide, which is what the
    // addresses in recovered code almost always are.
    token.width = token.integer_value > 0xffffffffULL ? 8 : 4;

    // Suffixes: u for unsigned, then any number of l, which Nova reads only so
    // that C pasted into a Nova file does not stop the tokeniser.
    while (peek() == 'u' || peek() == 'U' || peek() == 'l' || peek() == 'L') {
        if (peek() == 'u' || peek() == 'U')
            token.is_unsigned = true;
        else
            token.width = 8;
        advance();
    }
    return token;
}

Token Reader::read_name()
{
    Token token;
    token.kind = Token::Kind::Name;
    token.where = here();
    while (!done() && is_name_part(peek())) {
        token.text.push_back(peek());
        advance();
    }
    return token;
}

bool Reader::read_escaped(char &value)
{
    if (peek() != '\\') {
        value = peek();
        advance();
        return true;
    }
    Where where = here();
    advance();
    char c = peek();
    advance();
    switch (c) {
    case 'n': value = '\n'; return true;
    case 't': value = '\t'; return true;
    case 'r': value = '\r'; return true;
    case '0': value = '\0'; return true;
    case '\\': value = '\\'; return true;
    case '\'': value = '\''; return true;
    case '"': value = '"'; return true;
    case 'a': value = '\a'; return true;
    case 'b': value = '\b'; return true;
    case 'f': value = '\f'; return true;
    case 'v': value = '\v'; return true;
    case 'x': {
        std::string digits;
        while (digits.size() < 2 && std::isxdigit(static_cast<unsigned char>(peek()))) {
            digits.push_back(peek());
            advance();
        }
        if (digits.empty()) {
            complain(where, "\\x with no hexadecimal digits after it");
            value = 'x';
            return true;
        }
        value = static_cast<char>(std::strtoul(digits.c_str(), nullptr, 16));
        return true;
    }
    default:
        complain(where, std::string("\\") + c + " is not an escape Nova knows");
        value = c;
        return true;
    }
}

Token Reader::read_string()
{
    Token token;
    token.kind = Token::Kind::String;
    token.where = here();
    advance();  // the opening quote
    for (;;) {
        if (done() || peek() == '\n') {
            complain(token.where, "a string was never closed");
            break;
        }
        if (peek() == '"') {
            advance();
            break;
        }
        char value = 0;
        read_escaped(value);
        token.bytes.push_back(value);
    }
    token.text = token.bytes;
    return token;
}

Token Reader::read_character()
{
    Token token;
    token.kind = Token::Kind::Character;
    token.where = here();
    advance();  // the opening quote
    char value = 0;
    if (peek() == '\'') {
        complain(token.where, "a character literal with nothing in it");
    } else {
        read_escaped(value);
    }
    if (peek() == '\'')
        advance();
    else
        complain(token.where, "a character literal was never closed");
    token.integer_value = static_cast<unsigned char>(value);
    token.width = 4;
    return token;
}

bool Reader::read_assembly(std::vector<Token> &out)
{
    size_t look = at_;
    while (look < source_.size() && std::isspace(static_cast<unsigned char>(source_[look])))
        ++look;
    if (look >= source_.size() || source_[look] != '{')
        return false;
    while (at_ < look)
        advance();
    Token token;
    token.kind = Token::Kind::Assembly;
    token.where = here();
    advance();  // the brace
    int depth = 1;
    std::string text;
    while (!done()) {
        char c = peek();
        if (c == '{')
            ++depth;
        if (c == '}' && --depth == 0) {
            advance();
            break;
        }
        text.push_back(c);
        advance();
    }
    if (depth != 0)
        complain(token.where, "an asm block was never closed");
    token.text = text;
    out.push_back(token);
    return true;
}

Token Reader::read_punctuation()
{
    Token token;
    token.kind = Token::Kind::Punctuation;
    token.where = here();

    for (const char *candidate : kPunctuation) {
        size_t length = std::char_traits<char>::length(candidate);
        if (source_.compare(at_, length, candidate) == 0) {
            for (size_t i = 0; i < length; ++i)
                advance();
            token.text = candidate;
            return token;
        }
    }
    for (char candidate : kSinglePunctuation) {
        if (peek() == candidate) {
            advance();
            token.text = std::string(1, candidate);
            return token;
        }
    }
    if (peek() == '!') {
        advance();
        token.text = "!";
        return token;
    }

    complain(token.where, std::string("'") + peek() + "' is not a character Nova uses");
    advance();
    token.text = "";
    return token;
}

std::vector<Token> Reader::run()
{
    std::vector<Token> tokens;
    for (;;) {
        skip_blanks_and_comments(tokens);
        if (done())
            break;
        char c = peek();
        if (is_name_start(c)) {
            tokens.push_back(read_name());
            if (tokens.back().text == "asm")
                read_assembly(tokens);
        }
        else if (std::isdigit(static_cast<unsigned char>(c)))
            tokens.push_back(read_number());
        else if (c == '"')
            tokens.push_back(read_string());
        else if (c == '\'')
            tokens.push_back(read_character());
        else {
            Token token = read_punctuation();
            // An unrecognised character was already complained about; do not
            // hand the parser an empty punctuator to trip over.
            if (!token.text.empty())
                tokens.push_back(token);
        }
    }
    Token end;
    end.kind = Token::Kind::End;
    end.where = here();
    tokens.push_back(end);
    return tokens;
}

} // namespace

std::vector<Token> tokenise(const std::string &source, std::vector<Diagnostic> &diagnostics)
{
    Reader reader(source, diagnostics);
    return reader.run();
}

bool is_keyword(const std::string &name)
{
    return std::find_if(kKeywords.begin(), kKeywords.end(), [&](const char *word) {
               return name == word;
           }) != kKeywords.end();
}

// A register is a letter class and a number, or one of the handful of named
// registers an architecture has. Checked by shape rather than by table so that
// every architecture Astral grows keeps working without a change here.
bool is_register_name(const std::string &name)
{
    if (name == "sp" || name == "lr" || name == "pc" || name == "fp" || name == "wzr"
        || name == "xzr")
        return true;
    if (name.size() < 2 || name.size() > 4)
        return false;
    size_t at = 0;
    // x0, w22, d3, s1, q7, v12 on AArch64; r0 on ARM; e-- and r-- on x86 are
    // named rather than numbered and fall to the table above plus the x86
    // names below.
    if (name[0] != 'x' && name[0] != 'w' && name[0] != 'd' && name[0] != 's' && name[0] != 'q'
        && name[0] != 'v' && name[0] != 'r')
        return false;
    at = 1;
    if (at >= name.size())
        return false;
    for (size_t i = at; i < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i])))
            return false;
    return true;
}

} // namespace nova
} // namespace astral_internal
