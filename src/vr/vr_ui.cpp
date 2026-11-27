// The floating VR panel: menu, eye distance slider, word list and spoken bug reports.
// Everything is drawn on the CPU into one RGBA canvas (vr_ui.h), redrawn only when something changes.
//
// Controls (same on every screen):
//   point the controller at the panel + trigger = click / drag      thumbstick up/down = move the highlight
//   A = choose the highlight      B = back      menu button = open / close the panel
// Eye distance: thumbstick left/right changes it smoothly, or drag the bar with the pointer.
// Bug report: hold the trigger (or A) and speak, let go; the recognizer writes it down; Send.

#include "vr_ui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "hyp_asr.h"
#include "hyp_settings.h"
#include "hyp_vru.h"
#include "vr_font_roboto.h"     // VrFontRoboto[]: Roboto-Medium.ttf (Apache 2.0), embedded at build time

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

namespace {
    using hyp::vr::ui::W;
    using hyp::vr::ui::H;

    // ---------- drawing ----------

    std::vector<uint8_t> canvas(W * H * 4);
    bool dirty = true;

    struct Color { uint8_t r, g, b, a; };
    constexpr Color BG{ 18, 22, 36, 235 };
    constexpr Color PANEL{ 34, 40, 62, 255 };
    constexpr Color BUTTON{ 52, 60, 92, 255 };
    constexpr Color FOCUS{ 255, 214, 64, 255 };      // Pikachu yellow
    constexpr Color TEXT{ 240, 240, 245, 255 };
    constexpr Color DIM{ 160, 166, 190, 255 };
    constexpr Color DARK_TEXT{ 24, 24, 30, 255 };
    constexpr Color RED{ 230, 70, 60, 255 };
    constexpr Color GREEN{ 90, 200, 110, 255 };

