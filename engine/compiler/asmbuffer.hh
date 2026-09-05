// A run of instructions with labels in it, laid out and then assembled.
//
// Code generation wants to say "branch to the end of this loop" before it
// knows where the end is. This holds the instructions with their labels, works
// out where each one lands, and only then asks the assembler for bytes, by
// which time every label is an address.
#ifndef ASTRAL_COMPILER_ASMBUFFER_HH
#define ASTRAL_COMPILER_ASMBUFFER_HH

#include "assembler/assembler.hh"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace astral_internal {
namespace compiler {

// One thing in the run: an instruction, a place a branch can name, or a note
// carried alongside for whoever reads the assembly.
struct AsmBufferLine {
    enum class Kind { Instruction, Label, Comment } kind = Kind::Instruction;
    std::string text;
    uint64_t address = 0;
    uint64_t size = 0;
};

class AsmBuffer {
public:
    AsmBuffer(assembler::Target target, uint64_t address);

    // One instruction, written the way the assembler reads it. Any `%label%`
    // in the text is replaced by that label's address once it is known.
    void instruction(const std::string &text);
    // Names the point the next instruction will occupy.
    void label(const std::string &name);
    // A name nothing has produced yet, for a fresh label.
    std::string fresh(const std::string &prefix);
    // A comment carried alongside the code, for the assembly the caller reads.
    void comment(const std::string &text);

    // Takes out instructions the run does not need: a move from a register to
    // itself, and a branch to the very next thing. Both fall out of generating
    // each idea on its own, and neither can be seen until the whole run exists.
    void tighten();
    // Takes out stores into the frame that nothing ever reads back. Only sound
    // when no address inside the frame was ever handed to anything, which the
    // pass checks first: `lowest` is the first offset it is allowed to touch,
    // below which lie the arguments being passed to something else.
    void drop_dead_frame_stores(int64_t lowest);

    // Whether anything written reads or writes the frame between two offsets,
    // or puts an address inside it in a register. A function that touches none
    // of it does not need any of it.
    bool uses_frame_between(int64_t low, int64_t high) const;

    bool empty() const { return lines_.empty(); }
    // The assembly as text, labels and all.
    std::string text() const;

    // Lays the instructions out and assembles them. Every instruction must be
    // a fixed width for the layout to settle in one pass; on a target where
    // that does not hold the buffer widens the estimate and settles by
    // repeating. Fills `error` and returns false if anything will not assemble.
    bool assemble(std::vector<uint8_t> &bytes, std::string &error);

private:
    using Line = AsmBufferLine;

    assembler::Target target_;
    uint64_t address_ = 0;
    std::vector<Line> lines_;
    int next_label_ = 0;
};

} // namespace compiler
} // namespace astral_internal

#endif
