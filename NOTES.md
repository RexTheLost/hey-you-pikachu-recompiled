# Hey You, Pikachu! (USA) — PC port RE notes

ROM: `D:\AI\Hey You, Pikachu! (USA).z64`
- 16 MiB, big-endian .z64, SHA-1 `da1a1e47a86720f9d54fb2d2d247480041bda824`
- Game ID `NPGE`, CIC-6102, entry `0x80000400`, libultra rev 2.0K
- Data ends at ROM `0xF194A0` (rest is padding)
- No standard compression (no Yay0/Yaz0/MIO0 magic found)

## Microcode
- `F3DAM fifo 2.05` (Yasumoto) at ROM `0xA9126` — an F3DEX2 variant
- `L3DEX fifo 2.08` at ROM `0xA9556`
- Takeaway: F3DEX2-family, which RT64 handles

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

- `func_800135C0` is the **only** overlay load site, and it loads exactly one overlay. So recompilation only needs: main + one fixed overlay (+ VRU data blob).
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

## Port plan
1. **Recomp:** N64Recomp over main (`0x80000400`) plus the single overlay (`0x8010AAF0`); RT64 for graphics.
2. **VRU replacement**, hooking layer 2 (simplest, keeps the game logic intact):
   - `vruProbe`/`vruInit` → report present, `D_8009D530 = 1`, and still perform the word-data DMA.
   - `vruLoadSet(setId)` → tell the PC recognizer which grammar to use (names from set `setId`), then return 1.
   - `vruPoll` → when the PC recognizer has a final result, fill `D_800F8AD8`:
     - `answer[]`: up to 5 slots, ranked
     - `distance[]`: map confidence to 0…~3000, with ≤ 2000 meaning accepted
     - `voice_level`: from mic RMS, ≥ 200 when speech is audible
     - `voice_time`
     - then set `D_800F8AD4 = 1`, `D_800F8AC4 = 7`
   - Alternative: emulate at layer 1 (`osVoice*`). That's more faithful, but `osVoiceSetWord` only provides phoneme data, so the name ↔ slot mapping would have to be rebuilt by matching voice pointers. Layer 2 is easier.
3. **Recognizer:** offline (Vosk with a grammar restricted to the active set's phrases, or whisper.cpp plus fuzzy matching). Also provide a keyboard/menu fallback that injects a chosen slot.

## Open questions
- Set id → scene mapping (which set each scene uses). Find it via `func_80003F30` callers and the overlay.
- What category `0xD` means. No word in the table has it, so that may be dead code.
- How `voice_sn` and `warning` bits are used elsewhere. They are only passed through in `ev[2]`; consumers live in the overlay.

## Tooling
- venv with rabbitizer, spimdisasm, splat64, m2c (in the session scratchpad; recreate as needed)
- `tools/dump_vru_dict.py <rom> <out.json>`
