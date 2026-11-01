# Render a quick preview of a glTF in Blender (headless).
# blender -b -P tools/blender_preview.py -- <in.glb> <out_prefix> [animation name] [frame]
import math
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index('--') + 1:]
src, out = argv[0], argv[1]
anim = argv[2] if len(argv) > 2 else None
frame = int(argv[3]) if len(argv) > 3 else 1

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
scene = bpy.context.scene

if anim:
    for ob in scene.objects:
        if ob.animation_data:
            for track in ob.animation_data.nla_tracks:
                for strip in track.strips:
                    if strip.action and strip.action.name.startswith(anim):
                        ob.animation_data.action = strip.action
    scene.frame_set(frame)

meshes = [o for o in scene.objects if o.type == 'MESH']
pts = [o.matrix_world @ Vector(c) for o in meshes for c in o.bound_box]
lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
center, size = (lo + hi) / 2, max((hi - lo).length, 0.001)

scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO'
scene.display.shading.color_type = 'TEXTURE'
scene.render.resolution_x = scene.render.resolution_y = 512
scene.render.film_transparent = False
cam_data = bpy.data.cameras.new('cam')
cam = bpy.data.objects.new('cam', cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
for label, direction in (('front', Vector((0, -1, 0.15))), ('side', Vector((1, 0, 0.15)))):
    cam.location = center + direction.normalized() * size * 1.6
    cam.rotation_euler = (center - cam.location).to_track_quat('-Z', 'Y').to_euler()
    scene.render.filepath = '%s_%s.png' % (out, label)
    bpy.ops.render.render(write_still=True)
print('preview size %.3f, %d meshes' % (size, len(meshes)))
