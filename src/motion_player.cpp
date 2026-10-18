// Motion player: makes Pikachu play any action or motion on command.
//
// Requests are queued from any thread and run on the game thread (game_tick, called from
// the frame-loop hook) through the same call the game's behavior scripts use:
//   func_8000F404(obj, action, flag, &ticket)
// It records the request (ext+0x120), waits for the current motion if one is playing,
// and runs the action scheduler (func_8000ECB4), which inserts the right transition or
// blend and then plays the action (mode 1). The AI only issues a new action when no
// request is pending (ext+0x120 == -1), so it doesn't overwrite ours.
// (func_8000F534, used first, only blends into an action's first pose: it's for stances.)
//
// Pikachu's object is the global at 0x800760A8 (object type 37 at obj+0x34).
// A raw motion borrows a carrier action's motion field (action table 0x8006B590 +
// id * 0x2C + 0x10) until it has played.
// Each request plays to its end (length from the table at 0x800773E0, counted from when
// the motion actually starts) before the next queued one; repeat replays it.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include "recomp.h"
#include "hyp_motion.h"
#include "hyp_vru.h"

extern "C" void func_800120AC(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_8000F404(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr gpr PIKACHU_OBJ = (gpr)(int32_t)0x800760A8u;
    constexpr gpr ACTION_TABLE = (gpr)(int32_t)0x8006B590u;
    constexpr int ACTION_SIZE = 0x2C;
    constexpr int ACTION_COUNT = 330;
    constexpr int CARRIER_ACTION = 74;                         // "Ready stance (idle)"
    constexpr gpr TICKET = (gpr)(int32_t)0x807FFF00u;          // scratch word in unused RDRAM
    constexpr int PIKACHU_TYPE = 37;
    constexpr gpr MOTION_LENGTHS = (gpr)(int32_t)0x800773E0u;  // f32 seconds per motion
    constexpr double START_TIMEOUT_S = 4.0;                    // transitions can come first

    enum class Kind { None, Action, Motion };
    std::mutex queue_mutex;
    std::deque<std::pair<Kind, int>> queue;   // requests waiting for the current one to end
    std::atomic<bool> clear_requested = false;
    std::atomic<int> queued_count = 0;
    std::atomic<bool> repeat = false;
    std::atomic<bool> present = false;
    std::atomic<int> motion_now = -1;

    Kind playing_kind = Kind::None;
    int playing_id = 0, playing_motion = -1;
    bool confirmed = false;        // the motion has actually started playing
    double length_s = 0.0;         // the playing motion's length
    std::chrono::steady_clock::time_point started;

    // Raw motions: the borrowed carrier action's motion field.
    gpr borrowed_field = 0;
    int16_t borrowed_saved = 0;

    double elapsed() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }

    void enqueue(Kind kind, int id) {
        std::lock_guard lock{ queue_mutex };
        queue.emplace_back(kind, id);
        queued_count = (int)queue.size();
    }

    void release_carrier(uint8_t* rdram) {
        if (borrowed_field == 0) return;
        MEM_H(0, borrowed_field) = borrowed_saved;
        borrowed_field = 0;
    }

    void finish(uint8_t* rdram) {
        playing_kind = Kind::None;
        release_carrier(rdram);
    }

    gpr pikachu(uint8_t* rdram) {
        gpr obj = (gpr)(int32_t)MEM_W(0, PIKACHU_OBJ);
        if (obj == 0 || (uint32_t)obj < 0x80000000u || (uint32_t)obj >= 0x80800000u) return 0;
        if ((int16_t)MEM_H(0x34, obj) != PIKACHU_TYPE) return 0;
        gpr ext = (gpr)(int32_t)MEM_W(0x54, obj);
        return ext == 0 ? 0 : obj;
    }

    double motion_length(uint8_t* rdram, int motion) {
        if (motion < 0 || motion >= 231) return 0.0;
        uint32_t bits = MEM_W(motion * 4, MOTION_LENGTHS);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    void start(uint8_t* rdram, recomp_context* ctx, gpr obj, Kind kind, int id) {
        release_carrier(rdram);
        int action = kind == Kind::Action ? id : CARRIER_ACTION;
        gpr motion_field = ACTION_TABLE + action * ACTION_SIZE + 0x10;
        if (kind == Kind::Motion) {
            borrowed_field = motion_field;
            borrowed_saved = (int16_t)MEM_H(0, motion_field);
            MEM_H(0, motion_field) = (int16_t)id;
        }

        recomp_context saved = *ctx;
        ctx->r4 = obj;
        func_800120AC(rdram, ctx);  // stop the current behavior script
        ctx->r4 = obj;
        ctx->r5 = action;
        ctx->r6 = 0;
        ctx->r7 = TICKET;
        func_8000F404(rdram, ctx);
        *ctx = saved;

        playing_kind = kind;
        playing_id = id;
        playing_motion = (int16_t)MEM_H(0, motion_field);
        confirmed = false;
        length_s = motion_length(rdram, playing_motion);
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

bool hyp::motion::block_dispatch(unsigned char*, int32_t) {
    // Not needed with func_8000F404: the game itself keeps a pending request.
    return false;
}

void hyp::motion::game_tick(unsigned char* rdram_, void* ctx_) {
    uint8_t* rdram = rdram_;
    auto* ctx = static_cast<recomp_context*>(ctx_);
    gpr obj = pikachu(rdram);
    present = obj != 0;
    if (obj == 0) {
        motion_now = -1;
        finish(rdram);
        return;
    }
    gpr ext = (gpr)(int32_t)MEM_W(0x54, obj);
    int motion = (int16_t)MEM_H(0xFC, ext);
    motion_now = motion;

    if (clear_requested.exchange(false)) {
        std::lock_guard lock{ queue_mutex };
        queue.clear();
        queued_count = 0;
        finish(rdram);
    }

    if (playing_kind != Kind::None) {
        if (!confirmed) {
            if (motion == playing_motion && (int16_t)MEM_H(0x118, ext) == 0) {
                confirmed = true;                 // it's playing: time it from now
                started = std::chrono::steady_clock::now();
                hyp::vru::voice_log("[motion] motion %d started\n", playing_motion);
                return;
            }
            if (elapsed() < START_TIMEOUT_S) return;
            hyp::vru::voice_log("[motion] motion %d never started (scripted scene?)\n", playing_motion);
            finish(rdram);
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
        finish(rdram);
    }
}
