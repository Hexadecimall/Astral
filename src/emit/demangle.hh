// Turning a C++ name into the identifier Astral shows for it.
#ifndef ASTRAL_DEMANGLE_HH
#define ASTRAL_DEMANGLE_HH

#include <string>

namespace astral_internal {

// The identifier a demangled C++ name is shown under: template and argument
// groups dropped, the inline versioning namespace removed, and what is left
// run together as camelCase. `std::__1::basic_string<char>::append(char const*)`
// becomes `stdBasicStringAppend`.
//
// Reading a header has to arrive at the same name a binary does, or a
// prototype learned from one will never meet the other.
std::string cxx_identifier(const std::string &demangled);

} // namespace astral_internal

#endif
