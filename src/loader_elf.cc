// ELF reader. Replaces the part of Ghidra's Java ElfLoader that the decompiler
// actually needs: the mapped image, the entry point, and function symbols.
#include "image.hh"

#include <cstring>

namespace astral_internal {
namespace {

constexpr unsigned char ELF_MAGIC[4] = {0x7f, 'E', 'L', 'F'};

constexpr int PT_LOAD = 1;
constexpr int SHT_SYMTAB = 2;
constexpr int SHT_DYNSYM = 11;
constexpr int STT_FUNC = 2;
constexpr int STT_OBJECT = 1;

// Little/big-endian aware field reader over a byte buffer.
struct Reader {
    const std::vector<uint8_t> &b;
    bool big;
    bool ok = true;

    Reader(const std::vector<uint8_t> &bytes, bool big_endian) : b(bytes), big(big_endian) {}

    bool have(size_t off, size_t n) const { return off + n <= b.size() && off + n >= off; }

    uint64_t u(size_t off, int width)
    {
        if (!have(off, static_cast<size_t>(width))) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < width; ++i) {
            uint64_t byte = b[off + static_cast<size_t>(big ? i : width - 1 - i)];
            v = (v << 8) | byte;
        }
        return v;
    }

    uint16_t u16(size_t off) { return static_cast<uint16_t>(u(off, 2)); }
    uint32_t u32(size_t off) { return static_cast<uint32_t>(u(off, 4)); }
    uint64_t u64(size_t off) { return u(off, 8); }

    std::string cstr(size_t off)
    {
        if (off >= b.size())
            return std::string();
        size_t end = off;
        while (end < b.size() && b[end] != 0)
            ++end;
        return std::string(reinterpret_cast<const char *>(b.data() + off), end - off);
    }
};

std::string machine_name(unsigned e_machine, int bits)
{
    switch (e_machine) {
    case 3:   return "x86";        // EM_386
    case 62:  return "x86";        // EM_X86_64
    case 40:  return "ARM";
    case 183: return "AARCH64";
    case 8:   return "MIPS";
    case 10:  return "MIPS";       // EM_MIPS_RS3_LE
    case 20:  return "PowerPC";
    case 21:  return "PowerPC";    // EM_PPC64
    case 243: return "RISCV";
    case 2:   return "Sparc";
    case 18:  return "Sparc";      // EM_SPARC32PLUS
    case 43:  return "Sparc";      // EM_SPARCV9
    case 4:   return "68000";
    case 42:  return "SuperH";
    case 50:  return "IA64";
    case 22:  return "S390";
    case 106: return "Xtensa";
    case 94:  return "Xtensa";     // EM_TENSILICA legacy id
    case 220: return "Z80";
    case 83:  return "AVR8";
    case 105: return "MSP430";
    case 247: return "eBPF";
    case 258: return "Loongarch";
    default:  break;
    }
    (void)bits;
    return std::string();
}

} // namespace

