#include "macho_sign.hh"

#include "sha256.hh"

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace astral_internal {

namespace {

constexpr uint32_t MH_MAGIC_64 = 0xfeedfacf;
constexpr uint32_t MH_CIGAM_64 = 0xcffaedfe;
constexpr uint32_t FAT_MAGIC = 0xcafebabe;
constexpr uint32_t FAT_CIGAM = 0xbebafeca;
constexpr uint32_t LC_SEGMENT_64 = 0x19;
constexpr uint32_t LC_CODE_SIGNATURE = 0x1d;
constexpr uint32_t CODESIG_PAGE_SIZE = 4096;

uint32_t rd_u32(const std::vector<uint8_t> &f, size_t off)
{
    return uint32_t(f[off]) | (uint32_t(f[off + 1]) << 8) | (uint32_t(f[off + 2]) << 16) |
           (uint32_t(f[off + 3]) << 24);
}

uint64_t rd_u64(const std::vector<uint8_t> &f, size_t off)
{
    uint64_t lo = rd_u32(f, off);
    uint64_t hi = rd_u32(f, off + 4);
    return lo | (hi << 32);
}

void wr_u32(std::vector<uint8_t> &f, size_t off, uint32_t v)
{
    f[off] = uint8_t(v);
    f[off + 1] = uint8_t(v >> 8);
    f[off + 2] = uint8_t(v >> 16);
    f[off + 3] = uint8_t(v >> 24);
}

// The signature blobs are big-endian, unlike the rest of Mach-O.
void put_u32be(std::vector<uint8_t> &b, uint32_t v)
{
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
void put_u64be(std::vector<uint8_t> &b, uint64_t v)
{
    put_u32be(b, uint32_t(v >> 32));
    put_u32be(b, uint32_t(v));
}
void put_u8(std::vector<uint8_t> &b, uint8_t v) { b.push_back(v); }
void put_bytes(std::vector<uint8_t> &b, const uint8_t *p, size_t n) { b.insert(b.end(), p, p + n); }

} // namespace

bool is_macho(const std::vector<uint8_t> &file)
{
    if (file.size() < 4)
        return false;
    uint32_t m = rd_u32(file, 0);
    return m == MH_MAGIC_64 || m == MH_CIGAM_64 || m == FAT_MAGIC || m == FAT_CIGAM;
}

bool macho_adhoc_sign(std::vector<uint8_t> &file, std::string &error)
{
    if (file.size() < 32) {
        error = "file too small to be Mach-O";
        return false;
    }
    uint32_t magic = rd_u32(file, 0);
    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        error = "universal binaries are not signed natively yet";
        return false;
    }
    if (magic != MH_MAGIC_64) {
        error = "not a little-endian 64-bit Mach-O";
        return false;
    }

    uint32_t ncmds = rd_u32(file, 16);
    uint32_t sizeofcmds = rd_u32(file, 20);
    size_t cmds_start = 32; // 64-bit Mach-O header is 32 bytes
    if (cmds_start + sizeofcmds > file.size()) {
        error = "load commands run past the file";
        return false;
    }

    // Walk the load commands for the signature slot and the two segments whose
    // sizes the CodeDirectory records.
    size_t sig_cmd_pos = 0;
    uint32_t sig_dataoff = 0, sig_datasize = 0;
    uint64_t text_filesize = 0;
    size_t linkedit_cmd_pos = 0;
    uint64_t linkedit_fileoff = 0, linkedit_filesize = 0;

    size_t p = cmds_start;
    for (uint32_t i = 0; i < ncmds; ++i) {
        if (p + 8 > cmds_start + sizeofcmds)
            break;
        uint32_t cmd = rd_u32(file, p);
        uint32_t cmdsize = rd_u32(file, p + 4);
        if (cmdsize < 8 || p + cmdsize > cmds_start + sizeofcmds) {
            error = "malformed load command";
            return false;
        }
        if (cmd == LC_CODE_SIGNATURE) {
            sig_cmd_pos = p;
            sig_dataoff = rd_u32(file, p + 8);
            sig_datasize = rd_u32(file, p + 12);
        } else if (cmd == LC_SEGMENT_64) {
            char name[17] = {0};
            std::memcpy(name, &file[p + 8], 16);
            uint64_t fileoff = rd_u64(file, p + 40);
            uint64_t filesize = rd_u64(file, p + 48);
            if (std::strcmp(name, "__TEXT") == 0)
                text_filesize = filesize;
            else if (std::strcmp(name, "__LINKEDIT") == 0) {
                linkedit_cmd_pos = p;
                linkedit_fileoff = fileoff;
                linkedit_filesize = filesize;
            }
        }
        p += cmdsize;
    }

    if (sig_cmd_pos == 0) {
        error = "no code-signature slot to rewrite";
        return false;
    }
    (void)sig_datasize;

    // The signed range is everything before the signature. Its start is fixed;
    // only the hashes over the patched bytes change.
    uint32_t code_limit = sig_dataoff;
    uint32_t n_slots = (code_limit + CODESIG_PAGE_SIZE - 1) / CODESIG_PAGE_SIZE;

    const std::string identifier = "a"; // ad-hoc: any stable identifier is fine
    uint32_t fixed_len = 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 1 + 1 + 1 + 1 + 4 + 4 + 4 + 4 + 8 + 8 + 8 + 8;
    uint32_t ident_off = fixed_len;
    uint32_t hash_off = ident_off + uint32_t(identifier.size() + 1);
    uint32_t cd_length = hash_off + n_slots * 32;
    uint32_t sig_size = 12 + 8 + cd_length; // SuperBlob header + one blob index + CD

    // Make room for the new signature, and let __LINKEDIT cover it.
    size_t need = size_t(sig_dataoff) + sig_size;
    if (file.size() < need)
        file.resize(need, 0);
    uint64_t new_linkedit_filesize = need - linkedit_fileoff;
    if (linkedit_cmd_pos != 0 && new_linkedit_filesize > linkedit_filesize) {
        // filesize and vmsize live at +48 and +32 in a segment_command_64.
        // vmsize must be page-aligned to the 16KB segment alignment.
        uint64_t vmsize = (new_linkedit_filesize + 0x3fff) & ~uint64_t(0x3fff);
        // vmsize at +32, filesize at +48.
        for (int b = 0; b < 8; ++b)
            file[linkedit_cmd_pos + 32 + b] = uint8_t(vmsize >> (8 * b));
        for (int b = 0; b < 8; ++b)
            file[linkedit_cmd_pos + 48 + b] = uint8_t(new_linkedit_filesize >> (8 * b));
    }

    // datasize sits inside the hashed range, so it has to be right before the
    // pages are hashed.
    wr_u32(file, sig_cmd_pos + 12, sig_size);

    // Build the CodeDirectory over the now-final bytes.
    std::vector<uint8_t> cd;
    put_u32be(cd, 0xfade0c02);       // magic
    put_u32be(cd, cd_length);        // length
    put_u32be(cd, 0x00020400);       // version (supports execSeg fields)
    put_u32be(cd, 0x00000002);       // flags: CS_ADHOC
    put_u32be(cd, hash_off);
    put_u32be(cd, ident_off);
    put_u32be(cd, 0);                // nSpecialSlots
    put_u32be(cd, n_slots);
    put_u32be(cd, code_limit);
    put_u8(cd, 32);                  // hashSize
    put_u8(cd, 2);                   // hashType SHA256
    put_u8(cd, 0);                   // platform
    put_u8(cd, 12);                  // pageSize log2(4096)
    put_u32be(cd, 0);                // spare2
    put_u32be(cd, 0);                // scatterOffset
    put_u32be(cd, 0);                // teamOffset
    put_u32be(cd, 0);                // spare3
    put_u64be(cd, 0);                // codeLimit64
    put_u64be(cd, 0);                // execSegBase
    put_u64be(cd, text_filesize);    // execSegLimit
    put_u64be(cd, 1);                // execSegFlags: MAIN_BINARY
    put_bytes(cd, reinterpret_cast<const uint8_t *>(identifier.c_str()), identifier.size() + 1);

    for (uint32_t i = 0; i < n_slots; ++i) {
        size_t start = size_t(i) * CODESIG_PAGE_SIZE;
        size_t len = std::min<size_t>(CODESIG_PAGE_SIZE, code_limit - start);
        auto h = sha256(file.data() + start, len);
        put_bytes(cd, h.data(), h.size());
    }

    std::vector<uint8_t> sig;
    put_u32be(sig, 0xfade0cc0);      // SuperBlob magic
    put_u32be(sig, 12 + 8 + uint32_t(cd.size()));
    put_u32be(sig, 1);               // one blob
    put_u32be(sig, 0);               // CSSLOT_CODEDIRECTORY
    put_u32be(sig, 12 + 8);          // offset to the CodeDirectory
    put_bytes(sig, cd.data(), cd.size());

    if (sig.size() != sig_size) {
        error = "internal signature size mismatch";
        return false;
    }
    std::memcpy(&file[sig_dataoff], sig.data(), sig.size());
    return true;
}

bool codesign_adhoc(const std::string &path, std::string &error)
{
    // Quote the path so a space in it survives the shell.
    std::string quoted;
    quoted.reserve(path.size() + 2);
    quoted.push_back('\'');
    for (char c : path) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted.push_back(c);
    }
    quoted.push_back('\'');
    std::string command = "codesign --force --sign - " + quoted + " 2>/dev/null";
    int rc = std::system(command.c_str());
    if (rc != 0) {
        error = "codesign is unavailable or failed";
        return false;
    }
    return true;
}

} // namespace astral_internal
