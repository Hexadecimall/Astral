// Reading Nova, from tokens to a tree.
//
// The parser is deliberately forgiving in one direction only: it accepts every
// level, from a function that is nothing but an `asm` block to one that never
// mentions storage at all, and produces the same shape of tree for both. What
// it will not do is guess. A missing type stays missing, and the checker
// decides later whether that mattered.
#ifndef ASTRAL_NOVA_PARSER_HH
#define ASTRAL_NOVA_PARSER_HH

#include "ast.hh"
#include "lexer.hh"
#include "types.hh"

#include <string>
#include <vector>

namespace astral_internal {
namespace nova {

// Reads `tokens` into `unit`, naming types in `types`. Returns false when
// something could not be read. Diagnostics whose message starts with
// "warning: " never on their own make it fail.
bool parse(const std::vector<Token> &tokens, Types &types, Unit &unit,
           std::vector<Diagnostic> &diagnostics);

} // namespace nova
} // namespace astral_internal

#endif
