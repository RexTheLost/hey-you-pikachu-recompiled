"""Dump Hey You, Pikachu! (USA) VRU word table and per-scene dictionaries to JSON.

Usage: python dump_vru_dict.py <rom.z64> <out.json>
"""
import json
import struct
import sys

SEG_ROM, SEG_VRAM, SEG_END = 0xA9800, 0x80100680, 0xB3C70  # DMA'd by vruInit (func_800404E8)
WORDS_VRAM = 0x8010452C  # 16-byte entries {char *name, u16 *voice, u32 category, u32 action}
SETS_VRAM = 0x80106D2C   # 8-byte entries {u32 count, u32 *word_ids}


def main(rom_path, out_path):
    d = open(rom_path, 'rb').read()
    v2r = lambda v: v - SEG_VRAM + SEG_ROM
    in_seg = lambda v: SEG_VRAM <= v < SEG_VRAM + (SEG_END - SEG_ROM)
    u32 = lambda o: struct.unpack('>I', d[o:o + 4])[0]
    u16 = lambda o: struct.unpack('>H', d[o:o + 2])[0]

    def cstr(v):
        o = v2r(v)
        return d[o:d.index(b'\0', o)].decode('latin1')

    def voice(v):
        o, r = v2r(v), []
        while u16(o):
            r.append(u16(o))
            o += 2
        return r

    words = []
    o = v2r(WORDS_VRAM)
    while in_seg(u32(o)) and in_seg(u32(o + 4)):
        words.append({'id': len(words), 'name': cstr(u32(o)), 'category': u32(o + 8),
                      'action': u32(o + 12), 'voice': ['%04x' % c for c in voice(u32(o + 4))]})
        o += 16

    sets = []
    o = v2r(SETS_VRAM)
    while 0 < u32(o) < 256 and in_seg(u32(o + 4)):
        ids = [u32(v2r(u32(o + 4)) + 4 * i) for i in range(u32(o))]
        sets.append({'id': len(sets), 'word_ids': ids, 'names': [words[i]['name'] for i in ids]})
        o += 8

    json.dump({'words': words, 'sets': sets}, open(out_path, 'w'), indent=1)
    print(len(words), 'words,', len(sets), 'sets')


if __name__ == '__main__':
    main(*sys.argv[1:3])
