#ifndef ASTRAL_LANGMAP_HH
#define ASTRAL_LANGMAP_HH

#include "image.hh"

#include <string>

namespace astral_internal {

// Picks a full Ghidra architecture id ("<processor>:<endian>:<size>:<variant>:<compiler>")
// for what a loader read out of the file. Returns an empty string and sets
// `error` when the specification tree has nothing suitable.
std::string choose_architecture(const ArchHint &hint, std::string &error);

// Completes a user-supplied id: a four-field language id gains the best
// compiler for it, a five-field id is returned as-is.
std::string complete_architecture(const std::string &language_id, const std::string &abi,
                                  std::string &error);

} // namespace astral_internal

#endif
