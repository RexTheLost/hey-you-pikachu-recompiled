#ifndef HYP_RENDER_H
#define HYP_RENDER_H

#include <memory>
#include <string>

#include "ultramodern/renderer_context.hpp"

namespace hyp::renderer {
    // RT64-backed renderer; the class itself lives in rt64_render_context.cpp so
    // callers don't pull in RT64/DXC headers.
    std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

    // Ripping aids (tools/rip_model.py reads the output):
    // snapshot: at the next display list, write <path>.rdram (8 MB RDRAM, 32-bit words in
    // host order) and <path>.txt (the display list address).
    void request_snapshot(const std::string& path);
    // matrix capture: for the next <tasks> display lists, append every G_MTX matrix and
    // the display list drawn with it to <path> (text).
    void request_matrix_capture(const std::string& path, int tasks);
    // debug: writes the frame the VI shows (RDRAM at VI_ORIGIN, 16- or 32-bit) to <path> as a PPM.
    // This is the game's own frame in RDRAM, not RT64's GPU picture (CPU-drawn pixels show up here).
    bool save_vi_framebuffer(const std::string& path);
}

#endif
