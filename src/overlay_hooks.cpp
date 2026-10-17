// The game loads its code overlays with overlayLoad (func_800135C0):
//   overlayLoad(rom_start, rom_end, text, text, data, data, bss, bss, end)
// librecomp only registers overlays for the initial boot DMA, so this hook (see
// hyp.recomp.toml) tells it about every overlay the game loads: the game overlay
// at 0x8010AAF0 and the 23 scene overlays that share 0x80134270.
#include <cstdio>

#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "hyp_trace.h"

extern "C" void hyp_on_overlay_load(uint8_t* rdram, recomp_context* ctx) {
    uint32_t rom_start = (uint32_t)ctx->r4;
    uint32_t rom_end = (uint32_t)ctx->r5;
    int32_t text = (int32_t)ctx->r6;
    uint32_t size = rom_end - rom_start;

    unload_overlays(text, size);
    load_overlays(rom_start, text, size);
    if (hyp_trace_enabled()) std::printf("[ovl] rom %08X-%08X -> %08X\n", rom_start, rom_end, (uint32_t)text);
}
