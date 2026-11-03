# How Hey You, Pikachu!'s animations work, and how we make our own

Learned from measuring all 174 Pikachu motions (tools/export_motions.py output). Nothing
here copies the game's data; it describes the style and the pipeline, so new motions can
be authored from scratch and fit in.

## What the data says

| Measurement | Result | What it means |
|---|---|---|
| Curve types | constant 69%, cubic 31%, linear 0% (Pikachu) | Splines everywhere; most joints never move in a given clip |
| Joins between segments | 99.9% same value, 98.5% same slope | Smooth spline tangents (C1), not stepped or linear keys |
| Key spacing | median one key every ~3.7 frames per curve | Fairly dense |
| Keys shared across curves | only 44% on frames other curves also key | Keys placed per curve: a fitted / key-reduced result, not posed key frames |
| Animated axes | Z 2817, Y 1885, X 1138 channels | Each joint has a main hinge axis (Z), rigged that way on purpose |
| Clip length | median 1.4 s (0.13 to 6.7 s), mostly 0.5 to 2.5 s | Short, modular clips |
| Loops (LP) | joints: first frame = last frame | Seamless loops; the root may keep turning or travelling |
| ST to LP to ED joins | sometimes exact, sometimes ~20 degrees apart | The engine blends (0.1 s) between clips, so pieces needn't match exactly |
| Shared start pose | 2 of 174 start at the ready stance | No neutral-pose rule; transitions are their own motions (stance->walk etc.) |
| Playback speed (action table) | 1.0 for 305 of 330 actions; some 0.1-5.0 | One motion can be reused at another speed |

Swing sizes: the most active joints move a median 8-60 degrees (largest: joints 52, 59, 75, 70);
whole-body translation is small (median 8 model units, about 1/12 of his height).

## The likely original pipeline (inferred)

1. A hierarchical skeleton: root (part id = object type, 37 for Pikachu) plus 40 joints
   (ids 38-77), Euler rotations in degrees, each joint's main hinge on local Z.
2. Animated in a 3D package at 30 fps with spline (Bezier/Hermite) curves.
3. Exported per channel and curve-fitted with a tolerance: keys only where the curve needs
   them, tangents kept smooth; channels that don't move collapse to one constant.
4. Converted to the runtime form: each segment is a cubic `a t^3 + b t^2 + c t + d`
   (t in seconds, `d` as s16 + 1/256 fraction, duration in whole 1/30 s frames).
5. Built as small pieces: start / loop / end (ST / LP / ED) per behavior, plus explicit
   transition clips between stances. A table of *actions* gives each behavior a motion,
   flags and a speed; the engine blends 0.1 s into each new clip.
6. Rigid skinning: each vertex follows one joint; seams between parts and the tail chain
   are vertices bound to neighbouring joints.

## Our process for new animations

1. **Rig:** in Blender, an armature with Pikachu's root plus joints 38-77 (same hierarchy,
   each bone's hinge on local Z), rotation mode Euler XYZ, scene at 30 fps.
2. **Plan the clip:** short (0.5-2.5 s). If it loops, make ST / LP / ED pieces, with the
   loop's first and last frames identical. Neighbouring pieces should be close (within ~20
   degrees), not exact; the game's blend covers the rest.
3. **Animate:** pose the big shapes first (body, head), then the hinge axis of the limbs,
   ears and tail; leave the other axes still unless needed. Overlap and follow-through on
   ears and tail is where the game's motions get their life.
4. **Export (our converter):** sample every channel at 30 fps; a channel whose range is under
   0.05 degrees becomes a constant; otherwise fit cubic Hermite segments greedily (extend each
   segment while the error stays under ~0.25 degrees, slopes from central differences so
   joins stay smooth), then turn each segment into `a, b, c, d` + frame count (<= 254).
5. **Hook up:** give it a new motion id and an action entry (motion, flags, speed 1.0) in a
   mod file loaded at startup, so the game, the motion player or a voice word can play it.
6. **Check:** play it with the motion player (F2) in free roam; compare against a game clip
   of similar energy for timing.

Open work: map Blender bones (matrix slots in the rip) to joint ids 38-77 and their parent
chain, then write the converter and the runtime loader for added motions.
