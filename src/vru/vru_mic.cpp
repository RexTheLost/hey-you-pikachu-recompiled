// Microphone input for the emulated VRU.
//
// Flow (mirrors the real VRU's push-to-talk):
//  1. The player holds Z; the game calls osVoiceStartReadData -> mic_listen_start():
//     we start keeping the captured audio (plus ~0.3 s from before, so the first
//     syllable isn't lost).
//  2. The utterance ends when the player lets go of Z, after ~0.7 s of silence
//     following speech, or after 5 s.
//  3. While whisper runs, Z stays held for the game (input.cpp asks
//     mic_talk_update), so it keeps listening; then the matched word is delivered
//     through the same path as the word picker (say_slot), or a "didn't understand"
//     result if nothing matched.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include "SDL.h"

#include "hyp_asr.h"
#include "hyp_trace.h"
#include "hyp_vru.h"

namespace {
    using clock_type = std::chrono::steady_clock;
    constexpr int rate = hyp::asr::sample_rate;
    constexpr size_t preroll_samples = rate * 3 / 10;
    constexpr size_t block = rate / 50;                         // 20 ms analysis blocks
    constexpr auto end_silence = std::chrono::milliseconds(700);
    constexpr size_t max_samples = rate * 5;

    enum class State { Idle, Recording, Recognizing };

    std::mutex mutex;
    std::condition_variable wake;
    State state = State::Idle;
    SDL_AudioDeviceID device = 0;
    std::string device_name;
    std::atomic<hyp::vru::MicStatus> status = hyp::vru::MicStatus::NoModel;

    std::deque<float> preroll;
    std::vector<float> utterance;
    std::vector<float> partial_block;
    float noise_floor = 0.003f;
    bool heard_speech = false;
    float peak_rms = 0.0f;         // loudest 20 ms block of the current utterance
    clock_type::time_point last_voice;
    bool finish_requested = false;
    std::vector<float> injected;   // test audio for the next utterance

    std::mutex heard_mutex;
    std::string last_heard;
    clock_type::time_point last_heard_time;

    void set_heard(const std::string& s) {
        std::lock_guard lock{ heard_mutex };
        last_heard = s;
        last_heard_time = clock_type::now();
    }

    // Voice activity on one 20 ms block (under the lock).
    void analyze_block(const float* s, size_t n) {
        double sum = 0;
        for (size_t i = 0; i < n; i++) sum += (double)s[i] * s[i];
        float rms = (float)std::sqrt(sum / (double)n);
        if (state == State::Recording) peak_rms = std::max(peak_rms, rms);
        bool voice = rms > std::max(0.012f, noise_floor * 4.0f);
        if (voice) {
            heard_speech = true;
            last_voice = clock_type::now();
        } else {
            // Track the background level: fall fast, rise slowly.
            noise_floor = rms < noise_floor ? rms : noise_floor * 0.995f + rms * 0.005f;
        }
    }

    void SDLCALL capture_callback(void*, Uint8* stream, int len) {
        const float* s = reinterpret_cast<const float*>(stream);
        size_t n = (size_t)len / sizeof(float);
        std::lock_guard lock{ mutex };
        for (size_t i = 0; i < n; i++) {
            preroll.push_back(s[i]);
            if (preroll.size() > preroll_samples) preroll.pop_front();
            if (state == State::Recording && utterance.size() < max_samples) utterance.push_back(s[i]);
            partial_block.push_back(s[i]);
            if (partial_block.size() == block) {
                analyze_block(partial_block.data(), block);
                partial_block.clear();
            }
        }
        if (state == State::Recording) {
            bool silent_after_speech = heard_speech && clock_type::now() - last_voice > end_silence;
            if (silent_after_speech || utterance.size() >= max_samples) {
                finish_requested = true;
                wake.notify_all();
            }
        }
    }

    void open_device() {
        if (device != 0) SDL_CloseAudioDevice(device);
        SDL_AudioSpec want{};
        want.freq = rate;
        want.format = AUDIO_F32;
        want.channels = 1;
        want.samples = 512;
        want.callback = capture_callback;
        device = SDL_OpenAudioDevice(device_name.empty() ? nullptr : device_name.c_str(), 1, &want, nullptr, 0);
        if (device == 0) {
            std::fprintf(stderr, "[mic] could not open '%s': %s\n", device_name.c_str(), SDL_GetError());
            if (status != hyp::vru::MicStatus::NoModel && status != hyp::vru::MicStatus::Loading) {
                status = hyp::vru::MicStatus::NoMicrophone;
            }
            return;
        }
        SDL_PauseAudioDevice(device, 0);
        if (hyp_trace_enabled()) std::printf("[mic] capturing from '%s'\n", device_name.empty() ? "(default)" : device_name.c_str());
    }

