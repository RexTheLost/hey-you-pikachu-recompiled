// Tracing hooks injected into recompiled functions via hyp.recomp.toml
// [[patches.hook]] entries. Silent unless HYP_TRACE is set in the environment.
#include <cstdio>
#include <cstdlib>

#include "recomp.h"
#include "hyp_trace.h"

bool hyp_trace_enabled() {
    static const bool enabled = std::getenv("HYP_TRACE") != nullptr;
    return enabled;
}

extern "C" void hyp_trace_dma(uint8_t* rdram, recomp_context* ctx) {
    if (!hyp_trace_enabled()) return;
    // dmaCopy(rom, ram, size)
    std::printf("[dma] rom=%08X ram=%08X size=%X\n", (uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6);
}

extern "C" void hyp_trace_decompress(uint8_t* rdram, recomp_context* ctx) {
    if (!hyp_trace_enabled()) return;
    // func_8002D010(dst, src, ?, ?, fixup_offset, fixup_count); src = {s32 mode, s32 size, LH5 data...}
    gpr src = ctx->r5;
    std::printf("[lzh] dst=%08X src=%08X mode=%08X size=%X fixups@%X x%u\n",
                (uint32_t)ctx->r4, (uint32_t)src, (uint32_t)MEM_W(0, src), (uint32_t)MEM_W(4, src),
                (uint32_t)MEM_W(0x10, ctx->r29), (uint32_t)MEM_W(0x14, ctx->r29));
}

extern "C" void hyp_trace_stage_load(uint8_t* rdram, recomp_context* ctx) {
    if (!hyp_trace_enabled()) return;
    // func_80033A70(slot, stage, flag)
    std::printf("[stage] slot=%d stage=%d flag=%d\n", (int)ctx->r4, (int)ctx->r5, (int)ctx->r6);
}
