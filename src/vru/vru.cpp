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
    constexpr gpr SET_TABLE = (gpr)(int32_t)0x80106D2Cu;     // 43 x {u32 count, u32* word_ids}
    constexpr int SET_COUNT = 43;

    // Names for the 43 word sets, from what each contains (the game has none).
    const char* const set_names[SET_COUNT] = {
        "General talk",                  // 0  start, on-to-the-next-stage! (heard in the bedroom)
        "Items (all)",                   // 1  every food/object name
        "Talk & answers",                // 2  thats-fine, not-that-one, way-to-go
        "Talk about an item",            // 3  toss it, how's it taste/smell/sound?
        "Talk about food",               // 4  you can taste it, put that back
        "Talk: staying over",            // 5  stay with me, stay at my house
        "Flowers",                       // 6  its running away, flower bud, rose
        "Tag & garden",                  // 7  tag you're it, watering can
        "Digging for treasure",          // 8  dig, dig there, cannon
        "Treasure",                      // 9  hang on, buried treasure, I love you
        "Hide & seek, counting",         // 10 behind you, how many?
        "Steering: left, right, stop",   // 11 go left/right, turn around, keep going
        "Cooking: recipe & ingredients", // 12 recipe, ingredients, notepad
        "Praise & insults",              // 13 great job, cool, stupid
        "Waking Pikachu",                // 14 wake up, get up, good morning
        "Collecting objects",            // 15 bring it here, marbles, magnet
        "Food",                          // 16 eat it
        "Coins & jewels",                // 17 coin, jewel, gold coin
        "At home: quiz, TV, tidying",    // 18 quiz time, tv, clean it up
        "Pok\u00E9mon names A", "Pok\u00E9mon names B", "Pok\u00E9mon names C", "Pok\u00E9mon names D",
        "Pok\u00E9mon names E", "Pok\u00E9mon names F", "Pok\u00E9mon names G", "Pok\u00E9mon names H",
        "Pok\u00E9mon names I", "Pok\u00E9mon names J", "Pok\u00E9mon names K", // 19-29
        "Answers: right or wrong",       // 30 thats right, correct, bingo
        "Places",                        // 31 Viridian Forest, Ochre Woods, ...
        "Calling Pikachu",               // 32 c'mere, come here, over here
        "Talk: mushrooms & boxes",       // 33 jumbo shroom, cardboard box
        "Fishing & lunch",               // 34 reel it in, pull harder, let's eat
        "Replies: OK, no, why?",         // 35 I understand, how come?
        "Goodbyes",                      // 36 take care, I'll miss you
        "Heading out",                   // 37 c'mon let's go, we're off
        "Answers & games",               // 38 thats right, tag you're it
        "Exploring: what is it?",        // 39 what's wrong?, let's check it out
        "Talk: acorns & balloons",       // 40 acorn, balloon, Poke disc
        "Bedtime",                       // 41 sleep tight, see you tomorrow
        "Rock, paper, scissors",         // 42
    };

    struct TableWord { std::string name, display; int category; std::vector<uint16_t> blob; };
    std::vector<TableWord> table;                       // the game's 640 words
    std::vector<std::vector<int>> sets;                 // word ids per set

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
    bool pending_reject = false;   // mic heard something that matched no word
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
            std::string name = read_cstr(rdram, name_ptr);
            auto blob = read_blob(rdram, voice_ptr);
            names_by_blob.emplace(blob, name);
            table.push_back({ name, display_name(name), (int)MEM_W(8, entry), blob });
        }
        for (int s = 0; s < SET_COUNT; s++) {
            gpr set = SET_TABLE + s * 8;
            int count = MEM_W(0, set);
            gpr ids = (gpr)(int32_t)MEM_W(4, set);
            std::vector<int> words;
            for (int k = 0; k < count && k < 256; k++) {
                int id = MEM_W(k * 4, ids);
                if (id >= 0 && id < WORD_TABLE_COUNT) words.push_back(id);
            }
            sets.push_back(std::move(words));
        }
    }

    // The set whose words (by voice pattern, in order) are the current dictionary.
    std::vector<std::vector<uint16_t>> dict_blobs;
    int find_active_set() {
        for (int s = 0; s < (int)sets.size(); s++) {
            if (sets[s].size() != dict_blobs.size()) continue;
            bool same = true;
            for (size_t k = 0; same && k < dict_blobs.size(); k++) same = table[sets[s][k]].blob == dict_blobs[k];
            if (same) return s;
        }
        return -1;
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

void hyp::vru::say_unrecognized() {
    std::lock_guard lock{ mutex };
    pending_slot = -1;
    pending_reject = true;
    pending_since = std::chrono::steady_clock::now();
}

bool hyp::vru::talk_button_held() {
    std::lock_guard lock{ mutex };
    if (pending_slot < 0 && !pending_reject) return false;
    if (!is_listening && std::chrono::steady_clock::now() - pending_since > talk_timeout) {
        pending_slot = -1; // the game didn't listen; drop it
        pending_reject = false;
        return false;
    }
    return true;
}

bool hyp::vru::listening() {
    std::lock_guard lock{ mutex };
    return is_listening;
}

std::vector<hyp::vru::WordInfo> hyp::vru::all_words() {
    std::lock_guard lock{ mutex };
    std::vector<WordInfo> out;
    std::map<std::string, size_t> index;
    for (int id = 0; id < (int)table.size(); id++) {
        std::string key = table[id].display;
        for (auto& c : key) c = (char)std::tolower((unsigned char)c);
        auto [it, added] = index.emplace(key, out.size());
        if (added) out.push_back({ table[id].display, table[id].category, 0, {} });
        WordInfo& w = out[it->second];
        w.variants++;
        for (int s = 0; s < (int)sets.size(); s++) {
            if (std::find(sets[s].begin(), sets[s].end(), id) != sets[s].end() &&
                std::find(w.sets.begin(), w.sets.end(), s) == w.sets.end()) {
                w.sets.push_back(s);
            }
        }
    }
    for (auto& w : out) std::sort(w.sets.begin(), w.sets.end());
    return out;
}

int hyp::vru::set_count() {
    return SET_COUNT;
}

std::string hyp::vru::set_name(int set) {
    return set >= 0 && set < SET_COUNT ? set_names[set] : "Unknown";
}

int hyp::vru::active_set() {
    std::lock_guard lock{ mutex };
    static unsigned cached_generation = ~0u;
    static int cached = -1;
    if (cached_generation != dict_generation) {
        cached_generation = dict_generation;
        cached = find_active_set();
    }
    return cached;
}

std::string hyp::vru::group_name(int category, const std::string& display) {
    std::string d = display;
    for (auto& c : d) c = (char)std::tolower((unsigned char)c);
    bool pikachu_call = d.rfind("pika", 0) == 0 || d.rfind("pi ", 0) == 0 || d == "pikka" || d == "hey you pikachu";
    if (pikachu_call) return "Calling Pikachu";
    switch (category) {
        case 0x2: return "Greetings & talk";
        case 0x3: return "Commands";
        case 0x4: case 0x5: case 0x9: return "Praise & feelings";
        case 0x6: return "Items";
        case 0xA: return "Pok\u00E9mon & things";
        case 0xB: return "Questions";
        case 0x12: return "Places";
        default: return "Other";
    }
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
    dict_blobs.clear();
    dict_generation++;
    pending_slot = -1;
    pending_reject = false;
    ctx->r2 = 0;
}

// s32 osVoiceSetWord(OSVoiceHandle* handle, u8* word)
extern "C" void osVoiceSetWord_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    build_name_map(rdram);
    auto blob = read_blob(rdram, ctx->r5);
    auto it = names_by_blob.find(blob);
    std::string name = it != names_by_blob.end() ? it->second : ("word" + std::to_string(dict.size()));
    int category = 0;
    for (const auto& t : table) if (t.blob == blob) { category = t.category; break; }
    dict.push_back({ (int)dict.size(), name, display_name(name), category });
    dict_blobs.push_back(blob);
    dict_generation++;
    ctx->r2 = 0;
}

