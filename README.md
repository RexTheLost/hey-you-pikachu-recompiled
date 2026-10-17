# Hey You, Pikachu! — PC (recompiled)

A native Windows build of *Hey You, Pikachu!* (N64, USA). It's made by statically recompiling the game's code with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and running it on [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) with the [RT64](https://github.com/rt64/rt64) renderer.

The Voice Recognition Unit is emulated. You pick what to say from a word list now; microphone recognition is planned.

**No game data is included.** You need your own ROM of *Hey You, Pikachu!* (USA), `NPGE`, SHA-1 `da1a1e47a86720f9d54fb2d2d247480041bda824`.

## Playing
1. Run `HeyYouPikachuRecompiled.exe`.
2. On first launch, pick your ROM in the file dialog. You can also pass `--rom <path>`, or put it next to the exe as `baserom.us.z64`.
   - The ROM is verified and a copy is stored in `data\`.
   - Saves also go in `data\`, next to the exe.

### Controls
| Keyboard | Controller | N64 |
|---|---|---|
| Arrow keys | Left stick | Analog stick |
| X | A | A |
| C | B / X | B |
| Z | Left trigger | **Z — talk to Pikachu (mike switch)** |
| A / S | LB / RB, right trigger | L / R |
| Enter | Start | Start |
| I J K L | Right stick | C buttons |
| T F G H | D-pad | D-pad |
| **Tab** | — | open the **word picker** |
| F11 / Alt+Enter | — | fullscreen |

### Menu bar
The window has a menu bar (click it, or press Alt). Your choices are saved in `data\settings.json`.
- **Game**: open the save folder, exit.
- **Graphics**: fullscreen, window size, rendering resolution (Original 320�240, 2�, or Match window for the sharpest picture), aspect ratio (4:3 or an experimental widescreen fill), anti-aliasing.
- **Audio**: output device (switches immediately), volume, mute.
- **Input**: which controller to use (or keyboard only), which microphone to use, and the word picker.
  - The microphone choice is stored for the upcoming speech recognizer; for now, talk with the word picker.
- **Help**: controls, about.

### Talking to Pikachu
Press **Tab** to open the word picker. It lists exactly the words the game understands at that moment, and the list changes as you move between scenes.
- Type to filter.
- Press Enter or double-click a word to say it. The talk button is held automatically.

## Building
Requirements (Windows):
- Visual Studio 2022 Build Tools with clang-cl
- Windows SDK 10.0.22621
- Python 3.12
- Git with submodules

```bat
git submodule update --init --recursive
sh tools/apply_patches.sh
sh tools/regen.sh
tools\build.bat RelWithDebInfo
```

What these do:
- `tools/apply_patches.sh` applies our RT64 and runtime patches from `patches/`.
- `tools/regen.sh` regenerates `RecompiledFuncs/` and `rsp/n_aspMain.cpp` from `rom/baserom.us.z64`. It needs `.venv` with spimdisasm, plus `build/n64recomp` (built from `lib/N64ModernRuntime/N64Recomp`). See NOTES.md.
- `tools/package.ps1` builds Release and creates `build/dist/HeyYouPikachuRecompiled-win64.zip`.

Reverse-engineering notes, the memory map, the VRU interface and all porting gotchas are in [NOTES.md](NOTES.md).

## Licensing
This project is licensed under the **GNU General Public License v3.0**; see [COPYING](COPYING). Parts of `src/main` are adapted from Zelda64Recomp (also GPL-3.0).

The licence covers this repository's own code. It does not cover *Hey You, Pikachu!*, which remains the property of its rights holders. Recompiled game code, the ROM and game assets are not part of this repository or its release packages.
