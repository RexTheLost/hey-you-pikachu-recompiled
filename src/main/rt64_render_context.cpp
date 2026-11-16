// RT64 renderer context for the recompiled game.
// Adapted from Zelda64Recomp's src/main/rt64_render_context.cpp (GPL-3.0),
// without texture packs or UI render hooks.
#include <memory>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <string>
#include <cstdlib>
#include <algorithm>

#include "hle/rt64_application.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"

#include "hyp_render.h"
#include "hyp_trace.h"

namespace hyp::renderer {
    class RT64Context final : public ultramodern::renderer::RendererContext {
    public:
        RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
        ~RT64Context() override;

        bool valid() override { return static_cast<bool>(app); }

        bool update_config(const ultramodern::renderer::GraphicsConfig& old_config, const ultramodern::renderer::GraphicsConfig& new_config) override;

        void enable_instant_present() override;
        void send_dl(const OSTask* task) override;
        void update_screen() override;
        void shutdown() override;
        uint32_t get_display_framerate() const override;
        float get_resolution_scale() const override;

    private:
        std::unique_ptr<RT64::Application> app;
    };
}

static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

unsigned int MI_INTR_REG = 0;

unsigned int DPC_START_REG = 0;
unsigned int DPC_END_REG = 0;
unsigned int DPC_CURRENT_REG = 0;
unsigned int DPC_STATUS_REG = 0;
unsigned int DPC_CLOCK_REG = 0;
unsigned int DPC_BUFBUSY_REG = 0;
unsigned int DPC_PIPEBUSY_REG = 0;
unsigned int DPC_TMEM_REG = 0;

void dummy_check_interrupts() {}

RT64::UserConfiguration::Antialiasing compute_max_supported_aa(RT64::RenderSampleCounts bits) {
    if (bits & RT64::RenderSampleCount::Bits::COUNT_2) {
        if (bits & RT64::RenderSampleCount::Bits::COUNT_4) {
            if (bits & RT64::RenderSampleCount::Bits::COUNT_8) {
                return RT64::UserConfiguration::Antialiasing::MSAA8X;
            }
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        }
        return RT64::UserConfiguration::Antialiasing::MSAA2X;
    };
    return RT64::UserConfiguration::Antialiasing::None;
}

RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
    switch (option) {
        case ultramodern::renderer::AspectRatio::Original:
            return RT64::UserConfiguration::AspectRatio::Original;
        case ultramodern::renderer::AspectRatio::Expand:
            return RT64::UserConfiguration::AspectRatio::Expand;
        case ultramodern::renderer::AspectRatio::Manual:
            return RT64::UserConfiguration::AspectRatio::Manual;
        case ultramodern::renderer::AspectRatio::OptionCount:
            return RT64::UserConfiguration::AspectRatio::OptionCount;
    }
}

RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
    switch (option) {
        case ultramodern::renderer::Antialiasing::None:
            return RT64::UserConfiguration::Antialiasing::None;
        case ultramodern::renderer::Antialiasing::MSAA2X:
            return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X:
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X:
            return RT64::UserConfiguration::Antialiasing::MSAA8X;
        case ultramodern::renderer::Antialiasing::OptionCount:
            return RT64::UserConfiguration::Antialiasing::OptionCount;
    }
}

RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
    switch (option) {
        case ultramodern::renderer::RefreshRate::Original:
            return RT64::UserConfiguration::RefreshRate::Original;
        case ultramodern::renderer::RefreshRate::Display:
            return RT64::UserConfiguration::RefreshRate::Display;
        case ultramodern::renderer::RefreshRate::Manual:
            return RT64::UserConfiguration::RefreshRate::Manual;
        case ultramodern::renderer::RefreshRate::OptionCount:
            return RT64::UserConfiguration::RefreshRate::OptionCount;
    }
}

RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
    switch (option) {
        case ultramodern::renderer::HighPrecisionFramebuffer::Off:
            return RT64::UserConfiguration::InternalColorFormat::Standard;
        case ultramodern::renderer::HighPrecisionFramebuffer::On:
            return RT64::UserConfiguration::InternalColorFormat::High;
        case ultramodern::renderer::HighPrecisionFramebuffer::Auto:
            return RT64::UserConfiguration::InternalColorFormat::Automatic;
        case ultramodern::renderer::HighPrecisionFramebuffer::OptionCount:
            return RT64::UserConfiguration::InternalColorFormat::OptionCount;
    }
}

void set_application_user_config(RT64::Application* application, const ultramodern::renderer::GraphicsConfig& config) {
    switch (config.res_option) {
        default:
        case ultramodern::renderer::Resolution::Auto:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            application->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
        case ultramodern::renderer::Resolution::Original2x:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
    }

    switch (config.hr_option) {
        default:
        case ultramodern::renderer::HUDRatioMode::Original:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
            break;
        case ultramodern::renderer::HUDRatioMode::Clamp16x9:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
            application->userConfig.extAspectTarget = 16.0/9.0;
            break;
        case ultramodern::renderer::HUDRatioMode::Full:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
            break;
    }

    application->userConfig.aspectRatio = to_rt64(config.ar_option);
    application->userConfig.antialiasing = to_rt64(config.msaa_option);
    application->userConfig.refreshRate = to_rt64(config.rr_option);
    application->userConfig.refreshRateTarget = config.rr_manual_value;
    application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult rt64_result) {
    switch (rt64_result) {
        case RT64::Application::SetupResult::Success:
            return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound:
            return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:
            return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:
            return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:
            return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }

    fprintf(stderr, "Unhandled `RT64::Application::SetupResult` ?\n");
    assert(false);
    std::exit(EXIT_FAILURE);
}

ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12:
            return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan:
            return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal:
            return ultramodern::renderer::GraphicsApi::Metal;
        case RT64::UserConfiguration::GraphicsAPI::Automatic:
            return ultramodern::renderer::GraphicsApi::Auto;
    }

    fprintf(stderr, "Unhandled `RT64::UserConfiguration::GraphicsAPI` ?\n");
    assert(false);
    std::exit(EXIT_FAILURE);
}

hyp::renderer::RT64Context::RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool debug) {
    static unsigned char dummy_rom_header[0x40];

    RT64::Application::Core appCore{};
#ifdef _WIN32
    appCore.window = window_handle.window;
#else
    appCore.window = window_handle; // SDL_Window* (RT64_SDL_WINDOW_VULKAN)
#endif
    appCore.checkInterrupts = dummy_check_interrupts;

    appCore.HEADER = dummy_rom_header;
    appCore.RDRAM = rdram;
    appCore.DMEM = DMEM;
    appCore.IMEM = IMEM;

    appCore.MI_INTR_REG = &MI_INTR_REG;

    appCore.DPC_START_REG = &DPC_START_REG;
    appCore.DPC_END_REG = &DPC_END_REG;
    appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
    appCore.DPC_STATUS_REG = &DPC_STATUS_REG;
    appCore.DPC_CLOCK_REG = &DPC_CLOCK_REG;
    appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
    appCore.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
    appCore.DPC_TMEM_REG = &DPC_TMEM_REG;

    ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();

    appCore.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
    appCore.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
    appCore.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
    appCore.VI_INTR_REG = &vi_regs->VI_INTR_REG;
    appCore.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
    appCore.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
    appCore.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
    appCore.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
    appCore.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
    appCore.VI_H_START_REG = &vi_regs->VI_H_START_REG;
    appCore.VI_V_START_REG = &vi_regs->VI_V_START_REG;
    appCore.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
    appCore.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
    appCore.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

    RT64::ApplicationConfiguration appConfig;
    appConfig.useConfigurationFile = false;

    app = std::make_unique<RT64::Application>(appCore, appConfig);

    auto& cur_config = ultramodern::renderer::get_graphics_config();
    set_application_user_config(app.get(), cur_config);
    app->userConfig.developerMode = debug;
    switch (cur_config.api_option) {
        case ultramodern::renderer::GraphicsApi::D3D12:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
            break;
        case ultramodern::renderer::GraphicsApi::Vulkan:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
            break;
        default:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
            break;
    }

#ifdef _WIN32
    setup_result = map_setup_result(app->setup(window_handle.thread_id));
#else
    setup_result = map_setup_result(app->setup(0));
#endif
    chosen_api = map_graphics_api(app->chosenGraphicsAPI);
    std::printf("[start] RT64 setup result %d (0 = ok)\n", (int)setup_result);
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        app = nullptr;
        return;
    }

    app->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
}

