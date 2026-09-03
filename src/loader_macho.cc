// Mach-O reader, including thin slice selection out of a universal binary.
#include "image.hh"

#include <cstring>
#include <vector>

namespace astral_internal {
namespace {

constexpr uint32_t MH_MAGIC = 0xfeedface;
constexpr uint32_t MH_CIGAM = 0xcefaedfe;
constexpr uint32_t MH_MAGIC_64 = 0xfeedfacf;
constexpr uint32_t MH_CIGAM_64 = 0xcffaedfe;
constexpr uint32_t FAT_MAGIC = 0xcafebabe;
constexpr uint32_t FAT_CIGAM = 0xbebafeca;

constexpr uint32_t LC_SEGMENT = 0x1;
constexpr uint32_t LC_DYSYMTAB = 0xb;
constexpr uint32_t LC_SEGMENT_64 = 0x19;
constexpr uint32_t LC_SYMTAB = 0x2;
constexpr uint32_t LC_MAIN = 0x80000028;
constexpr uint32_t LC_UNIXTHREAD = 0x5;

constexpr uint32_t CPU_TYPE_X86 = 7;
constexpr uint32_t CPU_TYPE_X86_64 = 0x01000007;
constexpr uint32_t CPU_TYPE_ARM = 12;
constexpr uint32_t CPU_TYPE_ARM64 = 0x0100000c;
constexpr uint32_t CPU_TYPE_POWERPC = 18;
constexpr uint32_t CPU_TYPE_POWERPC64 = 0x01000012;

constexpr uint32_t N_STAB = 0xe0;
constexpr uint32_t N_TYPE = 0x0e;
constexpr uint32_t N_SECT = 0x0e;

// Section types that stand in for something in another image.
constexpr uint32_t S_NON_LAZY_SYMBOL_POINTERS = 6;
constexpr uint32_t S_LAZY_SYMBOL_POINTERS = 7;
constexpr uint32_t S_SYMBOL_STUBS = 8;

// Indirect symbol table entries that name nothing.
constexpr uint32_t INDIRECT_SYMBOL_LOCAL = 0x80000000;
constexpr uint32_t INDIRECT_SYMBOL_ABS = 0x40000000;

// A section whose entries each stand for an imported symbol.
struct IndirectSection {
    uint64_t address = 0;
    uint64_t size = 0;
    uint32_t type = 0;
    uint32_t first_indirect = 0;
    uint32_t entry_size = 0;
};

struct Reader {
    const uint8_t *b;
    size_t n;
    bool big;
    bool ok = true;