// s32 osVoiceStartReadData(OSVoiceHandle* handle)
extern "C" void osVoiceStartReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::unique_lock lock{ mutex };
    MEM_W(HANDLE_MODE, ctx->r4) = 1;
    set_status(rdram, ctx->r4, 0);
    is_listening = true;
    listen_start = std::chrono::steady_clock::now();
    if (hyp_trace_enabled()) {
        std::printf("[vru] listening; dictionary %zu words:", dict.size());
        for (size_t i = 0; i < dict.size() && i < 12; i++) std::printf(" %s", dict[i].name.c_str());
        std::printf("%s\n", dict.size() > 12 ? " ..." : "");
    }
    bool picked = pending_slot >= 0;
    lock.unlock();
    if (!picked) hyp::vru::mic_listen_start(); // the player is talking: record the mic
    ctx->r2 = 0;
}

// s32 osVoiceStopReadData(OSVoiceHandle* handle)
extern "C" void osVoiceStopReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::unique_lock lock{ mutex };
    MEM_W(HANDLE_MODE, ctx->r4) = 0;
    set_status(rdram, ctx->r4, 0);
    is_listening = false;
    lock.unlock();
    hyp::vru::mic_listen_stop();
    ctx->r2 = 0;
}

// s32 osVoiceGetReadData(OSVoiceHandle* handle, OSVoiceData* data)
extern "C" void osVoiceGetReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr handle = ctx->r4;
    gpr data = ctx->r5;
    std::lock_guard lock{ mutex };

    auto now = std::chrono::steady_clock::now();
    bool ready = is_listening && ((pending_slot >= 0 && pending_slot < (int)dict.size()) || pending_reject) &&
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
    MEM_H(0x0A, data) = (int16_t)(pending_reject ? 0 : pending_slot);
    MEM_H(0x14, data) = pending_reject ? 3000 : 0x100; // distance[0] (game accepts <= 2000)
    for (int i = 1; i < 5; i++) {
        MEM_H(0x0A + i * 2, data) = 0x7FFF;
        MEM_H(0x14 + i * 2, data) = 0x7FFF;
    }
    if (hyp_trace_enabled()) {
        if (pending_reject) std::printf("[vru] heard nothing usable (rejected result)\n");
        else std::printf("[vru] heard slot %d (%s)\n", pending_slot, dict[pending_slot].name.c_str());
    }

    pending_slot = -1;
    pending_reject = false;
    is_listening = false;
    MEM_W(HANDLE_MODE, handle) = 0;
    set_status(rdram, handle, 0);
    ctx->r2 = 0;
}

// Unused by the game; defined so librecomp's stub object is never linked.
extern "C" void osVoiceCheckWord_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 0; }
extern "C" void osVoiceMaskDictionary_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 0; }
extern "C" void osVoiceControlGain_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 0; }
