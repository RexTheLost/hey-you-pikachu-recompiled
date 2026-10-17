#ifndef HYP_INPUT_H
#define HYP_INPUT_H

#include <cstdint>
#include <string>
#include <vector>

#include "ultramodern/input.hpp"

union SDL_Event;

namespace hyp::input {
    void handle_controller_event(const SDL_Event& event);
    // Connected game controllers, and the player's choice ("" = first connected, "-" = keyboard only).
    std::vector<std::string> controller_names();
    void select_controller(const std::string& choice);
    std::string active_controller_name();
    void poll_inputs();
    bool get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y);
    void set_rumble(int controller_num, bool rumble);
    ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num);
}

#endif