    // Ends the recording and moves to recognition (under the lock).
    void finish_recording() {
        if (state != State::Recording) return;
        finish_requested = false;
        if (!heard_speech) {
            // Nothing said: let the game stop listening on its own.
            std::printf("[mic] no speech heard (loudest %.4f, background %.4f)\n", peak_rms, noise_floor);
            set_heard("(nothing - is the right microphone selected?)");
            state = State::Idle;
            utterance.clear();
            return;
        }
        state = State::Recognizing;
        wake.notify_all();
    }

    void recognizer_thread() {
        for (;;) {
            std::vector<float> audio;
            {
                std::unique_lock lock{ mutex };
                wake.wait(lock, [] { return finish_requested || state == State::Recognizing; });
                if (finish_requested) finish_recording();
                if (state != State::Recognizing) continue;
                audio.swap(utterance);
            }

            auto words = hyp::vru::dictionary();
            std::vector<std::string> phrases;
            for (const auto& w : words) phrases.push_back(w.display);
            auto r = hyp::asr::recognize(audio, phrases);

            if (r.index >= 0) {
                std::printf("[mic] heard \"%s\" -> %s (score %.2f, %.2f s audio, %.2f s)\n", r.text.c_str(),
                            words[r.index].name.c_str(), r.score, audio.size() / (double)rate, r.seconds);
                set_heard(words[r.index].display);
                hyp::vru::say_slot(words[r.index].slot);
            } else {
                std::printf("[mic] heard \"%s\" -> no match (best %.2f, %.2f s)\n", r.text.c_str(), r.score, r.seconds);
                set_heard(r.text.empty() ? "?" : "? (" + r.text + ")");
                hyp::vru::say_unrecognized();
            }
            std::lock_guard lock{ mutex };
            state = State::Idle;
        }
    }
}

void hyp::vru::mic_init(const std::filesystem::path& model_path, const std::string& microphone) {
    {
        std::lock_guard lock{ mutex };
        device_name = microphone;
    }
    status = MicStatus::Loading;
    std::thread([model_path] {
        if (!std::filesystem::exists(model_path)) {
            std::fprintf(stderr, "[mic] speech model not found: %s\n", model_path.string().c_str());
            status = MicStatus::NoModel;
            return;
        }
        auto start = clock_type::now();
        if (!hyp::asr::load_model(model_path)) {
            status = MicStatus::NoModel;
            return;
        }
        std::printf("[mic] speech model loaded in %.2f s\n", std::chrono::duration<double>(clock_type::now() - start).count());
        std::lock_guard lock{ mutex };
        open_device();
        if (device != 0) status = MicStatus::Ready;
        std::thread(recognizer_thread).detach();
    }).detach();
}

void hyp::vru::mic_set_device(const std::string& microphone) {
    std::lock_guard lock{ mutex };
    device_name = microphone;
    if (status == MicStatus::Ready || status == MicStatus::NoMicrophone) {
        open_device();
        status = device != 0 ? MicStatus::Ready : MicStatus::NoMicrophone;
    }
}

hyp::vru::MicStatus hyp::vru::mic_status() {
    return status;
}

void hyp::vru::mic_listen_start() {
    std::lock_guard lock{ mutex };
    if (status != MicStatus::Ready || state != State::Idle) return;
    utterance.assign(preroll.begin(), preroll.end());
    heard_speech = false;
    peak_rms = 0.0f;
    finish_requested = false;
    state = State::Recording;
    if (!injected.empty()) { // test audio: the whole utterance is already here
        utterance.swap(injected);
        injected.clear();
        heard_speech = true;
        finish_requested = true;
        wake.notify_all();
    }
}

void hyp::vru::mic_inject(std::vector<float> pcm) {
    std::lock_guard lock{ mutex };
    injected = std::move(pcm);
}

void hyp::vru::mic_listen_stop() {
    std::lock_guard lock{ mutex };
    if (state == State::Recording) { // the game gave up listening; drop the recording
        state = State::Idle;
        utterance.clear();
    }
}

bool hyp::vru::mic_talk_update(bool talk_pressed) {
    std::lock_guard lock{ mutex };
    if (state == State::Recording && !talk_pressed) {
        finish_recording();
    }
    return state != State::Idle;
}

std::string hyp::vru::mic_last_heard(double max_age_seconds) {
    std::lock_guard lock{ heard_mutex };
    if (last_heard.empty() || clock_type::now() - last_heard_time > std::chrono::duration<double>(max_age_seconds)) return "";
    return last_heard;
}
