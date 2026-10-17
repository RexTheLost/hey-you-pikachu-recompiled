"""Package the ROM's code/data segments as a MIPS big-endian ELF with function
symbols from syms/hyp.syms.toml, so ELF-only tools (Ghidra/REA) can analyze it.

Usage: python make_elf.py <rom.z64> <hyp.syms.toml> <out.elf>
"""
import re
import struct
import sys

# name, rom start, rom end, vram  (main includes its data; bss is not file-backed)
SEGMENTS = [
    ('.main', 0x1000, 0xA9800, 0x80000400),
    ('.vru_data', 0xA9800, 0xB3C70, 0x80100680),
    ('.game_ovl', 0xB3C70, 0xDCF60, 0x8010AAF0),
]
ENTRY = 0x80000400


def read_syms(path):
    return [(m.group(1), int(m.group(2), 16), int(m.group(3), 16)) for m in re.finditer(
        r'name = "(\w+)", vram = 0x([0-9A-F]+), size = 0x([0-9A-F]+)', open(path).read())]


def main(rom_path, syms_path, out_path):
    rom = open(rom_path, 'rb').read()
    syms = read_syms(syms_path)

    # string tables
    shstr = b'\0'
    def add(tab, s):
        return len(tab), tab + s.encode() + b'\0'
    sec_names = []
    for name, *_ in SEGMENTS:
        off, shstr = add(shstr, name)
        sec_names.append(off)
    symtab_name, shstr = add(shstr, '.symtab')
    strtab_name, shstr = add(shstr, '.strtab')
    shstrtab_name, shstr = add(shstr, '.shstrtab')

    strtab = b'\0'
    symtab = b'\0' * 16  # null symbol
    for name, vram, size in syms:
        shndx = next(i + 1 for i, (_, rs, re_, v) in enumerate(SEGMENTS) if v <= vram < v + (re_ - rs))
        off, strtab = add(strtab, name)
        symtab += struct.pack('>IIIBBH', off, vram, size, 0x12, 0, shndx)  # GLOBAL FUNC

    ehsize, phentsize, shentsize = 52, 32, 40
    phoff = ehsize
    data_off = phoff + phentsize * len(SEGMENTS)
    blobs, offsets = [], []
    cur = data_off
    for _, rs, re_, _ in SEGMENTS:
        cur = (cur + 15) & ~15
        offsets.append(cur)
        blobs.append(rom[rs:re_])
        cur += re_ - rs
    symtab_off = (cur + 3) & ~3
    strtab_off = symtab_off + len(symtab)
    shstr_off = strtab_off + len(strtab)
    shoff = (shstr_off + len(shstr) + 3) & ~3
    nsec = 1 + len(SEGMENTS) + 3

    out = bytearray(shoff + shentsize * nsec)
    out[0:16] = b'\x7fELF\x01\x02\x01' + b'\0' * 9
    # MIPS III, noreorder; ET_EXEC, EM_MIPS
    struct.pack_into('>HHIIIIIHHHHHH', out, 16, 2, 8, 1, ENTRY, phoff, shoff, 0x20000001,
                     ehsize, phentsize, len(SEGMENTS), shentsize, nsec, nsec - 1)
    for i, ((_, rs, re_, vram), off) in enumerate(zip(SEGMENTS, offsets)):
        size = re_ - rs
        struct.pack_into('>IIIIIIII', out, phoff + i * phentsize, 1, off, vram, vram, size, size, 7, 16)
        out[off:off + size] = blobs[i]
    out[symtab_off:symtab_off + len(symtab)] = symtab
    out[strtab_off:strtab_off + len(strtab)] = strtab
    out[shstr_off:shstr_off + len(shstr)] = shstr

    def sh(i, name, typ, flags, addr, off, size, link=0, info=0, align=1, entsize=0):
        struct.pack_into('>IIIIIIIIII', out, shoff + i * shentsize,
                         name, typ, flags, addr, off, size, link, info, align, entsize)
    for i, ((_, rs, re_, vram), off) in enumerate(zip(SEGMENTS, offsets)):
        sh(i + 1, sec_names[i], 1, 0x7, vram, off, re_ - rs, align=16)  # PROGBITS, WAX
    n = len(SEGMENTS)
    sh(n + 1, symtab_name, 2, 0, 0, symtab_off, len(symtab), link=n + 2, info=1, align=4, entsize=16)
    sh(n + 2, strtab_name, 3, 0, 0, strtab_off, len(strtab))
    sh(n + 3, shstrtab_name, 3, 0, 0, shstr_off, len(shstr))
    open(out_path, 'wb').write(out)
    print(f'{out_path}: {len(SEGMENTS)} segments, {len(syms)} function symbols')


if __name__ == '__main__':
    main(*sys.argv[1:4])
