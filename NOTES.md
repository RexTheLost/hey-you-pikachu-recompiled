# Hey You, Pikachu! (USA) — PC port RE notes

ROM: `D:\AI\Hey You, Pikachu! (USA).z64`
- 16 MiB, big-endian .z64, SHA-1 `da1a1e47a86720f9d54fb2d2d247480041bda824`
- Game ID `NPGE`, CIC-6102, entry `0x80000400`, libultra rev 2.0K
- Data ends at ROM `0xF194A0` (rest is padding)
- No standard compression (no Yay0/Yaz0/MIO0 magic found). Stage data blocks are **LH5 (LHA -lh5-)** compressed:
  - decoder: `func_80049F74`
  - loader: `func_8002D010`, which decompresses and then applies `{offset, value}` fixups
  - archive reader: `func_80049E44`

## Status (2026-10-17)
- **Boots, passes the VRU check, and plays.** Normal boot sequence → title screen → new game → first 3D scene, with voice commands delivered to the game.
- **VRU emulation** (`src/vru/`):
  - `osVoice*` are implemented in the runtime with no game patches. Words are identified by matching `osVoiceSetWord` phoneme blobs against the game's word table in RDRAM (`0x8010452C`).
  - Picker window: **Tab** in the game window opens it (`docs/vru_picker.png`). It lists the current dictionary; Enter/double-click says the word.
  - "Saying" a word holds Z (the talk button) until the game listens, then returns the slot with `distance` 0x100 and `voice_level` 1000.
  - Port 4 reports `CONT_TYPE_VOICE`. This needed a runtime patch: `patches/N64ModernRuntime/0001`, which enables `Device::VRU` and fixes `__osContGetInitData` overwriting the controller presence bitmask instead of OR-ing into it.
- **3D rendering fixed** (`patches/rt64/0002-f3dam-texture-t-scale.patch`):
  - F3DAM keeps the **texture T scale at `G_MW_FOG + 0x10`**. The game's gSPTexture emits `G_TEXTURE` with S only (T field 0), immediately followed by `DB080010 0000TTTT`.
  - RT64 first treated that write as the fog factor, which made every lit model white. After that was fixed, it ignored the write and left T = 0, which smeared textures (streaky ground, faceless Pikachu).
  - Now offset 0 sets fog and offset 0x10 sets the T scale.
  - **F3DAM texture scales are s.15** (`0x7FFF` ~ 1.0, `0x4000` = 0.5), not F3DEX2's 0.16. At F3DEX2 scale, textures were magnified 2×. With clamped 8×8 textures, Pikachu's eyes became huge black discs, and the ground looked blurry.
  - The `+0x10` handler (F3DAM-only, always right after `G_TEXTURE`) now doubles both S and T.
  - Verified: correct Pikachu face and sharp forest textures (`docs/title_fixed.png`, `docs/gameplay_forest.png`).
