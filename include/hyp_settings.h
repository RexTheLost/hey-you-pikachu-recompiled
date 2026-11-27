#ifndef HYP_SETTINGS_H
#define HYP_SETTINGS_H

#include <filesystem>
#include <string>

// Player settings chosen in the window menu, stored in data/settings.json.
namespace hyp::settings {
    enum class Resolution { Original, Original2x, Window };
    enum class Aspect { Original, Expand };

    struct Settings {
        // Graphics
        bool fullscreen = false;
        Resolution resolution = Resolution::Original;
        Aspect aspect = Aspect::Original;
        int msaa = 0;                 // 0, 2, 4, 8
        // Audio ("" = system default device)
        std::string audio_output;
        int volume = 100;             // percent
        bool muted = false;
        // Input ("" = first connected controller, "-" = keyboard only)
        std::string controller;
        std::string microphone;       // for the voice recognizer ("" = system default)
        // VR
        float vr_eye_scale = 1.0f;    // distance between the eyes, times the headset's own (eye distance slider)
        bool vr_eye_set = false;      // the slider popped up once at the start (after that: menu only)
    };

    Settings& get();
    void load(const std::filesystem::path& data_dir);
    void save();
}

#endif
