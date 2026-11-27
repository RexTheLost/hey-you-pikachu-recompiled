#ifndef HYP_TRACE_H
#define HYP_TRACE_H

// True when the HYP_TRACE environment variable is set: enables [dma]/[lzh]/[stage]/
// [ovl]/[aud]/[gfx] diagnostic output.
bool hyp_trace_enabled();

// The stage (scene) the game loaded last (-1 before the first): 19 = title, 1 = Pikachu's house, ...
int hyp_current_stage();

#endif
