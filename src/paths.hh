#ifndef ASTRAL_PATHS_HH
#define ASTRAL_PATHS_HH

#include <string>

namespace astral_internal {

// Where things are, worked out while running rather than written in at build
// time. A path baked into a binary is both a leak, since it names whoever built
// it, and a lie, since it stops being true the moment anything moves.

// The running program, and the library this code is part of.
std::string executable_path();
std::string library_path();

// The install this copy belongs to, found by walking up from the library or the
// program. Empty when it cannot be worked out.
std::string install_prefix();

// The compiled SLEIGH specifications: the ASTRAL_SPECS environment variable if
// set, then this install, then the usual system locations.
std::string default_spec_root();

} // namespace astral_internal

#endif
