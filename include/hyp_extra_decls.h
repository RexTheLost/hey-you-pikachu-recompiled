// Prototypes for functions the recompiled code calls that are on N64Recomp's
// ignored list (so funcs.h does not declare them). Definitions come from
// N64ModernRuntime or src/runtime_gaps.cpp. Force-included when compiling
// RecompiledFuncs/*.c (gcc -include).
#ifndef HYP_EXTRA_DECLS_H
#define HYP_EXTRA_DECLS_H

#include "recomp.h"

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

#ifdef __cplusplus
}
#endif

#endif
