#ifndef HYP_VRU_H
#define HYP_VRU_H

#include <filesystem>
#include <string>
#include <vector>

// Emulated Voice Recognition Unit (NUS-020). The game's osVoice* calls are
// implemented in src/vru/vru.cpp; recognition results come from a word picker
// (src/vru/vru_picker.cpp) or from the microphone (src/vru/vru_mic.cpp, whisper.cpp).
namespace hyp::vru {
    struct DictionaryWord {
        int slot;                 // index the game registered it at (osVoiceSetWord order)
        std::string name;         // game's word name, e.g. "come-here", "Pikachu2"
        std::string display;      // spoken form, e.g. "come here", "Pikachu"
        int category = 0;         // word table category (see group_name)
    };

    // Every word in the game's table, for browsing (merged by spoken form).
    struct WordInfo {
        std::string display;
        int category = 0;
        int variants = 0;         // pronunciations / table entries with this spoken form
        std::vector<int> sets;    // word sets that contain it
    };
    std::vector<WordInfo> all_words();     // empty until the game has loaded its word data
    int set_count();
    std::string set_name(int set);         // e.g. "Fishing & lunch"
    int active_set();                      // set matching the current dictionary, or -1
    // Display group for a word: "Calling Pikachu", "Commands", "Items", ...
    std::string group_name(int category, const std::string& display);

    // Current dictionary (copy) and a counter that changes whenever it does.
    std::vector<DictionaryWord> dictionary(unsigned* generation = nullptr);

    // Queue an utterance. The talk button (Z) is held automatically until the
    // game listens and receives it. Returns false if no such slot/word.
    bool say_slot(int slot);
    bool say_word(const std::string& display_or_name);
    // Deliver a "didn't understand" result (the game rejects it as too distant).
    void say_unrecognized();

    // True while an utterance is pending: input.cpp holds Z so the game listens.
    bool talk_button_held();

    // True while the game is listening (between osVoiceStartReadData and a result/stop).
    bool listening();

    // Word picker window (Win32). toggle_picker() shows/hides it.
    void toggle_picker();

    // Microphone recognizer (src/vru/vru_mic.cpp).
    enum class MicStatus { Loading, Ready, NoModel, NoMicrophone };
    void mic_init(const std::filesystem::path& model_path, const std::string& microphone);
    void mic_set_device(const std::string& microphone); // "" = system default
    MicStatus mic_status();
    void mic_listen_start();   // game started listening (not for a picked word)
    void mic_listen_stop();    // game stopped listening
    // Called with the player's own talk button (Z) every input poll; returns true
    // while the mic needs Z held for the game (finishing / recognizing an utterance).
    bool mic_talk_update(bool talk_pressed);
    std::string mic_last_heard(double max_age_seconds); // "" if nothing recent
    // Voice log: stdout plus data/voice.log (rewritten each launch), for troubleshooting.
    void voice_log(const char* fmt, ...);
    // Short message for the window title (shown a few seconds), e.g. a focus problem.
    void mic_notice(const std::string& text);
    // Test aid: the next utterance uses this audio instead of the microphone.
    void mic_inject(std::vector<float> pcm);
}

#endif
