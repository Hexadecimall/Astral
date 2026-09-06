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

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {
namespace nova {

// The architecture for `target`, which is a language id and optionally a
// compiler after it. Returns null and says why when the specification cannot be
// read. The architecture belongs to the cache and outlives the caller.
ghidra::SleighArchitecture *specification_for(const std::string &target, std::string &error);

// Reads bytes back the way the processor would, and says what it made of them.
//
// This is how a guess is checked. Choosing an instruction means proposing bytes
// and asking whether they mean what was wanted, and the only thing that can
// answer is the same reading the decompiler does. What comes back is how the
// instruction would be written out, empty when the bytes are not one.
//
// Nothing is remembered between calls: each is asked at an address of its own,
// because a reading is kept once made and asking twice at one address answers
// with the first bytes rather than the second.
std::string reads_as(const std::string &target, const std::vector<uint8_t> &bytes,
                     std::string &error);

} // namespace nova
} // namespace astral_internal

#endif
