// Rewrites C++ library idioms in an emitted C unit into the C that does the
// same thing: stream output becomes printf.
#pragma once

#include <string>

namespace astral_internal {

struct BinaryImage;

// `std::cout << a << b` recovered as nested inserter calls becomes one printf
// with a format built from the inserters' argument types. Inserters whose
// argument has no C equivalent (a std::string object) are left as calls.
void rewrite_stream_idioms(std::string &unit, const BinaryImage &image);

} // namespace astral_internal
