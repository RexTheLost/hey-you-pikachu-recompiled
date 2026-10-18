// Closed-vocabulary recognizer: whisper.cpp transcript -> closest active word.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "whisper.h"

#include "hyp_asr.h"

namespace {
    std::mutex ctx_mutex;
    whisper_context* ctx = nullptr;

    void log_callback(ggml_log_level level, const char* text, void*) {
        if (level >= GGML_LOG_LEVEL_ERROR || std::getenv("HYP_ASR_LOG") != nullptr) std::fputs(text, stderr);
    }

    // Lowercase letters, digits and single spaces only.
    std::string normalize(const std::string& s) {
        std::string out;
        bool space = true;
        for (unsigned char c : s) {
            if (std::isalnum(c)) {
                out += (char)std::tolower(c);
                space = false;
            } else if (c == '\'') {
                // "don't" -> "dont"
            } else if (!space) {
                out += ' ';
                space = true;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }

    std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> words;
        std::istringstream in(s);
        for (std::string w; in >> w;) words.push_back(w);
        return words;
    }

    // Rough sound-alike key: spelling variants that sound the same collapse together
    // ("pikachu"/"peekachoo" -> "pkkx" style skeletons compare closely).
    std::string sound_key(const std::string& word) {
        std::string w;
        for (char c : word) if (c != ' ') w += c;
        std::string out;
        for (size_t i = 0; i < w.size(); i++) {
            char c = w[i];
            char next = i + 1 < w.size() ? w[i + 1] : 0;
            std::string add;
            switch (c) {
                case 'a': case 'e': case 'i': case 'o': case 'u': case 'y': {
                    // A vowel run keeps only its first vowel, reduced to a class:
                    // a / e,i,y / o,u ("pikachu" ~ "peekachoo", but "banana" != "petunia").
                    bool after_vowel = i > 0 && std::strchr("aeiouy", w[i - 1]) != nullptr;
                    add = after_vowel ? "" : c == 'a' ? "a" : (c == 'o' || c == 'u') ? "u" : "i";
                    break;
                }
                case 'p': add = next == 'h' ? (i++, "f") : "p"; break;
                case 'c':
                    if (next == 'h') { add = "x"; i++; }
                    else if (next == 'k') { add = "k"; i++; }
                    else if (next == 'e' || next == 'i' || next == 'y') add = "s";
                    else add = "k";
                    break;
                case 's': add = next == 'h' ? (i++, "x") : "s"; break;
                case 't': add = next == 'h' ? (i++, "0") : "t"; break;
                case 'q': add = "k"; break;
                case 'x': add = "ks"; break;
                case 'z': add = "s"; break;
                case 'g': add = next == 'h' ? (i++, "") : "k"; break;
                case 'j': add = "x"; break;
                case 'w': case 'h': add = ""; break;
                case 'd': add = "t"; break;
                case 'b': add = "p"; break;
                case 'v': add = "f"; break;
                default: add = std::string(1, c); break;
            }
            for (char a : add) if (out.empty() || out.back() != a) out += a;
        }
        return out;
    }

    float ratio(const std::string& a, const std::string& b) {
        if (a.empty() && b.empty()) return 1.0f;
        std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++) prev[j] = j;
        for (size_t i = 1; i <= a.size(); i++) {
            cur[0] = i;
            for (size_t j = 1; j <= b.size(); j++) {
                cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1) });
            }
            std::swap(prev, cur);
        }
        return 1.0f - (float)prev[b.size()] / (float)std::max(a.size(), b.size());
    }

    // Sound-alike similarity; short keys ("pa" for "bye") match too easily, so they count less.
    float sound_ratio(const std::string& got, const std::string& want) {
        std::string a = sound_key(got), b = sound_key(want);
        float weight = 0.9f * std::min(1.0f, (float)std::min(a.size(), b.size()) / 4.0f);
        return weight * ratio(a, b);
    }

    // Similarity of a phrase to (part of) a transcript, 0..1.
    float similarity(const std::vector<std::string>& heard, const std::string& phrase) {
        auto want = split(phrase);
        if (want.empty() || heard.empty()) return 0.0f;
        std::string want_joined;
        for (const auto& w : want) want_joined += w;
        float best = 0.0f;
        // Compare against every run of heard words of about the phrase's length.
        for (size_t len = std::max<size_t>(1, want.size() - 1); len <= want.size() + 1; len++) {
            for (size_t start = 0; start + len <= heard.size(); start++) {
                std::string got;
                for (size_t k = start; k < start + len; k++) got += heard[k];
                if (got == want_joined) return 1.0f;
                float letters = ratio(got, want_joined);
                // Sound-alike counts a bit less than spelling; spelling breaks ties.
                float score = std::max(letters, sound_ratio(got, want_joined)) + 0.01f * letters;
                // Other words around the phrase ("hey pikachu!") cost a little, and more
                // when they come first. Repeats of the matched words are free: whisper
                // often loops on short clips ("flower, flower, flower...").
                auto in_window = [&](const std::string& w) {
                    for (size_t k = start; k < start + len; k++) if (heard[k] == w) return true;
                    return false;
                };
                int before = 0, after = 0;
                for (size_t k = 0; k < start; k++) before += !in_window(heard[k]);
                for (size_t k = start + len; k < heard.size(); k++) after += !in_window(heard[k]);
                score -= 0.02f * (float)(before + after) + 0.04f * (float)before;
                best = std::max(best, score);
            }
        }
        if (want.size() >= heard.size()) {
            std::string got;
            for (const auto& w : heard) got += w;
            float letters = ratio(got, want_joined);
            best = std::max(best, std::max(letters, sound_ratio(got, want_joined)) + 0.01f * letters);
        }
        return best;
    }
}