- Release packaging (`tools/package.ps1`):
  - GUI exe with no console window (set `HYP_CONSOLE=1` for one).
  - ROM picker on first run.
  - Zip of exe + SDL2 + DXC DLLs + README. The zip contains no ROM; the stored ROM copy lands in `data\` only when the game runs.
- **Saves confirmed** (2026-10-17): the game's own save routine was triggered mid-game. Round trip:
  1. `func_80032D44(file)`, where `file` is the 1-based current file from `D_800AE510`, writes a 0x74-byte EEPROM block via the service thread.
  2. The runtime writes `data\saves\hyp.n64.us.1.0.bin` (512 B, plus `.bak`).
  3. On relaunch the title menu offers Continue with the file's portrait.
  4. Continue reads block 1 and resumes at the start of the saved day.
  - Save API: `func_800045C0` (write request, type-5 message to the service thread `func_80002650`) and `func_8000451C` (read).
  - Block 0 holds a 1-byte header; files are at blocks 1+.
  - Test aid: `HYP_FORCE_SAVE_AT=<s>` (hooked into the game-logic frame loop at `0x8000BC34`; never from the service thread, which would self-deadlock).
- **Audio fixed** (`patches/N64ModernRuntime/0002-ai-buffer-pacing.patch`):
  - The game's audio thread (`func_8004982C`) makes one fixed **184-sample** frame (hardcoded `0xB8` in the audio init `func_8001F680`) per AI buffer-completion event. On hardware that's ~174 buffers/s at 32 kHz.
  - The runtime sent AI events once per VI (60/s). The game therefore produced only ~11 k frames/s for a 32 kHz output, and playback starved (garbled sound).
  - New opt-in runtime mode `ultramodern::set_ai_buffer_pacing(true, 40)`: an AI thread sends one event whenever buffered audio drops below 40 ms, and the next after `osAiSetNextBuffer`. Measured: 174–175 buffers/s, 32.0 k frames/s, queue steady at about 55 ms.
  - Also fixed the latency limiter adapted from Zelda64Recomp. It computed queued time with the input rate instead of the output format, overestimated it 1.5×, and dropped samples. It's now a 200 ms safety valve.
  - Debug: `HYP_AUDIO_DUMP=<file.raw>` writes raw 16-bit stereo at the game rate; `HYP_TRACE` logs per-second production.
- License: GPL-3.0 (`COPYING`), included in the release zip.
- Next:
  - microphone recognizer (Vosk/whisper.cpp, constrained to the current dictionary)
  - VRU word picker polish

## Microcode
- `F3DAM fifo 2.05` (Yasumoto) at ROM `0xA9126`. RT64 already knew it as `F3DAM2.fifo 2.05` but mapped it to `Unknown`. Our patch maps it to **F3DEX2**: its display lists are standard F3DEX2.
- `L3DEX fifo 2.08` at ROM `0xA9556`. RT64 already supports it as `L3DEX2.fifo 2.08`.
- RT64 identifies a microcode by hashing **RDRAM in the recompiler's layout (32-bit words byte-swapped)**, not big-endian ROM bytes. Hash the swapped bytes when checking a microcode against RT64's database.

## Memory map
| Segment | ROM | VRAM | Loaded by |
|---|---|---|---|
| main (code+data) | `0x1000–0xA9800` | `0x80000400–0x800A8C00` | IPL3 (boot) |
| main .bss | — | `0x800A8C00–0x80100680` | cleared by entrypoint |
| VRU word data | `0xA9800–0xB3C70` | `0x80100680–0x8010AAF0` | `func_800404E8` (vruInit), via DMA `func_80037B1C` |
| game overlay .text | `0xB3C70–…` | `0x8010AAF0–0x80123430` | `func_800135C0` called from `func_8000BD70`; entry `func_8010E650` |
| game overlay .data | `…–0xDCF60` | `0x80123430–0x80133DE0` | same |
| game overlay .bss | — | `0x80133DE0–0x80134270` | same |
| assets | `0xDCF60–0xF194A0` | — | various DMA (e.g. `func_80000518` loads `0x4B4990–0x505090`) |

| 23 scene overlays | table at ROM `0xCC724` (in game overlay data) | all at `0x80134270`; text 0x130–0xA20 bytes, then large data | `overlayLoad` (`func_800135C0`) |

- `func_800135C0` (`overlayLoad(rom_start, rom_end, text, text, data, data, bss, bss, end)`) loads the game overlay **and** the 23 scene overlays. The scene overlays overlap at `0x80134270`.
  - `tools/sections.py` reads their table straight from the ROM.
  - Each overlay is a separate recomp section, with function names prefixed `ovlNN_`.
- (The earlier claim that there's just one overlay was wrong: the scene overlays have tiny text sections, which prologue scans missed.)
- DMA helper: `func_80037B1C(rom, vram, size)`.
- Asset offset tables in main data: ROM `0x77C58` (227 entries) and `0x7838C` (226 entries).
- 236 `jalr` sites in main (function pointers). These need to be covered in the recomp function list.

## VRU (Voice Recognition Unit) — fully mapped

### Layer 1: libultra VRU (main segment, standard 2.0K)
| Func | Identity | Notes |
|---|---|---|
| `func_8004C8A0` | `osVoiceInit(mq, handle, channel)` | reset, ~50 ms wait, Read2 id → `D_8009D990`, SetADConverter ×5 from `D_8009D994`, Write4 `0x100` |
| `func_8004CAAC` | `osVoiceSetWord(handle, u8 *word)` | word copied byte-swapped into an 80-byte buffer, 4× Write20 |
| `func_8004CDA4` | `osVoiceClearDictionary(handle, u8 n)` | Write4 `0x02000000 \| n` |
| `func_80054BB0` | `osVoiceStartReadData(handle)` | Write4 `0x05000000` |
| `func_80054C70` | `osVoiceGetReadData(handle, OSVoiceData*)` | state machine, mode 1→4, Read36 |
| `func_80055040` | `osVoiceStopReadData(handle)` | Write4 `0x700` |
| `func_80055630` | `__osVoiceGetStatus` | |
| `func_80055D70` | `__osVoiceCheckResult` | |
| `func_80055910` | `__osVoiceContRead2` | cmd 0x0B |
| `func_8005AAE0` | `__osVoiceContRead36` | cmd 0x09 |
| `func_80055B40` | `__osVoiceContWrite4` | cmd 0x0C |
| `func_80055E20` | `__osVoiceContWrite20` | cmd 0x0A |
| `func_800551C0` | `__osVoiceSetADConverter` | cmd 0x0D |
| `func_80055820` | PIF channel reset (0xFD) | |
| helpers | `func_80058460` = `__osSiRawStartDma`; `func_800583F0`/`func_80058434` = SI get/release access; `func_800586A0` = `__osContAddressCrc`; `func_8005B030` = data CRC; `func_80051550` = `osRecvMesg`; `func_80052610` = `bcopy` | PIF buffer `D_80100390` |

Unused by the game: `osVoiceMaskDictionary`, `osVoiceControlGain`, `osVoiceCheckWord`.

```c
typedef struct {            // OSVoiceHandle — game instance at D_800F8AF8
    OSMesgQueue *mq;        // 0x0
    int channel;            // 0x4  (VRU must be in port 4)
    s32 mode;               // 0x8  GetReadData state
    u8  status;             // 0xC
} OSVoiceHandle;

