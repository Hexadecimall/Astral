// Reading C into the tree the rest of the compiler works on.
//
// Recursive descent, because the grammar it has to accept is fixed and small
// enough to write out by hand, and because a hand-written parser is the only
// kind that can say what went wrong in terms a person recognises.
//
// It accepts more than the emitter writes on purpose: the whole point is that
// someone edits the output, and what they type has to work.
#ifndef ASTRAL_COMPILER_PARSER_HH
#define ASTRAL_COMPILER_PARSER_HH

#include "ast.hh"
#include "compiler.hh"
#include "lexer.hh"

#include <string>
#include <vector>

namespace astral_internal {
namespace compiler {

// Reads `tokens` into `unit`. Types named along the way are added to `types`,
// which is also where the built-in decompiler typedefs come from. Returns false
// when something could not be read; diagnostics whose message starts with
// "warning: " do not on their own make it fail.
bool parse(const std::vector<Token> &tokens, TypeStore &types, Unit &unit,
           std::vector<Diagnostic> &diagnostics);

} // namespace compiler
} // namespace astral_internal

#endif
