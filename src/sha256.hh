#pragma once

#include <array>
#include <cstdint>
#include <vector>

// Minimal, dependency-free SHA-256. macOS requires every arm64 executable to
// carry at least an ad-hoc code signature (a CodeDirectory of per-page
// SHA-256 hashes) or the kernel refuses to run it - there's no way around
// needing this, and no crypto library is available to lean on, so it's
// implemented from scratch here.
std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len);
