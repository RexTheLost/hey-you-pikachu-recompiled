"""List the game's code sections (static + scene overlays) as JSON.

Scene overlays come from the table in the game overlay's data at ROM 0xCC724:
23 entries of {rom_start, rom_end, text, text, data, data, bss, bss, end}, all
linked at 0x80134270 and DMA'd there whole by overlayLoad (func_800135C0).

Usage: python sections.py <rom.z64> <out.json>
"""
import json
import struct
import sys

STATIC = [
    # name, rom, vram, text_end, data_end (vram)
    ('main', 0x1000, 0x80000400, 0x80060050, 0x800A8C00),
    ('game_ovl', 0xB3C70, 0x8010AAF0, 0x80123430, 0x80133DE0),
]
SCENE_TABLE_ROM = 0xCC724
SCENE_OVL_COUNT = 23
SCENE_OVL_VRAM = 0x80134270


def main(rom_path, out_path):
    rom = open(rom_path, 'rb').read()
    sections = [dict(name=n, rom=r, vram=v, text_end=t, data_end=d, overlapping=False)
                for n, r, v, t, d in STATIC]
    for i in range(SCENE_OVL_COUNT):
        e = struct.unpack('>9I', rom[SCENE_TABLE_ROM + i * 0x24:SCENE_TABLE_ROM + (i + 1) * 0x24])
        rom_start, rom_end, text, _, data, _, bss, _, end = e
        assert text == SCENE_OVL_VRAM and rom_end - rom_start == bss - text, (i, [hex(x) for x in e])
        sections.append(dict(name=f'ovl{i:02d}', rom=rom_start, vram=text, text_end=data,
                             data_end=bss, overlapping=True))
    json.dump(sections, open(out_path, 'w'), indent=1)
    print(f'{len(sections)} sections ({SCENE_OVL_COUNT} scene overlays at 0x{SCENE_OVL_VRAM:08X})')


if __name__ == '__main__':
    main(*sys.argv[1:3])
