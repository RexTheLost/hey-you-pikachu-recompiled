#pragma once
// VR (OpenXR) for the Quest / PC VR builds. See docs/vr-plan.txt.
//
// Order: init() before RT64 creates its Vulkan device (it fills RT64's Vulkan hooks: extensions, the
// headset's GPU, and a callback that creates the OpenXR session once the device exists). RT64 then asks
// the registered RT64::VRInterface for eye views and hands each finished frame back for the headset.

#include <string>

#ifndef HYP_VR_ENABLED
// Builds without VR: everything reports "off".
namespace hyp::vr {
    inline bool requested() { return false; }
    inline bool init() { return false; }
    inline bool enabled() { return false; }
    inline unsigned refresh_rate() { return 0; }
    inline unsigned eye_width() { return 0; }
    inline bool quit_requested() { return false; }
    inline void shutdown() {}
    inline void toggle_swap_eyes() {}
    inline void request_dump(const std::string&) {}
}
#else
namespace hyp::vr {
    // VR wanted for this run: VR builds always, other builds with HYP_VR=1.
    bool requested();

    // Create the OpenXR instance and register with RT64. False: no headset/runtime (falls back to flat).
    bool init();

    bool enabled();             // init() succeeded
    unsigned refresh_rate();    // headset display rate (Hz), for RT64's frame interpolation target
    unsigned eye_width();       // eye image width (px), for RT64's render resolution
    bool quit_requested();      // the runtime asked the app to exit
    void shutdown();
    void toggle_swap_eyes();    // debug (HYP_CONTROL "vrswap"): the right eye goes into the game's own framebuffer
    void request_dump(const std::string& prefix);   // debug (HYP_CONTROL "vrdump <prefix>"): save both eye images
}
#endif
