// Window menu bar: Game / Graphics / Audio / Input / Help (Win32).
//
// The menu lives on the SDL window, so its messages arrive on the window thread
// (the one that runs update_gfx). We subclass the SDL window procedure to handle
// WM_INITMENUPOPUP (device lists and check marks are rebuilt each time a menu opens)
// and WM_COMMAND. Choices are applied immediately and saved to data/settings.json.
#include <filesystem>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

#include "SDL.h"
#include "SDL_syswm.h"

#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")
#endif

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"

#include "hyp_audio.h"
#include "hyp_input.h"
#include "hyp_menu.h"
#include "hyp_motion.h"
#include "hyp_settings.h"
#include "hyp_vru.h"

#ifdef _WIN32
namespace {
    enum : UINT {
        ID_GAME_SAVES = 100, ID_GAME_EXIT,
        ID_GFX_FULLSCREEN = 200,
        ID_GFX_SIZE_FIRST = 210,                       // + index into window_sizes
        ID_GFX_RES_ORIGINAL = 220, ID_GFX_RES_2X, ID_GFX_RES_WINDOW,
        ID_GFX_ASPECT_ORIGINAL = 230, ID_GFX_ASPECT_EXPAND,
        ID_GFX_MSAA_FIRST = 240,                       // + index into msaa_levels
        ID_AUDIO_OUT_DEFAULT = 300, ID_AUDIO_OUT_FIRST, // + device index
        ID_AUDIO_VOLUME_FIRST = 400,                   // + index into volume_levels
        ID_AUDIO_MUTE = 410,
        ID_INPUT_PAD_AUTO = 500, ID_INPUT_KEYBOARD_ONLY, ID_INPUT_PAD_FIRST, // + controller index
        ID_INPUT_MIC_DEFAULT = 600, ID_INPUT_MIC_FIRST,                      // + device index
        ID_INPUT_PICKER = 650,
        ID_HELP_CONTROLS = 700, ID_HELP_ABOUT,
        ID_TOOLS_MOTION = 800,
        MAX_DEVICES = 64,
    };

    struct Size { int w, h; const wchar_t* label; };
    const Size window_sizes[] = {
        { 640, 480, L"640 × 480 (2×)" },
        { 960, 720, L"960 × 720 (3×)" },
        { 1280, 960, L"1280 × 960 (4×)" },
        { 1600, 1200, L"1600 × 1200 (5×)" },
        { 1920, 1440, L"1920 × 1440 (6×)" },
    };
    const int msaa_levels[] = { 0, 2, 4, 8 };
    const int volume_levels[] = { 100, 75, 50, 25, 10, 5 };   // IDs 400-405 (mute is 410)

    HWND hwnd = nullptr;
    HMENU menu_bar = nullptr;
    HMENU audio_out_menu = nullptr, controller_menu = nullptr, mic_menu = nullptr;
    WNDPROC sdl_window_proc = nullptr;
    bool fullscreen = false;
    WINDOWPLACEMENT windowed_placement{ sizeof(WINDOWPLACEMENT) };

    // Device lists as shown in the open menus (menu index -> name).
    std::vector<std::string> shown_outputs, shown_controllers, shown_mics;

