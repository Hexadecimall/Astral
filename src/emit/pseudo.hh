#ifndef ASTRAL_PSEUDO_HH
#define ASTRAL_PSEUDO_HH

#include <string>

namespace astral_internal {

// Rearranges a printed listing into the shape a person reads it in: labels
// named in the order they appear rather than after an address, and each
// declaration standing where the value is first given rather than in a block
// before the code starts.
//
// Text in, text out. The printer decides what each thing is called; this
// decides where it goes, which is a question about whole lines and so is
// answered on the printed form, the way the compilable path already does it.
std::string readable_listing(const std::string &source);

} // namespace astral_internal

#endif
