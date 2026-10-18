#!/usr/bin/env python3
"""Export Hey You, Pikachu!'s 231 motions (keyframed animations) to readable files.

Usage: python tools/export_motions.py rom/baserom.us.z64 build/motions [motion_names.txt]

Writes:
  <out>/motion_NNN.json   every part's 6 channels (position x/y/z, rotation x/y/z), each
                          with its raw segments and the value sampled at 30 fps
  <out>/index.md          one line per motion: length, parts, and a name if known
                          (names from docs/motion_names.txt: "<id> <name>" per line)

Format (see NOTES.md, "Animation system"):
  block at ROM 0xB77EB0; per motion id:
    start  = u32 table @ 0x80077778[id]   (header: float speed/scale at +0)
    parts  = u32 table @ 0x80077048[id]   -> {u32 part_id, u32 offset} ... until part_id 0
    length = f32 table @ 0x800773E0[id]   (seconds)
  part data @ offset: channels of {u32 type, u32 value}
    part id 1..37 = the object's root: 6 channels (position x/y/z, rotation x/y/z)
    other ids     = a child joint: 3 channels (rotation x/y/z in degrees, wrapped to 0..360)
    type 0: constant, value = 16.16 fixed
    type 1: cubic segments at block+value; type 2: linear segments at block+value
  segment (16 bytes): f32 a, b, c; s16 d_int; u8 d_frac; u8 frames (1/30 s units; 0xFF = end)
    cubic: v(t) = a t^3 + b t^2 + c t + d    linear: v(t) = a t + b   (t in seconds)
"""
import json
import os
import struct
import sys

MAIN_ROM, MAIN_VRAM = 0x1000, 0x80000400
BLOCK = 0xB77EB0
COUNT = 231
FPS = 30.0
ROOT_CHANNELS = ['pos_x', 'pos_y', 'pos_z', 'rot_x', 'rot_y', 'rot_z']
JOINT_CHANNELS = ['rot_x', 'rot_y', 'rot_z']


def main_table(rom, vram, fmt, n):
    o = vram - MAIN_VRAM + MAIN_ROM
    return struct.unpack('>%d%s' % (n, fmt), rom[o:o + 4 * n])


def read_segments(rom, offset):
    segs = []
    o = BLOCK + offset
    for _ in range(4096):
        a, b, c, d_int, d_frac, frames = struct.unpack('>fffhBB', rom[o:o + 16])
        d = d_int + d_frac / 256.0
        segs.append({'a': a, 'b': b, 'c': c, 'd': d, 'frames': None if frames == 0xFF else frames})
        if frames == 0xFF:
            break
        o += 16
    return segs


def sample(kind, segs, value, length):
    """Value at every 1/30 s from 0 to the motion's length, as the game evaluates it."""
    n = int(round(length * FPS)) + 1
    if kind == 0:
        return [round(value, 4)] * n
    out, i, t = [], 0, 0.0
    for _ in range(n):
        while segs[i]['frames'] is not None and t > segs[i]['frames'] / FPS:
            t -= segs[i]['frames'] / FPS
            i += 1
        s = segs[i]
        if s['frames'] is None:      # end: hold the final value
            v = s['d']
        elif kind == 1:
            v = ((s['a'] * t + s['b']) * t + s['c']) * t + s['d']
        else:
            v = s['a'] * t + s['b']
        out.append(round(v, 4))
        t += 1.0 / FPS
    return out


def export(rom_path, out_dir, names_path=None):
    rom = open(rom_path, 'rb').read()
    starts = main_table(rom, 0x80077778, 'I', COUNT)
    lists = main_table(rom, 0x80077048, 'I', COUNT)
    lengths = main_table(rom, 0x800773E0, 'f', COUNT)
    names = {}
    if names_path and os.path.exists(names_path):
        for line in open(names_path, encoding='utf-8'):
            line = line.split('#')[0].strip()
            if line:
                k, _, v = line.partition(' ')
                names[int(k)] = v.strip()
    os.makedirs(out_dir, exist_ok=True)
    U = lambda o: struct.unpack('>I', rom[o:o + 4])[0]
    index = ['| id | name | length (s) | parts | part ids |', '|---|---|---|---|---|']
    for m in range(COUNT):
        if (m + 1 < COUNT and lists[m] >= starts[m + 1]) or BLOCK + lists[m] + 8 > len(rom):
            continue  # no part list (the part-list table has one entry fewer)
        length = lengths[m]
        header = struct.unpack('>f', rom[BLOCK + starts[m]:BLOCK + starts[m] + 4])[0]
        parts = []
        o = BLOCK + lists[m]
        while True:
            pid, poff = U(o), U(o + 4)
            if pid == 0 or pid > 0x1000:
                break
            channels = {}
            for c, cname in enumerate(ROOT_CHANNELS if 1 <= pid <= 37 else JOINT_CHANNELS):
                kind, value = struct.unpack('>II', rom[BLOCK + poff + 8 * c:BLOCK + poff + 8 * c + 8])
                if kind not in (0, 1, 2):
                    channels[cname] = {'type': 'unknown %d' % kind, 'value': value}
                elif kind == 0:
                    const = struct.unpack('>i', struct.pack('>I', value))[0] / 65536.0
                    channels[cname] = {'type': 'constant', 'value': round(const, 4),
                                       'samples': sample(0, None, const, length)}
                else:
                    segs = read_segments(rom, value)
                    channels[cname] = {'type': 'cubic' if kind == 1 else 'linear', 'segments': segs,
                                       'samples': sample(kind, segs, 0, length)}
            parts.append({'part': pid, 'kind': 'root' if 1 <= pid <= 37 else 'joint', 'channels': channels})
            o += 8
        doc = {'id': m, 'name': names.get(m, ''), 'length_s': round(length, 4), 'frames_30fps': int(round(length * FPS)),
               'header_float': round(header, 4), 'rom_offset': '0x%X' % (BLOCK + starts[m]), 'parts': parts}
        with open(os.path.join(out_dir, 'motion_%03d.json' % m), 'w') as f:
            json.dump(doc, f, indent=1)
        index.append('| %d | %s | %.3f | %d | %s |' % (m, names.get(m, ''), length, len(parts),
                                                     ' '.join(str(p['part']) for p in parts)))
    with open(os.path.join(out_dir, 'index.md'), 'w', encoding='utf-8') as f:
        f.write('# Hey You, Pikachu! motions\n\nExported by tools/export_motions.py. '
                'Names come from docs/motion_names.txt (observed in game with data/motion.log).\n\n')
        f.write('\n'.join(index) + '\n')
    print('exported %d motions to %s' % (len(index) - 2, out_dir))


if __name__ == '__main__':
    export(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else 'docs/motion_names.txt')
