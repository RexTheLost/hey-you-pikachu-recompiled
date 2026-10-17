#ifndef HYP_RENDER_H
#define HYP_RENDER_H

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace hyp::renderer {
    // RT64-backed renderer; the class itself lives in rt64_render_context.cpp so
    // callers don't pull in RT64/DXC headers.
    std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
}

#endif