    void blend(int x, int y, Color c, float alpha) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        uint8_t* p = &canvas[(y * W + x) * 4];
        const float a = alpha * c.a / 255.0f;
        p[0] = (uint8_t)(p[0] + (c.r - p[0]) * a);
        p[1] = (uint8_t)(p[1] + (c.g - p[1]) * a);
        p[2] = (uint8_t)(p[2] + (c.b - p[2]) * a);
        p[3] = (uint8_t)std::min(255.0f, p[3] + (255 - p[3]) * a);
    }

    void fill(int x, int y, int w, int h, Color c) {
        for (int j = std::max(0, y); j < std::min(H, y + h); j++) {
            for (int i = std::max(0, x); i < std::min(W, x + w); i++) {
                uint8_t* p = &canvas[(j * W + i) * 4];
                p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
            }
        }
    }

    void frame(int x, int y, int w, int h, int t, Color c) {
        fill(x, y, w, t, c); fill(x, y + h - t, w, t, c); fill(x, y, t, h, c); fill(x + w - t, y, t, h, c);
    }

    void disc(int cx, int cy, int r, Color c) {
        for (int j = -r; j <= r; j++)
            for (int i = -r; i <= r; i++) {
                float d = std::sqrt(float(i * i + j * j));
                if (d <= r) blend(cx + i, cy + j, c, std::clamp(r - d, 0.0f, 1.0f));
            }
    }

    // Fonts: ASCII baked at two sizes.
    struct Font {
        float size = 0;
        int atlasW = 1024, atlasH = 512;
        std::vector<uint8_t> atlas;
        stbtt_bakedchar chars[96];
        bool ready = false;
    };
    Font small_font, big_font;

    void bake(Font& f, float size) {
        f.size = size;
        f.atlas.assign(f.atlasW * f.atlasH, 0);
        f.ready = stbtt_BakeFontBitmap(reinterpret_cast<const unsigned char*>(VrFontRoboto), 0, size, f.atlas.data(),
                                       f.atlasW, f.atlasH, 32, 96, f.chars) > 0;
    }

    int text_width(const Font& f, const std::string& s) {
        float x = 0;
        for (unsigned char ch : s) {
            if (ch < 32 || ch >= 128) ch = '?';
            x += f.chars[ch - 32].xadvance;
        }
        return (int)x;
    }

    // Draws s with its top-left at (x, y).
    void text(const Font& f, int x, int y, const std::string& s, Color c) {
        if (!f.ready) return;
        float px = (float)x, py = (float)y + f.size * 0.8f;     // baseline
        for (unsigned char ch : s) {
            if (ch < 32 || ch >= 128) ch = '?';
            stbtt_aligned_quad q;
            stbtt_GetBakedQuad(f.chars, f.atlasW, f.atlasH, ch - 32, &px, &py, &q, 1);
            int x0 = (int)q.x0, y0 = (int)q.y0;
            int sx0 = (int)(q.s0 * f.atlasW), sy0 = (int)(q.t0 * f.atlasH);
            int w = (int)(q.x1 - q.x0), h = (int)(q.y1 - q.y0);
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++) {
                    uint8_t a = f.atlas[(sy0 + j) * f.atlasW + sx0 + i];
                    if (a) blend(x0 + i, y0 + j, c, a / 255.0f);
                }
        }
    }

    void text_centered(const Font& f, int cx, int y, const std::string& s, Color c) {
        text(f, cx - text_width(f, s) / 2, y, s, c);
    }

    // Wraps s into lines no wider than w.
    std::vector<std::string> wrap(const Font& f, const std::string& s, int w) {
        std::vector<std::string> lines;
        std::string line, word;
        auto flush_word = [&]() {
            if (word.empty()) return;
            std::string trial = line.empty() ? word : line + " " + word;
            if (text_width(f, trial) > w && !line.empty()) {
                lines.push_back(line);
                line = word;
            } else {
                line = trial;
            }
            word.clear();
        };
        for (char ch : s) {
            if (ch == ' ' || ch == '\n') {
                flush_word();
                if (ch == '\n') { lines.push_back(line); line.clear(); }
            } else {
                word += ch;
            }
        }
        flush_word();
        if (!line.empty()) lines.push_back(line);
        return lines;
    }

    // ---------- state ----------

    enum class Screen { None, Menu, Eye, Sets, Words, Report };
    enum class ReportState { Ready, Recording, Working, Review, Sending, Done };

    struct Item {
        int x, y, w, h;
        std::string label;
        int id;
        bool slider = false;
    };

    enum Id {
        ID_EYE = 1, ID_WORDS, ID_REPORT, ID_RECENTER, ID_CLOSE,
        ID_SLIDER, ID_DEC, ID_INC, ID_RESET, ID_DONE, ID_BACK,
        ID_SEND, ID_AGAIN, ID_CANCEL, ID_SET_FIRST = 1000, ID_WORD_FIRST = 5000,
    };

    std::mutex mutex;                 // report results arrive from other threads
    Screen screen = Screen::None;
    int focus = 0;
    bool eye_first_time = false;
    float ipd_m = 0.063f;
    bool recenter_requested = false;
    bool dragging = false;
    float stick_repeat = 0;
    bool stick_neutral = true;
    float last_px = -1, last_py = -1;
    bool pointing = false;
    std::string flash;                // short message on the current screen
    float flash_time = 0;

    int set_index = 0;                // Words: the chosen set
    int list_top = 0;                 // first visible row of a long list
    constexpr int ROWS = 8;

    ReportState report_state = ReportState::Ready;
    std::string report_text;          // what the recognizer wrote down
    std::string report_message;       // Done: sent / failed
    bool report_ok = false;
    float record_time = 0;
    bool recording_by_trigger = false;
    std::atomic<bool> transcribing{ false };

    float eye_value() { return hyp::settings::get().vr_eye_scale; }

    void set_eye_value(float v) {
        v = std::clamp(std::round(v * 100.0f) / 100.0f, 0.0f, 3.0f);
        if (v != hyp::settings::get().vr_eye_scale) {
            hyp::settings::get().vr_eye_scale = v;
            dirty = true;
        }
    }

    void go(Screen s) {
        screen = s;
        focus = 0;
        list_top = 0;
        dragging = false;
        flash.clear();
        dirty = true;
    }

    void close_panel() {
        if (screen == Screen::Eye) {
            hyp::settings::get().vr_eye_set = true;
            hyp::settings::save();
        }
        if (report_state == ReportState::Recording) hyp::vru::mic_dictate_stop();
        go(Screen::None);
    }

    std::vector<std::string> words_in_set(int set) {
        std::vector<std::string> out;
        for (const auto& w : hyp::vru::all_words()) {
            if (std::find(w.sets.begin(), w.sets.end(), set) != w.sets.end()) out.push_back(w.display);
        }
        return out;
    }

    // Items of the current screen (rebuilt every frame: cheap, and always matches what is drawn).
    std::vector<Item> layout() {
        std::vector<Item> items;
        const int bw = 520, bh = 64, bx = (W - bw) / 2;
        switch (screen) {
            case Screen::Menu: {
                const char* labels[] = { "Eye distance", "Word list", "Report a bug", "Recenter view", "Close menu" };
                const int ids[] = { ID_EYE, ID_WORDS, ID_REPORT, ID_RECENTER, ID_CLOSE };
                for (int i = 0; i < 5; i++) items.push_back({ bx, 170 + i * 92, bw, bh + 8, labels[i], ids[i] });
                break;
            }
            case Screen::Eye:
                items.push_back({ 112, 330, 800, 56, "", ID_SLIDER, true });
                items.push_back({ 112, 430, 180, bh, "-", ID_DEC });
                items.push_back({ 732, 430, 180, bh, "+", ID_INC });
                items.push_back({ 312, 430, 400, bh, "Reset (normal)", ID_RESET });
                items.push_back({ 312, 560, 400, bh + 8, "Done", ID_DONE });
                break;
            case Screen::Sets: {
                items.push_back({ 60, 108, 430, 60, "Report a bug", ID_REPORT });
                items.push_back({ 534, 108, 430, 60, "Back", ID_BACK });
                const int n = hyp::vru::set_count();
                for (int r = 0; r < ROWS && list_top + r < n; r++) {
                    items.push_back({ 60, 196 + r * 66, 904, 58, hyp::vru::set_name(list_top + r), ID_SET_FIRST + list_top + r });
                }
                break;
            }
            case Screen::Words: {
                items.push_back({ 60, 108, 904, 60, "Back to word sets", ID_BACK });
                const auto words = words_in_set(set_index);
                for (int r = 0; r < ROWS && list_top + r < (int)words.size(); r++) {
                    items.push_back({ 60, 196 + r * 66, 904, 58, words[list_top + r], ID_WORD_FIRST + list_top + r });
                }
                break;
            }
            case Screen::Report:
                switch (report_state) {
                    case ReportState::Ready:
                    case ReportState::Recording:
                    case ReportState::Working:
                        items.push_back({ 312, 620, 400, bh, "Cancel", ID_CANCEL });
                        break;
                    case ReportState::Review:
                        items.push_back({ 60, 620, 280, bh, "Send", ID_SEND });
                        items.push_back({ 372, 620, 280, bh, "Say again", ID_AGAIN });
                        items.push_back({ 684, 620, 280, bh, "Cancel", ID_CANCEL });
                        break;
                    case ReportState::Sending:
                        break;
                    case ReportState::Done:
                        items.push_back({ 312, 620, 400, bh, "Close", ID_CLOSE });
                        break;
                }
                break;
            case Screen::None:
                break;
        }
        return items;
    }

    void start_recording() {
        report_state = ReportState::Recording;
        record_time = 0;
        hyp::vru::mic_dictate_start();
        dirty = true;
    }

    void stop_recording() {
        std::vector<float> pcm = hyp::vru::mic_dictate_stop();
        report_state = ReportState::Working;
        dirty = true;
        transcribing = true;
        std::thread([pcm = std::move(pcm)]() {
            std::string t = hyp::asr::transcribe(pcm);
            std::printf("[vr] report heard (%.1f s): \"%s\"\n", pcm.size() / 16000.0, t.c_str());
            std::fflush(stdout);
            std::lock_guard lock{ mutex };
            transcribing = false;
            if (screen != Screen::Report || report_state != ReportState::Working) return;
            report_text = t;
            report_state = ReportState::Review;
            if (t.empty()) {
                report_state = ReportState::Ready;
                flash = "Didn't catch that - hold the trigger and speak again.";
                flash_time = 4.0f;
            }
            dirty = true;
        }).detach();
    }

    std::string build_report() {
        char eye[64];
        std::snprintf(eye, sizeof(eye), "%.2fx (%.1f cm)", eye_value(), eye_value() * ipd_m * 100.0f);
        const int set = hyp::vru::active_set();
        std::string text = "VR bug report (spoken)\n";
        text += "version: " + hyp::vr::version_string() + "\n";
        text += "eye distance: " + std::string(eye) + "\n";
        text += "word set: " + (set >= 0 ? hyp::vru::set_name(set) : std::string("(none)")) + "\n";
        text += "last heard: " + hyp::vru::mic_last_heard(600.0) + "\n";
        text += "\n" + report_text + "\n";
        return text;
    }

    void activate(int id) {
        dirty = true;
        if (id >= ID_WORD_FIRST) {
            const auto words = words_in_set(set_index);
            const int i = id - ID_WORD_FIRST;
            if (i < (int)words.size()) {
                bool ok = hyp::vru::say_word(words[i]);
                flash = ok ? "Said \"" + words[i] + "\"" : "Pikachu isn't listening right now";
                flash_time = 2.5f;
            }
            return;
        }
        if (id >= ID_SET_FIRST) {
            set_index = id - ID_SET_FIRST;
            go(Screen::Words);
            return;
        }
        switch (id) {
            case ID_EYE: eye_first_time = false; go(Screen::Eye); break;
            case ID_WORDS: go(Screen::Sets); break;
            case ID_REPORT: report_state = ReportState::Ready; report_text.clear(); go(Screen::Report); break;
            case ID_RECENTER: recenter_requested = true; flash = "View recentred"; flash_time = 2.0f; break;
            case ID_CLOSE: close_panel(); break;
            case ID_DEC: set_eye_value(eye_value() - 0.05f); break;
            case ID_INC: set_eye_value(eye_value() + 0.05f); break;
            case ID_RESET: set_eye_value(1.0f); break;
            case ID_DONE:
                hyp::settings::get().vr_eye_set = true;
                hyp::settings::save();
                if (eye_first_time) close_panel(); else go(Screen::Menu);
                break;
            case ID_BACK: go(screen == Screen::Words ? Screen::Sets : Screen::Menu); break;
            case ID_SEND:
                report_state = ReportState::Sending;
                hyp::vr::send_report(build_report());
                break;
            case ID_AGAIN: report_state = ReportState::Ready; report_text.clear(); break;
            case ID_CANCEL:
                if (report_state == ReportState::Recording) hyp::vru::mic_dictate_stop();
                report_state = ReportState::Ready;
                go(Screen::Menu);
                break;
        }
    }

    // ---------- drawing the screens ----------

    void draw_button(const Item& it, bool focused) {
        fill(it.x, it.y, it.w, it.h, focused ? FOCUS : BUTTON);
        text_centered(small_font, it.x + it.w / 2, it.y + (it.h - (int)small_font.size) / 2, it.label, focused ? DARK_TEXT : TEXT);
    }

    void draw() {
        fill(0, 0, W, H, BG);
        frame(0, 0, W, H, 6, FOCUS);
        const auto items = layout();
        auto title = [](const std::string& t) { text_centered(big_font, W / 2, 36, t, FOCUS); };

        switch (screen) {
            case Screen::Menu:
                title("Menu");
                break;
            case Screen::Eye: {
                title("Eye distance");
                text_centered(small_font, W / 2, 120, eye_first_time ? "Set this before you play: move it until Pikachu looks right."
                                                                     : "Move it until the world looks the right size and depth.", DIM);
                text_centered(small_font, W / 2, 160, "Thumbstick left/right, or point at the bar and pull the trigger.", DIM);
                char v[96];
                std::snprintf(v, sizeof(v), "%.2fx   (%.1f cm between the eyes)", eye_value(), eye_value() * ipd_m * 100.0f);
                text_centered(big_font, W / 2, 230, v, TEXT);
                break;
            }
            case Screen::Sets:
                title("Word list");
                break;
            case Screen::Words:
                title(hyp::vru::set_name(set_index));
                break;
            case Screen::Report: {
                title("Report a bug");
                switch (report_state) {
                    case ReportState::Ready:
                        text_centered(small_font, W / 2, 200, "Hold the trigger (or A) and say what's wrong.", TEXT);
                        text_centered(small_font, W / 2, 250, "Let go when you're done. You can check it before sending.", DIM);
                        text_centered(small_font, W / 2, 300, "B = cancel", DIM);
                        break;
                    case ReportState::Recording: {
                        disc(W / 2, 290, 40, RED);
                        char t[64];
                        std::snprintf(t, sizeof(t), "Listening... %.0f s  (let go when done)", record_time);
                        text_centered(small_font, W / 2, 360, t, TEXT);
                        break;
                    }
                    case ReportState::Working:
                        text_centered(small_font, W / 2, 280, "Writing it down...", TEXT);
                        break;
                    case ReportState::Review: {
                        fill(60, 130, 904, 460, PANEL);
                        int y = 150;
                        for (const auto& line : wrap(small_font, report_text, 860)) {
                            if (y > 550) break;
                            text(small_font, 82, y, line, TEXT);
                            y += 44;
                        }
                        break;
                    }
                    case ReportState::Sending:
                        text_centered(small_font, W / 2, 280, "Sending...", TEXT);
                        break;
                    case ReportState::Done:
                        text_centered(big_font, W / 2, 260, report_ok ? "Sent - thank you!" : "Not sent", report_ok ? GREEN : RED);
                        text_centered(small_font, W / 2, 340, report_message, DIM);
                        break;
                }
                break;
            }
            case Screen::None:
                break;
        }

        for (int i = 0; i < (int)items.size(); i++) {
            const Item& it = items[i];
            const bool focused = (i == focus);
            if (it.slider) {
                fill(it.x, it.y + it.h / 2 - 6, it.w, 12, BUTTON);
                const float t = eye_value() / 3.0f;
                fill(it.x, it.y + it.h / 2 - 6, (int)(it.w * t), 12, focused ? FOCUS : TEXT);
                const int one = it.x + it.w / 3;            // the "normal" mark (1.0x)
                fill(one - 2, it.y, 4, it.h, DIM);
                disc(it.x + (int)(it.w * t), it.y + it.h / 2, 24, focused ? FOCUS : TEXT);
                text(small_font, it.x, it.y + it.h + 6, "flat", DIM);
                text_centered(small_font, one, it.y + it.h + 6, "normal", DIM);
                text(small_font, it.x + it.w - text_width(small_font, "3x"), it.y + it.h + 6, "3x", DIM);
            } else {
                draw_button(it, focused);
            }
        }

        const bool list = (screen == Screen::Sets || screen == Screen::Words);
        if (!flash.empty()) {
            text_centered(small_font, W / 2, list ? 722 : 700, flash, FOCUS);
        } else if (list && !items.empty()) {
            text(small_font, 60, 722, "Point and pull the trigger, or thumbstick + A. B = back.", DIM);
        }
        if (pointing) {
            disc((int)last_px, (int)last_py, 10, FOCUS);
            disc((int)last_px, (int)last_py, 5, DARK_TEXT);
        }
    }

    int list_length() {
        if (screen == Screen::Sets) return hyp::vru::set_count();
        if (screen == Screen::Words) return (int)words_in_set(set_index).size();
        return 0;
    }
}

