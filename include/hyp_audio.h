#ifndef HYP_AUDIO_H
#define HYP_AUDIO_H

#include <string>
#include <vector>

// Audio output control (implemented in src/main/main.cpp).
namespace hyp::audio {
    std::vector<std::string> output_devices();
    std::vector<std::string> input_devices();
    // "" = system default. Returns false (and keeps the current device) if it can't be opened.
    bool set_output_device(const std::string& name);
    void set_volume(float volume); // 0..1
}

#endif
