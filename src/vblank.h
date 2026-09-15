#ifndef EMUDORE_VBLANK_H
#define EMUDORE_VBLANK_H

#include <stdbool.h>
#include <stdint.h>

#define VBLANK_TIMER_HZ 32768u
#define VBLANK_FPS 50u
#define VBLANK_MAX_SKIPPED 2u

typedef struct
{
    uint32_t deadline;
    uint8_t remainder;
    uint8_t skipped;
    bool render;
} VBlankPacer;

void vblank_reset(VBlankPacer *pacer, uint32_t now);
uint16_t vblank_advance(VBlankPacer *pacer, uint32_t now,
                       bool paced, bool automatic, bool requested);

#endif