// Giving values somewhere to live.
//
// The representation lets a value have no home. That is not laziness: a
// function being read has values whose whereabouts nobody has worked out yet,
// and a form that demanded one could not hold such a function at all. But an
// instruction can only name a place, so before anything can be written every
// value needs one.
//
// This is that pass. Values that were pinned keep what they were pinned to -
// that is the whole reason the pin was carried this far - and the rest are
// given registers the processor has and this function is not otherwise using.
//
// What it does not do is spill. When a function wants more values at once than
// the processor has registers, some have to live in the frame and be fetched
// back, and choosing which is a question about how often each is read that this
// does not ask. It says so plainly instead, because a wrong answer there is a
// program that is quietly slower or quietly wrong, and no answer is neither.
#ifndef ASTRAL_NOVA_HOMES_HH
#define ASTRAL_NOVA_HOMES_HH

#include "catalogue.hh"
#include "ir.hh"
#include "pcode.hh"

#include <string>
#include <vector>

namespace astral_internal {
namespace nova {
namespace homes {

// Gives every value in `sequence` a place, in `sequence` itself.
//
// Returns false and says why when it cannot: too many values wanted at once, or
// a processor with too few registers to hold them. Nothing is half-done - a
// sequence that could not be housed is left as it was.
// `catalogue`, when given, says which registers this processor keeps values in.
// Without it the calling convention is asked instead, and that has an opinion
// about the floating-point file as well - which is how a value ended up in d10,
// where nothing that adds can reach it.
bool give(pcode::Sequence &sequence, const ir::Target &target,
          std::vector<std::string> &problems,
          const catalogue::Catalogue *catalogue = nullptr);

} // namespace homes
} // namespace nova
} // namespace astral_internal

#endif
