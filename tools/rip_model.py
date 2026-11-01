#!/usr/bin/env python3
"""Rip Pikachu's model (skinned mesh, textures, skeleton) and animations to glTF (.glb).

Input comes from the running game (tools/rip_capture.py drives it):
  <dir>/pose.rdram, pose.txt   RAM + root display list of one frame (HYP_CONTROL snapshot)
  <dir>/<name>.txt             per-frame matrices while an animation plays (HYP_CONTROL capture)

Usage:
  python tools/rip_model.py <pose.rdram> <out.glb> [name=anim.txt ...]

How the game draws Pikachu (and how it's exported):
  - Pikachu's object: global 0x800760A8; obj+0x00 scale, obj+0x18 position,
    ext(obj+0x54)+0x18 a 3x3 rotation; obj+0x3C..0x40 his per-frame matrix buffer.
  - Each frame the CPU writes one world matrix per joint into that buffer; the display
    list loads them with G_MTX and loads vertices under them. Every vertex therefore
    belongs to exactly one matrix (rigid N64 skinning), including the triangles that
    stitch neighbouring body parts and the skinned chains (tail) that load several
    matrices inside one display list.
  - Export: one skinned mesh; one bone per matrix slot (weight 1 per vertex); bind pose
    = the snapshot frame; animations = each bone's matrix per frame, made relative to the
    object (so walking around the scene isn't baked in). Units: model units / 100.
  - The display list is interpreted from the frame's root (F3DEX2 as used by F3DAM:
    G_VTX/TRI1/TRI2, G_MTX/POPMTX, segments, SETTIMG/SETTILE/LOADBLOCK/LOADTILE/LOADTLUT/
    SETTILESIZE, G_TEXTURE + the T scale at G_MW_FOG+0x10, light colors). Untextured
    parts get the color of their lights (that is where Pikachu's yellow comes from).
"""
import io
import json
import math
import struct
import sys

import numpy as np
from PIL import Image

PIKACHU_OBJ = 0x800760A8
MASK = 0x7FFFFF
UNITS = np.diag([0.01, 0.01, 0.01, 1.0])   # model units -> metres-ish


