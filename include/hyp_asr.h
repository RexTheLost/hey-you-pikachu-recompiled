#ifndef HYP_ASR_H
#define HYP_ASR_H

#include <filesystem>
#include <string>
#include <vector>

// Closed-vocabulary speech recognition for the emulated VRU: whisper.cpp
// transcribes the utterance (biased toward the active words with a prompt),
// then the text is matched to the closest active word.
namespace hyp::asr {
    constexpr int sample_rate = 16000; // input: mono float PCM at 16 kHz

    bool load_model(const std::filesystem::path& path);
    bool model_loaded();

    struct Result {
        int index = -1;        // into the phrase list; -1 = nothing close enough
        float score = 0.0f;    // 0..1 similarity of the best phrase
        std::string text;      // raw transcript
        double seconds = 0.0;  // recognition time
    };

    // phrases: the words the game is listening for, as spoken ("come here").
    Result recognize(const std::vector<float>& pcm, const std::vector<std::string>& phrases);

    // 16-bit PCM WAV (any rate, mono or stereo) -> mono float at 16 kHz.
    bool read_wav(const std::filesystem::path& path, std::vector<float>& out);

    // Text-only part, exposed for tests: best phrase for a transcript.
    Result match(const std::string& transcript, const std::vector<std::string>& phrases);
}

#endif
