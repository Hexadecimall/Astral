#include "image.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace astral_internal {

size_t BinaryImage::read(uint64_t address, uint8_t *out, size_t size) const
{
    std::memset(out, 0, size);
    size_t covered = 0;
    for (const Segment &seg : segments) {
        if (address + size <= seg.address || address >= seg.address + seg.size)
            continue;
        uint64_t start = std::max(address, seg.address);
        uint64_t end = std::min(address + size, seg.address + seg.size);
        if (end <= start)
            continue;
        uint64_t seg_off = start - seg.address;
        size_t chunk = static_cast<size_t>(end - start);
        // Bytes past the end of `data` are the uninitialized tail of the segment
        // and stay zero.
        if (seg_off < seg.data.size()) {
            size_t avail = std::min<size_t>(chunk, seg.data.size() - seg_off);
            std::memcpy(out + (start - address), seg.data.data() + seg_off, avail);
        }
        covered += chunk;
    }
    return std::min(covered, size);
}

bool BinaryImage::contains(uint64_t address) const
{
    for (const Segment &seg : segments)
        if (address >= seg.address && address < seg.address + seg.size)
            return true;
    return false;
}

void BinaryImage::sort_and_dedup_symbols()
{
    std::stable_sort(symbols.begin(), symbols.end(), [](const Symbol &a, const Symbol &b) {
        if (a.address != b.address)
            return a.address < b.address;
        return a.name < b.name;
    });
    auto same = [](const Symbol &a, const Symbol &b) {
        return a.address == b.address && a.name == b.name && a.is_import == b.is_import;
    };
    symbols.erase(std::unique(symbols.begin(), symbols.end(), same), symbols.end());
}

BinaryImage make_raw_image(const std::vector<uint8_t> &bytes, uint64_t base_address)
{
    BinaryImage image;
    image.format = ASTRAL_FORMAT_RAW;
    image.format_name = "raw";
    image.image_base = base_address;
    Segment seg;
    seg.name = "image";
    seg.address = base_address;
    seg.size = bytes.size();
    seg.executable = true;
    seg.writable = true;
    seg.data = bytes;
    image.segments.push_back(std::move(seg));
    image.entry_points.push_back(base_address);
    return image;
}

bool load_any(const std::vector<uint8_t> &bytes, BinaryImage &out, std::string &error)
{
    if (load_elf(bytes, out, error))
        return error.empty();
    if (load_pe(bytes, out, error))
        return error.empty();
    if (load_macho(bytes, out, error))
        return error.empty();
    error = "unrecognized executable format (not ELF, PE or Mach-O)";
    return false;
}

bool read_file(const std::string &path, std::vector<uint8_t> &out, std::string &error)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = "cannot open " + path;
        return false;
    }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        error = "cannot seek in " + path;
        return false;
    }
    long len = std::ftell(f);
    if (len < 0) {
        std::fclose(f);
        error = "cannot size " + path;
        return false;
    }
    std::rewind(f);
    out.resize(static_cast<size_t>(len));
    if (len > 0 && std::fread(out.data(), 1, out.size(), f) != out.size()) {
        std::fclose(f);
        error = "short read on " + path;
        return false;
    }
    std::fclose(f);
    return true;
}

} // namespace astral_internal