hyp::renderer::RT64Context::~RT64Context() = default;

// Lightweight progress counters (printed once per 60 screen updates).
static uint32_t dl_count = 0;
static uint32_t screen_count = 0;

// Debug: with HYP_DL_DUMP=<file>, dump the first tasks' display lists (F3DEX2
// command layout, sub-lists followed, segments resolved) before RT64 sees them.
static FILE* dl_dump_file = nullptr;

static void dump_display_list(const uint8_t* rdram, uint32_t dl_addr, uint32_t task_index) {
    uint32_t segments[16] = {};
    uint32_t stack[18];
    int depth = 0;
    uint32_t pc = dl_addr & 0x3FFFFFF;
    int printed = 0;
    auto word = [rdram](uint32_t addr) { return *reinterpret_cast<const uint32_t*>(rdram + (addr & 0x3FFFFFC)); };
    auto resolve = [&segments](uint32_t addr) {
        return (addr & 0x80000000) ? (addr & 0x3FFFFFF) : ((segments[(addr >> 24) & 0xF] + (addr & 0xFFFFFF)) & 0x3FFFFFF);
    };
    std::fprintf(dl_dump_file, "=== task %u dl=%08X\n", task_index, dl_addr);
    while (printed < 4000) {
        uint32_t w0 = word(pc), w1 = word(pc + 4);
        uint8_t op = w0 >> 24;
        std::fprintf(dl_dump_file, "%*s%08X: %08X %08X\n", depth * 2, "", pc, w0, w1);
        printed++;
        pc += 8;
        if (op == 0xDA) {                                          // G_MTX: decode s15.16 matrix
            uint32_t m = resolve(w1);
            float f[16];
            for (int i = 0; i < 16; i++) {
                // integer halves in words 0-7, fractional halves in words 8-15 (two per word, big-endian order)
                uint32_t iw = word(m + (i / 2) * 4), fw = word(m + 32 + (i / 2) * 4);
                int16_t ip = (int16_t)((i & 1) ? (iw & 0xFFFF) : (iw >> 16));
                uint16_t fp = (uint16_t)((i & 1) ? (fw & 0xFFFF) : (fw >> 16));
                f[i] = ip + fp / 65536.0f;
            }
            std::fprintf(dl_dump_file, "%*s  mtx(param %02X): [%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]\n",
                         depth * 2, "", w0 & 0xFF, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                         f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        }
        if (op == 0x01) {                                          // G_VTX: first vertex position
            uint32_t v = resolve(w1);
            uint32_t a = word(v), b = word(v + 4);
            std::fprintf(dl_dump_file, "%*s  vtx0: %d %d %d\n", depth * 2, "",
                         (int16_t)(a >> 16), (int16_t)(a & 0xFFFF), (int16_t)(b >> 16));
        }
        if (op == 0xDB && ((w0 >> 16) & 0xFF) == 0x06) {         // G_MOVEWORD G_MW_SEGMENT
            segments[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x3FFFFFF;
        } else if (op == 0xDE) {                                   // G_DL
            if (((w0 >> 16) & 0xFF) == 0 && depth < 17) stack[depth++] = pc;
            pc = resolve(w1);
        } else if (op == 0xDF) {                                   // G_ENDDL
            if (depth == 0) break;
            pc = stack[--depth];
        }
    }
    std::fflush(dl_dump_file);
}

// ---- ripping aids ----
static std::mutex capture_mutex;
static std::string snapshot_path;
static std::string capture_path;
static int capture_tasks_left = 0;
static FILE* capture_file = nullptr;

void hyp::renderer::request_snapshot(const std::string& path) {
    std::lock_guard lock{ capture_mutex };
    snapshot_path = path;
}

void hyp::renderer::request_matrix_capture(const std::string& path, int tasks) {
    std::lock_guard lock{ capture_mutex };
    capture_path = path;
    capture_tasks_left = tasks;
}

// One task's G_MTX loads: "M <task> <mtx addr> <16 floats>" then "D <task> <dl addr>" for
// each display list called while that matrix is current.
static void capture_matrices(const uint8_t* rdram, uint32_t dl_addr, uint32_t task_index) {
    uint32_t segments[16] = {};
    uint32_t stack[18];
    int depth = 0;
    uint32_t pc = dl_addr & 0x3FFFFFF;
    auto word = [rdram](uint32_t addr) { return *reinterpret_cast<const uint32_t*>(rdram + (addr & 0x3FFFFFC)); };
    auto resolve = [&segments](uint32_t addr) {
        return (addr & 0x80000000) ? (addr & 0x3FFFFFF) : ((segments[(addr >> 24) & 0xF] + (addr & 0xFFFFFF)) & 0x3FFFFFF);
    };
    for (int n = 0; n < 200000; n++) {
        uint32_t w0 = word(pc), w1 = word(pc + 4);
        uint8_t op = w0 >> 24;
        pc += 8;
        if (op == 0xDA) {
            uint32_t m = resolve(w1);
            std::fprintf(capture_file, "M %u %08X", task_index, m);
            for (int i = 0; i < 16; i++) {
                uint32_t iw = word(m + (i / 2) * 4), fw = word(m + 32 + (i / 2) * 4);
                int16_t ip = (int16_t)((i & 1) ? (iw & 0xFFFF) : (iw >> 16));
                uint16_t fp = (uint16_t)((i & 1) ? (fw & 0xFFFF) : (fw >> 16));
                std::fprintf(capture_file, " %.6f", ip + fp / 65536.0);
            }
            std::fprintf(capture_file, "\n");
        } else if (op == 0xDB && ((w0 >> 16) & 0xFF) == 0x06) {
            segments[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x3FFFFFF;
        } else if (op == 0xDE) {
            uint32_t target = resolve(w1);
            if (depth == 0 || depth == 1) std::fprintf(capture_file, "D %u %08X %08X\n", task_index, target, w1);
            if (((w0 >> 16) & 0xFF) == 0 && depth < 17) stack[depth++] = pc;
            pc = target;
        } else if (op == 0xDF) {
            if (depth == 0) break;
            pc = stack[--depth];
        }
    }
}

static void run_captures(const uint8_t* rdram, uint32_t dl_addr, uint32_t task_index) {
    std::lock_guard lock{ capture_mutex };
    if (!snapshot_path.empty()) {
        if (FILE* f = std::fopen((snapshot_path + ".rdram").c_str(), "wb")) {
            std::fwrite(rdram, 1, 0x800000, f);
            std::fclose(f);
        }
        if (FILE* f = std::fopen((snapshot_path + ".txt").c_str(), "w")) {
            std::fprintf(f, "dl=%08X task=%u\n", dl_addr, task_index);
            std::fclose(f);
        }
        std::printf("[rip] snapshot %s (dl %08X)\n", snapshot_path.c_str(), dl_addr);
        snapshot_path.clear();
    }
    if (capture_tasks_left > 0) {
        if (capture_file == nullptr) capture_file = std::fopen(capture_path.c_str(), "w");
        if (capture_file != nullptr) {
            std::fprintf(capture_file, "T %u %08X\n", task_index, dl_addr);
            // Pikachu's object this frame: matrix buffer range and transform (scale,
            // position, 3x3 rotation), so animations can be made object-relative.
            auto word = [rdram](uint32_t addr) { return *reinterpret_cast<const uint32_t*>(rdram + (addr & 0x3FFFFFC)); };
            auto flt = [&word](uint32_t addr) { uint32_t w = word(addr); float f; std::memcpy(&f, &w, 4); return f; };
            uint32_t obj = word(0x000760A8);
            if ((obj & 0xFF000000) == 0x80000000) {
                uint32_t ext = word(obj + 0x54);
                std::fprintf(capture_file, "R %u %08X %08X\n", task_index, word(obj + 0x3C) & 0x3FFFFFF, word(obj + 0x40) & 0x3FFFFFF);
                std::fprintf(capture_file, "O %u", task_index);
                for (int i = 0; i < 3; i++) std::fprintf(capture_file, " %.7g", flt(obj + i * 4));
                for (int i = 0; i < 3; i++) std::fprintf(capture_file, " %.7g", flt(obj + 0x18 + i * 4));
                for (int i = 0; i < 9; i++) std::fprintf(capture_file, " %.7g", flt(ext + 0x18 + i * 4));
                std::fprintf(capture_file, "\n");
            }
            capture_matrices(rdram, dl_addr, task_index);
        }
        if (--capture_tasks_left == 0 && capture_file != nullptr) {
            std::fclose(capture_file);
            capture_file = nullptr;
            std::printf("[rip] matrix capture done: %s\n", capture_path.c_str());
        }
    }
}

void hyp::renderer::RT64Context::send_dl(const OSTask* task) {
    run_captures(app->core.RDRAM, task->t.data_ptr, dl_count);
    if (dl_count == 0 && std::getenv("HYP_DL_DUMP") != nullptr) {
        dl_dump_file = std::fopen(std::getenv("HYP_DL_DUMP"), "w");
    }
    // HYP_DL_DUMP_FROM=<task index> picks where the 30-task window starts (default 0).
    static const uint32_t dump_from = std::getenv("HYP_DL_DUMP_FROM") ? (uint32_t)std::atoi(std::getenv("HYP_DL_DUMP_FROM")) : 0;
    if (dl_dump_file != nullptr && dl_count >= dump_from && dl_count < dump_from + 30) {
        dump_display_list(app->core.RDRAM, task->t.data_ptr, dl_count);
    }
    dl_count++;
    app->state->rsp->reset();
    app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
}

void hyp::renderer::RT64Context::update_screen() {
    if (++screen_count % 60 == 0 && hyp_trace_enabled()) {
        std::printf("[gfx] screens=%u display_lists=%u\n", screen_count, dl_count);
    }
    app->updateScreen();
}

void hyp::renderer::RT64Context::shutdown() {
    if (app != nullptr) {
        app->end();
    }
}

bool hyp::renderer::RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config, const ultramodern::renderer::GraphicsConfig& new_config) {
    if (old_config == new_config) {
        return false;
    }

    if (new_config.wm_option != old_config.wm_option) {
        app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }

    set_application_user_config(app.get(), new_config);
    app->updateUserConfig(true);

    if (new_config.msaa_option != old_config.msaa_option) {
        app->updateMultisampling();
    }
    return true;
}

void hyp::renderer::RT64Context::enable_instant_present() {
    app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
    app->updateEnhancementConfig();
}

uint32_t hyp::renderer::RT64Context::get_display_framerate() const {
    return app->presentQueue->ext.sharedResources->swapChainRate;
}

float hyp::renderer::RT64Context::get_resolution_scale() const {
    constexpr int ReferenceHeight = 240;
    switch (app->userConfig.resolution) {
        case RT64::UserConfiguration::Resolution::WindowIntegerScale:
            if (app->sharedQueueResources->swapChainHeight > 0) {
                return std::max(float((app->sharedQueueResources->swapChainHeight + ReferenceHeight - 1) / ReferenceHeight), 1.0f);
            }
            return 1.0f;
        case RT64::UserConfiguration::Resolution::Manual:
            return float(app->userConfig.resolutionMultiplier);
        case RT64::UserConfiguration::Resolution::Original:
        default:
            return 1.0f;
    }
}

std::unique_ptr<ultramodern::renderer::RendererContext> hyp::renderer::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<hyp::renderer::RT64Context>(rdram, window_handle, developer_mode);
}