bool load_elf(const std::vector<uint8_t> &bytes, BinaryImage &out, std::string &error)
{
    if (bytes.size() < 64 || std::memcmp(bytes.data(), ELF_MAGIC, 4) != 0)
        return false;

    const uint8_t ei_class = bytes[4];
    const uint8_t ei_data = bytes[5];
    if (ei_class != 1 && ei_class != 2) {
        error = "ELF: unsupported class byte";
        return true;
    }
    if (ei_data != 1 && ei_data != 2) {
        error = "ELF: unsupported data encoding byte";
        return true;
    }
    const bool is64 = ei_class == 2;
    const bool big = ei_data == 2;
    const int ptr = is64 ? 8 : 4;

    Reader r(bytes, big);
    out.format = ASTRAL_FORMAT_ELF;
    out.format_name = "ELF";

    const unsigned e_machine = r.u16(0x12);
    const uint64_t e_entry = r.u(is64 ? 0x18 : 0x18, ptr);
    const uint64_t e_phoff = r.u(is64 ? 0x20 : 0x1c, ptr);
    const uint64_t e_shoff = r.u(is64 ? 0x28 : 0x20, ptr);
    const size_t hdr_tail = is64 ? 0x30 : 0x28;
    const unsigned e_phentsize = r.u16(hdr_tail + 6);
    const unsigned e_phnum = r.u16(hdr_tail + 8);
    const unsigned e_shentsize = r.u16(hdr_tail + 10);
    const unsigned e_shnum = r.u16(hdr_tail + 12);
    const unsigned e_shstrndx = r.u16(hdr_tail + 14);
    if (!r.ok) {
        error = "ELF: header runs past end of file";
        return true;
    }

    out.arch.machine = machine_name(e_machine, is64 ? 64 : 32);
    out.arch.raw_machine = e_machine;
    out.arch.bits = is64 ? 64 : 32;
    out.arch.big_endian = big;
    out.arch.abi = "gcc";
    if (out.arch.machine.empty()) {
        error = "ELF: unsupported e_machine " + std::to_string(e_machine);
        return true;
    }
    if (e_entry != 0)
        out.entry_points.push_back(e_entry);

    // Program headers give the mapped layout.
    uint64_t lowest = UINT64_MAX;
    for (unsigned i = 0; i < e_phnum; ++i) {
        size_t off = static_cast<size_t>(e_phoff) + i * e_phentsize;
        if (!r.have(off, e_phentsize))
            break;
        uint32_t p_type = r.u32(off);
        if (p_type != PT_LOAD)
            continue;
        uint64_t p_offset, p_vaddr, p_filesz, p_memsz, p_flags;
        if (is64) {
            p_flags = r.u32(off + 4);
            p_offset = r.u64(off + 8);
            p_vaddr = r.u64(off + 16);
            p_filesz = r.u64(off + 32);
            p_memsz = r.u64(off + 40);
        } else {
            p_offset = r.u32(off + 4);
            p_vaddr = r.u32(off + 8);
            p_filesz = r.u32(off + 16);
            p_memsz = r.u32(off + 20);
            p_flags = r.u32(off + 24);
        }
        if (p_memsz == 0)
            continue;
        Segment seg;
        seg.name = "load" + std::to_string(i);
        seg.address = p_vaddr;
        seg.size = p_memsz;
        seg.readable = (p_flags & 4) != 0;
        seg.writable = (p_flags & 2) != 0;
        seg.executable = (p_flags & 1) != 0;
        if (p_offset < bytes.size()) {
            size_t avail = std::min<size_t>(static_cast<size_t>(p_filesz),
                                            bytes.size() - static_cast<size_t>(p_offset));
            seg.data.assign(bytes.begin() + static_cast<long>(p_offset),
                            bytes.begin() + static_cast<long>(p_offset) + static_cast<long>(avail));
        }
        lowest = std::min(lowest, p_vaddr);
        out.segments.push_back(std::move(seg));
    }
    out.image_base = lowest == UINT64_MAX ? 0 : lowest;

    // Section headers give names and symbol tables. Relocatable objects have no
    // program headers at all, so sections also supply the layout in that case.
    struct Sec {
        std::string name;
        uint32_t type = 0;
        uint64_t flags = 0, addr = 0, offset = 0, size = 0, entsize = 0;
        uint32_t link = 0;
    };
    std::vector<Sec> sections;
    if (e_shnum != 0 && e_shoff != 0) {
        uint64_t shstr_off = 0;
        {
            size_t off = static_cast<size_t>(e_shoff) + e_shstrndx * e_shentsize;
            shstr_off = is64 ? r.u64(off + 0x18) : r.u32(off + 0x10);
        }
        for (unsigned i = 0; i < e_shnum; ++i) {
            size_t off = static_cast<size_t>(e_shoff) + i * e_shentsize;
            if (!r.have(off, e_shentsize))
                break;
            Sec s;
            uint32_t name_off = r.u32(off);
            s.type = r.u32(off + 4);
            if (is64) {
                s.flags = r.u64(off + 8);
                s.addr = r.u64(off + 16);
                s.offset = r.u64(off + 24);
                s.size = r.u64(off + 32);
                s.link = r.u32(off + 40);
                s.entsize = r.u64(off + 56);
            } else {
                s.flags = r.u32(off + 8);
                s.addr = r.u32(off + 12);
                s.offset = r.u32(off + 16);
                s.size = r.u32(off + 20);
                s.link = r.u32(off + 24);
                s.entsize = r.u32(off + 36);
            }
            s.name = r.cstr(static_cast<size_t>(shstr_off) + name_off);
            sections.push_back(std::move(s));
        }
    }

    if (out.segments.empty()) {
        for (size_t i = 0; i < sections.size(); ++i) {
            const Sec &s = sections[i];
            const bool alloc = (s.flags & 0x2) != 0; // SHF_ALLOC
            if (!alloc || s.size == 0)
                continue;
            Segment seg;
            seg.name = s.name.empty() ? ("sec" + std::to_string(i)) : s.name;
            seg.address = s.addr;
            seg.size = s.size;
            seg.writable = (s.flags & 0x1) != 0;
            seg.executable = (s.flags & 0x4) != 0;
            if (s.type != 8 /* SHT_NOBITS */ && s.offset < bytes.size()) {
                size_t avail = std::min<size_t>(static_cast<size_t>(s.size),
                                                bytes.size() - static_cast<size_t>(s.offset));
                seg.data.assign(bytes.begin() + static_cast<long>(s.offset),
                                bytes.begin() + static_cast<long>(s.offset) + static_cast<long>(avail));
            }
            out.segments.push_back(std::move(seg));
        }
    } else {
        // Give the mapped segments their section names where they line up.
        for (const Sec &s : sections) {
            if (s.addr == 0 || s.name.empty())
                continue;
            for (Segment &seg : out.segments)
                if (seg.address == s.addr)
                    seg.name = s.name;
        }
    }

    for (const Sec &s : sections) {
        if (s.type != SHT_SYMTAB && s.type != SHT_DYNSYM)
            continue;
        if (s.entsize == 0 || s.link >= sections.size())
            continue;
        const uint64_t strtab = sections[s.link].offset;
        const uint64_t count = s.size / s.entsize;
        for (uint64_t i = 1; i < count; ++i) {
            size_t off = static_cast<size_t>(s.offset + i * s.entsize);
            if (!r.have(off, static_cast<size_t>(s.entsize)))
                break;
            uint32_t st_name = r.u32(off);
            uint8_t st_info;
            uint64_t st_value, st_size;
            if (is64) {
                st_info = bytes[off + 4];
                st_value = r.u64(off + 8);
                st_size = r.u64(off + 16);
            } else {
                st_value = r.u32(off + 4);
                st_size = r.u32(off + 8);
                st_info = bytes[off + 12];
            }
            if (st_value == 0 || st_name == 0)
                continue;
            Symbol sym;
            sym.name = r.cstr(static_cast<size_t>(strtab) + st_name);
            if (sym.name.empty())
                continue;
            sym.address = st_value;
            sym.size = st_size;
            const int type = st_info & 0xf;
            sym.is_function = type == STT_FUNC;
            if (type != STT_FUNC && type != STT_OBJECT && type != 0)
                continue;
            out.symbols.push_back(std::move(sym));
        }
    }
    out.sort_and_dedup_symbols();

    if (out.segments.empty()) {
        error = "ELF: no loadable segments";
        return true;
    }
    error.clear();
    return true;
}

} // namespace astral_internal
