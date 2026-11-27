#pragma once
// The floating VR panel (one OpenXR quad layer): menu, eye distance slider, word list, bug report.
// Drawn on the CPU into an RGBA canvas; hyp_vr.cpp uploads it when it changes and feeds controller input.

#include <cstdint>
#include <string>
#include <vector>

namespace hyp::vr::ui {
    constexpr int W = 1024;          // canvas size (pixels)
    constexpr int H = 768;
    constexpr float WIDTH_M = 0.9f;  // panel size in the world (metres); height follows the canvas aspect

    struct Input {
        bool menuPressed = false;    // menu button (opens / closes the panel)
        float stickX = 0, stickY = 0;// thumbstick (either hand), up = +y
        bool selectPressed = false;  // A (or X) pressed this frame
        bool selectHeld = false;     // ... still held
        bool backPressed = false;    // B (or Y) pressed this frame
        bool pointing = false;       // the controller ray hits the panel
        float px = 0, py = 0;        // ... at this canvas pixel
        bool triggerPressed = false; // trigger edge / held / released (either hand)
        bool triggerHeld = false;
        bool triggerReleased = false;
        float dt = 0;                // seconds since the last update
    };

    void init();
    // One frame. Returns true while the panel is open (then the game gets no controller input).
    bool update(const Input& in);
    bool visible();
    // Canvas pixels when they changed since the last call (RGBA, W*H*4).
    bool take_pixels(std::vector<uint8_t>& rgba);

    // The Words button that rides on the left controller (its own small quad layer).
    constexpr int DOCK_W = 320;
    constexpr int DOCK_H = 112;
    constexpr float DOCK_WIDTH_M = 0.13f;
    void dock_pixels(bool hovered, std::vector<uint8_t>& rgba);
    void open_words_current();                 // the word list, at the set Pikachu is listening for now

    void open_eye_distance(bool first_time);   // first_time: the one-time popup at the start
    void set_ipd_metres(float ipd);            // headset eye distance (shown next to the slider)
    bool take_recenter();                      // "Recenter view" was chosen
    float eye_scale();                         // current slider value (live)

    // Bug report delivery: hyp_vr.cpp calls this back with the result of send_report().
    void report_result(bool ok, const std::string& message);
}

namespace hyp::vr {
    // Sends a bug report to the report inbox (async; the result comes back through ui::report_result).
    void send_report(const std::string& text);
    std::string version_string();
    std::string device_string();
}
