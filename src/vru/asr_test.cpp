// asr_test <model.bin> <wav> "phrase" "phrase" ...
// Runs the VRU recognizer on a WAV file (any rate, 16-bit PCM, mono or stereo).
// With --text "<transcript>" instead of a wav, tests only the word matcher.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hyp_asr.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: asr_test <model.bin> <file.wav | --text transcript> phrase...\n");
        return 2;
    }
    int first_phrase = 3;
    std::vector<std::string> phrases;
    bool text_only = std::strcmp(argv[2], "--text") == 0;
    if (text_only) first_phrase = 4;
    for (int i = first_phrase; i < argc; i++) phrases.push_back(argv[i]);

    hyp::asr::Result r;
    if (text_only) {
        r = hyp::asr::match(argv[3], phrases);
    } else {
        std::vector<float> pcm;
        if (!hyp::asr::read_wav(argv[2], pcm)) { std::fprintf(stderr, "can't read %s\n", argv[2]); return 1; }
        if (!hyp::asr::load_model(argv[1])) { std::fprintf(stderr, "can't load %s\n", argv[1]); return 1; }
        r = hyp::asr::recognize(pcm, phrases);
    }
    std::printf("heard \"%s\" -> %s (score %.2f, %.2fs)\n", r.text.c_str(),
                r.index >= 0 ? phrases[r.index].c_str() : "(no match)", r.score, r.seconds);
    return 0;
}
