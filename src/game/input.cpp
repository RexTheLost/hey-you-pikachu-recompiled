// Minimal SDL input for the recompiled game: keyboard + first game controller
// mapped onto N64 controller 1.
//
// Keyboard: arrows = stick, X = A, C = B, Z = Z (VRU talk button), A = L, S = R,
//           Enter = Start, I/J/K/L = C-up/left/down/right, T/F/G/H = D-pad.
// Controller: left stick = stick, A = A, B/X = B, left trigger = Z,
//             LB = L, RB/right trigger = R, Start = Start, right stick = C buttons,
//             D-pad = D-pad.
#include <algorithm>
#include <cmath>

#include "SDL.h"

#include "hyp_input.h"

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

    float axis(SDL_GameControllerAxis a) {
        float v = SDL_GameControllerGetAxis(controller, a) / 32767.0f;
        return std::abs(v) < deadzone ? 0.0f : std::clamp(v, -1.0f, 1.0f);
    }

    bool button(SDL_GameControllerButton b) {
        return SDL_GameControllerGetButton(controller, b) != 0;
    }
}

void hyp::input::handle_controller_event(const SDL_Event& event) {
    if (event.type == SDL_CONTROLLERDEVICEADDED && controller == nullptr) {
        controller = SDL_GameControllerOpen(event.cdevice.which);
    }
    else if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller != nullptr &&
             event.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller))) {
        SDL_GameControllerClose(controller);
        controller = nullptr;
    }
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
        if (axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > c_button_threshold) b |= N64_Z;
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

    *buttons = b;
    *x = std::clamp(sx, -1.0f, 1.0f) * stick_range;
    *y = std::clamp(sy, -1.0f, 1.0f) * stick_range;
    return true;
}

void hyp::input::set_rumble(int controller_num, bool rumble) {
    if (controller_num == 0 && controller != nullptr) {
        SDL_GameControllerRumble(controller, rumble ? 0xFFFF : 0, rumble ? 0xFFFF : 0, rumble ? 1000 : 0);
    }
}

ultramodern::input::connected_device_info_t hyp::input::get_connected_device_info(int controller_num) {
    // Port 1: the player's controller. Port 4 is where the real game expects the VRU;
    // the voice unit is emulated separately through the osVoice* runtime functions.
    if (controller_num == 0) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}
