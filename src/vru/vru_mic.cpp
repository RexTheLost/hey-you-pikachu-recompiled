// Microphone input for the emulated VRU.
//
// Flow (mirrors the real VRU's push-to-talk):
//  1. The game listens (osVoiceStartReadData -> mic_listen_start(), usually because Z
//     is held, but sometimes on its own) and the player holds Z (mic_talk_update):
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
#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <cctype>
#include <deque>
#include <fstream>
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

    FILE* voice_log_file = nullptr;
    std::mutex log_mutex;
    bool talk_was_pressed = false;
    clock_type::time_point talk_pressed_since;
    bool warned_not_listening = false;
    bool armed = false;            // the game is listening for a spoken word
    bool press_used = false;       // this Z press already made an utterance

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
        // Twice the background level counts as voice. Headset mics can be quiet:
        // speech peaking at 0.01 over a 0.0045 background is normal for some.
        bool voice = rms > std::max(0.003f, noise_floor * 2.0f);
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
            hyp::vru::voice_log("[mic] could not open '%s': %s\n", device_name.c_str(), SDL_GetError());
            if (status != hyp::vru::MicStatus::NoModel && status != hyp::vru::MicStatus::Loading) {
                status = hyp::vru::MicStatus::NoMicrophone;
            }
            return;
        }
        SDL_PauseAudioDevice(device, 0);
        hyp::vru::voice_log("[mic] capturing from '%s'\n", device_name.empty() ? "(default)" : device_name.c_str());
    }

    // On a miss, list what the game was listening for (once per dictionary).
    void log_dictionary(const std::vector<hyp::vru::DictionaryWord>& words) {
        static std::string last;
        std::string list;
        std::string seen = "|";
        for (const auto& w : words) {
            if (seen.find("|" + w.display + "|") != std::string::npos) continue;
            seen += w.display + "|";
            list += (list.empty() ? "" : ", ") + w.display;
        }
        if (list == last) return;
        last = list;
        hyp::vru::voice_log("[mic] the game was listening for %zu words: %s\n", words.size(), list.c_str());
    }

    // Troubleshooting: HYP_MIC_SAVE=<folder> keeps every utterance as a WAV file.
    void save_clip(const std::vector<float>& audio, const std::string& text) {
        static const char* dir = std::getenv("HYP_MIC_SAVE");
        if (dir == nullptr) return;
        static int count = 0;
        std::string name;
        for (char c : text) if (std::isalnum((unsigned char)c) || c == ' ') name += c == ' ' ? '_' : c;
        if (name.size() > 40) name.resize(40);
        std::filesystem::create_directories(dir);
        char file[64];
        std::snprintf(file, sizeof(file), "%03d", count++);
        std::ofstream out(std::filesystem::path(dir) / (std::string(file) + name + ".wav"), std::ios::binary);
        auto put32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        auto put16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        uint32_t bytes = (uint32_t)audio.size() * 2;
        out.write("RIFF", 4); put32(36 + bytes); out.write("WAVEfmt ", 8);
        put32(16); put16(1); put16(1); put32(rate); put32(rate * 2); put16(2); put16(16);
        out.write("data", 4); put32(bytes);
        for (float s : audio) put16((uint16_t)(int16_t)std::clamp(s * 32767.0f, -32768.0f, 32767.0f));
    }

    // Ends the recording and moves to recognition (under the lock).
    void finish_recording() {
        if (state != State::Recording) return;
        finish_requested = false;
        // Even if the voice detector never fired, a deliberate press (0.4 s+) with any
        // sound above the background still goes to the recognizer.
        bool held_long = utterance.size() >= preroll_samples + rate * 4 / 10;
        if (!heard_speech && held_long && peak_rms > std::max(0.002f, noise_floor * 1.5f)) heard_speech = true;
        if (!heard_speech) {
            // Nothing said: let the game stop listening on its own.
            hyp::vru::voice_log("[mic] no speech heard (loudest %.4f, background %.4f)\n", peak_rms, noise_floor);
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
            save_clip(audio, r.text);

            bool blank = std::none_of(r.text.begin(), r.text.end(), [](char c) { return std::isalnum((unsigned char)c); });
            if (blank) {
                // Whisper heard no words: same as not speaking.
                hyp::vru::voice_log("[mic] no words recognized (%.2f s audio)\n", audio.size() / (double)rate);
                set_heard("(nothing)");
            } else if (r.index >= 0) {
                hyp::vru::voice_log("[mic] heard \"%s\" -> %s (score %.2f, %.2f s audio, %.2f s)\n", r.text.c_str(),
                            words[r.index].name.c_str(), r.score, audio.size() / (double)rate, r.seconds);
                set_heard(words[r.index].display);
                hyp::vru::say_slot(words[r.index].slot);
            } else {
                hyp::vru::voice_log("[mic] heard \"%s\" -> no match (best %.2f, %.2f s)\n", r.text.c_str(), r.score, r.seconds);
                log_dictionary(words);
                set_heard(r.text.empty() ? "?" : "? (" + r.text + ")");
                hyp::vru::say_unrecognized();
            }
            std::lock_guard lock{ mutex };
            state = State::Idle;
            if (!blank) armed = false; // the game ends this listen when it takes the result
        }
    }
}

