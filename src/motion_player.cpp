// Motion player: makes Pikachu play any action or motion on command.
//
// Requests are queued from any thread and run on the game thread (game_tick, called from
// the frame-loop hook) through the game's own calls, as its scene code does:
//   func_800120AC(obj); func_8000F534(obj, action, 0.1f /* blend seconds */);
// Pikachu's object is the global at 0x800760A8 (object type 37 at obj+0x34).
// A raw motion is played by pointing a carrier action's motion field (action table
// 0x8006B590 + id * 0x2C + 0x10) at it for the duration of the call.
// Each request plays to its end (length from the table at 0x800773E0) before the next
// queued one starts; repeat replays it after it ends.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

#include <cstring>

#include "recomp.h"
#include "hyp_motion.h"
#include "hyp_vru.h"

extern "C" void func_800120AC(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_8000F534(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr gpr PIKACHU_OBJ = (gpr)(int32_t)0x800760A8u;
    constexpr gpr ACTION_TABLE = (gpr)(int32_t)0x8006B590u;
    constexpr int ACTION_SIZE = 0x2C;
    constexpr int ACTION_COUNT = 330;
    constexpr int CARRIER_ACTION = 74;          // "Ready stance (idle)"
    constexpr uint32_t BLEND_SECONDS = 0x3DCCCCCD; // 0.1f, as the game passes it
    constexpr int PIKACHU_TYPE = 37;
    constexpr gpr MOTION_LENGTHS = (gpr)(int32_t)0x800773E0u; // f32 seconds per motion

    enum class Kind { None, Action, Motion };
    std::mutex queue_mutex;
    std::deque<std::pair<Kind, int>> queue;   // requests waiting for the current one to end
    std::atomic<bool> clear_requested = false;
    std::atomic<int> queued_count = 0;
    std::atomic<bool> repeat = false;
    std::atomic<bool> present = false;
    std::atomic<int> motion_now = -1;

    // What is being repeated. The request only takes effect on the next game update
    // (func_8000F534 sets ext+0x118; func_8000F14C -> func_8000E914 starts the motion).
    Kind playing_kind = Kind::None;
    int playing_id = 0, playing_motion = -1;
    bool confirmed = false;        // the motion has actually started playing
    double length_s = 0.0;         // the playing motion's length
    // Pikachu's motion fields (ext+0xEC..0x117, set by func_8000EAF8) for our motion, to
    // put back when the game tries to start another one before ours ends.
    constexpr int STATE_START = 0xEC, STATE_END = 0x118;
    uint32_t saved_state[(STATE_END - STATE_START) / 4];
    bool have_state = false;
    gpr playing_obj = 0;
    std::chrono::steady_clock::time_point started;

    double elapsed() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }

    void enqueue(Kind kind, int id) {
        std::lock_guard lock{ queue_mutex };
        queue.emplace_back(kind, id);
        queued_count = (int)queue.size();
    }

    gpr pikachu(uint8_t* rdram) {
        gpr obj = (gpr)(int32_t)MEM_W(0, PIKACHU_OBJ);
        if (obj == 0 || (uint32_t)obj < 0x80000000u || (uint32_t)obj >= 0x80800000u) return 0;
        if ((int16_t)MEM_H(0x34, obj) != PIKACHU_TYPE) return 0;
        gpr ext = (gpr)(int32_t)MEM_W(0x54, obj);
        return ext == 0 ? 0 : obj;
    }

    void start(uint8_t* rdram, recomp_context* ctx, gpr obj, Kind kind, int id) {
        int action = kind == Kind::Action ? id : CARRIER_ACTION;
        gpr motion_field = ACTION_TABLE + action * ACTION_SIZE + 0x10;
        int16_t saved_motion = (int16_t)MEM_H(0, motion_field);
        if (kind == Kind::Motion) MEM_H(0, motion_field) = (int16_t)id;

        recomp_context saved = *ctx;
        ctx->r4 = obj;
        func_800120AC(rdram, ctx);
        ctx->r4 = obj;
        ctx->r5 = action;
        ctx->r6 = (gpr)(int32_t)BLEND_SECONDS;
        func_8000F534(rdram, ctx);
        *ctx = saved;

        MEM_H(0, motion_field) = saved_motion;
        gpr ext = (gpr)(int32_t)MEM_W(0x54, obj);
        playing_kind = kind;
        playing_id = id;
        playing_motion = (int16_t)MEM_H(0xFC, ext);
        playing_obj = obj;
        have_state = false;
        confirmed = false;
        length_s = 0.0;
        if (playing_motion >= 0 && playing_motion < 231) {
            uint32_t bits = MEM_W(playing_motion * 4, MOTION_LENGTHS);
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            length_s = f;
        }
        started = std::chrono::steady_clock::now();
        hyp::vru::voice_log("[motion] Pikachu plays %s %d (motion %d, %.2f s)\n", kind == Kind::Action ? "action" : "motion",
                            id, playing_motion, length_s);
    }
}