typedef struct {            // OSVoiceData, 0x1E bytes
    u16 warning;            // 0x00
    u16 answer_num;         // 0x02 (0..5)
    u16 voice_level;        // 0x04
    u16 voice_sn;           // 0x06
    u16 voice_time;         // 0x08
    u16 answer[5];          // 0x0A dictionary slot indices, best first
    u16 distance[5];        // 0x14 lower = better match
} OSVoiceData;
```

### Layer 2: game voice wrapper (main, `0x800404E8–0x80040F10`)
This is the **recommended replacement boundary**.

| Func | Proposed name | Behaviour |
|---|---|---|
| `func_800404E8` | `vruInit(mq, ch)` | `osVoiceInit` ×2 retries; requires id `D_8009D990 == 1`; sets `D_8009D530` (0=absent, 1=ok, 2=error); DMAs word data |
| `func_80040DD4` | `vruProbe(mq, ch)` | controller type `(type & 0x1F07) == 0x0100` means VRU |
| `func_80040608` | `vruClearDict(n)` | |
| `func_800406B4` | `vruLoadSet(setId, batch)` | incremental: clear, then `osVoiceSetWord` for each word of set `setId`, `batch` per call. Returns 1 when done. Sets `D_8009D534 = setId`, `D_800F8AD0 = count` |
| `func_8004090C` | `vruStart()` | |
| `func_800409CC` | `vruPoll()` | per-frame. On success copies `OSVoiceData` → `D_800F8AD8`, ready flag `D_800F8AD4 = 1`, status `D_800F8AC4 = 7`; status 0xC = still listening |
| `func_80040BA0` | `vruGetResult(OSVoiceData *out)` | returns `answer[0]` (slot) or `0xFFFF`; clears the ready flag |
| `func_80040C70` / `C80` / `DC8` / `F10` | status / started / state / gain-mode getters | |
| `func_80040440` | `vruSlotToWord(slot)` | `sets[D_8009D534].word_ids[slot]` |
| `func_80040C8C` / `CF8` / `D60` | slot → word name / category / action | |
| `func_80040E64` | `vruSetMicMode(u8)` | `__osVoiceSetADConverter`; 8 = on |

Callers:
- `func_80003F30`: load-set driver
- `func_80003FB8`: init
- `func_80004204`: per-frame poll
- `func_80003C98` / `func_80003CC8`: mic mode
- **`func_80012508`**: the gameplay consumer

### Layer 3: gameplay consumer `func_80012508(VoiceEvent *ev)`
- Takes `answer[0]`. If the word's category is `0xD`, it falls through to the next answer while `distance[i] ≤ 2000`.
- Event fields:
  - `ev[0]` = word category
  - `ev[1]` = word action id
  - `ev[2]` = flags: `warning`, plus `0x400` if `voice_level < 200` or `== 0`, plus `0x4000` if `distance[0] > 2000` ("didn't understand")
  - `ev[3]` = voice_level
  - `ev[4]` = voice_time
- Thresholds: `D_800722E4 = 2000` (distance), `D_800722E8 = 200` (level).

### Word data (dumped by `tools/dump_vru_dict.py` → `vru_dict.json`)
- Word table `0x8010452C` (ROM `0xAD6AC`): **640 entries**, 16 bytes each: `{char *name, u16 *voice, u32 category, u32 action}`
- Voice data (US VRU phoneme format, not kana): `{u16 nbytes, u16 n, u16 phoneme[n], u16 checksum}`, zero-terminated
- Set table `0x80106D2C` (ROM `0xAFEAC`): **43 sets**, 8 bytes each: `{u32 count, u32 *word_ids}`. One set per scene/context (3–109 words each, e.g. set 42 = rock/scissors/paper)
- Name variants like "Pikachu2…8" or "apple2" are alternate pronunciations of the same word. The recognizer can collapse them (strip the trailing digit) and report the first matching slot.

## Recompilation (working)
`tools/regen.sh` runs the whole pipeline, from ROM to verified `RecompiledFuncs/`:
1. spimdisasm main `.text` (ROM `0x1000–0x60C50`) and overlay `.text` (ROM `0xB3C70–0xCC5B0`) → `asm/`
2. `tools/gen_syms.py` builds `syms/hyp.syms.toml` (N64Recomp symbol-file mode, no ELF). Function starts are the union of:
   - spimdisasm functions
   - all `jal` targets (including overlay→main calls)
   - n64sym and manual names
   Two special cases:
   - Data range `0x8004AB94–0x8004C8A0` (font tables inside `.text`) is excluded.
   - `handle_CpU` is not a function start.
3. `N64Recomp.exe hyp.recomp.toml` → 2457 functions, of which 2301 are emitted. The other 156 are libultra, which the runtime provides or which are ignored.
4. `tools/check_link.py`: every call target is defined by the emitted code, N64ModernRuntime, or `src/`. **0 unresolved.**
5. `gcc -fsyntax-only` on all 47 output files with `-include include/hyp_extra_decls.h`: **0 errors, 0 warnings.**

Stability findings:
- n64sym named 293 functions. A scan of the emitted code finds no direct MMIO/PIF/COP0 access, so no unnamed libultra function slipped through.
- Float mode: MIPS2 (odd FPRs only appear as double high halves), so `uses_mips3_float_mode` is off.
- Config `ignored`: `vimgr_text_0180` (VI manager thread) and `func_80051E48` (motor helper). Both are only reachable from runtime-reimplemented functions.
- `src/runtime_gaps.cpp` supplies functions that are on N64Recomp's ignored list but missing from N64ModernRuntime: `__f_to_ull`, `__ll_rshift`, `osGetIntMask`, `osDpGetCounters`, `__osVoiceSetADConverter`.
- External pins:
  - N64Recomp `ffb39cd`
  - N64ModernRuntime `cdf5abb`
  - n64sym `master`, patched locally: `#ifdef WIN32` → `#if defined(WIN32) || defined(_WIN32)` so it builds with MinGW

