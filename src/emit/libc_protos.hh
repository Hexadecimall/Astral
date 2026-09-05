#ifndef ASTRAL_LIBC_PROTOS_HH
#define ASTRAL_LIBC_PROTOS_HH

namespace ghidra {
class Architecture;
}

#include <string>

namespace astral_internal {

// Replaces the pointer-sized-integer placeholders in a prototype with the core
// type names of the running architecture.
std::string size_types_for(const std::string &declaration);

// Describes well-known library functions the image imports, so their arguments
// and return values get real types. Returns how many were applied.
int apply_library_prototypes(ghidra::Architecture *arch);

} // namespace astral_internal

#endif
