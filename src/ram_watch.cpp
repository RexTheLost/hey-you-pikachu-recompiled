// Timer finder: while a stage runs, sample the game's 8 MB of RDRAM every few seconds and keep,
// for every 32-bit word, how often it went up and down. When the stage changes (or on request),
// log the words that moved steadily in one direction: a stage/day timer is one of them.
//
// Log lines look like:
//   [ramwatch] stage 12 ran 312 s, 31 samples: 18 steady counters
//   [ramwatch]   800F1234 u32 up    every sample  +60.0/s   1200 -> 19920   (f32 0.0 -> 0.0)
// Cheap: one compare pass over 2M words per sample (every 10 s), ~24 MB of bookkeeping.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "hyp_ramwatch.h"

namespace {
    using clock_type = std::chrono::steady_clock;
    constexpr size_t WORDS = 0x800000 / 4;           // 8 MB of RDRAM
    constexpr auto SAMPLE_EVERY = std::chrono::seconds(10);
    constexpr int MIN_SAMPLES = 6;                    // a stage must run at least ~1 minute to be reported
    constexpr size_t MAX_REPORT = 45;
    constexpr auto REPORT_EVERY = std::chrono::minutes(3);    // also report while the stage runs

    std::mutex mutex;
    std::vector<uint32_t> first, prev;
    std::vector<uint16_t> ups, downs;
    int samples = 0;
    int stage = -1;
    clock_type::time_point stage_start, first_time, last_time, next_sample, next_report;

    uint32_t word(const uint8_t* rdram, size_t i) {
        // recompiler RDRAM layout: 32-bit words in host order
        return reinterpret_cast<const uint32_t*>(rdram)[i];
    }

    void reset(const uint8_t* rdram, clock_type::time_point now) {
        first.resize(WORDS);
        prev.resize(WORDS);
        ups.assign(WORDS, 0);
        downs.assign(WORDS, 0);
        for (size_t i = 0; i < WORDS; i++) first[i] = prev[i] = word(rdram, i);
        samples = 1;
        first_time = last_time = now;
        next_sample = now + SAMPLE_EVERY;
        next_report = now + REPORT_EVERY;
    }

    float as_float(uint32_t v) {
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }

    bool plausible_float(uint32_t v) {
        const uint32_t e = (v >> 23) & 0xFF;
        return v == 0 || (e > 100 && e < 160);
    }

    void report(const char* why) {
        if (samples < MIN_SAMPLES) return;
        const double secs = std::chrono::duration<double>(last_time - first_time).count();
        struct Cand { size_t i; bool up; int steps; double rate; bool f; };
        std::vector<Cand> c;
        size_t noise = 0;
        const int need = samples - 1;                 // changed on every sample, never the other way
        for (size_t i = 0; i < WORDS; i++) {
            const int u = ups[i], d = downs[i];
            if (!((u >= need - 1 && d == 0) || (d >= need - 1 && u == 0)) || (u + d) == 0) continue;
            const uint32_t a = first[i], b = prev[i];
            const bool fl = plausible_float(a) && plausible_float(b) && (a != 0 || b != 0) &&
                            std::abs((double)as_float(b) - (double)as_float(a)) < 1e7;
            double rate;
            if (fl) rate = ((double)as_float(b) - (double)as_float(a)) / std::max(secs, 1.0);
            else rate = ((double)(int64_t)(int32_t)b - (double)(int64_t)(int32_t)a) / std::max(secs, 1.0);
            // known noise: audio buffer counters (~174/s) and frame counters (60/s, ~56/s)
            const double ar = std::abs(rate);
            if (!fl && ((ar > 165 && ar < 185) || (ar > 50 && ar < 62))) { noise++; continue; }
            c.push_back({ i, u > 0, u + d, rate, fl });
        }
        // Stack/scratch words churn; prefer the steadiest (changed every sample) and small rates first.
        std::sort(c.begin(), c.end(), [](const Cand& x, const Cand& y) {
            if (x.up != y.up) return !x.up;                       // countdowns first
            if (x.steps != y.steps) return x.steps > y.steps;
            return std::abs(x.rate) < std::abs(y.rate);
        });
        std::printf("[ramwatch] stage %d ran %.0f s, %d samples (%s): %zu steady counters (+%zu audio/frame counters skipped)\n",
                    stage, std::chrono::duration<double>(last_time - stage_start).count(), samples, why, c.size(), noise);
        for (size_t k = 0; k < c.size() && k < MAX_REPORT; k++) {
            const Cand& x = c[k];
            const uint32_t a = first[x.i], b = prev[x.i];
            if (x.f) {
                std::printf("[ramwatch]   %08X f32 %-4s %d/%d  %+.4f/s  %g -> %g\n", 0x80000000u + (uint32_t)x.i * 4,
                            x.up ? "up" : "down", x.steps, samples - 1, x.rate, as_float(a), as_float(b));
            } else {
                std::printf("[ramwatch]   %08X u32 %-4s %d/%d  %+.2f/s  %d -> %d (0x%X -> 0x%X)\n", 0x80000000u + (uint32_t)x.i * 4,
                            x.up ? "up" : "down", x.steps, samples - 1, x.rate, (int32_t)a, (int32_t)b, a, b);
            }
        }
    }
}

void hyp::ramwatch::tick(const uint8_t* rdram) {
    std::lock_guard lock{ mutex };
    const auto now = clock_type::now();
    if (stage < 0) return;                            // no stage yet
    if (first.empty()) {
        reset(rdram, now);
        return;
    }
    if (now < next_sample) return;
    next_sample = now + SAMPLE_EVERY;
    for (size_t i = 0; i < WORDS; i++) {
        const uint32_t v = word(rdram, i);
        const uint32_t p = prev[i];
        if (v != p) {
            // compare as signed ints; floats of one sign order the same way as their bits
            const bool up = (int32_t)v > (int32_t)p;
            if (up) { if (ups[i] < 0xFFFF) ups[i]++; }
            else { if (downs[i] < 0xFFFF) downs[i]++; }
            prev[i] = v;
        }
    }
    samples++;
    last_time = now;
    if (now >= next_report) {
        next_report = now + REPORT_EVERY;
        report("still running");
    }
}

void hyp::ramwatch::stage_changed(const uint8_t* rdram, int new_stage, int flag) {
    std::lock_guard lock{ mutex };
    const auto now = clock_type::now();
    if (!first.empty() && new_stage == stage && flag == 1) {
        // the same stage reloaded (walking between its areas): a day timer keeps running, keep watching
        std::printf("[ramwatch] stage %d area change, still watching (%d samples)\n", stage, samples);
        return;
    }
    if (!first.empty()) report("stage changed");
    stage = new_stage;
    stage_start = now;
    reset(rdram, now);
}