class Ram:
    def __init__(self, path):
        a = np.frombuffer(open(path, 'rb').read(), dtype='<u4')   # host-order words
        self.be = a.astype('>u4').tobytes()                         # big-endian image

    def u8(self, a): return self.be[a & MASK]
    def u16(self, a): return struct.unpack('>H', self.be[a & MASK:(a & MASK) + 2])[0]
    def s16(self, a): return struct.unpack('>h', self.be[a & MASK:(a & MASK) + 2])[0]
    def u32(self, a): return struct.unpack('>I', self.be[a & MASK:(a & MASK) + 4])[0]
    def f32(self, a): return struct.unpack('>f', self.be[a & MASK:(a & MASK) + 4])[0]

    def mtx(self, a):
        """N64 Mtx (s15.16 fixed, integer halves then fractions) -> 4x4 row-vector matrix."""
        f = []
        for i in range(16):
            iw, fw = self.u32(a + (i // 2) * 4), self.u32(a + 32 + (i // 2) * 4)
            ip = (iw & 0xFFFF) if i & 1 else (iw >> 16)
            fp = (fw & 0xFFFF) if i & 1 else (fw >> 16)
            f.append((ip - 0x10000 if ip & 0x8000 else ip) + fp / 65536.0)
        return np.array(f).reshape(4, 4)


def object_matrix(scale, pos, rot):
    m = np.identity(4)
    m[:3, :3] = np.diag(scale) @ np.array(rot).reshape(3, 3)
    m[3, :3] = pos
    return m


def object_from_ram(ram):
    obj = ram.u32(PIKACHU_OBJ)
    ext = ram.u32(obj + 0x54)
    m = object_matrix([ram.f32(obj + i * 4) for i in range(3)], [ram.f32(obj + 0x18 + i * 4) for i in range(3)],
                      [ram.f32(ext + 0x18 + i * 4) for i in range(9)])
    return m, (ram.u32(obj + 0x3C) & 0x3FFFFFF, ram.u32(obj + 0x40) & 0x3FFFFFF)


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
            if fmt == 0:                                  # RGBA
                c = rgba16(v) if bpp == 16 else ((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255)
            elif fmt == 2:                                # CI
                idx = v + (pal_index * 16 if bpp == 4 else 0)
                c = rgba16(ram.u16(pal_addr + idx * 2)) if pal_addr else (v,) * 3 + (255,)
            elif fmt == 3:                                # IA
                if bpp == 4:
                    i, a = (v >> 1) * 255 // 7, 255 if v & 1 else 0
                elif bpp == 8:
                    i, a = (v >> 4) * 17, (v & 15) * 17
                else:
                    i, a = v >> 8, v & 255
                c = (i, i, i, a)
            else:                                         # I
                i = v * 17 if bpp == 4 else v & 255
                c = (i, i, i, i)
            px[x, y] = c
    return img


# ---------------------------------------------------------------- display lists

class Interp:
    """Walks a frame's display list and collects Pikachu's triangles."""

    def __init__(self, ram, mtx_range):
        self.ram = ram
        self.mtx_range = mtx_range
        self.textures = {}       # key -> PIL image
        self.tex_wrap = {}       # key -> (cms, cmt)
        self.seg = [0] * 16
        self.vtx = [None] * 64   # (world xyz, s, t, matrix addr)
        self.timg = (0, 0, 0, 1)
        self.tiles = [dict(fmt=0, siz=0, line=0, tmem=0, pal=0, cms=0, cmt=0, ss=0, st=0, uls=0, ult=0, lrs=0, lrt=0)
                      for _ in range(8)]
        self.tmem = {}
        self.tlut = 0
        self.tex_on, self.tex_tile = False, 0
        self.scale_s = self.scale_t = 1.0
        self.lights = {}
        self.cur, self.cur_addr, self.mstack = np.identity(4), None, []
        self.drawing = False     # inside one of Pikachu's parts
        self.tris = []           # (key, [(world, s, t, mtx addr, uv)] * 3)

    def addr(self, a):
        if a & 0x80000000:
            return a & 0x3FFFFFF
        return (self.seg[(a >> 24) & 15] + (a & 0xFFFFFF)) & 0x3FFFFFF

    def material(self):
        t = self.tiles[self.tex_tile]
        w, h = ((t['lrs'] - t['uls']) >> 2) + 1, ((t['lrt'] - t['ult']) >> 2) + 1
        if self.tex_on and self.tmem and 0 < w <= 1024 and 0 < h <= 1024 and t['tmem'] in self.tmem:
            addr, stride = self.tmem[t['tmem']]
            if t['line']:
                stride = t['line'] * 8
            pal = self.tlut if t['fmt'] == 2 else 0
            key = ('tex', addr, t['fmt'], t['siz'], w, h, stride, pal, t['pal'])
            if key not in self.textures:
                self.textures[key] = decode_texture(self.ram, addr, t['fmt'], t['siz'], w, h, stride, pal, t['pal'])
                self.tex_wrap[key] = (t['cms'], t['cmt'])
            return key
        c = [min(255, sum(l[k] for l in self.lights.values())) for k in range(3)] if self.lights else [255, 220, 40]
        return ('color',) + tuple(c)

    def uv(self, s, t):
        tile = self.tiles[self.tex_tile]
        w, h = ((tile['lrs'] - tile['uls']) >> 2) + 1, ((tile['lrt'] - tile['ult']) >> 2) + 1
        def shift(v, sh):
            return v / (1 << sh) if sh <= 10 else v * (1 << (16 - sh))
        u = shift(s / 32.0 * self.scale_s, tile['ss']) - tile['uls'] / 4.0
        v = shift(t / 32.0 * self.scale_t, tile['st']) - tile['ult'] / 4.0
        return ((u + 0.5) / max(w, 1), (v + 0.5) / max(h, 1))

    def tri(self, idx):
        if not self.drawing or any(i >= 64 or self.vtx[i] is None for i in idx):
            return
        key = self.material()
        self.tris.append((key, [self.vtx[i] + (self.uv(self.vtx[i][1], self.vtx[i][2]) if key[0] == 'tex' else (0.0, 0.0),)
                                for i in idx]))

    def run(self, dl, depth=0):
        ram = self.ram
        pc = dl
        for _ in range(100000):
            w0, w1 = ram.u32(0x80000000 | pc), ram.u32(0x80000000 | (pc + 4))
            op = w0 >> 24
            pc += 8
            if op == 0x01:                                   # G_VTX
                n, end = (w0 >> 12) & 0xFF, (w0 & 0xFF) >> 1
                a = self.addr(w1) | 0x80000000
                for k in range(n):
                    if 0 <= end - n + k < 64:
                        v = a + k * 16
                        world = np.array([ram.s16(v), ram.s16(v + 2), ram.s16(v + 4), 1.0]) @ self.cur
                        self.vtx[end - n + k] = (world, ram.s16(v + 8), ram.s16(v + 10), self.cur_addr)
            elif op == 0x05:                                 # G_TRI1
                self.tri([((w0 >> 16) & 0xFF) // 2, ((w0 >> 8) & 0xFF) // 2, (w0 & 0xFF) // 2])
            elif op in (0x06, 0x07):                         # G_TRI2 / G_QUAD
                self.tri([((w0 >> 16) & 0xFF) // 2, ((w0 >> 8) & 0xFF) // 2, (w0 & 0xFF) // 2])
                self.tri([((w1 >> 16) & 0xFF) // 2, ((w1 >> 8) & 0xFF) // 2, (w1 & 0xFF) // 2])
            elif op == 0xDA:                                 # G_MTX
                param = (w0 & 0xFF) ^ 1                      # F3DEX2 stores PUSH inverted
                if not param & 4:                            # modelview
                    a = self.addr(w1)
                    m = ram.mtx(a | 0x80000000)
                    if param & 1:
                        self.mstack.append((self.cur, self.cur_addr))
                    if param & 2:
                        self.cur, self.cur_addr = m, a
                    else:
                        self.cur, self.cur_addr = m @ self.cur, None
                    if depth <= 1:                           # object list level: whose part?
                        self.drawing = self.mtx_range[0] <= a < self.mtx_range[1]
            elif op == 0xD8:                                 # G_POPMTX
                if self.mstack:
                    self.cur, self.cur_addr = self.mstack.pop()
            elif op == 0xD7:                                 # G_TEXTURE
                self.tex_on = (w0 & 0xFF) != 0
                self.tex_tile = (w0 >> 8) & 7
                self.scale_s = (w1 >> 16) / 32768.0          # F3DAM: s.15
            elif op == 0xDB:                                 # G_MOVEWORD
                index, offset = (w0 >> 16) & 0xFF, w0 & 0xFFFF
                if index == 0x06:
                    self.seg[(offset // 4) & 15] = w1 & 0x3FFFFFF
                elif index == 0x08 and offset == 0x10:       # F3DAM texture T scale
                    self.scale_t = (w1 & 0xFFFF) / 32768.0
                elif index == 0x0A:                          # light colors
                    self.lights[offset // 0x18] = ((w1 >> 24) & 255, (w1 >> 16) & 255, (w1 >> 8) & 255)
            elif op == 0xFD:                                 # G_SETTIMG
                self.timg = (self.addr(w1), (w0 >> 21) & 7, (w0 >> 19) & 3, (w0 & 0xFFF) + 1)
            elif op == 0xF5:                                 # G_SETTILE
                t = self.tiles[(w1 >> 24) & 7]
                t.update(fmt=(w0 >> 21) & 7, siz=(w0 >> 19) & 3, line=(w0 >> 9) & 0x1FF, tmem=w0 & 0x1FF,
                         pal=(w1 >> 20) & 15, cmt=(w1 >> 18) & 3, st=(w1 >> 10) & 15, cms=(w1 >> 8) & 3, ss=w1 & 15)
            elif op == 0xF2:                                 # G_SETTILESIZE
                self.tiles[(w1 >> 24) & 7].update(uls=(w0 >> 12) & 0xFFF, ult=w0 & 0xFFF,
                                                   lrs=(w1 >> 12) & 0xFFF, lrt=w1 & 0xFFF)
            elif op == 0xF3:                                 # G_LOADBLOCK
                self.tmem[self.tiles[(w1 >> 24) & 7]['tmem']] = (self.timg[0], 0)
            elif op == 0xF4:                                 # G_LOADTILE
                uls, ult = (w0 >> 12) & 0xFFF, w0 & 0xFFF
                bpp = [4, 8, 16, 32][self.timg[2]]
                stride = self.timg[3] * bpp // 8
                self.tmem[self.tiles[(w1 >> 24) & 7]['tmem']] = (
                    self.timg[0] + (ult >> 2) * stride + (uls >> 2) * bpp // 8, stride)
            elif op == 0xF0:                                 # G_LOADTLUT
                self.tlut = self.timg[0] | 0x80000000
            elif op == 0xDE:                                 # G_DL
                target = self.addr(w1)
                if ((w0 >> 16) & 0xFF) == 0 and depth < 16:
                    self.run(target, depth + 1)
                else:
                    pc = target
            elif op == 0xDF:                                 # G_ENDDL
                return


# ---------------------------------------------------------------- captures

def read_frames(path):
    """Animation capture -> list of frames {matrix offset in Pikachu's buffer: world matrix},
    each with that frame's object matrix."""
    frames = []
    cur = None
    for line in open(path):
        f = line.split()
        if f[0] == 'T':
            cur = {'mtx': {}, 'obj': None, 'range': None}
            frames.append(cur)
        elif f[0] == 'R' and cur is not None:
            cur['range'] = (int(f[2], 16), int(f[3], 16))
        elif f[0] == 'O' and cur is not None:
            v = [float(x) for x in f[2:]]
            cur['obj'] = object_matrix(v[0:3], v[3:6], v[6:15])
        elif f[0] == 'M' and cur is not None and cur['range']:
            a = int(f[2], 16)
            lo, hi = cur['range']
            if lo <= a < hi:
                cur['mtx'][a - lo] = np.array([float(x) for x in f[3:19]]).reshape(4, 4)
    frames = [fr for fr in frames if fr['mtx'] and fr['obj'] is not None]
    # Two display lists per screen and the game animates at 30 fps: drop repeats.
    out = []
    for fr in frames:
        if out and out[-1]['mtx'].keys() == fr['mtx'].keys() and all(
                np.allclose(out[-1]['mtx'][k], fr['mtx'][k], atol=1e-5) for k in fr['mtx']):
            continue
        out.append(fr)
    return out


def trs(m):
    """Row-vector 4x4 -> glTF translation, rotation (x, y, z, w), scale."""
    t = m[3, :3]
    basis = m[:3, :3]
    s = np.linalg.norm(basis, axis=1)
    r = (basis / s[:, None]).T                           # column-vector rotation
    if np.linalg.det(r) < 0:
        s[0], r[:, 0] = -s[0], -r[:, 0]
    tr = np.trace(r)
    if tr > 0:
        w = math.sqrt(1 + tr) / 2
        x, y, z = (r[2, 1] - r[1, 2]) / (4 * w), (r[0, 2] - r[2, 0]) / (4 * w), (r[1, 0] - r[0, 1]) / (4 * w)
    else:
        i = int(np.argmax([r[0, 0], r[1, 1], r[2, 2]]))
        j, k = (i + 1) % 3, (i + 2) % 3
        q = [0.0, 0.0, 0.0]
        q[i] = math.sqrt(max(1e-12, 1 + r[i, i] - r[j, j] - r[k, k])) / 2
        q[j] = (r[j, i] + r[i, j]) / (4 * q[i])
        q[k] = (r[k, i] + r[i, k]) / (4 * q[i])
        w = (r[k, j] - r[j, k]) / (4 * q[i])
        x, y, z = q
    return list(map(float, t)), [float(x), float(y), float(z), float(w)], list(map(float, s))


# ---------------------------------------------------------------- glTF

class Gltf:
    def __init__(self):
        self.doc = {'asset': {'version': '2.0', 'generator': 'hey-you-pikachu-pc tools/rip_model.py'},
                    'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [], 'meshes': [], 'skins': [],
                    'materials': [], 'textures': [], 'images': [], 'samplers': [], 'accessors': [],
                    'bufferViews': [], 'buffers': [], 'animations': []}
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
        a = {'bufferView': self.view(arr.tobytes(), target), 'componentType': comp, 'count': int(arr.shape[0]), 'type': typ}
        if minmax:
            a['min'] = arr.min(axis=0).tolist() if arr.ndim > 1 else [float(arr.min())]
            a['max'] = arr.max(axis=0).tolist() if arr.ndim > 1 else [float(arr.max())]
        self.doc['accessors'].append(a)
        return len(self.doc['accessors']) - 1

    def save(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.doc['buffers'] = [{'byteLength': len(self.bin)}]
        for k in [k for k, v in self.doc.items() if v == []]:
            del self.doc[k]
        js = json.dumps(self.doc, separators=(',', ':')).encode()
        js += b' ' * ((4 - len(js) % 4) % 4)
        with open(path, 'wb') as f:
            f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(self.bin)))
            f.write(struct.pack('<II', len(js), 0x4E4F534A) + js)
            f.write(struct.pack('<II', len(self.bin), 0x004E4942) + bytes(self.bin))


WRAP = {0: 10497, 1: 33648, 2: 33071, 3: 33071}   # repeat, mirror, clamp, mirror+clamp


def build(pose_rdram, out_path, anims):
    ram = Ram(pose_rdram)
    obj_m, mtx_range = object_from_ram(ram)
    to_obj = np.linalg.inv(obj_m) @ UNITS             # world -> exported object space
    dl_root = int(open(pose_rdram.rsplit('.', 1)[0] + '.txt').read().split('dl=')[1].split()[0], 16)

    interp = Interp(ram, mtx_range)
    interp.run(dl_root & 0x3FFFFFF)
    print('pose: display list %08X, matrices %06X..%06X, %d triangles' % (dl_root, *mtx_range, len(interp.tris)))

    # Bones: one per matrix slot used by Pikachu's vertices (offset in his buffer).
    slots = sorted({v[3] - mtx_range[0] for _, tri in interp.tris for v in tri if v[3] is not None})
    bone = {s: i for i, s in enumerate(slots)}
    rest = {s: ram.mtx(0x80000000 | (mtx_range[0] + s)) @ to_obj for s in slots}   # bone -> object space

    g = Gltf()
    doc = g.doc
    root = {'name': 'Pikachu', 'children': []}
    doc['nodes'].append(root)
    joint_nodes = []
    for s in slots:
        t, r, sc = trs(rest[s])
        doc['nodes'].append({'name': 'bone_%02d_%03X' % (bone[s], s), 'translation': t, 'rotation': r, 'scale': sc})
        joint_nodes.append(len(doc['nodes']) - 1)
        root['children'].append(len(doc['nodes']) - 1)

    # Materials
    material = {}
    for key in sorted({k for k, _ in interp.tris if k[0] == 'color'}):
        doc['materials'].append({'name': 'color_%02X%02X%02X' % key[1:], 'doubleSided': True, 'pbrMetallicRoughness': {
            'baseColorFactor': [key[1] / 255, key[2] / 255, key[3] / 255, 1.0], 'metallicFactor': 0, 'roughnessFactor': 1}})
        material[key] = len(doc['materials']) - 1
    for key, img in interp.textures.items():
        buf = io.BytesIO()
        img.save(buf, 'PNG')
        name = 'tex_%06X' % key[1]
        doc['images'].append({'bufferView': g.view(buf.getvalue()), 'mimeType': 'image/png', 'name': name})
        ws, wt = interp.tex_wrap[key]
        doc['samplers'].append({'magFilter': 9728, 'minFilter': 9728, 'wrapS': WRAP[ws], 'wrapT': WRAP[wt]})
        doc['textures'].append({'source': len(doc['images']) - 1, 'sampler': len(doc['samplers']) - 1})
        doc['materials'].append({'name': name, 'alphaMode': 'MASK', 'doubleSided': True, 'pbrMetallicRoughness': {
            'baseColorTexture': {'index': len(doc['textures']) - 1}, 'metallicFactor': 0, 'roughnessFactor': 1}})
        material[key] = len(doc['materials']) - 1

    # One skinned mesh, a primitive per material; vertices in object space (bind pose).
    prims = []
    for key in material:
        pos, uv, joints, idx, seen = [], [], [], [], {}
        for k, tri in interp.tris:
            if k != key:
                continue
            for world, s, t, maddr, tuv in tri:
                if maddr is None:
                    continue
                p = tuple(np.round((world @ to_obj)[:3], 6))
                vid = (p, tuv, maddr)
                if vid not in seen:
                    seen[vid] = len(pos)
                    pos.append(p)
                    uv.append(tuv)
                    joints.append(bone[maddr - mtx_range[0]])
                idx.append(seen[vid])
        if not idx:
            continue
        n = len(pos)
        attrs = {'POSITION': g.accessor(np.array(pos, dtype='<f4'), 5126, 'VEC3', 34962, minmax=True),
                 'JOINTS_0': g.accessor(np.array([[j, 0, 0, 0] for j in joints], dtype='<u2'), 5123, 'VEC4', 34962),
                 'WEIGHTS_0': g.accessor(np.tile(np.array([1, 0, 0, 0], dtype='<f4'), (n, 1)), 5126, 'VEC4', 34962)}
        if key[0] == 'tex':
            attrs['TEXCOORD_0'] = g.accessor(np.array(uv, dtype='<f4'), 5126, 'VEC2', 34962)
        prims.append({'attributes': attrs, 'indices': g.accessor(np.array(idx, dtype='<u4'), 5125, 'SCALAR', 34963),
                      'material': material[key]})
    doc['meshes'].append({'name': 'Pikachu', 'primitives': prims})
    ibm = np.array([np.linalg.inv(rest[s]).reshape(16) for s in slots], dtype='<f4')
    doc['skins'].append({'name': 'Pikachu', 'joints': joint_nodes, 'inverseBindMatrices': g.accessor(ibm, 5126, 'MAT4')})
    doc['nodes'].append({'name': 'Pikachu_mesh', 'mesh': 0, 'skin': 0})
    root['children'].append(len(doc['nodes']) - 1)

    # Animations: each bone's matrix per frame, object-relative.
    for name, path in anims:
        frames = read_frames(path)
        if not frames:
            print('animation %s: no frames' % name)
            continue
        times = g.accessor(np.arange(len(frames), dtype='<f4') / 30.0, 5126, 'SCALAR', minmax=True)
        samplers, channels = [], []
        for s, node in zip(slots, joint_nodes):
            T, R, S = [], [], []
            last = rest[s]
            for fr in frames:
                if s in fr['mtx']:
                    last = fr['mtx'][s] @ np.linalg.inv(fr['obj']) @ UNITS
                t, r, sc = trs(last)
                T.append(t); R.append(r); S.append(sc)
            R = np.array(R, dtype='<f4')
            for k in range(1, len(R)):                  # keep quaternions continuous
                if np.dot(R[k], R[k - 1]) < 0:
                    R[k] = -R[k]
            for path_name, data, typ in (('translation', np.array(T, dtype='<f4'), 'VEC3'), ('rotation', R, 'VEC4'),
                                         ('scale', np.array(S, dtype='<f4'), 'VEC3')):
                samplers.append({'input': times, 'output': g.accessor(data, 5126, typ), 'interpolation': 'LINEAR'})
                channels.append({'sampler': len(samplers) - 1, 'target': {'node': node, 'path': path_name}})
        doc['animations'].append({'name': name, 'samplers': samplers, 'channels': channels})
        print('animation %s: %d frames (%.2f s at 30 fps)' % (name, len(frames), len(frames) / 30.0))

    g.save(out_path)
    print('wrote %s: %d bones, %d triangles, %d textures' % (out_path, len(slots), len(interp.tris), len(interp.textures)))


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2], [tuple(a.split('=', 1)) for a in sys.argv[3:]])
