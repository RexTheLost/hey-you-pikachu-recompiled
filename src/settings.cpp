// Player settings (window menu choices), persisted as data/settings.json.
#include <cstdio>
#include <fstream>

#include "json/json.hpp"

#include "hyp_settings.h"

namespace {
    hyp::settings::Settings current;
    std::filesystem::path settings_path;
}

hyp::settings::Settings& hyp::settings::get() {
    return current;
}

void hyp::settings::load(const std::filesystem::path& data_dir) {
    settings_path = data_dir / "settings.json";
    std::ifstream in(settings_path);
    if (!in) return;
    try {
        nlohmann::json j = nlohmann::json::parse(in);
        Settings d;
        current.fullscreen = j.value("fullscreen", d.fullscreen);
        current.resolution = (Resolution)j.value("resolution", (int)d.resolution);
        current.aspect = (Aspect)j.value("aspect", (int)d.aspect);
        current.msaa = j.value("msaa", d.msaa);
        current.audio_output = j.value("audio_output", d.audio_output);
        current.volume = j.value("volume", d.volume);
        current.muted = j.value("muted", d.muted);
        current.controller = j.value("controller", d.controller);
        current.microphone = j.value("microphone", d.microphone);
        current.vr_eye_scale = j.value("vr_eye_scale", d.vr_eye_scale);
        current.vr_eye_set = j.value("vr_eye_set", d.vr_eye_set);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "settings.json ignored: %s\n", e.what());
        current = Settings{};
    }
}

void hyp::settings::save() {
    if (settings_path.empty()) return;
    nlohmann::json j{
        { "fullscreen", current.fullscreen },
        { "resolution", (int)current.resolution },
        { "aspect", (int)current.aspect },
        { "msaa", current.msaa },
        { "audio_output", current.audio_output },
        { "volume", current.volume },
        { "muted", current.muted },
        { "controller", current.controller },
        { "microphone", current.microphone },
        { "vr_eye_scale", current.vr_eye_scale },
        { "vr_eye_set", current.vr_eye_set },
    };
    std::ofstream out(settings_path);
    out << j.dump(4) << "\n";
}
