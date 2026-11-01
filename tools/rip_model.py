#!/usr/bin/env python3
"""Rip Pikachu's model (meshes, textures, part transforms) and animations to glTF (.glb).

Input comes from the running game (HYP_CONTROL commands, see include/hyp_control.h):
  snapshot <dir>\\pose            -> pose.rdram / pose.txt: RAM of one frame
  capture <tasks> <dir>\\anim.txt -> every G_MTX + display list of the next tasks
                                   (one capture per animation, made while the motion
                                   player plays it)

Usage:
  python tools/rip_model.py <pose.rdram> <pose_mtx.txt> <out.glb> [name=anim.txt ...]

How it works:
  - Pikachu's object: global 0x800760A8; obj+0x00 scale xyz, obj+0x18 position xyz,
    ext(obj+0x54)+0x18 a 3x4 rotation matrix. Part matrices (world space, row-vector
    convention, from G_MTX) are made object-relative: M_part_obj = M_part_world * inv(M_obj).
  - Each part = the display lists drawn under one G_MTX. They are interpreted (F3DEX2
    as used by F3DAM: G_VTX/TRI1/TRI2, SETTIMG, SETTILE, LOADBLOCK/TILE/TLUT,
    SETTILESIZE, G_TEXTURE + the T scale at G_MW_FOG+0x10) into vertices with UVs, and
    textures are decoded from RAM to PNG.
  - glTF: a "Pikachu" root (scale 0.01, like the game), one child node per part with its
    rest transform, one mesh per part; each animation = per-node TRS keys at 30 fps.
"""
import io
import json
import math
import re
import struct
import sys

import numpy as np
from PIL import Image

PIKACHU_OBJ = 0x800760A8
MASK = 0x7FFFFF


class Ram:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        a = np.frombuffer(raw, dtype='<u4')          # host-order 32-bit words
        self.be = a.astype('>u4').tobytes()          # -> big-endian byte image

    def u8(self, a): return self.be[a & MASK]
    def u16(self, a): return struct.unpack('>H', self.be[a & MASK:(a & MASK) + 2])[0]
    def s16(self, a): return struct.unpack('>h', self.be[a & MASK:(a & MASK) + 2])[0]
    def u32(self, a): return struct.unpack('>I', self.be[a & MASK:(a & MASK) + 4])[0]
    def f32(self, a): return struct.unpack('>f', self.be[a & MASK:(a & MASK) + 4])[0]
    def bytes(self, a, n): return self.be[a & MASK:(a & MASK) + n]


def object_matrix(ram):
    """Pikachu's object transform (row-vector convention, like the game's matrices)."""
    obj = ram.u32(PIKACHU_OBJ)
    ext = ram.u32(obj + 0x54)
    scale = [ram.f32(obj + i * 4) for i in range(3)]
    pos = [ram.f32(obj + 0x18 + i * 4) for i in range(3)]
    rot = np.array([[ram.f32(ext + 0x18 + (r * 3 + c) * 4) for c in range(3)] for r in range(3)])
    m = np.identity(4)
    m[:3, :3] = np.diag(scale) @ rot
    m[3, :3] = pos
    return m, scale


# ---------------------------------------------------------------- textures

