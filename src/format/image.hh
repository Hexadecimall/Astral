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
    // Byte position in the source file where `data` begins. Meaningful only when
    // `has_file_offset` is set; a .bss-style region maps to no file bytes and a
    // patch there cannot be written back.
    uint64_t file_offset = 0;
    bool has_file_offset = false;
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
    // Named for other images to call. A library's exports are its whole point:
    // everything else in it is an implementation detail.
    bool is_exported = false;
    // For a demangled C++ symbol: the original mangled name, so the rebuilt
    // unit can still link against the real thing.
    std::string linkage_name;
    // A prototype in the engine's type names, read out of the mangled
    // signature ("extern void *stdOperatorShl(void *os, char *s);").
    std::string prototype;
    // For a stream inserter: the printf conversion that writes the same thing
    // ("%s", "%d"), or "manip" for an endl-style manipulator.
    std::string stream_format;
    // What a C++ library function does, when it is one the idiom pass can
    // write in C: "stringFromCStr", "stringDestroy", "stringEqCStr", ...
    std::string role;
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

    // Maps a virtual address to a byte position in the source file. Returns
    // false when the address lands in a segment with no file backing (.bss) or
    // in no segment at all: nothing there can be patched onto disk.
    bool file_offset_for(uint64_t address, uint64_t &out) const;

    // Overwrites up to `size` bytes at `address` in the in-memory segment data,
    // so a later read reflects the change. Returns bytes actually written; a
    // short count means the range ran off the mapped data. Used to make a
    // queued patch visible to disassembly and decompilation right away.
    size_t write(uint64_t address, const uint8_t *data, size_t size);

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

// Renames C++ mangled symbols (_Z...) to readable, valid C identifiers.
void demangle_symbols(BinaryImage &image);

// Reads a whole file. Returns false and sets `error` on failure.
bool read_file(const std::string &path, std::vector<uint8_t> &out, std::string &error);

} // namespace astral_internal

#endif
