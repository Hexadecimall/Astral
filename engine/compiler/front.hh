// Reading C, from text to a checked tree.
//
// One call does the whole front end: tokenise, parse, check. What comes back
// is a Unit whose every expression has a type and whose every name is either
// something the unit defines or something it records as belonging to the
// program around it.
//
// Also here: comparing two versions of the same function. A patch should only
// rewrite what actually changed, and the only place that can be decided is on
// the tree, where the things that do not reach the generated code - comments,
// spacing, and the names a person gives their locals - have already fallen
// away.
#ifndef ASTRAL_COMPILER_FRONT_HH
#define ASTRAL_COMPILER_FRONT_HH

#include "ast.hh"
#include "compiler.hh"

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {
namespace compiler {

// Reads `source` into `unit`. Types named along the way go into `types`, which
// also supplies the typedefs the decompiler's output uses. Returns false when
// the source could not be read or checked; diagnostics whose message starts
// with "warning: " never on their own make it fail, because the C a decompiler
// writes earns plenty of them.
bool parse_unit(const std::string &source, TypeStore &types, Unit &unit,
                std::vector<Diagnostic> &diagnostics);

// Whether two functions mean the same thing, ignoring what does not reach the
// generated code: comments, whitespace, and the names of locals and
// parameters. Names of called functions and of globals do count, since those
// resolve to addresses. Types count for what they are rather than for which
// spelling was used, so `uint4` and `uint32_t` compare the same.
bool same_meaning(const Function &left, const Function &right);

// A digest of a function's meaning under the same rules, so a version can be
// remembered without keeping its tree.
uint64_t meaning_digest(const Function &function);

// One literal that is not what it was.
struct LiteralChange {
    std::string before;
    std::string after;
    Where where;
    bool is_text = false;
};

// Fills `changes` and returns true when the two functions are identical apart
// from the values of literals, which is the case a patch can serve by writing
// bytes without generating any code. Returns false the moment anything else
// differs.
bool only_literals_differ(const Function &before, const Function &after,
                          std::vector<LiteralChange> &changes);

} // namespace compiler
} // namespace astral_internal

#endif