def rgba16(v):
    r, g, b, a = (v >> 11) & 31, (v >> 6) & 31, (v >> 1) & 31, v & 1
    return (r * 255 // 31, g * 255 // 31, b * 255 // 31, 255 if a else 0)


def decode_texture(ram, addr, fmt, siz, w, h, stride, pal_addr, pal_index):
    img = Image.new('RGBA', (w, h))
    px = img.load()
    bpp = [4, 8, 16, 32][siz]
    for y in range(h):
        row = addr + y * stride
        for x in range(w):
            if bpp == 4:
                b = ram.u8(row + x // 2)
                v = (b >> 4) if x % 2 == 0 else (b & 15)
            elif bpp == 8:
                v = ram.u8(row + x)
            elif bpp == 16:
                v = ram.u16(row + x * 2)
            else:
                v = ram.u32(row + x * 4)
            if fmt == 0:      # RGBA
                c = rgba16(v) if bpp == 16 else ((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255)
            elif fmt == 2:    # CI
                idx = v + (pal_index * 16 if bpp == 4 else 0)
                c = rgba16(ram.u16(pal_addr + idx * 2)) if pal_addr else (v * 17 if bpp == 4 else v,) * 3 + (255,)
            elif fmt == 3:    # IA
                if bpp == 4:
                    i, a = (v >> 1) * 255 // 7, 255 if v & 1 else 0
                elif bpp == 8:
                    i, a = (v >> 4) * 17, (v & 15) * 17
                else:
                    i, a = v >> 8, v & 255
                c = (i, i, i, a)
            else:             # I
                i = v * 17 if bpp == 4 else v & 255
                c = (i, i, i, i)
            px[x, y] = c
    return img


# ---------------------------------------------------------------- display lists

class Part:
    def __init__(self, index, matrix_world, mtx_addr):
        self.index = index
        self.mtx_addr = mtx_addr
        self.world = matrix_world
        self.prims = {}          # texture key -> {'pos': [], 'uv': [], 'idx': []}


class Interp:
    def __init__(self, ram):
        self.ram = ram
        self.textures = {}       # key -> PIL image
        self.tex_wrap = {}       # key -> (wrap_s, wrap_t)
        self.reset()

    def read_mtx(self, a):
        ram = self.ram
        f = []
        for i in range(16):
            iw, fw = ram.u32(a + (i // 2) * 4), ram.u32(a + 32 + (i // 2) * 4)
            ip = (iw & 0xFFFF) if i & 1 else (iw >> 16)
            fp = (fw & 0xFFFF) if i & 1 else (fw >> 16)
            ip = ip - 0x10000 if ip & 0x8000 else ip
            f.append(ip + fp / 65536.0)
        return np.array(f).reshape(4, 4)

    def begin_part(self, base):
        """Vertices are stored relative to the part's base matrix; nested G_MTX loads
        (skinned chains like the tail: one mesh bent across several matrices) are folded in."""
        self.base_inv = np.linalg.inv(base)
        self.cur = np.array(base)
        self.mstack = []

    def walk_frame(self, root, mtx_range):
        """Interpret a whole frame from its root display list. A G_MTX load at the object
        list level (depth <= 1) whose matrix lives in Pikachu's buffer starts a new part;
        other objects' loads end it; deeper loads bend the current part's vertices."""
        self.reset()
        self.mtx_range = mtx_range
        self.part = None
        self.parts = []
        self.cur = np.identity(4)
        self.base_inv = np.identity(4)
        self.mstack = []
        self.run(None, root & 0x3FFFFFF)
        return self.parts

    def reset(self):
        self.seg = [SEGMENTS.get(i, 0) for i in range(16)]
        self.lights = {}
        self.part = None
        self.mtx_range = None
        self.vtx = [None] * 64
        self.timg = (0, 0, 0, 1)   # addr, fmt, siz, width
        self.tiles = [dict(fmt=0, siz=0, line=0, tmem=0, pal=0, cms=0, cmt=0, ms=0, mt=0, ss=0, st=0,
                           uls=0, ult=0, lrs=0, lrt=0) for _ in range(8)]
        self.tmem = {}             # tmem address -> (src addr, stride bytes)
        self.tlut = 0
        self.tex_on = False
        self.tex_tile = 0
        self.scale_s = self.scale_t = 1.0

    def addr(self, a):
        if a & 0x80000000:
            return a & 0x3FFFFFF
        return (self.seg[(a >> 24) & 15] + (a & 0xFFFFFF)) & 0x3FFFFFF

    def current_texture(self):
        if not self.tex_on or not self.tmem:
            return None
        t = self.tiles[self.tex_tile]
        w = ((t['lrs'] - t['uls']) >> 2) + 1
        h = ((t['lrt'] - t['ult']) >> 2) + 1
        if w <= 0 or h <= 0 or w > 1024 or h > 1024:
            return None
        src = self.tmem.get(t['tmem'])
        if src is None:
            return None
        addr, stride = src
        if t['line']:
            stride = t['line'] * 8
        pal = self.tlut if t['fmt'] == 2 else 0
        key = (addr, t['fmt'], t['siz'], w, h, stride, pal, t['pal'])
        if key not in self.textures:
            self.textures[key] = decode_texture(self.ram, addr, t['fmt'], t['siz'], w, h, stride, pal, t['pal'])
            self.tex_wrap[key] = (t['cms'], t['cmt'])
        return key

    def uv(self, s, t):
        tile = self.tiles[self.tex_tile]
        w = ((tile['lrs'] - tile['uls']) >> 2) + 1
        h = ((tile['lrt'] - tile['ult']) >> 2) + 1
        def shift(v, sh):
            return v / (1 << sh) if sh <= 10 else v * (1 << (16 - sh))
        u = shift(s / 32.0 * self.scale_s, tile['ss']) - tile['uls'] / 4.0
        v = shift(t / 32.0 * self.scale_t, tile['st']) - tile['ult'] / 4.0
        return (u + 0.5) / w, (v + 0.5) / h

    def tri(self, part, idx):
        part = part or self.part
        if part is None or any(i >= len(self.vtx) or self.vtx[i] is None for i in idx):
            return
        key = self.current_texture()
        if key is None and self.lights:
            # untextured: the color is the lights' sum (each part sets its own)
            c = [min(255, sum(l[k] for l in self.lights.values())) for k in range(3)]
            key = ('color',) + tuple(c)
        p = part.prims.setdefault(key, {'pos': [], 'uv': [], 'idx': []})
        for i in idx:
            world, s, t = self.vtx[i]
            x, y, z = (world @ self.base_inv)[:3]
            p['idx'].append(len(p['pos']))
            p['pos'].append((float(x), float(y), float(z)))
            p['uv'].append(self.uv(s, t) if key is not None else (0.0, 0.0))

    def run(self, part, dl, depth=0):
        ram = self.ram
        pc = dl
        for _ in range(20000):
            w0, w1 = ram.u32(0x80000000 | pc), ram.u32(0x80000000 | (pc + 4))
            op = w0 >> 24
            pc += 8
            if op == 0x01:                                   # G_VTX
                n = (w0 >> 12) & 0xFF
                end = (w0 & 0xFF) >> 1
                a = self.addr(w1) | 0x80000000
                for k in range(n):
                    v = a + k * 16
                    if 0 <= end - n + k < len(self.vtx):
                        world = np.array([ram.s16(v), ram.s16(v + 2), ram.s16(v + 4), 1.0]) @ self.cur
                        self.vtx[end - n + k] = (world, ram.s16(v + 8), ram.s16(v + 10))
            elif op == 0x05:                                 # G_TRI1
                self.tri(part, [((w0 >> 16) & 0xFF) // 2, ((w0 >> 8) & 0xFF) // 2, (w0 & 0xFF) // 2])
            elif op in (0x06, 0x07):                         # G_TRI2 / G_QUAD
                self.tri(part, [((w0 >> 16) & 0xFF) // 2, ((w0 >> 8) & 0xFF) // 2, (w0 & 0xFF) // 2])
                self.tri(part, [((w1 >> 16) & 0xFF) // 2, ((w1 >> 8) & 0xFF) // 2, (w1 & 0xFF) // 2])
            elif op == 0xDA:                                 # G_MTX
                param = (w0 & 0xFF) ^ 1                      # F3DEX2 stores PUSH inverted
                if not param & 4:                            # modelview only
                    a = self.addr(w1)
                    m = self.read_mtx(a | 0x80000000)
                    if param & 1:
                        self.mstack.append(self.cur)
                    self.cur = m if param & 2 else m @ self.cur
                    if self.mtx_range is not None and depth <= 1:
                        if self.mtx_range[0] <= a < self.mtx_range[1]:
                            self.part = Part(len(self.parts), self.cur, a)
                            self.parts.append(self.part)
                            self.base_inv = np.linalg.inv(self.cur) if abs(np.linalg.det(self.cur[:3, :3])) > 1e-12 else np.identity(4)
                        else:
                            self.part = None
                            self.base_inv = np.identity(4)
            elif op == 0xD8:                                 # G_POPMTX
                if self.mstack:
                    self.cur = self.mstack.pop()
            elif op == 0xD7:                                 # G_TEXTURE
                self.tex_on = (w0 & 0xFF) != 0 or (w0 >> 1) & 0x7F != 0
                self.tex_tile = (w0 >> 8) & 7
                self.scale_s = (w1 >> 16) / 32768.0          # F3DAM: s.15
            elif op == 0xDB:                                 # G_MOVEWORD
                index, offset = (w0 >> 16) & 0xFF, w0 & 0xFFFF
                if index == 0x06:
                    self.seg[(offset // 4) & 15] = w1 & 0x3FFFFFF
                elif index == 0x08 and offset == 0x10:       # F3DAM texture T scale
                    self.scale_t = (w1 & 0xFFFF) / 32768.0
                elif index == 0x0A:                          # G_MW_LIGHTCOL: light colors
                    self.lights[offset // 0x18] = ((w1 >> 24) & 255, (w1 >> 16) & 255, (w1 >> 8) & 255)
            elif op == 0xFD:                                 # G_SETTIMG
                self.timg = (self.addr(w1), (w0 >> 21) & 7, (w0 >> 19) & 3, (w0 & 0xFFF) + 1)
            elif op == 0xF5:                                 # G_SETTILE
                t = self.tiles[(w1 >> 24) & 7]
                t.update(fmt=(w0 >> 21) & 7, siz=(w0 >> 19) & 3, line=(w0 >> 9) & 0x1FF, tmem=w0 & 0x1FF,
                         pal=(w1 >> 20) & 15, cmt=(w1 >> 18) & 3, mt=(w1 >> 14) & 15, st=(w1 >> 10) & 15,
                         cms=(w1 >> 8) & 3, ms=(w1 >> 4) & 15, ss=w1 & 15)
            elif op == 0xF2:                                 # G_SETTILESIZE
                t = self.tiles[(w1 >> 24) & 7]
                t.update(uls=(w0 >> 12) & 0xFFF, ult=w0 & 0xFFF, lrs=(w1 >> 12) & 0xFFF, lrt=w1 & 0xFFF)
            elif op == 0xF3:                                 # G_LOADBLOCK
                t = self.tiles[(w1 >> 24) & 7]
                self.tmem[t['tmem']] = (self.timg[0], 0)
            elif op == 0xF4:                                 # G_LOADTILE
                t = self.tiles[(w1 >> 24) & 7]
                uls, ult = (w0 >> 12) & 0xFFF, w0 & 0xFFF
                bpp = [4, 8, 16, 32][self.timg[2]]
                stride = self.timg[3] * bpp // 8
                self.tmem[t['tmem']] = (self.timg[0] + (ult >> 2) * stride + (uls >> 2) * bpp // 8, stride)
            elif op == 0xF0:                                 # G_LOADTLUT
                self.tlut = self.timg[0] | 0x80000000
            elif op == 0xDE:                                 # G_DL
                target = self.addr(w1)
                if ((w0 >> 16) & 0xFF) == 0 and depth < 16:
                    self.run(part, target, depth + 1)
                else:
                    pc = target
            elif op == 0xDF:                                 # G_ENDDL
                return


# ---------------------------------------------------------------- captures

SEGMENTS = {}   # segment number -> base (from the capture's "D" lines)


def read_capture(path):
    """task -> list of (mtx addr, 4x4 matrix, [dl addrs])"""
    tasks = {}
    cur = None
    for line in open(path):
        f = line.split()
        if f[0] == 'T':
            cur = tasks.setdefault(int(f[1]), [])
        elif f[0] == 'M':
            m = np.array([float(x) for x in f[3:19]]).reshape(4, 4)
            cur.append((int(f[2], 16), m, []))
        elif f[0] == 'D':
            target, raw = int(f[2], 16), int(f[3], 16) if len(f) > 3 else 0
            if raw and not raw & 0x80000000:
                SEGMENTS.setdefault((raw >> 24) & 15, target - (raw & 0xFFFFFF))
            if cur:
                cur[-1][2].append(target)
    return tasks


MTX_RANGE = (0, 0)   # Pikachu's per-frame matrix buffer: obj+0x3C .. obj+0x40


def pikachu_entries(entries, obj_inv=None):
    """Entries whose matrix lives in Pikachu's matrix buffer (other objects nearby, like
    the title screen's Butterfree, have their own)."""
    lo, hi = MTX_RANGE
    return [(a, m, dls) for a, m, dls in entries if lo <= a < hi and dls]


def trs(m):
    """Row-vector 4x4 -> glTF translation, rotation (x,y,z,w), scale."""
    t = m[3, :3]
    basis = m[:3, :3]
    s = np.linalg.norm(basis, axis=1)
    r = (basis / s[:, None]).T                     # column-vector rotation matrix
    if np.linalg.det(r) < 0:
        s[0] = -s[0]
        r[:, 0] = -r[:, 0]
    w = math.sqrt(max(0.0, 1 + r[0, 0] + r[1, 1] + r[2, 2])) / 2
    if w > 1e-6:
        x = (r[2, 1] - r[1, 2]) / (4 * w); y = (r[0, 2] - r[2, 0]) / (4 * w); z = (r[1, 0] - r[0, 1]) / (4 * w)
    else:
        i = int(np.argmax([r[0, 0], r[1, 1], r[2, 2]]))
        q = [0.0, 0.0, 0.0]
        q[i] = math.sqrt(max(0.0, 1 + 2 * r[i, i] - np.trace(r))) / 2
        j, k = (i + 1) % 3, (i + 2) % 3
        q[j] = (r[j, i] + r[i, j]) / (4 * q[i]); q[k] = (r[k, i] + r[i, k]) / (4 * q[i])
        w = (r[k, j] - r[j, k]) / (4 * q[i]); x, y, z = q
    return list(map(float, t)), [float(x), float(y), float(z), float(w)], list(map(float, s))


# ---------------------------------------------------------------- glTF

class Gltf:
    def __init__(self):
        self.doc = {'asset': {'version': '2.0', 'generator': 'hey-you-pikachu-pc tools/rip_model.py'},
                    'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [], 'meshes': [], 'materials': [],
                    'textures': [], 'images': [], 'samplers': [], 'accessors': [], 'bufferViews': [],
                    'buffers': [], 'animations': []}
        self.bin = bytearray()

    def view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        v = {'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(data)}
        if target:
            v['target'] = target
        self.bin += data
        self.doc['bufferViews'].append(v)
        return len(self.doc['bufferViews']) - 1

    def accessor(self, arr, comp, typ, target=None, minmax=False):
        arr = np.ascontiguousarray(arr)
        a = {'bufferView': self.view(arr.tobytes(), target), 'componentType': comp,
             'count': int(arr.shape[0]), 'type': typ}
        if minmax:
            a['min'] = arr.min(axis=0).tolist() if arr.ndim > 1 else [float(arr.min())]
            a['max'] = arr.max(axis=0).tolist() if arr.ndim > 1 else [float(arr.max())]
        self.doc['accessors'].append(a)
        return len(self.doc['accessors']) - 1

    def save(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.doc['buffers'] = [{'byteLength': len(self.bin)}]
        js = json.dumps(self.doc, separators=(',', ':')).encode()
        js += b' ' * ((4 - len(js) % 4) % 4)
        with open(path, 'wb') as f:
            f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(self.bin)))
            f.write(struct.pack('<II', len(js), 0x4E4F534A) + js)
            f.write(struct.pack('<II', len(self.bin), 0x004E4942) + bytes(self.bin))


WRAP = {0: 10497, 1: 33648, 2: 33071, 3: 33071}   # repeat, mirror, clamp, mirror+clamp


def build(pose_rdram, pose_capture, out_path, anims):
    ram = Ram(pose_rdram)
    obj_m, scale = object_matrix(ram)
    obj_inv = np.linalg.inv(obj_m)
    global MTX_RANGE
    obj = ram.u32(PIKACHU_OBJ)
    MTX_RANGE = (ram.u32(obj + 0x3C) & 0x3FFFFFF, ram.u32(obj + 0x40) & 0x3FFFFFF)
    print('Pikachu matrices at %06X..%06X' % MTX_RANGE)
    tasks = read_capture(pose_capture)
    task, entries = max(((t, pikachu_entries(e, obj_inv)) for t, e in tasks.items()), key=lambda x: len(x[1]))
    dl_root = int(open(pose_rdram.rsplit('.', 1)[0] + '.txt').read().split('dl=')[1].split()[0], 16)
    interp = Interp(ram)
    parts = [p for p in interp.walk_frame(dl_root, MTX_RANGE) if p.prims]
    print('pose: display list %08X, %d Pikachu parts' % (dl_root, len(parts)))

    g = Gltf()
    doc = g.doc
    root = {'name': 'Pikachu', 'scale': [0.01, 0.01, 0.01], 'children': []}
    doc['nodes'].append(root)
    tex_index = {}
    color_keys = {k for part in parts for k in part.prims if k is not None and k[0] == 'color'}
    for key in sorted(color_keys):
        doc['materials'].append({'name': 'color_%02X%02X%02X' % key[1:], 'pbrMetallicRoughness': {
            'baseColorFactor': [key[1] / 255, key[2] / 255, key[3] / 255, 1.0], 'metallicFactor': 0,
            'roughnessFactor': 1}, 'doubleSided': True})
        tex_index[key] = len(doc['materials']) - 1
    for key, img in interp.textures.items():
        buf = io.BytesIO()
        img.save(buf, 'PNG')
        doc['images'].append({'bufferView': g.view(buf.getvalue()), 'mimeType': 'image/png',
                              'name': 'tex_%08X' % key[0]})
        ws, wt = interp.tex_wrap[key]
        doc['samplers'].append({'magFilter': 9729, 'minFilter': 9729, 'wrapS': WRAP[ws], 'wrapT': WRAP[wt]})
        doc['textures'].append({'source': len(doc['images']) - 1, 'sampler': len(doc['samplers']) - 1})
        doc['materials'].append({'name': 'tex_%08X' % key[0], 'pbrMetallicRoughness': {
            'baseColorTexture': {'index': len(doc['textures']) - 1}, 'metallicFactor': 0, 'roughnessFactor': 1},
            'alphaMode': 'MASK', 'doubleSided': True})
        tex_index[key] = len(doc['materials']) - 1
    doc['materials'].append({'name': 'untextured', 'pbrMetallicRoughness': {
        'baseColorFactor': [1.0, 0.85, 0.1, 1.0], 'metallicFactor': 0, 'roughnessFactor': 1}, 'doubleSided': True})
    plain = len(doc['materials']) - 1

    part_nodes = []
    for part in parts:
        local = part.world @ obj_inv
        t, r, s = trs(local)
        node = {'name': 'part_%02d' % part.index, 'translation': t, 'rotation': r, 'scale': s}
        prims = []
        for key, p in part.prims.items():
            if not p['idx']:
                continue
            pos = np.array(p['pos'], dtype='<f4')
            uv = np.array(p['uv'], dtype='<f4')
            idx = np.array(p['idx'], dtype='<u4')
            prim = {'attributes': {'POSITION': g.accessor(pos, 5126, 'VEC3', 34962, minmax=True)},
                    'indices': g.accessor(idx, 5125, 'SCALAR', 34963), 'material': tex_index.get(key, plain)}
            if key is not None and key[0] != 'color':
                prim['attributes']['TEXCOORD_0'] = g.accessor(uv, 5126, 'VEC2', 34962)
            prims.append(prim)
        if prims:
            doc['meshes'].append({'name': 'part_%02d' % part.index, 'primitives': prims})
            node['mesh'] = len(doc['meshes']) - 1
        doc['nodes'].append(node)
        part_nodes.append(len(doc['nodes']) - 1)
        root['children'].append(len(doc['nodes']) - 1)

    # Animations: match each frame's Pikachu entries to parts by order (same draw order).
    for name, path in anims:
        caps = read_capture(path)
        frames = []
        for t in sorted(caps):
            # the object moves while animating: each frame relative to that frame's object
            # transform, approximated by the body part (part 0) staying in rest space
            ents = [e for e in caps[t] if e[2]]
            if len(ents) < len(parts):
                continue
            # find the run of entries with the same display lists as the pose parts
            dl_sig = [tuple(p_dls) for _, _, p_dls in entries]
            for start in range(0, len(ents) - len(parts) + 1):
                if [tuple(e[2]) for e in ents[start:start + len(parts)]] == dl_sig:
                    frames.append([e[1] for e in ents[start:start + len(parts)]])
                    break
        frames = frames[::2] if len(frames) > 2 and len(caps) > 1.5 * len(frames) else frames
        if not frames:
            print('animation %s: no frames matched' % name)
            continue
        # Object transform per frame isn't captured; express parts relative to the body
        # (part 0) and put the body back at its rest transform.
        body_rest = parts[0].world @ obj_inv
        times = np.arange(len(frames), dtype='<f4') / 30.0
        tacc = g.accessor(times, 5126, 'SCALAR', minmax=True)
        samplers, channels = [], []
        for pi, node in enumerate(part_nodes):
            T, R, S = [], [], []
            for fr in frames:
                rel = fr[pi] @ np.linalg.inv(fr[0]) @ body_rest
                t, r, s = trs(rel)
                T.append(t); R.append(r); S.append(s)
            for path_name, data, typ in (('translation', T, 'VEC3'), ('rotation', R, 'VEC4'), ('scale', S, 'VEC3')):
                arr = np.array(data, dtype='<f4')
                if path_name == 'rotation':      # keep quaternions continuous
                    for k in range(1, len(arr)):
                        if np.dot(arr[k], arr[k - 1]) < 0:
                            arr[k] = -arr[k]
                samplers.append({'input': tacc, 'output': g.accessor(arr, 5126, typ), 'interpolation': 'LINEAR'})
                channels.append({'sampler': len(samplers) - 1, 'target': {'node': node, 'path': path_name}})
        doc['animations'].append({'name': name, 'samplers': samplers, 'channels': channels})
        print('animation %s: %d frames' % (name, len(frames)))

    for k in [k for k, v in doc.items() if v == []]:
        del doc[k]
    g.save(out_path)
    tri = sum(len(p['idx']) // 3 for part in parts for p in part.prims.values())
    print('wrote %s: %d parts, %d triangles, %d textures' % (out_path, len(parts), tri, len(interp.textures)))


if __name__ == '__main__':
    anims = [tuple(a.split('=', 1)) for a in sys.argv[4:]]
    build(sys.argv[1], sys.argv[2], sys.argv[3], anims)