    uint64_t u(size_t off, int width)
    {
        if (off + static_cast<size_t>(width) > n) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < width; ++i)
            v = (v << 8) | b[off + static_cast<size_t>(big ? i : width - 1 - i)];
        return v;
    }
    uint32_t u32(size_t off) { return static_cast<uint32_t>(u(off, 4)); }
    uint64_t u64(size_t off) { return u(off, 8); }
    std::string fixed(size_t off, size_t len)
    {
        if (off + len > n)
            return std::string();
        size_t end = off;
        while (end < off + len && b[end] != 0)
            ++end;
        return std::string(reinterpret_cast<const char *>(b + off), end - off);
    }
    std::string cstr(size_t off)
    {
        if (off >= n)
            return std::string();
        size_t end = off;
        while (end < n && b[end] != 0)
            ++end;
        return std::string(reinterpret_cast<const char *>(b + off), end - off);
    }
};

bool arch_from_cputype(uint32_t cputype, ArchHint &hint)
{
    switch (cputype) {
    case CPU_TYPE_X86: hint.machine = "x86"; hint.bits = 32; break;
    case CPU_TYPE_X86_64: hint.machine = "x86"; hint.bits = 64; break;
    case CPU_TYPE_ARM: hint.machine = "ARM"; hint.bits = 32; break;
    case CPU_TYPE_ARM64: hint.machine = "AARCH64"; hint.bits = 64; break;
    case CPU_TYPE_POWERPC: hint.machine = "PowerPC"; hint.bits = 32; hint.big_endian = true; break;
    case CPU_TYPE_POWERPC64: hint.machine = "PowerPC"; hint.bits = 64; hint.big_endian = true; break;
    default: return false;
    }
    hint.raw_machine = cputype;
    hint.abi = "macos";
    return true;
}

// Parses one thin Mach-O image. `base` is the slice offset inside `bytes`.
bool parse_thin(const std::vector<uint8_t> &bytes, size_t base, BinaryImage &out, std::string &error)
{
    Reader head{bytes.data() + base, bytes.size() - base, false};
    uint32_t magic = head.u32(0);
    bool is64 = magic == MH_MAGIC_64 || magic == MH_CIGAM_64;
    bool big = magic == MH_CIGAM || magic == MH_CIGAM_64;
    Reader r{bytes.data() + base, bytes.size() - base, big};

    out.format = ASTRAL_FORMAT_MACHO;
    out.format_name = "Mach-O";

    uint32_t cputype = r.u32(4);
    uint32_t ncmds = r.u32(16);
    size_t cmd_off = is64 ? 32 : 28;
    if (!arch_from_cputype(cputype, out.arch)) {
        error = "Mach-O: unsupported cputype " + std::to_string(cputype);
        return true;
    }
    out.arch.big_endian = big || out.arch.big_endian;

    uint64_t lowest = UINT64_MAX;
    uint64_t text_vmaddr = 0;
    bool have_text = false;
    uint32_t symbol_offset = 0, symbol_count = 0, string_offset = 0;
    uint32_t indirect_offset = 0, indirect_count = 0;
    std::vector<IndirectSection> indirect_sections;

    for (uint32_t i = 0; i < ncmds; ++i) {
        uint32_t cmd = r.u32(cmd_off);
        uint32_t cmdsize = r.u32(cmd_off + 4);
        if (!r.ok || cmdsize < 8)
            break;

        if (cmd == LC_SEGMENT || cmd == LC_SEGMENT_64) {
            const bool seg64 = cmd == LC_SEGMENT_64;
            std::string name = r.fixed(cmd_off + 8, 16);
            size_t p = cmd_off + 24;
            uint64_t vmaddr = seg64 ? r.u64(p) : r.u32(p);
            uint64_t vmsize = seg64 ? r.u64(p + (seg64 ? 8 : 4)) : r.u32(p + 4);
            uint64_t fileoff = seg64 ? r.u64(p + 16) : r.u32(p + 8);
            uint64_t filesize = seg64 ? r.u64(p + 24) : r.u32(p + 12);
            uint32_t maxprot = seg64 ? r.u32(p + 32) : r.u32(p + 16);
            uint32_t initprot = seg64 ? r.u32(p + 36) : r.u32(p + 20);
            (void)maxprot;
            if (name == "__PAGEZERO" || vmsize == 0) {
                cmd_off += cmdsize;
                continue;
            }
            if (name == "__TEXT" && !have_text) {
                text_vmaddr = vmaddr;
                have_text = true;
            }
            Segment seg;
            seg.name = name;
            seg.address = vmaddr;
            seg.size = vmsize;
            seg.readable = (initprot & 1) != 0;
            seg.writable = (initprot & 2) != 0;
            seg.executable = (initprot & 4) != 0;
            size_t abs_off = base + static_cast<size_t>(fileoff);
            if (abs_off < bytes.size()) {
                size_t avail = std::min<size_t>(static_cast<size_t>(filesize), bytes.size() - abs_off);
                seg.data.assign(bytes.begin() + static_cast<long>(abs_off),
                                bytes.begin() + static_cast<long>(abs_off + avail));
            }
            lowest = std::min(lowest, vmaddr);
            out.segments.push_back(std::move(seg));

            // Sections whose entries each stand for an imported symbol are
            // resolved once the symbol table has been read.
            const uint32_t nsects = seg64 ? r.u32(p + 40) : r.u32(p + 24);
            const size_t sect_size = seg64 ? 80 : 68;
            size_t sect_off = cmd_off + (seg64 ? 72 : 56);
            for (uint32_t k = 0; k < nsects; ++k, sect_off += sect_size) {
                const size_t base_off = sect_off + 32;
                IndirectSection section;
                section.address = seg64 ? r.u64(base_off) : r.u32(base_off);
                section.size = seg64 ? r.u64(base_off + 8) : r.u32(base_off + 4);
                const size_t flags_off = sect_off + (seg64 ? 64 : 56);
                section.type = r.u32(flags_off) & 0xff;
                section.first_indirect = r.u32(flags_off + 4);
                const uint32_t reserved2 = r.u32(flags_off + 8);
                if (section.type != S_SYMBOL_STUBS &&
                    section.type != S_LAZY_SYMBOL_POINTERS &&
                    section.type != S_NON_LAZY_SYMBOL_POINTERS)
                    continue;
                section.entry_size = section.type == S_SYMBOL_STUBS
                    ? reserved2
                    : static_cast<uint32_t>(is64 ? 8 : 4);
                if (section.entry_size != 0 && section.size != 0)
                    indirect_sections.push_back(section);
            }
        } else if (cmd == LC_SYMTAB) {
            symbol_offset = r.u32(cmd_off + 8);
            symbol_count = r.u32(cmd_off + 12);
            string_offset = r.u32(cmd_off + 16);
        } else if (cmd == LC_DYSYMTAB) {
            indirect_offset = r.u32(cmd_off + 56);
            indirect_count = r.u32(cmd_off + 60);
        } else if (cmd == LC_MAIN) {
            uint64_t entryoff = r.u64(cmd_off + 8);
            out.entry_points.push_back(entryoff); // relative to __TEXT, fixed up below
        } else if (cmd == LC_UNIXTHREAD) {
            // The entry register sits in a thread state blob whose layout is
            // cpu-specific; the __TEXT start is a good enough fallback.
        }
        cmd_off += cmdsize;
    }

    // The symbol table names both what this image defines and what it imports.
    // Imports have no address of their own, so they are matched to the stub or
    // pointer slot that stands for them, which is what call sites target.
    std::vector<std::string> symbol_names;
    if (symbol_offset != 0 && symbol_count != 0) {
        symbol_names.resize(symbol_count);
        for (uint32_t k = 0; k < symbol_count; ++k) {
            const size_t off = static_cast<size_t>(symbol_offset) + k * (is64 ? 16u : 12u);
            if (off + 8 > r.n)
                break;
            const uint32_t n_strx = r.u32(off);
            const uint8_t n_type = r.b[off + 4];
            const uint64_t n_value = is64 ? r.u64(off + 8) : r.u32(off + 8);
            std::string name = n_strx != 0 ? r.cstr(static_cast<size_t>(string_offset) + n_strx)
                                           : std::string();
            // Mach-O prefixes C symbols with an underscore.
            if (!name.empty() && name[0] == '_')
                name.erase(0, 1);
            symbol_names[k] = name;

            if ((n_type & N_STAB) != 0 || name.empty())
                continue;
            if ((n_type & N_TYPE) != N_SECT || n_value == 0)
                continue;
            Symbol sym;
            sym.name = name;
            sym.address = n_value;
            // mh_execute_header and its kin name the Mach-O header itself, not
            // code; treating them as functions would put a body in the output
            // that collides with the real one at link time.
            // Mach-O names these with two leading underscores, one of which is
            // stripped above, so both spellings have to be recognised.
            const bool is_header_symbol =
                name.size() > 10 && name.compare(name.size() - 7, 7, "_header") == 0 &&
                (name.compare(0, 3, "mh_") == 0 || name.compare(0, 4, "_mh_") == 0);
            sym.is_function = !is_header_symbol;
            out.symbols.push_back(std::move(sym));
        }
    }

    if (indirect_offset != 0 && indirect_count != 0) {
        for (const IndirectSection &section : indirect_sections) {
            const uint64_t entries = section.size / section.entry_size;
            for (uint64_t k = 0; k < entries; ++k) {
                const uint64_t slot = section.first_indirect + k;
                if (slot >= indirect_count)
                    break;
                const uint32_t index = r.u32(static_cast<size_t>(indirect_offset) + slot * 4);
                if ((index & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) != 0)
                    continue;
                if (index >= symbol_names.size() || symbol_names[index].empty())
                    continue;
                Symbol sym;
                sym.name = symbol_names[index];
                sym.address = section.address + k * section.entry_size;
                sym.size = section.entry_size;
                // A stub is code that call sites branch to; a pointer slot is data.
                sym.is_function = section.type == S_SYMBOL_STUBS;
                sym.is_import = true;
                out.symbols.push_back(std::move(sym));
            }
        }
    }

    out.image_base = lowest == UINT64_MAX ? 0 : lowest;
    if (have_text)
        for (uint64_t &entry : out.entry_points)
            entry += text_vmaddr;

    // A symbol carries no size in Mach-O; derive one from the next symbol.
    out.sort_and_dedup_symbols();
    for (size_t i = 0; i + 1 < out.symbols.size(); ++i)
        if (out.symbols[i].size == 0)
            out.symbols[i].size = out.symbols[i + 1].address - out.symbols[i].address;

    if (out.segments.empty()) {
        error = "Mach-O: no mapped segments";
        return true;
    }
    error.clear();
    return true;
}

// Picks the slice of a universal binary matching the host, else the first one.
size_t choose_fat_slice(const std::vector<uint8_t> &bytes, bool big, uint32_t nfat, size_t &out_offset)
{
    Reader r{bytes.data(), bytes.size(), big};
#if defined(__aarch64__) || defined(__arm64__)
    const uint32_t preferred = CPU_TYPE_ARM64;
#elif defined(__x86_64__)
    const uint32_t preferred = CPU_TYPE_X86_64;
#else
    const uint32_t preferred = 0;
#endif
    size_t first = 0;
    bool have_first = false;
    for (uint32_t i = 0; i < nfat; ++i) {
        size_t e = 8 + i * 20;
        uint32_t cputype = r.u32(e);
        uint32_t offset = r.u32(e + 8);
        if (!have_first) {
            first = offset;
            have_first = true;
        }
        if (cputype == preferred) {
            out_offset = offset;
            return 1;
        }
    }
    out_offset = first;
    return have_first ? 1 : 0;
}

} // namespace

bool load_macho(const std::vector<uint8_t> &bytes, BinaryImage &out, std::string &error)
{
    if (bytes.size() < 32)
        return false;
    Reader probe{bytes.data(), bytes.size(), false};
    uint32_t magic_le = probe.u32(0);
    Reader probe_be{bytes.data(), bytes.size(), true};
    uint32_t magic_be = probe_be.u32(0);

    if (magic_be == FAT_MAGIC || magic_be == FAT_CIGAM || magic_le == FAT_MAGIC) {
        const bool big = magic_be == FAT_MAGIC;
        Reader r{bytes.data(), bytes.size(), big};
        uint32_t nfat = r.u32(4);
        size_t offset = 0;
        if (nfat == 0 || choose_fat_slice(bytes, big, nfat, offset) == 0 || offset >= bytes.size()) {
            error = "Mach-O: universal binary has no usable slice";
            return true;
        }
        return parse_thin(bytes, offset, out, error);
    }

    if (magic_le == MH_MAGIC || magic_le == MH_MAGIC_64 || magic_le == MH_CIGAM ||
        magic_le == MH_CIGAM_64)
        return parse_thin(bytes, 0, out, error);

    return false;
}

} // namespace astral_internal