## Runtime app (in progress)
- Pins, matching Zelda64Recomp's known-good pair:
  - `lib/N64ModernRuntime` @ `df7e820`; its embedded N64Recomp is `c1a6dc9`, and `build/n64recomp/N64Recomp.exe` is built from that pin
  - `lib/rt64` @ `23cab60`
- Our RT64 patches live in `patches/rt64/` and are applied with `tools/apply_patches.sh`:
  - `0001-hyp-microcode-database.patch` is one line: RT64's existing `F3DAM2_FIFO_2_05` entry, `Unknown` → `F3DEX2`.
  - Without it, RT64 walked display lists as raw RDP and crashed in `loadTLUTOperation`.
  - Microcode table in ROM: `0x8009D41C` `{text, data, ?, size}`:
    - F3DAM: text `0x80060D80`, data `0x800A83E0`
    - L3DEX: text `0x80062120`, data `0x800A8810`
- Audio microcode: `rsp/n_aspMain.us.toml` → `rsp/n_aspMain.cpp` (RSPRecomp at the runtime pin).
  - Text: vram `0x80060120` (ROM `0x60D20`), size `0xC60`. Data: `0x800A8130`.
  - **`text_address = 0x04001080`**: rspboot loads ucode text at IMEM `0x1080`. With `0x1000`, every `j`/`jal` was off by 0x80 and the audio task looped forever, so the game never drew a frame.
  - Indirect targets are the 16-entry command table at the start of data. Entry 14 (`0x02B0`) is a DMEM buffer address, not code.
  - Audio init is at `0x8001F948`. It passes `0x8004DBD8` (an audio callback) as a pointer.