void hyp::motion::play_action(int action) {
    if (action < 0 || action >= ACTION_COUNT) return;
    enqueue(Kind::Action, action);
}

void hyp::motion::play_motion(int motion) {
    if (motion < pikachu_first_motion || motion > pikachu_last_motion) return;
    enqueue(Kind::Motion, motion);
}

void hyp::motion::set_repeat(bool on) {
    repeat = on;
}

void hyp::motion::stop() {
    repeat = false;
    clear_requested = true; // drop the queue and let the game take over
}

int hyp::motion::queued() {
    return queued_count;
}

bool hyp::motion::pikachu_present() {
    return present;
}

int hyp::motion::current_motion() {
    return motion_now;
}

bool hyp::motion::block_dispatch(unsigned char* rdram_, int32_t obj_) {
    uint8_t* rdram = rdram_;
    gpr obj = obj_;
    if (playing_kind == Kind::None || !have_state || obj != playing_obj) return false;
    if (elapsed() >= length_s) return false;
    gpr ext = (gpr)(int32_t)MEM_W(0x54, obj);
    if ((int16_t)MEM_H(0xFC, ext) == playing_motion) return false;
    // The game asked for another motion: keep ours.
    for (int i = 0; i < (STATE_END - STATE_START) / 4; i++) MEM_W(STATE_START + i * 4, ext) = saved_state[i];
    MEM_H(0x118, ext) = 0;
    return true;
}

void hyp::motion::game_tick(unsigned char* rdram_, void* ctx_) {
    uint8_t* rdram = rdram_;
    auto* ctx = static_cast<recomp_context*>(ctx_);
    gpr obj = pikachu(rdram);
    present = obj != 0;
    if (obj == 0) {
        motion_now = -1;
        playing_kind = Kind::None;
        return;
    }
    gpr ext = (gpr)(int32_t)MEM_W(0x54, obj);
    int motion = (int16_t)MEM_H(0xFC, ext);
    motion_now = motion;

    if (clear_requested.exchange(false)) {
        std::lock_guard lock{ queue_mutex };
        queue.clear();
        queued_count = 0;
        playing_kind = Kind::None;
    }

    if (playing_kind != Kind::None) {
        bool requested = (int16_t)MEM_H(0x118, ext) != 0; // not started yet
        if (!confirmed && !requested && motion == playing_motion) {
            confirmed = true;
            for (int i = 0; i < (STATE_END - STATE_START) / 4; i++) saved_state[i] = MEM_W(STATE_START + i * 4, ext);
            have_state = true;
        }
        if (!confirmed && elapsed() > 1.0) {
            hyp::vru::voice_log("[motion] the game replaced it before it started (scripted scene?)\n");
            playing_kind = Kind::None;
        } else if (elapsed() < length_s + 0.05) {
            return; // let it finish
        }
    }

    // The current one is done (or nothing is playing): next in the queue, or repeat.
    std::pair<Kind, int> next{ Kind::None, 0 };
    {
        std::lock_guard lock{ queue_mutex };
        if (!queue.empty()) {
            next = queue.front();
            queue.pop_front();
            queued_count = (int)queue.size();
        }
    }
    if (next.first != Kind::None) {
        start(rdram, ctx, obj, next.first, next.second);
    } else if (repeat && playing_kind != Kind::None) {
        start(rdram, ctx, obj, playing_kind, playing_id);
    } else {
        playing_kind = Kind::None;
    }
}
