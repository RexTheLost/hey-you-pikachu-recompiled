// Minimal SDL input for the recompiled game: keyboard + first game controller
// mapped onto N64 controller 1.
//
// Keyboard: arrows = stick, X = A, C = B, Z = Z (VRU talk button), A = L, S = R,
//           Enter = Start, I/J/K/L = C-up/left/down/right, T/F/G/H = D-pad,
//           Tab = word picker ("say" a word; Z is held automatically).
// Controller: left stick = stick, A = A, B/X = B, left trigger = Z,
//             LB = L, RB/right trigger = R, Start = Start, right stick = C buttons,
//             D-pad = D-pad.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "SDL.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include "hyp_control.h"
#include "hyp_input.h"
#include "hyp_vru.h"

namespace {
    enum : uint16_t {
        N64_A = 0x8000, N64_B = 0x4000, N64_Z = 0x2000, N64_START = 0x1000,
        N64_DU = 0x0800, N64_DD = 0x0400, N64_DL = 0x0200, N64_DR = 0x0100,
        N64_L = 0x0020, N64_R = 0x0010,
        N64_CU = 0x0008, N64_CD = 0x0004, N64_CL = 0x0002, N64_CR = 0x0001,
    };

    // Real N64 sticks reach about +-85 of the +-127 range the runtime scales by.
    constexpr float stick_range = 85.0f / 127.0f;
    constexpr float deadzone = 0.15f;
    constexpr float c_button_threshold = 0.5f;

    SDL_GameController* controller = nullptr;
    std::string controller_choice; // "" = first connected, "-" = keyboard only, else a controller name
    std::recursive_mutex controller_mutex; // the menu switches controllers on the window thread

    // Open the controller matching the player's choice (window thread, under the lock).
    void open_preferred_controller() {
        if (controller != nullptr) {
            SDL_GameControllerClose(controller);
            controller = nullptr;
        }
        if (controller_choice == "-") return;
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (!SDL_IsGameController(i)) continue;
            const char* name = SDL_GameControllerNameForIndex(i);
            if (controller_choice.empty() || (name != nullptr && controller_choice == name)) {
                controller = SDL_GameControllerOpen(i);
                return;
            }
        }
    }

    float axis(SDL_GameControllerAxis a) {
        float v = SDL_GameControllerGetAxis(controller, a) / 32767.0f;
        return std::abs(v) < deadzone ? 0.0f : std::clamp(v, -1.0f, 1.0f);
    }

    bool button(SDL_GameControllerButton b) {
        return SDL_GameControllerGetButton(controller, b) != 0;
    }
}

void hyp::input::handle_controller_event(const SDL_Event& event) {
    std::lock_guard lock{ controller_mutex };
    if (event.type == SDL_CONTROLLERDEVICEADDED && controller == nullptr) {
        open_preferred_controller();
        const char* name = controller != nullptr ? SDL_GameControllerName(controller) : nullptr;
        if (name != nullptr) hyp::vru::voice_log("[input] controller connected: %s\n", name);
    }
    else if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller != nullptr &&
             event.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller))) {
        SDL_GameControllerClose(controller);
        controller = nullptr;
        hyp::vru::voice_log("[input] controller disconnected\n");
        open_preferred_controller(); // fall back to another connected controller, if any
    }
}

std::vector<std::string> hyp::input::controller_names() {
    std::vector<std::string> names;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        const char* name = SDL_GameControllerNameForIndex(i);
        names.emplace_back(name != nullptr ? name : "Controller");
    }
    return names;
}

void hyp::input::select_controller(const std::string& choice) {
    std::lock_guard lock{ controller_mutex };
    controller_choice = choice;
    open_preferred_controller();
}

std::string hyp::input::active_controller_name() {
    std::lock_guard lock{ controller_mutex };
    const char* name = controller != nullptr ? SDL_GameControllerName(controller) : nullptr;
    return name != nullptr ? name : "";
}

void hyp::input::poll_inputs() {
    // Events are pumped on the main thread (update_gfx); nothing to do here.
}

