// Live test control channel (see include/hyp_control.h).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#endif

#include "ultramodern/ultramodern.hpp"

#include "hyp_asr.h"
#include "hyp_control.h"
#include "hyp_vru.h"

namespace {
    using clock_type = std::chrono::steady_clock;

    std::mutex mutex;
    uint16_t held_buttons = 0;
    clock_type::time_point buttons_until;
    float stick_x = 0.0f, stick_y = 0.0f;
    clock_type::time_point stick_until;
    std::atomic_bool save_requested = false;

    uint16_t button_bit(const std::string& name) {
        struct { const char* n; uint16_t b; } map[] = {
            { "a", 0x8000 }, { "b", 0x4000 }, { "z", 0x2000 }, { "start", 0x1000 },
            { "up", 0x0800 }, { "down", 0x0400 }, { "left", 0x0200 }, { "right", 0x0100 },
            { "l", 0x0020 }, { "r", 0x0010 }, { "cu", 0x0008 }, { "cd", 0x0004 }, { "cl", 0x0002 }, { "cr", 0x0001 },
        };
        for (const auto& m : map) if (name == m.n) return m.b;
        return 0;
    }

    void screenshot(const std::string& path) {
#ifdef _WIN32
        // Ask the capture script (window-only, DPI-aware) to do it, detached.
        std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File tools\\capture_window.ps1 HeyYouPikachuRecompiled \"" + path + "\"";
        STARTUPINFOA si{ sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::string dir;
        if (const char* root = std::getenv("HYP_REPO")) dir = root;
        if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
#endif
    }

    void execute(const std::string& line) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        auto now = clock_type::now();
        if (cmd == "press") {
            std::string name; double secs = 0.2;
            in >> name >> secs;
            std::lock_guard lock{ mutex };
            held_buttons |= button_bit(name);
            buttons_until = now + std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(secs));
        } else if (cmd == "stick") {
            float x = 0, y = 0; double secs = 0.5;
            in >> x >> y >> secs;
            std::lock_guard lock{ mutex };
            stick_x = x; stick_y = y;
            stick_until = now + std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(secs));
        } else if (cmd == "say") {
            std::string word; std::getline(in >> std::ws, word);
            bool ok = hyp::vru::say_word(word);
            std::printf("[control] say '%s': %s\n", word.c_str(), ok ? "queued" : "not in dictionary");
        } else if (cmd == "speak") {
            // speak <file.wav>: hold Z and use the clip as the microphone utterance
            std::string path; std::getline(in >> std::ws, path);
            std::vector<float> pcm;
            if (hyp::asr::read_wav(path, pcm)) {
                hyp::vru::mic_inject(std::move(pcm));
                std::lock_guard lock{ mutex };
                held_buttons |= button_bit("z");
                buttons_until = now + std::chrono::milliseconds(300);
            } else {
                std::printf("[control] can't read %s\n", path.c_str());
            }
        } else if (cmd == "save") {
            save_requested = true;
        } else if (cmd == "shot") {
            std::string path; std::getline(in >> std::ws, path);
            screenshot(path);
        } else if (cmd == "quit") {
            ultramodern::quit();
        }
        if (!cmd.empty()) std::printf("[control] %s\n", line.c_str());
    }
}

void hyp::control::poll() {
    static const char* path = std::getenv("HYP_CONTROL");
    if (path == nullptr) return;
    static long offset = 0;
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return;
    std::fseek(f, offset, SEEK_SET);
    char buf[512];
    while (std::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        if (line.empty() || line.back() != '\n') break; // incomplete line: read it next time
        offset += (long)line.size();
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        execute(line);
    }
    std::fclose(f);
}

void hyp::control::apply_input(uint16_t* buttons, float* x, float* y) {
    std::lock_guard lock{ mutex };
    auto now = clock_type::now();
    if (now < buttons_until) *buttons |= held_buttons; else held_buttons = 0;
    if (now < stick_until) { *x = stick_x; *y = stick_y; }
}

bool hyp::control::take_save_request() {
    return save_requested.exchange(false);
}
