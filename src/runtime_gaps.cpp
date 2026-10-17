// libultra functions the game calls that N64Recomp skips (they are on its
// ignored list) but N64ModernRuntime does not implement. Calling conventions
// follow librecomp/src/math_routines.cpp.

#include "recomp.h"

// u64 __f_to_ull(f32): float in $f12, result in $v0:$v1
extern "C" void __f_to_ull_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint64_t ret = (uint64_t)ctx->f12.fl;

    ctx->r2 = (int32_t)(ret >> 32);
    ctx->r3 = (int32_t)(ret >> 0);
}

// s64 __ll_rshift(s64 a, s64 b): arithmetic shift, args in $a0:$a1 / $a2:$a3
extern "C" void __ll_rshift_recomp(uint8_t* rdram, recomp_context* ctx) {
    int64_t a = (ctx->r4 << 32) | ((ctx->r5 << 0) & 0xFFFFFFFFu);
    int64_t b = (ctx->r6 << 32) | ((ctx->r7 << 0) & 0xFFFFFFFFu);
    int64_t ret = a >> (b & 63);

    ctx->r2 = (int32_t)(ret >> 32);
    ctx->r3 = (int32_t)(ret >> 0);
}

// OSIntMask osGetIntMask(void): used by vruGetResult around a critical
// section; the runtime's osSetIntMask is a no-op, so report "all enabled".
extern "C" void osGetIntMask_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0x003FFF01; // OS_IM_ALL
}

// void osDpGetCounters(u32* counters): RDP perf counters (clock, cmd, pipe, tmem).
extern "C" void osDpGetCounters_recomp(uint8_t* rdram, recomp_context* ctx) {
    for (int i = 0; i < 4; i++) {
        MEM_W(i * 4, ctx->r4) = 0;
    }
}

// s32 __osVoiceSetADConverter(OSMesgQueue*, int channel, u8 data): VRU mic
// gain/mode write (vruSetMicMode). Accept it; the PC microphone path has no
// analog front end to configure.
extern "C" void __osVoiceSetADConverter_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
}
