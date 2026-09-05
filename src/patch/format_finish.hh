// Making a patched file whole again.
//
// Changing bytes inside an executable breaks whatever the format records about
// them. What has to be repaired differs by format, and doing nothing is only
// right for one of the three.
#ifndef ASTRAL_FORMAT_FINISH_HH
#define ASTRAL_FORMAT_FINISH_HH

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {

// Repairs `bytes` in place after a patch. Returns false and fills `error` only
// when the file is recognised but cannot be repaired; a format that needs no
// repair succeeds without touching anything. `note` says what was done, for a
// caller that wants to report it.
bool finish_patched_file(std::vector<uint8_t> &bytes, std::string &note, std::string &error);

// Whether these bytes start a Windows executable.
bool is_pe(const std::vector<uint8_t> &bytes);
// Whether these bytes start an ELF executable.
bool is_elf(const std::vector<uint8_t> &bytes);

// The checksum a PE header records over its own file.
uint32_t pe_checksum(const std::vector<uint8_t> &bytes, size_t checksum_offset);

} // namespace astral_internal

#endif