    std::wstring widen(const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    std::filesystem::path data_dir() {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path() / "data";
    }

    void check(HMENU m, UINT id, bool on) {
        CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }

    void clear_menu(HMENU m) {
        while (GetMenuItemCount(m) > 0) DeleteMenu(m, 0, MF_BYPOSITION);
    }

    // ---- Graphics ----

    void apply_graphics() {
        const auto& s = hyp::settings::get();
        ultramodern::renderer::GraphicsConfig config = ultramodern::renderer::get_graphics_config();
        switch (s.resolution) {
            case hyp::settings::Resolution::Original: config.res_option = ultramodern::renderer::Resolution::Original; break;
            case hyp::settings::Resolution::Original2x: config.res_option = ultramodern::renderer::Resolution::Original2x; break;
            case hyp::settings::Resolution::Window: config.res_option = ultramodern::renderer::Resolution::Auto; break;
        }
        config.ar_option = s.aspect == hyp::settings::Aspect::Expand
            ? ultramodern::renderer::AspectRatio::Expand : ultramodern::renderer::AspectRatio::Original;
        config.hr_option = ultramodern::renderer::HUDRatioMode::Original;
        switch (s.msaa) {
            default: config.msaa_option = ultramodern::renderer::Antialiasing::None; break;
            case 2: config.msaa_option = ultramodern::renderer::Antialiasing::MSAA2X; break;
            case 4: config.msaa_option = ultramodern::renderer::Antialiasing::MSAA4X; break;
            case 8: config.msaa_option = ultramodern::renderer::Antialiasing::MSAA8X; break;
        }
        config.ds_option = 1;
        // Fullscreen is borderless and handled here (set_fullscreen), not by the renderer.
        config.wm_option = ultramodern::renderer::WindowMode::Windowed;
        ultramodern::renderer::set_graphics_config(config);
    }

    void set_fullscreen(bool on) {
        if (on == fullscreen) return;
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (on) {
            GetWindowPlacement(hwnd, &windowed_placement);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetMenu(hwnd, nullptr);
            SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
            const RECT& r = mi.rcMonitor;
            SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
        } else {
            SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, menu_bar);
            SetWindowPlacement(hwnd, &windowed_placement);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        fullscreen = on;
        hyp::settings::get().fullscreen = on;
        hyp::settings::save();
    }

    // Resize so the game area (client area, below the menu bar) is exactly w x h.
    void set_client_size(int w, int h) {
        if (fullscreen) set_fullscreen(false);
        ShowWindow(hwnd, SW_RESTORE);
        RECT r{ 0, 0, w, h };
        AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), TRUE,
                                 (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE), GetDpiForWindow(hwnd));
        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    }

    bool fits_on_monitor(int w, int h) {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        return w <= mi.rcWork.right - mi.rcWork.left && h + 60 <= mi.rcWork.bottom - mi.rcWork.top;
    }

    // ---- Audio ----

    void apply_volume() {
        const auto& s = hyp::settings::get();
        hyp::audio::set_volume(s.muted ? 0.0f : s.volume / 100.0f);
    }

    // ---- Menu contents ----

    void fill_audio_outputs() {
        clear_menu(audio_out_menu);
        const auto& s = hyp::settings::get();
        shown_outputs = hyp::audio::output_devices();
        AppendMenuW(audio_out_menu, MF_STRING | (s.audio_output.empty() ? MF_CHECKED : 0), ID_AUDIO_OUT_DEFAULT, L"System default");
        AppendMenuW(audio_out_menu, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < shown_outputs.size() && i < MAX_DEVICES; i++) {
            AppendMenuW(audio_out_menu, MF_STRING | (s.audio_output == shown_outputs[i] ? MF_CHECKED : 0),
                        ID_AUDIO_OUT_FIRST + (UINT)i, widen(shown_outputs[i]).c_str());
        }
        if (shown_outputs.empty()) AppendMenuW(audio_out_menu, MF_STRING | MF_GRAYED, 0, L"(no output devices found)");
    }

    void fill_controllers() {
        clear_menu(controller_menu);
        const auto& s = hyp::settings::get();
        shown_controllers = hyp::input::controller_names();
        std::string active = hyp::input::active_controller_name();
        std::wstring auto_label = L"Automatic (all connected)";
        if (s.controller.empty() && !active.empty()) auto_label += L": " + widen(active);
        AppendMenuW(controller_menu, MF_STRING | (s.controller.empty() ? MF_CHECKED : 0), ID_INPUT_PAD_AUTO, auto_label.c_str());
        AppendMenuW(controller_menu, MF_STRING | (s.controller == "-" ? MF_CHECKED : 0), ID_INPUT_KEYBOARD_ONLY, L"Keyboard only");
        AppendMenuW(controller_menu, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < shown_controllers.size() && i < MAX_DEVICES; i++) {
            AppendMenuW(controller_menu, MF_STRING | (s.controller == shown_controllers[i] ? MF_CHECKED : 0),
                        ID_INPUT_PAD_FIRST + (UINT)i, widen(shown_controllers[i]).c_str());
        }
        if (shown_controllers.empty()) AppendMenuW(controller_menu, MF_STRING | MF_GRAYED, 0, L"(no controllers connected)");
    }

    void fill_microphones() {
        clear_menu(mic_menu);
        const auto& s = hyp::settings::get();
        shown_mics = hyp::audio::input_devices();
        AppendMenuW(mic_menu, MF_STRING | (s.microphone.empty() ? MF_CHECKED : 0), ID_INPUT_MIC_DEFAULT, L"System default");
        AppendMenuW(mic_menu, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < shown_mics.size() && i < MAX_DEVICES; i++) {
            AppendMenuW(mic_menu, MF_STRING | (s.microphone == shown_mics[i] ? MF_CHECKED : 0),
                        ID_INPUT_MIC_FIRST + (UINT)i, widen(shown_mics[i]).c_str());
        }
        if (shown_mics.empty()) AppendMenuW(mic_menu, MF_STRING | MF_GRAYED, 0, L"(no microphones found)");
        AppendMenuW(mic_menu, MF_SEPARATOR, 0, nullptr);
        const wchar_t* status = L"";
        switch (hyp::vru::mic_status()) {
            case hyp::vru::MicStatus::Ready: status = L"Speech recognition ready: hold Z, say a word, let go"; break;
            case hyp::vru::MicStatus::Loading: status = L"Speech recognition: loading the speech model…"; break;
            case hyp::vru::MicStatus::NoModel: status = L"Speech model missing: put ggml-base.en.bin in data\\models"; break;
            case hyp::vru::MicStatus::NoMicrophone: status = L"Could not open this microphone"; break;
        }
        AppendMenuW(mic_menu, MF_STRING | MF_GRAYED, 0, status);
    }

    void update_checks() {
        const auto& s = hyp::settings::get();
        check(menu_bar, ID_GFX_FULLSCREEN, fullscreen);
        RECT client;
        GetClientRect(hwnd, &client);
        for (UINT i = 0; i < std::size(window_sizes); i++) {
            check(menu_bar, ID_GFX_SIZE_FIRST + i, !fullscreen && client.right == window_sizes[i].w && client.bottom == window_sizes[i].h);
            EnableMenuItem(menu_bar, ID_GFX_SIZE_FIRST + i,
                           MF_BYCOMMAND | (fits_on_monitor(window_sizes[i].w, window_sizes[i].h) ? MF_ENABLED : MF_GRAYED));
        }
        check(menu_bar, ID_GFX_RES_ORIGINAL, s.resolution == hyp::settings::Resolution::Original);
        check(menu_bar, ID_GFX_RES_2X, s.resolution == hyp::settings::Resolution::Original2x);
        check(menu_bar, ID_GFX_RES_WINDOW, s.resolution == hyp::settings::Resolution::Window);
        check(menu_bar, ID_GFX_ASPECT_ORIGINAL, s.aspect == hyp::settings::Aspect::Original);
        check(menu_bar, ID_GFX_ASPECT_EXPAND, s.aspect == hyp::settings::Aspect::Expand);
        for (UINT i = 0; i < std::size(msaa_levels); i++) check(menu_bar, ID_GFX_MSAA_FIRST + i, s.msaa == msaa_levels[i]);
        for (UINT i = 0; i < std::size(volume_levels); i++) check(menu_bar, ID_AUDIO_VOLUME_FIRST + i, s.volume == volume_levels[i]);
        check(menu_bar, ID_AUDIO_MUTE, s.muted);
    }

    void build_menu() {
        menu_bar = CreateMenu();

        HMENU game = CreatePopupMenu();
        AppendMenuW(game, MF_STRING, ID_GAME_SAVES, L"Open &save folder");
        AppendMenuW(game, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(game, MF_STRING, ID_GAME_EXIT, L"E&xit\tAlt+F4");
        AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)game, L"&Game");

        HMENU gfx = CreatePopupMenu();
        AppendMenuW(gfx, MF_STRING, ID_GFX_FULLSCREEN, L"&Fullscreen\tF11");
        HMENU sizes = CreatePopupMenu();
        for (UINT i = 0; i < std::size(window_sizes); i++) AppendMenuW(sizes, MF_STRING, ID_GFX_SIZE_FIRST + i, window_sizes[i].label);
        AppendMenuW(gfx, MF_POPUP, (UINT_PTR)sizes, L"&Window size");
        AppendMenuW(gfx, MF_SEPARATOR, 0, nullptr);
        HMENU res = CreatePopupMenu();
        AppendMenuW(res, MF_STRING, ID_GFX_RES_ORIGINAL, L"Original (320 × 240)");
        AppendMenuW(res, MF_STRING, ID_GFX_RES_2X, L"2× (640 × 480)");
        AppendMenuW(res, MF_STRING, ID_GFX_RES_WINDOW, L"Match window (sharpest)");
        AppendMenuW(gfx, MF_POPUP, (UINT_PTR)res, L"&Rendering resolution");
        HMENU aspect = CreatePopupMenu();
        AppendMenuW(aspect, MF_STRING, ID_GFX_ASPECT_ORIGINAL, L"Original (4:3)");
        AppendMenuW(aspect, MF_STRING, ID_GFX_ASPECT_EXPAND, L"Fill window (widescreen, experimental)");
        AppendMenuW(gfx, MF_POPUP, (UINT_PTR)aspect, L"&Aspect ratio");
        HMENU msaa = CreatePopupMenu();
        AppendMenuW(msaa, MF_STRING, ID_GFX_MSAA_FIRST + 0, L"Off");
        AppendMenuW(msaa, MF_STRING, ID_GFX_MSAA_FIRST + 1, L"2× MSAA");
        AppendMenuW(msaa, MF_STRING, ID_GFX_MSAA_FIRST + 2, L"4× MSAA");
        AppendMenuW(msaa, MF_STRING, ID_GFX_MSAA_FIRST + 3, L"8× MSAA");
        AppendMenuW(gfx, MF_POPUP, (UINT_PTR)msaa, L"Anti-a&liasing");
        AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)gfx, L"G&raphics");

        HMENU audio = CreatePopupMenu();
        audio_out_menu = CreatePopupMenu();
        AppendMenuW(audio, MF_POPUP, (UINT_PTR)audio_out_menu, L"&Output device");
        HMENU volume = CreatePopupMenu();
        for (UINT i = 0; i < std::size(volume_levels); i++) {
            AppendMenuW(volume, MF_STRING, ID_AUDIO_VOLUME_FIRST + i, (std::to_wstring(volume_levels[i]) + L"%").c_str());
        }
        AppendMenuW(audio, MF_POPUP, (UINT_PTR)volume, L"&Volume");
        AppendMenuW(audio, MF_STRING, ID_AUDIO_MUTE, L"&Mute");
        AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)audio, L"&Audio");

        HMENU input = CreatePopupMenu();
        controller_menu = CreatePopupMenu();
        AppendMenuW(input, MF_POPUP, (UINT_PTR)controller_menu, L"&Controller");
        mic_menu = CreatePopupMenu();
        AppendMenuW(input, MF_POPUP, (UINT_PTR)mic_menu, L"&Microphone");
        AppendMenuW(input, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(input, MF_STRING, ID_INPUT_PICKER, L"&Word picker (talk to Pikachu)\tTab");
        AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)input, L"&Input");

        HMENU tools = CreatePopupMenu();
        AppendMenuW(tools, MF_STRING, ID_TOOLS_MOTION, L"&Motion player (make Pikachu move)\tF2");
        AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)tools, L"&Tools");

        HMENU help = CreatePopupMenu();
        AppendMenuW(help, MF_STRING, ID_HELP_CONTROLS, L"&Controls\tF1");
        AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(help, MF_STRING, ID_HELP_ABOUT, L"&About");
        AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    }

    void on_command(UINT id) {
        auto& s = hyp::settings::get();
        if (id == ID_GAME_SAVES) {
            auto saves = data_dir() / "saves";
            std::filesystem::create_directories(saves);
            ShellExecuteW(hwnd, L"open", saves.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == ID_GAME_EXIT) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (id == ID_GFX_FULLSCREEN) {
            set_fullscreen(!fullscreen);
        } else if (id >= ID_GFX_SIZE_FIRST && id < ID_GFX_SIZE_FIRST + std::size(window_sizes)) {
            set_client_size(window_sizes[id - ID_GFX_SIZE_FIRST].w, window_sizes[id - ID_GFX_SIZE_FIRST].h);
        } else if (id >= ID_GFX_RES_ORIGINAL && id <= ID_GFX_RES_WINDOW) {
            s.resolution = (hyp::settings::Resolution)(id - ID_GFX_RES_ORIGINAL);
            apply_graphics();
        } else if (id == ID_GFX_ASPECT_ORIGINAL || id == ID_GFX_ASPECT_EXPAND) {
            s.aspect = id == ID_GFX_ASPECT_EXPAND ? hyp::settings::Aspect::Expand : hyp::settings::Aspect::Original;
            apply_graphics();
        } else if (id >= ID_GFX_MSAA_FIRST && id < ID_GFX_MSAA_FIRST + std::size(msaa_levels)) {
            s.msaa = msaa_levels[id - ID_GFX_MSAA_FIRST];
            apply_graphics();
        } else if (id == ID_AUDIO_OUT_DEFAULT || (id >= ID_AUDIO_OUT_FIRST && id < ID_AUDIO_OUT_FIRST + shown_outputs.size())) {
            std::string name = id == ID_AUDIO_OUT_DEFAULT ? "" : shown_outputs[id - ID_AUDIO_OUT_FIRST];
            if (hyp::audio::set_output_device(name)) {
                s.audio_output = name;
            } else {
                MessageBoxW(hwnd, (L"Could not open \"" + widen(name) + L"\".").c_str(), L"Audio", MB_ICONWARNING);
            }
        } else if (id >= ID_AUDIO_VOLUME_FIRST && id < ID_AUDIO_VOLUME_FIRST + std::size(volume_levels)) {
            s.volume = volume_levels[id - ID_AUDIO_VOLUME_FIRST];
            s.muted = false;
            apply_volume();
        } else if (id == ID_AUDIO_MUTE) {
            s.muted = !s.muted;
            apply_volume();
        } else if (id == ID_INPUT_PAD_AUTO || id == ID_INPUT_KEYBOARD_ONLY ||
                   (id >= ID_INPUT_PAD_FIRST && id < ID_INPUT_PAD_FIRST + shown_controllers.size())) {
            s.controller = id == ID_INPUT_PAD_AUTO ? "" : id == ID_INPUT_KEYBOARD_ONLY ? "-" : shown_controllers[id - ID_INPUT_PAD_FIRST];
            hyp::input::select_controller(s.controller);
        } else if (id == ID_INPUT_MIC_DEFAULT || (id >= ID_INPUT_MIC_FIRST && id < ID_INPUT_MIC_FIRST + shown_mics.size())) {
            s.microphone = id == ID_INPUT_MIC_DEFAULT ? "" : shown_mics[id - ID_INPUT_MIC_FIRST];
            hyp::vru::mic_set_device(s.microphone);
        } else if (id == ID_INPUT_PICKER) {
            hyp::vru::toggle_picker();
        } else if (id == ID_TOOLS_MOTION) {
            hyp::motion::toggle_window();
            return;
        } else if (id == ID_HELP_CONTROLS) {
            hyp::menu::show_controls();
        } else if (id == ID_HELP_ABOUT) {
            MessageBoxW(hwnd,
                L"Hey You, Pikachu! Recompiled\n\n"
                L"A PC port made by statically recompiling the N64 game (N64Recomp), "
                L"running on N64ModernRuntime with the RT64 renderer.\n\n"
                L"Licensed under the GNU GPL v3.0. You need your own copy of the game.\n"
                L"Pokémon and Hey You, Pikachu! are trademarks of Nintendo / Creatures / GAME FREAK.",
                L"About", MB_ICONINFORMATION);
            return;
        } else {
            return;
        }
        hyp::settings::save();
    }

    LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM wparam, LPARAM lparam) {
        switch (msg) {
            case WM_INITMENUPOPUP: {
                HMENU popup = (HMENU)wparam;
                if (popup == audio_out_menu) fill_audio_outputs();
                else if (popup == controller_menu) fill_controllers();
                else if (popup == mic_menu) fill_microphones();
                update_checks();
                break;
            }
            case WM_SYSCOMMAND:
                // SDL swallows SC_KEYMENU; let Alt, Alt+letter and F10 open the menus,
                // but not Alt+Enter (fullscreen) or other combos that would only beep.
                if ((wparam & 0xFFF0) == SC_KEYMENU && !fullscreen && (lparam == 0 || std::iswalpha((wint_t)lparam))) {
                    return DefWindowProcW(h, msg, wparam, lparam);
                }
                break;
            case WM_COMMAND:
                if (HIWORD(wparam) == 0 && lparam == 0) { // from the menu
                    on_command(LOWORD(wparam));
                    return 0;
                }
                break;
        }
        return CallWindowProcW(sdl_window_proc, h, msg, wparam, lparam);
    }
}

