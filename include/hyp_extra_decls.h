// Prototypes for functions the recompiled code calls that are on N64Recomp's
// ignored list (so funcs.h does not declare them). Definitions come from
// N64ModernRuntime or src/runtime_gaps.cpp. Force-included when compiling
// RecompiledFuncs/*.c (gcc -include).
#ifndef HYP_EXTRA_DECLS_H
#define HYP_EXTRA_DECLS_H

#include "recomp.h"

#if defined(__aarch64__)
// ARM64 (Android): make float -> int conversions return exactly what the x86 build gets, so the game
// logic runs the same on the phone as on PC. A bare C cast is undefined out of range; x86
// (cvttss2si) gives INT_MIN for NaN and out-of-range values, while ARM saturates (and gives 0 for
// NaN). Phone-only crashes (a garbage pointer in Pikachu's object on the way into Ochre Woods)
// pointed at the game state drifting from the PC's.
#include <math.h>
static inline int32_t hyp_f2w(double v) {
    return (v >= -2147483648.0 && v < 2147483648.0) ? (int32_t)v : INT32_MIN;
}
static inline int64_t hyp_f2l(double v) {
    return (v >= -9223372036854775808.0 && v < 9223372036854775808.0) ? (int64_t)v : INT64_MIN;
}
#undef TRUNC_W_S
#undef TRUNC_W_D
#undef TRUNC_L_S
#undef TRUNC_L_D
#undef CVT_W_S
#undef CVT_W_D
#undef CVT_L_S
#undef CVT_L_D
#define TRUNC_W_S(val) hyp_f2w((double)(val))
#define TRUNC_W_D(val) hyp_f2w((double)(val))
#define TRUNC_L_S(val) hyp_f2l((double)(val))
#define TRUNC_L_D(val) hyp_f2l((double)(val))
// rounding-mode aware (the game can change the FPU rounding mode): round first, then convert
#define CVT_W_S(val) hyp_f2w((double)nearbyintf(val))
#define CVT_W_D(val) hyp_f2w(nearbyint(val))
#define CVT_L_S(val) hyp_f2l((double)nearbyintf(val))
#define CVT_L_D(val) hyp_f2l(nearbyint(val))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// N64ModernRuntime
void __f_to_ll_recomp(uint8_t* rdram, recomp_context* ctx);
void __ll_to_f_recomp(uint8_t* rdram, recomp_context* ctx);
void osContReset_recomp(uint8_t* rdram, recomp_context* ctx);
void osYieldThread_recomp(uint8_t* rdram, recomp_context* ctx);

// src/runtime_gaps.cpp
void __f_to_ull_recomp(uint8_t* rdram, recomp_context* ctx);
void __ll_rshift_recomp(uint8_t* rdram, recomp_context* ctx);
void osGetIntMask_recomp(uint8_t* rdram, recomp_context* ctx);
void osDpGetCounters_recomp(uint8_t* rdram, recomp_context* ctx);
void __osVoiceSetADConverter_recomp(uint8_t* rdram, recomp_context* ctx);

// src/overlay_hooks.cpp (hook target)
void hyp_on_overlay_load(uint8_t* rdram, recomp_context* ctx);

// src/debug_trace.cpp (hook targets)
void hyp_trace_dma(uint8_t* rdram, recomp_context* ctx);
void hyp_trace_decompress(uint8_t* rdram, recomp_context* ctx);
void hyp_trace_stage_load(uint8_t* rdram, recomp_context* ctx);
void hyp_trace_save_request(uint8_t* rdram, recomp_context* ctx);
void hyp_force_save_check(uint8_t* rdram, recomp_context* ctx);
int hyp_trace_motion(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
