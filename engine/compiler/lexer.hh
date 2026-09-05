// Turning C text into the pieces a parser can read.
//
// The input is the C Astral emits plus whatever a person changed in it, so the
// tokeniser has to be a real C tokeniser rather than something that only knows
// the shapes the emitter happens to write. The one thing it does not do is
// preprocess: lines beginning with `#` are dropped, because the emitted file
// uses them only for includes and pragmas, neither of which changes what the
// code means.
#ifndef ASTRAL_COMPILER_LEXER_HH
#define ASTRAL_COMPILER_LEXER_HH

#include "ast.hh"
#include "compiler.hh"

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {
namespace compiler {

struct Token {
    enum class Kind {
        End,
        Name,          // an identifier or a keyword; the parser tells them apart
        Integer,
        Float,
        Character,     // already converted to its numeric value
        String,        // already unescaped, without a terminator
        Punctuation,
    };

    Kind kind = Kind::End;
    Where where;
    // The spelling, for a name or a punctuator.
    std::string text;
    // Integer and Character.
    uint64_t integer_value = 0;
    // Float.
    double float_value = 0;
    // String: the bytes it stands for.
    std::string bytes;
    // What an integer literal's suffix and value ask for.
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
// so one bad character does not stop the rest from being read.
std::vector<Token> tokenise(const std::string &source, std::vector<Diagnostic> &diagnostics);

// Whether `name` is a C keyword. The tokeniser hands keywords back as names;
// this is how the parser recognises them.
bool is_keyword(const std::string &name);

} // namespace compiler
} // namespace astral_internal

#endif
