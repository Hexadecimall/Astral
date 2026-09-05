// Nova, end to end: text in, bytes out, and the shortcuts that avoid most of it.
//
// Every level compiles. A function that is only instructions is assembled; a
// function that says where things live is checked against where they would
// live anyway; a function that says nothing about storage is compiled as a
// program. The caller does not pick a path, the text does.
//
// The incremental entry point matters more than the plain one. Recovered code
// is edited a line at a time, and a patch should cost what the edit cost:
// renaming a local costs nothing, changing a string costs its bytes, and only
// a changed function is generated again, and even then only the bytes that
// differ are written.
#ifndef ASTRAL_NOVA_NOVA_HH
#define ASTRAL_NOVA_NOVA_HH

#include "compiler/compiler.hh"

#include <cstdint>
#include <string>

namespace astral_internal {
namespace nova {

using compiler::Diagnostic;
using compiler::Environment;
using compiler::Options;
using compiler::Result;
using compiler::Update;

// Reads `source` and says whether it could become code, without making any.
// Diagnostics carry everything found; the return is whether none were errors.
bool check(const std::string &source, std::vector<Diagnostic> &diagnostics);

// Reads `source` and writes it back in the one layout Nova has. Returns false
// and leaves `formatted` empty when the source could not be read.
bool format_source(const std::string &source, std::string &formatted,
                   std::vector<Diagnostic> &diagnostics);

// Compiles `source` for `target` as if the function were loaded at `address`.
Result compile(assembler::Target target, const std::string &source, uint64_t address,
               const Environment &environment, const Options &options = Options());

// The same, stopping at the assembly.
Result compile_to_assembly(assembler::Target target, const std::string &source, uint64_t address,
                           const Environment &environment, const Options &options = Options());

// Compiles only what changed between `before` and `after`. See
// compiler::compile_update; the tiers are the same and are decided the same
// way, on the lowered tree, where names and spacing have already fallen away.
Result compile_update(assembler::Target target, const std::string &before, const std::string &after,
                      uint64_t address, const Environment &environment, Update &update,
                      const Options &options = Options());

} // namespace nova
} // namespace astral_internal

#endif
