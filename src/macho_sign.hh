#ifndef ASTRAL_MACHO_SIGN_HH
#define ASTRAL_MACHO_SIGN_HH

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {

// True when the bytes begin with a Mach-O magic (thin or fat).
bool is_macho(const std::vector<uint8_t> &file);

// Re-signs a thin arm64 Mach-O in place with an ad-hoc signature, recomputing
// the per-page SHA-256 hashes over the (patched) bytes so the kernel will run
// it. Apple Silicon refuses to execute an arm64 binary whose signature no
// longer matches, so any patched Mach-O has to pass through here.
//
// Works on a thin arm64 image that already carries an LC_CODE_SIGNATURE, which
// is every arm64 executable a modern toolchain produces. Returns false with a
// reason for anything else (a fat binary, an unsigned one), so the caller can
// fall back to the platform `codesign` tool.
bool macho_adhoc_sign(std::vector<uint8_t> &file, std::string &error);

// Runs `codesign --force --sign - <path>`. The platform's own tool, used only
// when the native signer above cannot handle a file. Returns false when
// codesign is absent or fails.
bool codesign_adhoc(const std::string &path, std::string &error);

} // namespace astral_internal

#endif
