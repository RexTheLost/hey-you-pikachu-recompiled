#ifndef HYP_RAMWATCH_H
#define HYP_RAMWATCH_H

#include <cstdint>

// Timer finder (src/ram_watch.cpp): logs the RDRAM words that moved steadily in one direction
// while a stage ran, when the stage changes.
namespace hyp::ramwatch {
    void tick(const uint8_t* rdram);                      // every game frame (game thread)
    void stage_changed(const uint8_t* rdram, int new_stage); // from the stage-load hook
}

#endif
