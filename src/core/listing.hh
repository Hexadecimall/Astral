// A disassembly listing meant to be read, and edited.
//
// The raw form of a listing answers "what bytes are here". What someone reading
// or patching a function needs is "what does this do": where a call goes by
// name, which branch comes back to which point, and what a loaded address holds.
// The addresses stay, because that is what a patch is addressed by.
#ifndef ASTRAL_LISTING_HH
#define ASTRAL_LISTING_HH

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {

struct BinaryImage;

// One line of a listing.
struct ListingLine {
    uint64_t address = 0;
    // The instruction as it would be written back, with targets named. Empty on
    // a label line, whose text is the label.
    std::string text;
    // A label this address is the target of, without its colon.
    std::string label;
    // What the instruction refers to, said in words: the string it loads, or
    // the function it calls. Shown beside the instruction, never inside it.
    std::string comment;
    bool is_label = false;
};

// Renders `raw`, as produced by the disassembler, into something readable.
// `names` maps an address to what lives there.
std::vector<ListingLine> readable_listing(const std::string &raw, const BinaryImage &image,
                                          uint64_t start, uint64_t end);

// The same, as text, with the labels on their own lines.
std::string readable_listing_text(const std::string &raw, const BinaryImage &image, uint64_t start,
                                  uint64_t end);

} // namespace astral_internal

#endif
