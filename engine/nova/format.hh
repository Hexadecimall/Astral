// Writing a Nova tree back out as Nova.
//
// There is one way to lay out a Nova file, and this is it. The decompiler's
// emitter, the formatter, and any tool that rewrites a file all go through
// here, so what a person reads is always the same shape regardless of which
// tool wrote it. The rules are the ones the language was designed with: four
// spaces, braces on the line, a semicolon after every statement, and a pair
// of parentheses around any expression that produces a truth value.
#ifndef ASTRAL_NOVA_FORMAT_HH
#define ASTRAL_NOVA_FORMAT_HH

#include "ast.hh"
#include "types.hh"

#include <string>

namespace astral_internal {
namespace nova {

std::string format(const Unit &unit, const Types &types);
std::string format(const Function &function, const Types &types);
std::string format(const Expression &expression, const Types &types);

// How a type is spelled in Nova: `*i8`, `[u8; 64]`, `unknown32`.
std::string spell(TypePtr type, const Types &types);

// How storage is spelled after `@`: `x0`, `0x10000c0d0`, `-0x70`, `x29@entry`.
std::string spell(const Storage &storage);

} // namespace nova
} // namespace astral_internal

#endif
