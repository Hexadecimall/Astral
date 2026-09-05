// Turning Nova text into the pieces a parser can read.
//
// Nova's surface comes from Fusion, so comments begin with `#` and there is no
// preprocessor to run before this: what the file says is what it means. The
// two additions the machine forces are `@`, which introduces storage, and the
// register names that follow it, which are ordinary identifiers here and only
// become registers once the parser knows where it is.
#ifndef ASTRAL_NOVA_LEXER_HH
#define ASTRAL_NOVA_LEXER_HH

#include "ast.hh"
#include "compiler/compiler.hh"

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {
namespace nova {

using compiler::Diagnostic;

struct Token {
    enum class Kind {
        End,
        Name,          // an identifier or a keyword; the parser tells them apart
        Integer,
        Float,
        Character,     // already converted to its numeric value
        String,        // already unescaped, without a terminator
        Punctuation,
        // The text between `#/` and `#\`, kept because a person wrote it about
        // code they were reading and it should survive a round trip.
        Documentation,
        // The raw body of an `asm { }` block. Assembly is another language with
        // its own comment character and its own punctuation, so the tokeniser
        // does not look inside; it hands the text over whole.
        Assembly,
    };

    Kind kind = Kind::End;
    Where where;
    std::string text;           // the spelling, for a name or a punctuator
    uint64_t integer_value = 0; // Integer and Character
    double float_value = 0;     // Float
    std::string bytes;          // String: the bytes it stands for
    bool is_unsigned = false;
    int width = 4;

    bool is(Kind k) const { return kind == k; }
    bool is_punctuation(const char *what) const
    {
        return kind == Kind::Punctuation && text == what;
    }
    bool is_name(const char *what) const { return kind == Kind::Name && text == what; }
};

// Reads all of `source`. The last token is always End, so a parser can look
// ahead without checking bounds. Anything malformed is reported and skipped,
// so one bad character does not stop the rest of the file from being read.
std::vector<Token> tokenise(const std::string &source, std::vector<Diagnostic> &diagnostics);

// Whether `name` is a Nova keyword. The tokeniser hands keywords back as
// names; this is how the parser recognises them.
bool is_keyword(const std::string &name);

// Whether `name` spells a machine register for some architecture Astral
// supports. Only consulted after `@`, so a local called `x0` is still legal.
bool is_register_name(const std::string &name);

} // namespace nova
} // namespace astral_internal

#endif