- Runtime integration gotchas (all fixed):
  - librecomp registers overlays only for the initial boot DMA. The hook `overlayLoad` → `hyp_on_overlay_load` (`src/overlay_hooks.cpp`) calls `unload_overlays` + `load_overlays` for every overlay the game loads.
  - Calling `recomp::start_game` before `recomp::start` skips the VI thread's dummy-mode setup, which led to a null VI mode crash in `vi_thread_func`. The game is now started from `gfx_init_callback` + 100 ms.
- Debug switches (environment variables):
  - `HYP_TRACE=1`: `[dma]`/`[lzh]`/`[stage]`/`[ovl]`/`[aud]`/`[gfx]` logs
  - `HYP_DUMP_STACKS_AFTER=<s>`: symbolized stacks of all threads, with source lines
  - `HYP_DL_DUMP=<file>`: display lists of 30 tasks, starting at `HYP_DL_DUMP_FROM` (default 0); includes decoded `G_MTX` matrices and first vertex positions
  - `HYP_ON_TOP=1`: always-on-top window, for captures
  - `HYP_PRESS="start@10,a@14"`: scripted button taps
  - `HYP_VRU_SAY="Pikachu@60,hello@75"`: scripted speech
  - `HYP_PICKER=1`: open the word picker at launch
  - `HYP_FORCE_SAVE_AT=<s>`: trigger the game's save routine (test)
  - `HYP_CONTROL=<file>`: live test control; the game runs lines appended to the file: `press <button> [secs]`, `stick <x> <y> <secs>`, `say <word>`, `speak <wav>`, `save`, `shot <png>`, `quit` (src/debug_control.cpp)
  - `HYP_ASR_AUDIO_CTX=<n>`: override whisper's encoder window (0 = full 30 s); `HYP_ASR_LOG=1`: whisper/ggml logs
  - `HYP_PRESS` also supports `sup/sdown/sleft/sright` (analog stick)
  - A crash handler prints a symbolized stack with source lines.
  - `tools/capture_window.ps1` captures only the game window.
- App sources:
  - `src/main/main.cpp` uses no launcher UI. On first run it validates and stores the ROM with `--rom <path>` (or `baserom.us.z64` next to the exe), then `start_game` → `recomp::start`.
  - Data (stored ROM, saves) goes in `<exe>/data`.
  - Save type is `Eep4k`: the game only accepts `osEepromProbe() == 1`.
  - ROM XXH3 is `0x9D29140FAB450973`.
- Other app files:
  - `src/main/rt64_render_context.cpp`: adapted from Zelda64Recomp (GPL-3.0), without texture packs or UI hooks
  - `src/game/input.cpp`: SDL keyboard/controller → N64 port 1
- Licensing: code adapted from Zelda64Recomp is **GPL-3.0**, so distributed builds of this port must be GPL-3.0.
- Build: `tools\build.bat [RelWithDebInfo]` → `build\win\HeyYouPikachuRecompiled.exe`. Needs:
  - VS 2022 Build Tools with clang-cl
  - Windows SDK 10.0.22621 (installed 2026-10-17; C: was full, see memory)
- Overlay registration caveat: librecomp's initial 1 MB boot DMA also "loads" sections at ROM-relative addresses. That's harmless:
  - The real DMA (`0xB3C70 → 0x8010AAF0`) registers the correct addresses.
  - Direct calls are plain C calls.
- REA: its Ghidra lane is PE-only on Windows, so REA can't analyze this MIPS code here. We run Ghidra 12.1.4 headless directly:
  - `tools/make_elf.py` builds the ELF, and `tools/ghidra/DumpFunctions.java` + `tools/compare_funcs.py` compare function lists.
  - This check found the missing `0x8004DBD8` function, a pointer-only audio callback that would have crashed at audio init.