bool hyp::input::get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) {
        return false;
    }

    uint16_t b = 0;
    float sx = 0.0f, sy = 0.0f;

    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    struct { SDL_Scancode key; uint16_t bit; } key_map[] = {
        { SDL_SCANCODE_X, N64_A }, { SDL_SCANCODE_C, N64_B }, { SDL_SCANCODE_Z, N64_Z },
        { SDL_SCANCODE_A, N64_L }, { SDL_SCANCODE_S, N64_R }, { SDL_SCANCODE_RETURN, N64_START },
        { SDL_SCANCODE_I, N64_CU }, { SDL_SCANCODE_K, N64_CD }, { SDL_SCANCODE_J, N64_CL }, { SDL_SCANCODE_L, N64_CR },
        { SDL_SCANCODE_T, N64_DU }, { SDL_SCANCODE_G, N64_DD }, { SDL_SCANCODE_F, N64_DL }, { SDL_SCANCODE_H, N64_DR },
    };
    for (const auto& m : key_map) {
        if (keys[m.key]) b |= m.bit;
    }
    sx += (keys[SDL_SCANCODE_RIGHT] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_LEFT] ? 1.0f : 0.0f);
    sy += (keys[SDL_SCANCODE_UP] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_DOWN] ? 1.0f : 0.0f);

    bool keyboard_z = keys[SDL_SCANCODE_Z] != 0, pad_z = false;
    std::unique_lock controller_lock{ controller_mutex };
    if (controller != nullptr) {
        struct { SDL_GameControllerButton btn; uint16_t bit; } pad_map[] = {
            { SDL_CONTROLLER_BUTTON_A, N64_A }, { SDL_CONTROLLER_BUTTON_B, N64_B }, { SDL_CONTROLLER_BUTTON_X, N64_B },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, N64_L }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, N64_R },
            { SDL_CONTROLLER_BUTTON_START, N64_START },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, N64_DU }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, N64_DD },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, N64_DL }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, N64_DR },
        };
        for (const auto& m : pad_map) {
            if (button(m.btn)) b |= m.bit;
        }
        if (axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > c_button_threshold) {
            b |= N64_Z;
            pad_z = true;
        }
        if (axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > c_button_threshold) b |= N64_R;

        float rx = axis(SDL_CONTROLLER_AXIS_RIGHTX), ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
        if (rx > c_button_threshold) b |= N64_CR;
        if (rx < -c_button_threshold) b |= N64_CL;
        if (ry > c_button_threshold) b |= N64_CD;
        if (ry < -c_button_threshold) b |= N64_CU;

        float lx = axis(SDL_CONTROLLER_AXIS_LEFTX), ly = -axis(SDL_CONTROLLER_AXIS_LEFTY);
        if (lx != 0.0f || ly != 0.0f) {
            sx = lx;
            sy = ly;
        }
    }

    controller_lock.unlock();

    // Test aid: HYP_PRESS="start@8,a@12,z@20" taps buttons (0.25 s) at fixed times.
    {
        constexpr uint32_t STICK_UP = 0x10000, STICK_DOWN = 0x20000, STICK_LEFT = 0x40000, STICK_RIGHT = 0x80000;
        static std::vector<std::pair<double, uint32_t>> presses = [] {
            std::vector<std::pair<double, uint32_t>> p;
            const char* env = std::getenv("HYP_PRESS");
            if (env == nullptr) return p;
            struct { const char* name; uint32_t bit; } names[] = {
                { "a", N64_A }, { "b", N64_B }, { "z", N64_Z }, { "start", N64_START }, { "l", N64_L }, { "r", N64_R },
                { "up", N64_DU }, { "down", N64_DD }, { "left", N64_DL }, { "right", N64_DR },
                // analog stick directions (above the 16 button bits)
                { "sup", STICK_UP }, { "sdown", STICK_DOWN }, { "sleft", STICK_LEFT }, { "sright", STICK_RIGHT },
            };
            std::string spec = env;
            size_t pos = 0;
            while (pos < spec.size()) {
                size_t end = spec.find(',', pos);
                std::string item = spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                size_t at = item.find('@');
                for (const auto& n : names) {
                    if (at != std::string::npos && item.substr(0, at) == n.name) p.emplace_back(std::atof(item.c_str() + at + 1), n.bit);
                }
                if (end == std::string::npos) break;
                pos = end + 1;
            }
            return p;
        }();
        static const auto start = std::chrono::steady_clock::now();
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        for (const auto& [when, bit] : presses) {
            if (t >= when && t < when + 0.25) {
                switch (bit) {
                    case STICK_UP: sy = 1.0f; break;
                    case STICK_DOWN: sy = -1.0f; break;
                    case STICK_LEFT: sx = -1.0f; break;
                    case STICK_RIGHT: sx = 1.0f; break;
                    default: b |= (uint16_t)bit; break;
                }
            }
        }
    }

    // Test aid: live commands from the HYP_CONTROL file.
    hyp::control::apply_input(&b, &sx, &sy);

    // Voice log: where each Z press came from, and Z key presses the game can't see
    // because another window has the keyboard (a common reason "talking does nothing").
    {
        static bool was_z = false, warned_focus = false;
        bool z = (b & N64_Z) != 0;
        if (z && !was_z) {
            hyp::vru::voice_log("[input] Z pressed (%s)\n", keyboard_z ? "keyboard" : pad_z ? "controller trigger" : "test input");
        }
        was_z = z;
#ifdef _WIN32
        bool z_key_down = (GetAsyncKeyState('Z') & 0x8000) != 0;
        if (z_key_down && !keyboard_z && !warned_focus) {
            warned_focus = true;
            hyp::vru::voice_log("[input] the Z key is down, but the game window doesn't have the keyboard\n");
            hyp::vru::mic_notice("click the game window first: it doesn't have the keyboard");
        }
        if (!z_key_down) warned_focus = false;
#endif
    }

    // Microphone: the player's own Z press is push-to-talk; keep Z held for the game
    // while the utterance is being finished and recognized.
    if (hyp::vru::mic_talk_update((b & N64_Z) != 0)) {
        b |= N64_Z;
    }

    // A word was picked/spoken: hold the talk button so the game listens for it.
    if (hyp::vru::talk_button_held()) {
        b |= N64_Z;
    }

    *buttons = b;
    *x = std::clamp(sx, -1.0f, 1.0f) * stick_range;
    *y = std::clamp(sy, -1.0f, 1.0f) * stick_range;
    return true;
}

void hyp::input::set_rumble(int controller_num, bool rumble) {
    std::lock_guard lock{ controller_mutex };
    if (controller_num == 0 && controller != nullptr) {
        SDL_GameControllerRumble(controller, rumble ? 0xFFFF : 0, rumble ? 0xFFFF : 0, rumble ? 1000 : 0);
    }
}

ultramodern::input::connected_device_info_t hyp::input::get_connected_device_info(int controller_num) {
    // Port 1: the player's controller. Port 4: the VRU (reported as CONT_TYPE_VOICE,
    // which the game checks before osVoiceInit); its osVoice* calls are emulated in src/vru.
    if (controller_num == 0) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    if (controller_num == 3) {
        return { ultramodern::input::Device::VRU, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}
