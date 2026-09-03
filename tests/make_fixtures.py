#!/usr/bin/env python3
"""Builds the minimal ELF and PE binaries the loader tests run against.

Both carry the same x86-64 function so the decompiled output can be compared
across formats:

    int add_values(int a, int b) { int s = a + b; if (s > 100) s -= 100; return s; }
"""

import argparse
import os
import struct

# lea eax,[rdi+rsi] / cmp eax,100 / jle +3 / sub eax,100 / ret
ADD_VALUES = bytes.fromhex("8d0437" "83f864" "7e03" "83e864" "c3")
# xor eax,eax / ret
MAIN = bytes.fromhex("31c0" "c3")

CODE = ADD_VALUES + MAIN
ADD_VALUES_OFF = 0
MAIN_OFF = len(ADD_VALUES)


def build_elf(path):
    base = 0x400000
    ehsize, phentsize, shentsize = 64, 56, 64
    phoff = ehsize
    nph = 1
    code_off = phoff + phentsize * nph
    code_off = (code_off + 15) & ~15
    code_vaddr = base + code_off

    shstrtab = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
    names = {
        ".text": shstrtab.index(b".text\0"),
        ".symtab": shstrtab.index(b".symtab\0"),
        ".strtab": shstrtab.index(b".strtab\0"),
        ".shstrtab": shstrtab.index(b".shstrtab\0"),
    }
    strtab = b"\0add_values\0main\0"
    sym_names = {"add_values": strtab.index(b"add_values\0"), "main": strtab.index(b"main\0")}

    def sym(name_off, value, size, info, shndx):
        return struct.pack("<IBBHQQ", name_off, info, 0, shndx, value, size)

    symtab = sym(0, 0, 0, 0, 0)
    symtab += sym(sym_names["add_values"], code_vaddr + ADD_VALUES_OFF, len(ADD_VALUES), 0x12, 1)
    symtab += sym(sym_names["main"], code_vaddr + MAIN_OFF, len(MAIN), 0x12, 1)

    symtab_off = code_off + len(CODE)
    strtab_off = symtab_off + len(symtab)
    shstrtab_off = strtab_off + len(strtab)
    shoff = (shstrtab_off + len(shstrtab) + 7) & ~7

    header = bytearray(64)
    header[0:4] = b"\x7fELF"
    header[4] = 2  # 64-bit
    header[5] = 1  # little endian
    header[6] = 1  # version
    header[7] = 0  # System V
    struct.pack_into("<HHI", header, 16, 2, 62, 1)  # ET_EXEC, EM_X86_64, version
    struct.pack_into("<QQQ", header, 24, code_vaddr + MAIN_OFF, phoff, shoff)
    struct.pack_into("<IHHHHHH", header, 48, 0, ehsize, phentsize, nph, shentsize, 5, 4)

    filesz = code_off + len(CODE)
    phdr = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base, filesz, filesz, 0x1000)

    def shdr(name, stype, flags, addr, off, size, link=0, info=0, align=1, entsize=0):
        return struct.pack("<IIQQQQIIQQ", name, stype, flags, addr, off, size, link, info,
                           align, entsize)

    sections = b"".join([
        shdr(0, 0, 0, 0, 0, 0),
        shdr(names[".text"], 1, 0x6, code_vaddr, code_off, len(CODE), align=16),
        shdr(names[".symtab"], 2, 0, 0, symtab_off, len(symtab), link=3, info=1, align=8,
             entsize=24),
        shdr(names[".strtab"], 3, 0, 0, strtab_off, len(strtab), align=1),
        shdr(names[".shstrtab"], 3, 0, 0, shstrtab_off, len(shstrtab), align=1),
    ])

    blob = bytearray()
    blob += header
    blob += phdr
    blob += b"\0" * (code_off - len(blob))
    blob += CODE
    blob += symtab
    blob += strtab
    blob += shstrtab
    blob += b"\0" * (shoff - len(blob))
    blob += sections

    with open(path, "wb") as f:
        f.write(blob)
    return code_vaddr


def build_pe(path):
    image_base = 0x140000000
    section_align, file_align = 0x1000, 0x200
    text_rva = 0x1000
    headers_size = 0x400
    text_raw = 0x400
    raw_size = ((len(CODE) + file_align - 1) // file_align) * file_align

    dos = bytearray(0x40)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3c, 0x40)

    # COFF symbol table, so the PE symbol path gets exercised too.
    strtab = bytearray(struct.pack("<I", 4))
    def long_name(name):
        offset = len(strtab)
        strtab.extend(name.encode() + b"\0")
        return struct.pack("<II", 0, offset)

    def short_name(name):
        return name.encode().ljust(8, b"\0")

    symbols = b"".join([
        long_name("add_values") + struct.pack("<IhHBB", ADD_VALUES_OFF, 1, 0x20, 2, 0),
        short_name("main") + struct.pack("<IhHBB", MAIN_OFF, 1, 0x20, 2, 0),
    ])
    struct.pack_into("<I", strtab, 0, len(strtab))

    symtab_ptr = text_raw + raw_size
    coff = struct.pack("<4sHHIIIHH", b"PE\0\0", 0x8664, 1, 0, symtab_ptr, 2, 240, 0x0022)

    # PE32+ optional header: no BaseOfData field, so ImageBase lands at offset 24.
    opt = struct.pack("<HBBIIIII", 0x20B, 14, 0, raw_size, 0, 0, text_rva + MAIN_OFF, text_rva)
    opt += struct.pack("<QIIHHHHHHIIIIHHQQQQII",
                       image_base, section_align, file_align,
                       6, 0, 0, 0, 6, 0, 0,
                       text_rva + raw_size, headers_size, 0,
                       3, 0x8160,
                       0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    opt += b"\0" * (16 * 8)
    assert len(opt) == 240, len(opt)

    section = struct.pack("<8sIIIIIIHHI", b".text\0\0\0", len(CODE), text_rva, raw_size,
                          text_raw, 0, 0, 0, 0, 0x60000020)

    blob = bytearray(b"\0" * headers_size)
    blob[0:len(dos)] = dos
    off = 0x40
    blob[off:off + len(coff)] = coff
    off += len(coff)
    blob[off:off + len(opt)] = opt
    off += len(opt)
    blob[off:off + len(section)] = section

    blob += b"\0" * (text_raw - len(blob))
    blob += CODE
    blob += b"\0" * (raw_size - len(CODE))
    blob += symbols
    blob += bytes(strtab)

    with open(path, "wb") as f:
        f.write(blob)
    return image_base + text_rva


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("outdir")
    args = parser.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    elf_text = build_elf(os.path.join(args.outdir, "tiny_x64.elf"))
    pe_text = build_pe(os.path.join(args.outdir, "tiny_x64.exe"))
    print("elf .text 0x%x  add_values 0x%x  main 0x%x" %
          (elf_text, elf_text + ADD_VALUES_OFF, elf_text + MAIN_OFF))
    print("pe  .text 0x%x  add_values 0x%x  main 0x%x" %
          (pe_text, pe_text + ADD_VALUES_OFF, pe_text + MAIN_OFF))


if __name__ == "__main__":
    main()
