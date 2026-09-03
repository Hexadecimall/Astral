#ifndef ASTRAL_IMAGE_HH
#define ASTRAL_IMAGE_HH

#include "astral/astral.h"

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {

// One mapped region of an executable. `data` is the file content for the region;
// when it is shorter than `size` the remainder is zero-filled .bss-style memory.
struct Segment {
    std::string name;
    uint64_t address = 0;
    uint64_t size = 0;
    bool readable = true;
    bool writable = false;
    bool executable = false;
    std::vector<uint8_t> data;
};

struct Symbol {
    std::string name;
    uint64_t address = 0;
    uint64_t size = 0;
    bool is_function = false;
    // A stub or slot standing in for a function in another image. Its name is
    // worth having so calls read properly, but its body is not this program's
    // code and should not be decompiled or emitted.
    bool is_import = false;
};

// Architecture facts a loader can read out of a container format. These feed the
// language-id lookup in langmap.cc.
struct ArchHint {
    std::string machine;   // canonical processor name, e.g. "x86", "AARCH64"
    int bits = 0;          // 16, 32 or 64
    bool big_endian = false;
    std::string abi;       // "gcc", "windows", "macos", "" when unknown
    unsigned raw_machine = 0; // format-specific machine code, kept for diagnostics
};

// Format-neutral view of an executable. Every loader produces one of these; the
// decompiler session never sees the container format again.
struct BinaryImage {
    astral_format format = ASTRAL_FORMAT_UNKNOWN;
    std::string format_name = "unknown";
    std::string path;
    ArchHint arch;
    uint64_t image_base = 0;
    std::vector<uint64_t> entry_points;
    std::vector<Segment> segments;
    std::vector<Symbol> symbols;

    // Copies up to `size` bytes at `address` into `out`, zero-filling any part
    // that falls in a mapped but uninitialized region. Returns bytes covered by
    // some segment; a short count means the range runs off the end of the image.
    size_t read(uint64_t address, uint8_t *out, size_t size) const;

    // True when any segment covers the address.
    bool contains(uint64_t address) const;

    void sort_and_dedup_symbols();
};

// Each loader returns true when the bytes are its format. `error` is filled in
// only when the format matched but the file could not be parsed.
bool load_elf(const std::vector<uint8_t> &bytes, BinaryImage &out, std::string &error);
bool load_pe(const std::vector<uint8_t> &bytes, BinaryImage &out, std::string &error);
bool load_macho(const std::vector<uint8_t> &bytes, BinaryImage &out, std::string &error);

// Runs every loader in turn. Returns false and sets `error` when nothing matched.
bool load_any(const std::vector<uint8_t> &bytes, BinaryImage &out, std::string &error);

// Builds a single-segment image covering the whole buffer.
BinaryImage make_raw_image(const std::vector<uint8_t> &bytes, uint64_t base_address);

// Reads a whole file. Returns false and sets `error` on failure.
bool read_file(const std::string &path, std::vector<uint8_t> &out, std::string &error);

} // namespace astral_internal

#endif
