#ifndef HYP_MOTION_H
#define HYP_MOTION_H

#include <cstdint>

// Motion player: make Pikachu play any action or motion on command (src/motion_player.cpp),
// with a window to pick them (src/motion_player_window.cpp, F2 or Tools > Motion player).
namespace hyp::motion {
    constexpr int pikachu_first_motion = 28, pikachu_last_motion = 201;

    // Requests queue up: each plays to its end before the next starts.
    void play_action(int action);   // an entry of the action table (0..329)
    void play_motion(int motion);   // any Pikachu motion, even ones no action uses
    void set_repeat(bool repeat);   // replay the last one each time it ends
    void stop();                    // clear the queue and repeat; the game takes over again
    int queued();                   // requests waiting
    bool pikachu_present();         // Pikachu exists in the current scene
    int current_motion();           // Pikachu's motion right now, or -1

    void toggle_window();

    // Game thread, once per game frame (called from the frame-loop hook).
    void game_tick(unsigned char* rdram, void* ctx);
    // Game thread, from the motion dispatcher hook: true = skip this dispatch (our motion
    // is still playing; its state is restored).
    bool block_dispatch(unsigned char* rdram, int32_t obj);
}

#endif
