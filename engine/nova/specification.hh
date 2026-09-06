// The processor specifications, kept once each.
//
// Loading one reads a compiled file and builds a translator out of it, which is
// slow enough that doing it per question makes compiling anything absurd. They
// are read-only once built and are asked the same things every time, so one of
// each is kept and shared.
//
// This is internal to the parts of Nova that ask a processor about itself. What
// comes back is the decompiler's own architecture, so anything using it is
// already reaching into the engine on purpose.
#ifndef ASTRAL_NOVA_SPECIFICATION_HH
#define ASTRAL_NOVA_SPECIFICATION_HH

#include "sleigh_arch.hh"

#include <string>

namespace astral_internal {
namespace nova {

// The architecture for `target`, which is a language id and optionally a
// compiler after it. Returns null and says why when the specification cannot be
// read. The architecture belongs to the cache and outlives the caller.
ghidra::SleighArchitecture *specification_for(const std::string &target, std::string &error);

} // namespace nova
} // namespace astral_internal

#endif