bool hyp::asr::load_model(const std::filesystem::path& path) {
    std::lock_guard lock{ ctx_mutex };
    if (ctx != nullptr) return true;
    whisper_log_set(log_callback, nullptr);
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false;
    ctx = whisper_init_from_file_with_params(path.string().c_str(), cparams);
    return ctx != nullptr;
}

bool hyp::asr::model_loaded() {
    std::lock_guard lock{ ctx_mutex };
    return ctx != nullptr;
}

hyp::asr::Result hyp::asr::match(const std::string& transcript, const std::vector<std::string>& phrases) {
    Result result;
    result.text = transcript;
    auto heard = split(normalize(transcript));
    for (size_t i = 0; i < phrases.size(); i++) {
        float score = similarity(heard, normalize(phrases[i]));
        // Prefer the longer phrase on ties ("come here" over "here").
        if (score > result.score + 1e-4f ||
            (std::abs(score - result.score) <= 1e-4f && result.index >= 0 && phrases[i].size() > phrases[result.index].size())) {
            result.score = score;
            result.index = (int)i;
        }
    }
    constexpr float accept = 0.66f;
    if (result.score < accept) result.index = -1;
    return result;
}

hyp::asr::Result hyp::asr::recognize(const std::vector<float>& pcm, const std::vector<std::string>& phrases) {
    auto start = std::chrono::steady_clock::now();
    std::string text;
    {
        std::lock_guard lock{ ctx_mutex };
        if (ctx == nullptr) return {};

        // Bias the decoder toward what the game is listening for. Written as separate
        // short sentences: a comma-separated list primes whisper to answer with a list
        // and loop ("flower, flower, flower...").
        std::string prompt;
        int style = std::getenv("HYP_ASR_PROMPT") ? std::atoi(std::getenv("HYP_ASR_PROMPT")) : 1;
        for (const auto& p : phrases) {
            if (style == 2) break;
            if (prompt.find(p) != std::string::npos) continue;
            if (!prompt.empty()) prompt += style == 0 ? ", " : ". ";
            prompt += p;
            if (style == 1 && !p.empty()) prompt[prompt.size() - p.size()] = (char)std::toupper((unsigned char)p[0]);
            if (prompt.size() > 600) break;
        }
        if (!prompt.empty()) prompt += ".";

        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.n_threads = (int)std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
        params.language = "en";
        params.translate = false;
        params.no_context = true;
        params.no_timestamps = true;
        params.single_segment = true;
        params.print_special = false;
        params.print_progress = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.suppress_blank = true;
        params.suppress_nst = true;
        params.temperature = 0.0f;
        params.temperature_inc = 0.0f;   // no fallback re-decodes: keep latency predictable
        params.max_tokens = 12;
        params.initial_prompt = prompt.c_str();

        // whisper wants at least 1 s of audio; pad short utterances with silence.
        std::vector<float> audio = pcm;
        // Quiet microphones: bring the peak up to about half scale (at most 20x).
        float peak = 0.0f;
        for (float s : audio) peak = std::max(peak, std::abs(s));
        if (peak > 0.0f) {
            float gain = std::min(0.5f / peak, 20.0f);
            if (gain > 1.0f) for (float& s : audio) s *= gain;
        }
        // Encode only about as much audio as there is (whisper's default is a full
        // 30 s window): much faster for short words. HYP_ASR_AUDIO_CTX overrides (0 = full).
        int audio_ctx = std::max(256, (int)(audio.size() * 50 / sample_rate) + 128);
        if (const char* env = std::getenv("HYP_ASR_AUDIO_CTX")) audio_ctx = std::atoi(env);
        params.audio_ctx = std::clamp(audio_ctx, 0, 1500);
        const size_t min_samples = sample_rate * 11 / 10;
        if (audio.size() < min_samples) audio.resize(min_samples, 0.0f);

        if (whisper_full(ctx, params, audio.data(), (int)audio.size()) == 0) {
            for (int i = 0; i < whisper_full_n_segments(ctx); i++) text += whisper_full_get_segment_text(ctx, i);
        }
    }
    Result result = match(text, phrases);
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

// 16-bit PCM WAV (any rate, mono/stereo) -> mono float at 16 kHz.
bool hyp::asr::read_wav(const std::filesystem::path& path, std::vector<float>& out) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(in)), {});
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    uint16_t channels = 1, bits = 16;
    uint32_t rate = 16000;
    size_t pos = 12;
    while (pos + 8 <= b.size()) {
        uint32_t size;
        std::memcpy(&size, b.data() + pos + 4, 4);
        if (!std::memcmp(b.data() + pos, "fmt ", 4)) {
            std::memcpy(&channels, b.data() + pos + 10, 2);
            std::memcpy(&rate, b.data() + pos + 12, 4);
            std::memcpy(&bits, b.data() + pos + 22, 2);
        } else if (!std::memcmp(b.data() + pos, "data", 4)) {
            if (bits != 16) return false;
            size_t frames = std::min<size_t>(size, b.size() - pos - 8) / (2 * channels);
            std::vector<float> mono(frames);
            for (size_t i = 0; i < frames; i++) {
                float sum = 0;
                for (int c = 0; c < channels; c++) {
                    int16_t s;
                    std::memcpy(&s, b.data() + pos + 8 + (i * channels + c) * 2, 2);
                    sum += s / 32768.0f;
                }
                mono[i] = sum / channels;
            }
            // Linear resample to 16 kHz.
            size_t n = (size_t)((double)frames * hyp::asr::sample_rate / rate);
            out.resize(n);
            for (size_t i = 0; i < n; i++) {
                double src = (double)i * rate / hyp::asr::sample_rate;
                size_t k = (size_t)src;
                double f = src - k;
                float a = mono[std::min(k, frames - 1)], c = mono[std::min(k + 1, frames - 1)];
                out[i] = (float)(a + (c - a) * f);
            }
            return true;
        }
        pos += 8 + size + (size & 1);
    }
    return false;
}
