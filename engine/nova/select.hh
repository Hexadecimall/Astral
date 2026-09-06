// Choosing the instructions that do what the p-code says, and writing them.
//
// The catalogue says what a processor can do and what each way of doing it
// costs. This picks between them: for each operation, the forms that do it, the
// ones whose slots can hold the registers wanted, and of those the shortest.
//
// Shortest is the right thing to prefer and not an arbitrary one. A recovered
// function being patched has a fixed number of bytes to fit inside, and the
// difference between fitting and not is which encoding was chosen - a RISC-V
// add is two bytes one way and four the other. Somewhere to say "and no more
// than this many bytes altogether" belongs here, and is not here yet.
//
// What is not done here is deciding where values live. An operation over
// registers can be written; one over a value that has no home yet cannot,
// because there is nothing to name. Giving those values homes is register
// allocation, it is a pass over the representation rather than a question for
// this, and until it exists this refuses such operations by name rather than
// inventing a register to put them in.
#ifndef ASTRAL_NOVA_SELECT_HH
#define ASTRAL_NOVA_SELECT_HH

#include "catalogue.hh"
#include "ir.hh"
#include "pcode.hh"

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {
namespace nova {
namespace select {

// One operation, written.
struct Chosen {
    std::vector<uint8_t> bytes;
    // Which form was used, for anyone asking why these bytes and not others.
    const catalogue::Form *form = nullptr;
};

// Writes what it can of `sequence` for `target`.
//
// Everything written is in `out`, in the order it was written. Everything that
// could not be is in `problems`, said plainly, and the return says whether
// there were none: a partial answer is still worth having while the reasons it
// is partial are worth reading.
bool write(const pcode::Sequence &sequence, const ir::Target &target,
           const catalogue::Catalogue &catalogue, std::vector<Chosen> &out,
           std::vector<std::string> &problems);

} // namespace select
} // namespace nova
} // namespace astral_internal

#endif
