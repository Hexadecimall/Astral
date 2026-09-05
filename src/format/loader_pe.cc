// PE/COFF reader: sections, entry point, export table and import thunk names.
#include "image.hh"

#include <cstring>

namespace astral_internal {
namespace {

constexpr uint16_t IMAGE_FILE_MACHINE_I386 = 0x014c;
constexpr uint16_t IMAGE_FILE_MACHINE_AMD64 = 0x8664;
constexpr uint16_t IMAGE_FILE_MACHINE_ARM = 0x01c0;
constexpr uint16_t IMAGE_FILE_MACHINE_ARMNT = 0x01c4;
constexpr uint16_t IMAGE_FILE_MACHINE_ARM64 = 0xaa64;
constexpr uint16_t IMAGE_FILE_MACHINE_IA64 = 0x0200;
constexpr uint16_t IMAGE_FILE_MACHINE_POWERPC = 0x01f0;
constexpr uint16_t IMAGE_FILE_MACHINE_RISCV64 = 0x5064;
constexpr uint16_t IMAGE_FILE_MACHINE_MIPS16 = 0x0266;

// PE is little-endian on every machine it supports.
struct Reader {
    const std::vector<uint8_t> &b;
    bool ok = true;

    explicit Reader(const std::vector<uint8_t> &bytes) : b(bytes) {}
    bool have(size_t off, size_t n) const { return off + n <= b.size() && off + n >= off; }
    uint64_t u(size_t off, int width)
    {
        if (!have(off, static_cast<size_t>(width))) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = width - 1; i >= 0; --i)
            v = (v << 8) | b[off + static_cast<size_t>(i)];
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

bool arch_from_machine(uint16_t machine, ArchHint &hint)
{
    switch (machine) {
    case IMAGE_FILE_MACHINE_I386: hint.machine = "x86"; hint.bits = 32; break;
    case IMAGE_FILE_MACHINE_AMD64: hint.machine = "x86"; hint.bits = 64; break;
    case IMAGE_FILE_MACHINE_ARM:
    case IMAGE_FILE_MACHINE_ARMNT: hint.machine = "ARM"; hint.bits = 32; break;
    case IMAGE_FILE_MACHINE_ARM64: hint.machine = "AARCH64"; hint.bits = 64; break;
    case IMAGE_FILE_MACHINE_POWERPC: hint.machine = "PowerPC"; hint.bits = 32; break;
    case IMAGE_FILE_MACHINE_RISCV64: hint.machine = "RISCV"; hint.bits = 64; break;
    case IMAGE_FILE_MACHINE_MIPS16: hint.machine = "MIPS"; hint.bits = 32; break;
    default: return false;
    }
    hint.raw_machine = machine;
    hint.abi = "windows";
    return true;
}

} // namespace

bool load_pe(const std::vector<uint8_t> &bytes, BinaryImage &out, std::string &error)
{
    if (bytes.size() < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z')
        return false;
    Reader r(bytes);
    uint32_t pe_off = r.u32(0x3c);
    if (!r.have(pe_off, 24) || std::memcmp(bytes.data() + pe_off, "PE\0\0", 4) != 0)
        return false;

    out.format = ASTRAL_FORMAT_PE;
    out.format_name = "PE";

    const size_t coff = pe_off + 4;
    uint16_t machine = r.u16(coff);
    uint16_t nsections = r.u16(coff + 2);
    uint32_t symtab_off = r.u32(coff + 8);
    uint32_t nsymbols = r.u32(coff + 12);
    uint16_t opt_size = r.u16(coff + 16);
    const size_t opt = coff + 20;

    if (!arch_from_machine(machine, out.arch)) {
        error = "PE: unsupported machine 0x" + std::to_string(machine);
        return true;
    }

    uint16_t opt_magic = r.u16(opt);
    const bool plus = opt_magic == 0x20b;
    uint32_t entry_rva = r.u32(opt + 16);
    uint64_t image_base = plus ? r.u64(opt + 24) : r.u32(opt + 28);
    const size_t dir_off = opt + (plus ? 112 : 96);
    uint32_t export_rva = r.u32(dir_off);
    uint32_t import_rva = r.u32(dir_off + 8);

    out.image_base = image_base;
    if (entry_rva != 0)
        out.entry_points.push_back(image_base + entry_rva);

    const size_t sec_table = opt + opt_size;
    struct Sec {
        std::string name;
        uint32_t vsize = 0, vaddr = 0, rawsize = 0, rawptr = 0, flags = 0;
    };
    std::vector<Sec> sections;
    for (uint16_t i = 0; i < nsections; ++i) {
        size_t off = sec_table + i * 40;
        if (!r.have(off, 40))
            break;
        Sec s;
        char raw[9] = {0};
        std::memcpy(raw, bytes.data() + off, 8);
        s.name = raw;
        s.vsize = r.u32(off + 8);
        s.vaddr = r.u32(off + 12);
        s.rawsize = r.u32(off + 16);
        s.rawptr = r.u32(off + 20);
        s.flags = r.u32(off + 36);
        sections.push_back(s);

        Segment seg;
        seg.name = s.name;
        seg.address = image_base + s.vaddr;
        seg.size = s.vsize != 0 ? s.vsize : s.rawsize;
        seg.executable = (s.flags & 0x20000000u) != 0;
        seg.writable = (s.flags & 0x80000000u) != 0;
        seg.readable = (s.flags & 0x40000000u) != 0;
        if (s.rawptr < bytes.size() && s.rawsize != 0) {
            size_t avail = std::min<size_t>(s.rawsize, bytes.size() - s.rawptr);
            seg.data.assign(bytes.begin() + s.rawptr, bytes.begin() + s.rawptr + static_cast<long>(avail));
            seg.file_offset = s.rawptr;
            seg.has_file_offset = true;
        }
        if (seg.size != 0)
            out.segments.push_back(std::move(seg));
    }

    // Resolves a relative virtual address to a file offset through the sections.
    auto rva_to_off = [&](uint32_t rva) -> size_t {
        for (const Sec &s : sections)
            if (rva >= s.vaddr && rva < s.vaddr + std::max(s.vsize, s.rawsize))
                return s.rawptr + (rva - s.vaddr);
        return SIZE_MAX;
    };

    if (export_rva != 0) {
        size_t e = rva_to_off(export_rva);
        if (e != SIZE_MAX && r.have(e, 40)) {
            uint32_t nfunctions = r.u32(e + 20);
            uint32_t func_rva = r.u32(e + 28);
            uint32_t name_rva = r.u32(e + 32);
            uint32_t ord_rva = r.u32(e + 36);
            size_t funcs = rva_to_off(func_rva);
            size_t names = rva_to_off(name_rva);
            size_t ords = rva_to_off(ord_rva);
            uint32_t nnames = r.u32(e + 24);
            (void)nfunctions;
            for (uint32_t i = 0; i < nnames && names != SIZE_MAX && funcs != SIZE_MAX; ++i) {
                if (!r.have(names + i * 4, 4))
                    break;
                uint32_t str_rva = r.u32(names + i * 4);
                uint16_t ordinal = ords == SIZE_MAX ? static_cast<uint16_t>(i)
                                                    : r.u16(ords + i * 2);
                size_t str_off = rva_to_off(str_rva);
                if (str_off == SIZE_MAX)
                    continue;
                uint32_t target = r.u32(funcs + ordinal * 4);
                if (target == 0)
                    continue;
                Symbol sym;
                sym.name = r.cstr(str_off);
                sym.address = image_base + target;
                sym.is_function = true;
                if (!sym.name.empty())
                    out.symbols.push_back(std::move(sym));
            }
        }
    }

    if (import_rva != 0) {
        size_t d = rva_to_off(import_rva);
        const int ptr = plus ? 8 : 4;
        for (int i = 0; d != SIZE_MAX && r.have(d + i * 20, 20); ++i) {
            size_t desc = d + static_cast<size_t>(i) * 20;
            uint32_t orig_thunk = r.u32(desc);
            uint32_t first_thunk = r.u32(desc + 16);
            if (orig_thunk == 0 && first_thunk == 0)
                break;
            uint32_t lookup = orig_thunk != 0 ? orig_thunk : first_thunk;
            size_t lo = rva_to_off(lookup);
            if (lo == SIZE_MAX)
                continue;
            for (int k = 0;; ++k) {
                uint64_t ent = ptr == 8 ? r.u64(lo + k * 8) : r.u32(lo + k * 4);
                if (!r.ok || ent == 0)
                    break;
                const uint64_t ordinal_flag = ptr == 8 ? 0x8000000000000000ull : 0x80000000ull;
                if ((ent & ordinal_flag) != 0)
                    continue;
                size_t hint_off = rva_to_off(static_cast<uint32_t>(ent));
                if (hint_off == SIZE_MAX)
                    continue;
                Symbol sym;
                sym.name = r.cstr(hint_off + 2);
                sym.address = image_base + first_thunk + static_cast<uint64_t>(k) * ptr;
                sym.is_function = false;
                sym.is_import = true;
                if (!sym.name.empty())
                    out.symbols.push_back(std::move(sym));
            }
            r.ok = true;
        }
    }

    // COFF symbol table, present in object files and unstripped images.
    if (symtab_off != 0 && nsymbols != 0) {
        const size_t strtab = symtab_off + static_cast<size_t>(nsymbols) * 18;
        for (uint32_t i = 0; i < nsymbols; ++i) {
            size_t off = symtab_off + static_cast<size_t>(i) * 18;
            if (!r.have(off, 18))
                break;
            uint32_t value = r.u32(off + 8);
            int16_t section = static_cast<int16_t>(r.u16(off + 12));
            uint16_t type = r.u16(off + 14);
            uint8_t naux = bytes[off + 17];
            std::string name;
            if (r.u32(off) == 0)
                name = r.cstr(strtab + r.u32(off + 4));
            else {
                char raw[9] = {0};
                std::memcpy(raw, bytes.data() + off, 8);
                name = raw;
            }
            if (!name.empty() && section > 0 &&
                static_cast<size_t>(section - 1) < sections.size()) {
                Symbol sym;
                sym.name = name;
                sym.address = image_base + sections[section - 1].vaddr + value;
                sym.is_function = (type >> 4) == 2;
                out.symbols.push_back(std::move(sym));
            }
            i += naux;
        }
    }

    out.sort_and_dedup_symbols();
    if (out.segments.empty()) {
        error = "PE: no sections";
        return true;
    }
    error.clear();
    return true;
}

} // namespace astral_internal
