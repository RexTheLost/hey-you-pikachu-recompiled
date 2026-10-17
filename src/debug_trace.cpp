// Tracing hooks injected into recompiled functions via hyp.recomp.toml
// [[patches.hook]] entries. Silent unless HYP_TRACE is set in the environment.
#include "hyp_control.h"
#include <chrono>
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

extern "C" void hyp_trace_save_request(uint8_t* rdram, recomp_context* ctx) {
    if (!hyp_trace_enabled()) return;
    gpr req = ctx->r4;
    std::printf("[save] %s block=%d data=%08X size=%X\n", MEM_W(4, req) == 1 ? "WRITE" : "read",
                (int)MEM_W(0x10, req), (uint32_t)MEM_W(0x18, req), (uint32_t)MEM_W(0x1C, req));
}

// Test aid: HYP_FORCE_SAVE_AT=<seconds> calls the game's own save routine
// (func_80032D44(current file)) once on the game thread, exercising the real save path:
// save data build -> service-thread request -> osEepromWrite -> data/saves/*.bin.
extern "C" void func_80032D44(uint8_t* rdram, recomp_context* ctx);

extern "C" void hyp_force_save_check(uint8_t* rdram, recomp_context* ctx) {
    static const char* env = std::getenv("HYP_FORCE_SAVE_AT");
    static bool done = false;
    static const auto start = std::chrono::steady_clock::now();
    bool timed = env != nullptr && !done &&
        std::chrono::steady_clock::now() - start >= std::chrono::duration<double>(std::atof(env));
    // HYP_CONTROL "save" command
    if (!timed && !hyp::control::take_save_request()) return;
    if (timed) done = true;
    recomp_context saved = *ctx;
    // Same argument the game's own save sites pass: the current file number (1 or 2),
    // kept in D_800AE510.
    int32_t file = MEM_W(0, (gpr)(int32_t)0x800AE510u);
    ctx->r4 = file;
    std::printf("[save] forcing game save of file %d\n", file);
    func_80032D44(rdram, ctx);
    *ctx = saved;
    std::printf("[save] forced save returned\n");
}