void hyp::vru::voice_log(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    std::lock_guard lock{ log_mutex };
    std::fputs(line, stdout);
    if (voice_log_file != nullptr) {
        static const auto start = clock_type::now();
        std::fprintf(voice_log_file, "%8.2f  %s", std::chrono::duration<double>(clock_type::now() - start).count(), line);
        std::fflush(voice_log_file);
    }
}

void hyp::vru::mic_init(const std::filesystem::path& model_path, const std::string& microphone) {
    {
        // model: <exe>/data/models/x.bin -> log: <exe>/data/voice.log
        std::lock_guard lock{ log_mutex };
        voice_log_file = std::fopen((model_path.parent_path().parent_path() / "voice.log").string().c_str(), "w");
    }
    {
        std::lock_guard lock{ mutex };
        device_name = microphone;
    }
    status = MicStatus::Loading;
    std::thread([model_path] {
        if (!std::filesystem::exists(model_path)) {
            hyp::vru::voice_log("[mic] speech model not found: %s\n", model_path.string().c_str());
            status = MicStatus::NoModel;
            return;
        }
        auto start = clock_type::now();
        if (!hyp::asr::load_model(model_path)) {
            status = MicStatus::NoModel;
            return;
        }
        hyp::vru::voice_log("[mic] speech model loaded in %.2f s\n", std::chrono::duration<double>(clock_type::now() - start).count());
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
    // Recording starts on the player's Z press (mic_talk_update), not here: the game
    // also starts listening on its own (e.g. after throwing an item) without Z held.
    std::lock_guard lock{ mutex };
    armed = true;
}

namespace {
    // Begin an utterance (under the lock).
    void start_recording() {
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
}

void hyp::vru::mic_inject(std::vector<float> pcm) {
    std::lock_guard lock{ mutex };
    injected = std::move(pcm);
}

void hyp::vru::mic_listen_stop() {
    std::lock_guard lock{ mutex };
    armed = false;
    if (state == State::Recording) { // the game gave up listening; drop the recording
        state = State::Idle;
        utterance.clear();
    }
}

bool hyp::vru::mic_talk_update(bool talk_pressed) {
    // Holding Z while the game isn't listening (mic icon crossed out) does nothing in
    // the game; say so once per press so it doesn't look like the microphone failed.
    bool game_listening = hyp::vru::listening();
    std::lock_guard lock{ mutex };
    auto now = clock_type::now();
    if (talk_pressed && !talk_was_pressed) {
        talk_pressed_since = now;
        warned_not_listening = false;
        press_used = false;
    }
    talk_was_pressed = talk_pressed;
    // One utterance per Z press, whichever came first: the press or the game listening.
    if (talk_pressed && !press_used && armed && game_listening && state == State::Idle && status == MicStatus::Ready) {
        press_used = true;
        start_recording();
    }
    if (talk_pressed && !game_listening && state == State::Idle && !warned_not_listening &&
        now - talk_pressed_since > std::chrono::milliseconds(600)) {
        warned_not_listening = true;
        hyp::vru::voice_log("[mic] Z is held, but the game isn't listening for words right now\n");
        set_heard("(Pikachu isn't listening right now)");
    }
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
