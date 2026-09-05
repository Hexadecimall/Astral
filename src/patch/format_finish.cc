#include "format_finish.hh"

#include "macho_sign.hh"
#include <cstdint>

namespace astral_internal {
namespace {

uint16_t read16(const std::vector<uint8_t> &bytes, size_t at)
{
    if (at + 1 >= bytes.size())
        return 0;
    return static_cast<uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

uint32_t read32(const std::vector<uint8_t> &bytes, size_t at)
{
    if (at + 3 >= bytes.size())
        return 0;
    return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

void write32(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
{
    if (at + 3 >= bytes.size())
        return;
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8);
    bytes[at + 2] = static_cast<uint8_t>(value >> 16);
    bytes[at + 3] = static_cast<uint8_t>(value >> 24);
}

// Where the PE header proper starts, or zero if this is not one.
size_t pe_header_offset(const std::vector<uint8_t> &bytes)
{
    if (bytes.size() < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z')
        return 0;
    const uint32_t at = read32(bytes, 0x3c);
    if (at == 0 || static_cast<size_t>(at) + 24 > bytes.size())
        return 0;
    if (bytes[at] != 'P' || bytes[at + 1] != 'E' || bytes[at + 2] != 0 || bytes[at + 3] != 0)
        return 0;
    return at;
}

} // namespace

bool is_pe(const std::vector<uint8_t> &bytes) { return pe_header_offset(bytes) != 0; }

bool is_elf(const std::vector<uint8_t> &bytes)
{
    return bytes.size() > 4 && bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' &&
           bytes[3] == 'F';
}

uint32_t pe_checksum(const std::vector<uint8_t> &bytes, size_t checksum_offset)
{
    // A sum of the file read as sixteen-bit words, carrying the overflow back
    // in, with the field being computed read as zero. The file's length is
    // added at the end.
    uint32_t sum = 0;
    for (size_t at = 0; at + 1 < bytes.size(); at += 2) {
        if (at == checksum_offset || at == checksum_offset + 2)
            continue; // the field itself counts as zero
        sum += read16(bytes, at);
        sum = (sum & 0xffff) + (sum >> 16);
    }
    if (bytes.size() % 2 != 0) {
        sum += bytes.back();
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum = (sum & 0xffff) + (sum >> 16);
    return sum + static_cast<uint32_t>(bytes.size());
}

bool finish_patched_file(std::vector<uint8_t> &bytes, std::string &note, std::string &error)
{
    note.clear();
    error.clear();

    if (is_macho(bytes)) {
        // Apple Silicon refuses a binary whose signature no longer covers its
        // bytes, so a patched one has to be signed again before it will run.
        std::string sign_error;
        if (!macho_adhoc_sign(bytes, sign_error)) {
            error = sign_error;
            return false;
        }
        note = "re-signed";
        return true;
    }

    const size_t header = pe_header_offset(bytes);
    if (header != 0) {
        // The optional header follows the twenty-byte file header; its checksum
        // sits sixty-four bytes in, whichever of the two forms it takes.
        const size_t optional = header + 24;
        const uint16_t magic = read16(bytes, optional);
        if (magic != 0x10b && magic != 0x20b) {
            note = "left alone: this PE has no optional header to repair";
            return true;
        }
        const size_t checksum_at = optional + 64;
        if (checksum_at + 4 > bytes.size()) {
            note = "left alone: this PE is truncated before its checksum";
            return true;
        }
        if (read32(bytes, checksum_at) == 0) {
            // Nothing recorded one, so nothing expects one.
            note = "no checksum to repair";
            return true;
        }
        write32(bytes, checksum_at, pe_checksum(bytes, checksum_at));
        note = "checksum recomputed";
        return true;
    }

    if (is_elf(bytes)) {
        // An ELF records nothing about its own bytes: no checksum, no
        // signature. The patched file is complete as it stands.
        note = "nothing to repair";
        return true;
    }

    note = "nothing to repair";
    return true;
}

} // namespace astral_internal
