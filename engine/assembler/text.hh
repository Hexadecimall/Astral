// Reading the text of one instruction: the parts every architecture needs
// before it can start deciding what to encode.
#ifndef ASTRAL_ASSEMBLER_TEXT_HH
#define ASTRAL_ASSEMBLER_TEXT_HH

#include "assembler.hh"

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {
namespace assembler {

std::string lower(std::string text);
std::string trim(std::string text);

// Splits operands, keeping a bracketed memory operand whole: "x0, [sp, #0x10]"
// is two operands, not three.
std::vector<std::string> split_operands(const std::string &text);

// An immediate, written the way a listing prints one: #0x30, $48, 0x10, -8.
bool parse_immediate(const std::string &text, int64_t &out);

// One instruction, split into the mnemonic and its operands, with any trailing
// comment removed.
struct Line {
    std::string mnemonic;             // lower case
    std::vector<std::string> operands;
};
bool split_line(const std::string &text, Line &out);

Result fail(const std::string &why);
Result bytes_of(const std::vector<uint8_t> &bytes);
Result word_of(uint32_t word);        // four bytes, little endian
Result halfword_of(uint16_t value);   // two bytes, little endian

// "add takes 3 operands, not 2".
Result wrong_operand_count(const Line &line, size_t wanted);
// "Astral does not write frobnicate yet. It writes: ..."
Result unknown_mnemonic(const Line &line, Target target);

} // namespace assembler
} // namespace astral_internal

#endif