## Port plan
1. **Runtime app (next):** N64ModernRuntime (librecomp + ultramodern) + RT64, modeled on Zelda64Recomp. Compile `RecompiledFuncs` + `src/` into an exe.
   - Needs: ROM loading/hash check, `recomp_entrypoint`, overlay section table for `0x8010AAF0`, controller mapping, EEPROM save (game probes the EEPROM).
2. **VRU replacement at layer 1 (osVoice*)**. N64Recomp already treats `osVoice*` as runtime-reimplemented, and librecomp's stubs return "no device" or assert. So we provide our own:
   - `osVoiceInit` → 0 (present). The game also checks VRU id `D_8009D990 == 1`, which our init sets via the rdram pointer, or we patch.
   - `osVoiceClearDictionary(n)` → reset the slot list.
   - `osVoiceSetWord(word)` → hash the phoneme blob and look up the word name (599 distinct blobs, **no collisions** between different names), then append it to the slot list. The recognizer grammar is the slot list.
   - `osVoiceStart/StopReadData` → open/close the mic stream.
   - `osVoiceGetReadData` → while listening, return status 0xC/busy. On a final result, fill `OSVoiceData`:
     - `answer[]`: ranked slots
     - `distance[]`: confidence mapped to 0…3000 (≤ 2000 = accepted)
     - `voice_level`: mic RMS, ≥ 200 when audible
     - `voice_time`
   - Needs no game patching, and keeps the game's own thresholds and fallbacks.
3. **Recognizer:** offline. Either Vosk with a grammar restricted to the current slot names, or whisper.cpp plus fuzzy matching. Also provide a keyboard/menu fallback that injects a chosen slot.

## Open questions
- Set id → scene mapping (which set each scene uses). Find it via `func_80003F30` callers and the overlay.
- What category `0xD` means. No word in the table has it, so that may be dead code.
- How `voice_sn` and `warning` bits are used elsewhere. They are only passed through in `ev[2]`; consumers live in the overlay.

## Tooling
- venv with rabbitizer, spimdisasm, splat64, m2c (in the session scratchpad; recreate as needed)
- `tools/dump_vru_dict.py <rom> <out.json>`

## Word sets and the picker
- Word table entries are `{name, voice, category, action}`; category groups words: 2 greetings/talk, 3 commands, 4/5/9 praise and feelings, 6 items, 0xA Pokemon and objects, 0xB questions, 0x12 places. The TCRF notes page lists the same table (name, sizes, voice codes, checksum).
- The 43 sets (ROM `0xAFEAC`) have no names in the game; `set_names` in vru.cpp names them from their contents. The active set is found by matching the dictionary's voice patterns to a set (`hyp::vru::active_set`). Observed: bedroom free roam = set 0, Viridian Forest practice = set 5.
- Picker tabs: "Say now" (active set, grouped) and "Browse all words" (all 458 spoken forms, read-only, with the sets that use each).

## Microphone recognizer (src/vru/vru_mic.cpp, src/vru/asr.cpp)
- whisper.cpp v1.9.4 submodule (lib/whisper.cpp), static CPU build with AVX2/FMA/F16C (`GGML_NATIVE OFF`, so it runs on Haswell+). Model: `data/models/ggml-base.en.bin` (sha1 137c4040...), dev copy in `models/` (gitignored).
- Push-to-talk follows the game: Z held -> game calls osVoiceStartReadData -> `mic_listen_start` records (with 0.3 s pre-roll). Ends on Z release, 0.7 s silence after speech, or 5 s. While recognizing, `mic_talk_update` keeps Z held so the game keeps listening; the result goes through `say_slot` like the picker, or `say_unrecognized` (distance 3000, which voiceGetEvent rejects).
- Recognition: greedy decode, `initial_prompt` = active words, `audio_ctx` = max(256, len*50+128) (~0.3 s vs ~0.8 s for the full 30 s window), `max_tokens` 12.
- Talk button: keyboard Z, controller LB or LT (N64 L moved to Y). The game only gets Xbox controller input while its window has focus; `voice.log` notes focus with each Z press. The title screen starts listening on its own, so recording is tied to the Z press, not to osVoiceStartReadData.
- Live test with the player (release build, title screen, set 0): Pikachu, Hello, Come here, Good morning, Thunderbolt ("Thunderbault"), I choose you, See you later all matched; Magnemite was heard correctly and rejected (not in set 0) and matches Magnemite against the full list.
- Real-voice findings (Logi USB headset): speech peaks ~0.005-0.01 RMS over ~0.0045 background, so voice = 2x background (min 0.003), and a 0.4 s+ press with sound above 1.5x background is recognized even without a voice trigger; audio is normalized to ~0.5 peak (max 20x) before whisper.
- A comma-separated prompt made whisper answer in loops ("flower, flower, ..."); the prompt is now one short sentence per word (`HYP_ASR_PROMPT` 0 = list, 1 = sentences, 2 = none). Repeats of the matched words don't count as extra words.
- On a miss the log lists the active dictionary; `HYP_MIC_SAVE=<folder>` keeps each utterance as a WAV for offline tests with asr_test.
- Matching (asr.cpp `match`): letters edit-distance ratio vs a sound-alike key (vowel runs -> class a / e,i,y / o,u; short keys down-weighted); windows of the transcript; penalties for extra words and late position (whisper loops on short clips: "Pikachu, pikachu, pika pika..."). Accept >= 0.66.
- Tests: `asr_test <model> <wav> phrases...` / `asr_test x --text "<transcript>" phrases...`; Windows TTS clips (System.Speech, David/Zira) of 24 phrases: 46-47/48 correct with 124-458 word vocabularies, all out-of-vocabulary words rejected, ~0.3 s each.
- In-game test aid: HYP_CONTROL `speak <file.wav>` presses Z and uses the clip as the utterance (`mic_inject`).
- `[mic] no speech heard (loudest 0.0000 ...)` means the capture is digital silence: hardware-muted or dead mic (Windows privacy and levels were fine in that case).

