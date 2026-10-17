#ifndef HYP_VRU_H
#define HYP_VRU_H

#include <string>
#include <vector>

// Emulated Voice Recognition Unit (NUS-020). The game's osVoice* calls are
// implemented in src/vru/vru.cpp; recognition results come from a word picker
// (src/vru/vru_picker.cpp) now, and from a microphone recognizer later.
namespace hyp::vru {
    struct DictionaryWord {
        int slot;                 // index the game registered it at (osVoiceSetWord order)
        std::string name;         // game's word name, e.g. "come-here", "Pikachu2"
        std::string display;      // spoken form, e.g. "come here", "Pikachu"
    };

    // Current dictionary (copy) and a counter that changes whenever it does.
    std::vector<DictionaryWord> dictionary(unsigned* generation = nullptr);

    // Queue an utterance. The talk button (Z) is held automatically until the
    // game listens and receives it. Returns false if no such slot/word.
    bool say_slot(int slot);
    bool say_word(const std::string& display_or_name);

    // True while an utterance is pending: input.cpp holds Z so the game listens.
    bool talk_button_held();

    // True while the game is listening (between osVoiceStartReadData and a result/stop).
    bool listening();

    // Word picker window (Win32). toggle_picker() shows/hides it.
    void toggle_picker();
}

#endif
