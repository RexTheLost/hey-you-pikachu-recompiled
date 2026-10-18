#ifndef HYP_CONTROL_H
#define HYP_CONTROL_H

#include <cstdint>

// Live test control: with HYP_CONTROL=<file>, the running game executes commands
// appended to that file (one per line):
//   press <a|b|z|start|l|r|up|down|left|right|cu|cd|cl|cr> [seconds=0.2]
//   stick <x> <y> <seconds>          analog stick, -1..1
//   say <word>                       VRU word (as in the picker)
//   speak <file.wav>                 press Z and use the clip as the microphone utterance
//   action <id> / motion <id>        make Pikachu play an action / motion
//   repeat on|off                    keep replaying it
//   save                             run the game's own save routine (game thread)
//   shot <file.png>                  screenshot of the game window
//   quit
namespace hyp::control {
    void poll();                                  // main thread, every update
    void apply_input(uint16_t* buttons, float* x, float* y);
    bool take_save_request();                     // game thread
}

#endif