void hyp::menu::attach(SDL_Window* window) {
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(window, &wm_info)) return;
    hwnd = wm_info.info.win.window;

    build_menu();
    // Adding the menu bar takes height from the game area; grow the window to keep it.
    RECT before;
    GetClientRect(hwnd, &before);
    SetMenu(hwnd, menu_bar);
    RECT after;
    GetClientRect(hwnd, &after);
    RECT wr;
    GetWindowRect(hwnd, &wr);
    SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, (wr.bottom - wr.top) + (before.bottom - after.bottom),
                 SWP_NOMOVE | SWP_NOZORDER);
    sdl_window_proc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)window_proc);

    // Apply the saved settings.
    const auto& s = hyp::settings::get();
    apply_graphics();
    apply_volume();
    hyp::input::select_controller(s.controller);
    if (s.fullscreen) set_fullscreen(true);
}

void* hyp::menu::native_window() {
    return hwnd;
}

void hyp::menu::toggle_fullscreen() {
    if (hwnd != nullptr) set_fullscreen(!fullscreen);
}

void hyp::menu::show_controls() {
    MessageBoxW(hwnd,
        L"Keyboard\n"
        L"  Arrow keys\tMove (control stick)\n"
        L"  X\t\tA button\n"
        L"  C\t\tB button\n"
        L"  Z\t\tZ (microphone switch)\n"
        L"  A / S\t\tL / R\n"
        L"  Enter\t\tStart\n"
        L"  I J K L\t\tC buttons\n"
        L"  T F G H\tD-pad\n"
        L"  Tab\t\tWord picker: pick what to say to Pikachu\n"
        L"  F11, Alt+Enter\tFullscreen\n"
        L"  F1\t\tThis help\n"
        L"  F2\t\tMotion player\n\n"
        L"Controller\n"
        L"  Left stick\tMove\n"
        L"  A\t\tA button\n"
        L"  B / X\t\tB button\n"
        L"  LB or LT\t\tZ (talk: hold, speak, let go)\n"
        L"  Y / RB\t\tL / R\n"
        L"  Right stick\tC buttons\n"
        L"  Start\t\tStart\n",
        L"Controls", MB_ICONINFORMATION);
}
#else
#include "ultramodern/config.hpp"
// No menu bar here (Android): set the graphics options the Windows menu would apply.
// 2x the N64 resolution (640x480) keeps phones fast; 4:3 picture with the controls beside it.
void hyp::menu::attach(SDL_Window*) {
    ultramodern::renderer::GraphicsConfig config = ultramodern::renderer::get_graphics_config();
    config.res_option = ultramodern::renderer::Resolution::Original2x;
    config.ar_option = ultramodern::renderer::AspectRatio::Original;
    config.hr_option = ultramodern::renderer::HUDRatioMode::Original;
    config.msaa_option = ultramodern::renderer::Antialiasing::None;
    config.api_option = ultramodern::renderer::GraphicsApi::Auto;
    config.rr_option = ultramodern::renderer::RefreshRate::Original;
    config.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Off;
    config.ds_option = 1;
    config.wm_option = ultramodern::renderer::WindowMode::Windowed; // the SDL window is already full screen
    ultramodern::renderer::set_graphics_config(config);
}
void hyp::menu::toggle_fullscreen() {}
void* hyp::menu::native_window() { return nullptr; }
void hyp::menu::show_controls() {}
#endif
