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

    bool empty() const { return lines_.empty(); }
    // The assembly as text, labels and all.
    std::string text() const;

    // Lays the instructions out and assembles them. Every instruction must be
    // a fixed width for the layout to settle in one pass; on a target where
    // that does not hold the buffer widens the estimate and settles by
    // repeating. Fills `error` and returns false if anything will not assemble.
    bool assemble(std::vector<uint8_t> &bytes, std::string &error);

private:
    struct Line {
        enum class Kind { Instruction, Label, Comment } kind = Kind::Instruction;
        std::string text;
        uint64_t address = 0;
        uint64_t size = 0;
    };

    assembler::Target target_;
    uint64_t address_ = 0;
    std::vector<Line> lines_;
    int next_label_ = 0;
};

} // namespace compiler
} // namespace astral_internal

#endif
