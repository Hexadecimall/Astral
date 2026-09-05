// Working out what the parsed tree actually means.
//
// The parser knows the shape of the code; this knows the types. It fills in
// every expression's type, ties every name to what it refers to, and writes
// down the things the code reaches for but does not contain, which is what
// lets a patched function call back into the program around it.
//
// It is deliberately forgiving. Decompiled C mixes integers and pointers
// constantly, and the emitted file turns the corresponding compiler warnings
// off; refusing that code would mean refusing Astral's own output. Anything of
// that sort is reported as a warning and the check carries on.
#ifndef ASTRAL_COMPILER_SEMA_HH
#define ASTRAL_COMPILER_SEMA_HH

#include "ast.hh"
#include "compiler.hh"

#include <vector>

namespace astral_internal {
namespace compiler {

// Checks `unit` in place. Returns false only when something is wrong enough
// that no code could be generated for it; diagnostics whose message begins
// with "warning: " never cause that.
bool check(TypeStore &types, Unit &unit, std::vector<Diagnostic> &diagnostics);

} // namespace compiler
} // namespace astral_internal

#endif