void hyp::vr::ui::init() {
    bake(small_font, 32.0f);
    bake(big_font, 48.0f);
    dirty = true;
}

bool hyp::vr::ui::visible() { return screen != Screen::None; }

void hyp::vr::ui::open_eye_distance(bool first_time) {
    std::lock_guard lock{ mutex };
    eye_first_time = first_time;
    go(Screen::Eye);
    focus = 0;
}

void hyp::vr::ui::dock_pixels(bool hovered, std::vector<uint8_t>& rgba) {
    const int dw = DOCK_W, dh = DOCK_H;
    rgba.assign(size_t(dw) * dh * 4, 0);
    auto put = [&](int x, int y, Color c, float a) {
        if (x < 0 || y < 0 || x >= dw || y >= dh) return;
        uint8_t* p = &rgba[(size_t(y) * dw + x) * 4];
        const float k = a * c.a / 255.0f;
        p[0] = (uint8_t)(p[0] + (c.r - p[0]) * k);
        p[1] = (uint8_t)(p[1] + (c.g - p[1]) * k);
        p[2] = (uint8_t)(p[2] + (c.b - p[2]) * k);
        p[3] = (uint8_t)std::min(255.0f, p[3] + (255 - p[3]) * k);
    };
    // Rounded pill.
    const Color body = hovered ? FOCUS : PANEL;
    const float r = dh / 2.0f - 4;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            float cx = std::clamp((float)x, r + 4, dw - r - 4), cy = dh / 2.0f;
            float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            if (d <= r + 1) put(x, y, body, std::clamp(r + 1 - d, 0.0f, 1.0f));
            else if (d <= r + 4) put(x, y, FOCUS, std::clamp(r + 4 - d, 0.0f, 1.0f) * 0.9f);
        }
    // "Words" in the big font.
    const Font& f = big_font;
    if (!f.ready) return;
    const std::string label = "Words";
    float px = (dw - text_width(f, label)) / 2.0f, py = (dh - f.size) / 2.0f + f.size * 0.8f;
    for (unsigned char ch : label) {
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(f.chars, f.atlasW, f.atlasH, ch - 32, &px, &py, &q, 1);
        int sx0 = (int)(q.s0 * f.atlasW), sy0 = (int)(q.t0 * f.atlasH);
        for (int j = 0; j < (int)(q.y1 - q.y0); j++)
            for (int i = 0; i < (int)(q.x1 - q.x0); i++) {
                uint8_t a = f.atlas[(sy0 + j) * f.atlasW + sx0 + i];
                if (a) put((int)q.x0 + i, (int)q.y0 + j, hovered ? DARK_TEXT : TEXT, a / 255.0f);
            }
    }
}

