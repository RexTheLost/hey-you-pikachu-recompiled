// Emulated VRU: replaces librecomp's osVoice* stubs (ultra_stubs.cpp, which
// report "no device"). Because all nine osVoice* functions are defined here,
// that stub object is never linked in.
//
// Game-side behaviour this satisfies (see NOTES.md, "VRU"):
//  - vruInit (0x800404E8) needs osVoiceInit() == 0 and the VRU id word
//    D_8009D990 == 1 (normally read from the device inside osVoiceInit).
//  - vruLoadSet registers the active dictionary: osVoiceClearDictionary, then
//    osVoiceSetWord per word. Slot index = registration order.
//  - vruPoll (0x800409CC) calls osVoiceStartReadData when the talk button (Z)
//    is held, then osVoiceGetReadData every frame. 0xC + status 0 = "keep
//    waiting"; 0 = result ready (OSVoiceData copied, answer[0] used).
//  - voiceGetEvent (0x80012508) accepts a result if distance[0] <= 2000 and
//    voice_level >= 200.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>

#include "recomp.h"
#include "hyp_trace.h"
#include "hyp_vru.h"

namespace {
    constexpr gpr VRU_ID_WORD = (gpr)(int32_t)0x8009D990u;   // id read by osVoiceInit; vruInit wants 1
    constexpr gpr WORD_TABLE = (gpr)(int32_t)0x8010452Cu;    // {char* name, u16* voice, u32 category, u32 action}
    constexpr int WORD_TABLE_COUNT = 640;

    // OSVoiceHandle: {OSMesgQueue* mq; int channel; s32 mode; u8 status}
    constexpr int HANDLE_MQ = 0x0, HANDLE_CHANNEL = 0x4, HANDLE_MODE = 0x8, HANDLE_STATUS = 0xC;

    // libultra error codes
    constexpr int32_t CONT_ERR_VOICE_NO_RESPONSE = 0xC; // "no result yet"

    // How long the game should "hear" speech before the result arrives.
    constexpr auto utterance_delay = std::chrono::milliseconds(250);
    // Give up holding Z if the game never starts listening (e.g. not in a talk state).
    constexpr auto talk_timeout = std::chrono::milliseconds(2500);

    std::mutex mutex;
    std::map<std::vector<uint16_t>, std::string> names_by_blob; // built from the game's word table
    std::vector<hyp::vru::DictionaryWord> dict;
    unsigned dict_generation = 0;

    bool is_listening = false;
    std::chrono::steady_clock::time_point listen_start;

    int pending_slot = -1;
    std::chrono::steady_clock::time_point pending_since;

    std::vector<uint16_t> read_blob(uint8_t* rdram, gpr addr) {
        std::vector<uint16_t> blob;
        for (int i = 0; i < 64; i++) {
            uint16_t h = MEM_HU(i * 2, addr);
            if (h == 0) break;
            blob.push_back(h);
        }
        return blob;
    }

    std::string read_cstr(uint8_t* rdram, gpr addr) {
        std::string s;
        for (int i = 0; i < 64; i++) {
            char c = (char)MEM_B(i, addr);
            if (c == 0) break;
            s += c;
        }
        return s;
    }

    // "come-here" -> "come here", "Pikachu2" -> "Pikachu", "BULBASAUR" -> "Bulbasaur"
    std::string display_name(const std::string& name) {
        std::string s = name;
        while (!s.empty() && std::isdigit((unsigned char)s.back())) s.pop_back();
        std::replace(s.begin(), s.end(), '-', ' ');
        bool all_upper = std::none_of(s.begin(), s.end(), [](char c) { return std::islower((unsigned char)c); });
        if (all_upper) {
            for (size_t i = 1; i < s.size(); i++) {
                if (s[i - 1] != ' ') s[i] = (char)std::tolower((unsigned char)s[i]);
            }
        }
        return s;
    }

    void build_name_map(uint8_t* rdram) {
        if (!names_by_blob.empty()) return;
        for (int i = 0; i < WORD_TABLE_COUNT; i++) {
            gpr entry = WORD_TABLE + i * 16;
            gpr name_ptr = (gpr)(int32_t)MEM_W(0, entry);
            gpr voice_ptr = (gpr)(int32_t)MEM_W(4, entry);
            names_by_blob.emplace(read_blob(rdram, voice_ptr), read_cstr(rdram, name_ptr));
        }
    }

    void set_status(uint8_t* rdram, gpr handle, uint8_t status) {
        MEM_B(HANDLE_STATUS, handle) = status;
    }
}

// ---- public API (picker / input / tests) ----

std::vector<hyp::vru::DictionaryWord> hyp::vru::dictionary(unsigned* generation) {
    std::lock_guard lock{ mutex };
    if (generation) *generation = dict_generation;
    return dict;
}

bool hyp::vru::say_slot(int slot) {
    std::lock_guard lock{ mutex };
    if (slot < 0 || slot >= (int)dict.size()) return false;
    pending_slot = slot;
    pending_since = std::chrono::steady_clock::now();
    if (hyp_trace_enabled()) std::printf("[vru] say slot %d (%s)\n", slot, dict[slot].name.c_str());
    return true;
}

