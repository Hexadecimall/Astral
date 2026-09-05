// Turning a line of assembly back into the bytes it stands for.
//
// This is what makes a disassembly listing editable: a person changes a line
// and the instruction they wrote replaces the one that was there. It is
// deliberately not a whole assembler with sections, macros and a linker. The
// unit of work is one instruction at a known address, because that is what a
// patch is.
#ifndef ASTRAL_ASSEMBLER_HH
#define ASTRAL_ASSEMBLER_HH

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {
namespace assembler {

// The instruction sets Astral can write, as opposed to only read.
enum class Target {
    Unknown,
    Arm64,
};

// Picks the target from a language id such as "AARCH64:LE:64:AppleSilicon".
Target target_for_language(const std::string &language_id);
const char *target_name(Target target);

struct Result {
    bool ok = false;
    std::vector<uint8_t> bytes;
    // Why it could not be assembled, said the way someone editing a line needs
    // to hear it: what was wrong with what they wrote.
    std::string error;
};

// Assembles one instruction. `address` is where it will live, which matters for
// anything pc-relative: a branch is stored as a distance, so the same text is
// different bytes at a different address.
Result assemble(Target target, const std::string &text, uint64_t address);

// What can be written, so a refusal can say what is available rather than only
// what failed.
std::vector<std::string> known_mnemonics(Target target);

} // namespace assembler
} // namespace astral_internal

#endif