void hyp::vr::ui::open_words_current() {
    std::lock_guard lock{ mutex };
    const int set = hyp::vru::active_set();
    if (set >= 0) {
        set_index = set;
        go(Screen::Words);
        focus = 1;                    // the first word
    } else {
        go(Screen::Sets);
    }
}

void hyp::vr::ui::set_ipd_metres(float ipd) {
    if (ipd > 0.03f && ipd < 0.09f && std::fabs(ipd - ipd_m) > 0.0005f) {
        ipd_m = ipd;
        dirty = true;
    }
}

bool hyp::vr::ui::take_recenter() {
    bool r = recenter_requested;
    recenter_requested = false;
    return r;
}

float hyp::vr::ui::eye_scale() { return eye_value(); }

void hyp::vr::ui::report_result(bool ok, const std::string& message) {
    std::lock_guard lock{ mutex };
    report_ok = ok;
    report_message = message;
    report_state = ReportState::Done;
    dirty = true;
}

bool hyp::vr::ui::update(const Input& in) {
    std::lock_guard lock{ mutex };

    if (in.menuPressed) {
        if (screen == Screen::None) go(Screen::Menu);
        else close_panel();
    }
    if (screen == Screen::None) return false;

    if (flash_time > 0) {
        flash_time -= in.dt;
        if (flash_time <= 0) { flash.clear(); dirty = true; }
    }

    auto items = layout();
    if (items.empty()) focus = 0;
    else focus = std::clamp(focus, 0, (int)items.size() - 1);

    // Pointer: hover moves the highlight.
    int hovered = -1;
    if (in.pointing) {
        for (int i = 0; i < (int)items.size(); i++) {
            const Item& it = items[i];
            if (in.px >= it.x && in.px < it.x + it.w && in.py >= it.y - (it.slider ? 20 : 0) && in.py < it.y + it.h + (it.slider ? 20 : 0)) hovered = i;
        }
        if (hovered >= 0 && hovered != focus) { focus = hovered; dirty = true; }
        if (std::fabs(in.px - last_px) > 1.5f || std::fabs(in.py - last_py) > 1.5f || !pointing) dirty = true;
        last_px = in.px; last_py = in.py;
    }
    if (pointing != in.pointing) { pointing = in.pointing; dirty = true; }

    // Bug report: hold to talk (trigger anywhere except on a button, or A).
    if (screen == Screen::Report) {
        if (report_state == ReportState::Recording) {
            record_time += in.dt;
            dirty = true;
            const bool held = recording_by_trigger ? in.triggerHeld : in.selectHeld;
            if (!held || record_time >= 30.0f) stop_recording();
            return true;
        }
        if (report_state == ReportState::Ready) {
            const bool on_button = (hovered >= 0);
            if ((in.triggerPressed && !on_button)) {
                recording_by_trigger = true;
                start_recording();
                return true;
            }
        }
    }

    // Eye distance: thumbstick left/right changes it smoothly.
    if (screen == Screen::Eye && std::fabs(in.stickX) > 0.2f) {
        set_eye_value(eye_value() + in.stickX * in.dt * 0.8f);
    }

    // Slider drag with the pointer.
    if (screen == Screen::Eye) {
        const Item& s = items[0];
        if (in.triggerPressed && hovered == 0) dragging = true;
        if (dragging) {
            if (!in.triggerHeld) dragging = false;
            else if (in.pointing) set_eye_value(std::clamp((in.px - s.x) / s.w, 0.0f, 1.0f) * 3.0f);
        }
    }

    // Thumbstick up/down: highlight (with repeat); long lists scroll.
    const float sy = in.stickY;
    if (std::fabs(sy) < 0.4f) {
        stick_neutral = true;
        stick_repeat = 0;
    } else if (screen != Screen::Eye || std::fabs(in.stickX) < 0.5f) {
        stick_repeat -= in.dt;
        if (stick_neutral || stick_repeat <= 0) {
            const int dir = sy > 0 ? -1 : 1;     // up = previous item
            const int n = list_length();
            const int fixed = (screen == Screen::Sets) ? 2 : (screen == Screen::Words) ? 1 : 0;
            if (n > 0 && focus >= fixed) {
                // in a list: move through all rows, scrolling the window
                int row = list_top + (focus - fixed) + dir;
                if (row < 0) focus = 0;
                else if (row >= n) row = n - 1;
                if (row >= 0) {
                    if (row < list_top) list_top = row;
                    if (row >= list_top + ROWS) list_top = row - ROWS + 1;
                    focus = fixed + (row - list_top);
                }
            } else {
                focus = std::clamp(focus + dir, 0, std::max(0, (int)layout().size() - 1));
            }
            stick_repeat = stick_neutral ? 0.35f : 0.12f;     // a pause after the first step, then quicker
            stick_neutral = false;
            dirty = true;
        }
    }

    // Choose: A, or trigger on a pointed button.
    items = layout();
    if (!items.empty()) focus = std::clamp(focus, 0, (int)items.size() - 1);
    if (screen == Screen::Report && report_state == ReportState::Ready && in.selectPressed) {     // A = hold to talk (Cancel: B or point)
        recording_by_trigger = false;
        start_recording();
        return true;
    }
    const bool busy = (screen == Screen::Report) && (report_state == ReportState::Working || report_state == ReportState::Sending);
    if (busy) {
        // A does nothing while it writes down / sends (B or pointing at Cancel still cancels).
    } else if (in.selectPressed && !items.empty() && !items[focus].slider) activate(items[focus].id);
    else if (in.triggerPressed && hovered >= 0 && !items[hovered].slider) activate(items[hovered].id);

    if (in.backPressed) {
        if (screen == Screen::Words) go(Screen::Sets);
        else if (screen == Screen::Menu || (screen == Screen::Eye && eye_first_time)) close_panel();
        else if (screen != Screen::Report || report_state != ReportState::Sending) go(Screen::Menu);
    }
    return screen != Screen::None;
}

bool hyp::vr::ui::take_pixels(std::vector<uint8_t>& rgba) {
    std::lock_guard lock{ mutex };
    if (!dirty || screen == Screen::None) return false;
    draw();
    dirty = false;
    rgba = canvas;
    return true;
}