bool hyp::vru::say_word(const std::string& word) {
    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    std::string want = lower(word);
    int slot = -1;
    {
        std::lock_guard lock{ mutex };
        for (const auto& w : dict) {
            if (lower(w.display) == want || lower(w.name) == want) {
                slot = w.slot;
                break;
            }
        }
    }
    return slot >= 0 && say_slot(slot);
}

bool hyp::vru::talk_button_held() {
    std::lock_guard lock{ mutex };
    if (pending_slot < 0) return false;
    if (!is_listening && std::chrono::steady_clock::now() - pending_since > talk_timeout) {
        pending_slot = -1; // the game didn't listen; drop it
        return false;
    }
    return true;
}

bool hyp::vru::listening() {
    std::lock_guard lock{ mutex };
    return is_listening;
}

// ---- osVoice* (libultra) ----

// s32 osVoiceInit(OSMesgQueue* mq, OSVoiceHandle* handle, int channel)
extern "C" void osVoiceInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr handle = ctx->r5;
    MEM_W(HANDLE_MQ, handle) = (int32_t)ctx->r4;
    MEM_W(HANDLE_CHANNEL, handle) = (int32_t)ctx->r6;
    MEM_W(HANDLE_MODE, handle) = 0;
    set_status(rdram, handle, 0);
    MEM_W(0, VRU_ID_WORD) = 1;
    if (hyp_trace_enabled()) std::printf("[vru] init channel %d\n", (int)ctx->r6);
    ctx->r2 = 0;
}

// s32 osVoiceClearDictionary(OSVoiceHandle* handle, u8 numWords)
extern "C" void osVoiceClearDictionary_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    dict.clear();
    dict_generation++;
    pending_slot = -1;
    ctx->r2 = 0;
}

// s32 osVoiceSetWord(OSVoiceHandle* handle, u8* word)
extern "C" void osVoiceSetWord_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    build_name_map(rdram);
    auto blob = read_blob(rdram, ctx->r5);
    auto it = names_by_blob.find(blob);
    std::string name = it != names_by_blob.end() ? it->second : ("word" + std::to_string(dict.size()));
    dict.push_back({ (int)dict.size(), name, display_name(name) });
    dict_generation++;
    ctx->r2 = 0;
}

// s32 osVoiceStartReadData(OSVoiceHandle* handle)
extern "C" void osVoiceStartReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    MEM_W(HANDLE_MODE, ctx->r4) = 1;
    set_status(rdram, ctx->r4, 0);
    is_listening = true;
    listen_start = std::chrono::steady_clock::now();
    if (hyp_trace_enabled()) {
        std::printf("[vru] listening; dictionary %zu words:", dict.size());
        for (size_t i = 0; i < dict.size() && i < 12; i++) std::printf(" %s", dict[i].name.c_str());
        std::printf("%s\n", dict.size() > 12 ? " ..." : "");
    }
    ctx->r2 = 0;
}

// s32 osVoiceStopReadData(OSVoiceHandle* handle)
extern "C" void osVoiceStopReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    MEM_W(HANDLE_MODE, ctx->r4) = 0;
    set_status(rdram, ctx->r4, 0);
    is_listening = false;
    ctx->r2 = 0;
}

// s32 osVoiceGetReadData(OSVoiceHandle* handle, OSVoiceData* data)
extern "C" void osVoiceGetReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr handle = ctx->r4;
    gpr data = ctx->r5;
    std::lock_guard lock{ mutex };

    auto now = std::chrono::steady_clock::now();
    bool ready = is_listening && pending_slot >= 0 && pending_slot < (int)dict.size() &&
                 now - listen_start >= utterance_delay;
    if (!ready) {
        set_status(rdram, handle, 0);
        ctx->r2 = CONT_ERR_VOICE_NO_RESPONSE;
        return;
    }

    // OSVoiceData {u16 warning, answer_num, voice_level, voice_sn, voice_time, answer[5], distance[5]}
    MEM_H(0x00, data) = 0;       // warning
    MEM_H(0x02, data) = 1;       // answer_num
    MEM_H(0x04, data) = 1000;    // voice_level (game requires >= 200)
    MEM_H(0x06, data) = 40;      // voice_sn (signal/noise)
    MEM_H(0x08, data) = 800;     // voice_time
    MEM_H(0x0A, data) = (int16_t)pending_slot;
    MEM_H(0x14, data) = 0x100;   // distance[0] (game accepts <= 2000)
    for (int i = 1; i < 5; i++) {
        MEM_H(0x0A + i * 2, data) = 0x7FFF;
        MEM_H(0x14 + i * 2, data) = 0x7FFF;
    }
    if (hyp_trace_enabled()) std::printf("[vru] heard slot %d (%s)\n", pending_slot, dict[pending_slot].name.c_str());

    pending_slot = -1;
    is_listening = false;
    MEM_W(HANDLE_MODE, handle) = 0;
    set_status(rdram, handle, 0);
    ctx->r2 = 0;
}

// Unused by the game; defined so librecomp's stub object is never linked.
extern "C" void osVoiceCheckWord_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 0; }
extern "C" void osVoiceMaskDictionary_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 0; }
extern "C" void osVoiceControlGain_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 0; }