## Window menu (src/main/menu.cpp)
- Win32 menu bar on the SDL window, handled by subclassing SDL's window procedure (menu messages arrive on the window/update_gfx thread). Device lists are rebuilt on WM_INITMENUPOPUP.
- SDL2 swallows `SC_KEYMENU`; the subclass passes Alt / Alt+letter / F10 to DefWindowProc so keyboard menu access works (Alt+Enter stays fullscreen).
- Fullscreen is borderless and done by the menu code (style + `SetMenu(nullptr)`), not RT64's `setFullScreen`: RT64 only hides a menu it captured in `makeResizable`, which is never called here.
- Graphics options map to `ultramodern::renderer::GraphicsConfig` (res/ar/msaa); `set_graphics_config` queues an update the gfx thread applies.
- Audio output switching reopens the SDL device under `audio_device_mutex` (queue_samples / get_frames_remaining take it too).
- Controller choice: `hyp::input::select_controller` ("" = first connected, "-" = keyboard only), guarded by a mutex since input is read on game threads.
- Settings: `data/settings.json` (src/settings.cpp).

## Animation system (motions) — first pass
- Motions are keyframed **tracks**, evaluated on the CPU; each body part gets a full matrix per frame (`G_MTX` load + `DE` into model segment 0x0A).
- Motion data: **ROM `0xB77EB0`**, ~2 MB, 231 motions, streamed on demand (resource handle `0x800B6930`, `FUN_80017290`).
  - `0x80077778[id]`: motion start offset in the block; header float at +0 (speed/scale, e.g. 10.4 for motion 0).
  - `0x80077048[id]`: offset of the motion's part list: `{u32 part_id, u32 offset}` pairs, ending at id 0. Motion 0 has 9 parts (ids 8, 38, 39, 42, 43, 62, 63, 82, 83).
  - `0x800773E0[id]`: length in seconds (float).
  - Each part: 3 position + 3 rotation channels, 8 bytes each `{u32 type, u32 offset}` (offset relative to the block, relocated at load by `FUN_8000a34c`/`a3ac`).
- Channel types (`FUN_8000a26c`): 0 = constant (16.16 fixed), 1 = cubic segments (`FUN_8000a0f4`), 2 = linear segments (`FUN_8000a1f0`).
  - Segment (16 bytes): `float a, b, c; s16 d_int; u8 d_frac; u8 duration`; value = a t^3 + b t^2 + c t + d (d = d_int + d_frac/256); `duration` x `DAT_800a0558` time units; duration 0xFF = end (hold d). Segment walk: `FUN_8000a000`.
- Motion start: `FUN_8000e208(obj)`, motion id at `*(s16*)(obj->0x54 + 0xFC)`; vector tracks evaluated by `FUN_8000a410` (position) / `FUN_8000a4d4` (rotation, delta-applied).
- Tools: `tools/ghidra/DecompileAll.java` dumps every function's C (`build/decomp_all.c`).
