#ifndef HYP_TOUCH_H
#define HYP_TOUCH_H

#include <cstdint>

// Android on-screen controls (android/src/.../ControlsView.java) report their state here
// through JNI (src/android/android_glue.cpp); input.cpp merges it with keyboard/controllers.
namespace hyp::touch {
    uint16_t buttons();               // N64 button bits currently held on screen
    void stick(float* x, float* y);   // on-screen stick, -1..1 (y up)
}

#endif
