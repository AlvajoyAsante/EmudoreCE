#include "vblank.h"

void vblank_reset(VBlankPacer *pacer, uint32_t now)
{
    pacer->deadline = now;
    pacer->remainder = 0;
    pacer->skipped = 0;
    pacer->render = true;
}

uint16_t vblank_advance(VBlankPacer *pacer, uint32_t now,
                       bool paced, bool automatic, bool requested)
{
    uint16_t period = VBLANK_TIMER_HZ / VBLANK_FPS;
    int32_t lead;
    bool behind;

    pacer->remainder += VBLANK_TIMER_HZ % VBLANK_FPS;
    if (pacer->remainder >= VBLANK_FPS)
    {
        pacer->remainder -= VBLANK_FPS;
        ++period;
    }
    pacer->deadline += period;
    lead = (int32_t)(pacer->deadline - now);
    behind = lead < 0;
    if (!paced || lead < -(int32_t)(period * (VBLANK_MAX_SKIPPED + 1u)))
    {
        pacer->deadline = now;
        pacer->remainder = 0;
        if (lead < 0)
            lead = 0;
    }
    else if (lead > period)
    {
        pacer->deadline = now + period;
        pacer->remainder = 0;
        lead = period;
    }

    pacer->render = !automatic || requested ||
        pacer->skipped >= VBLANK_MAX_SKIPPED ||
        (!behind && pacer->skipped != 1u);
    if (pacer->render)
        pacer->skipped = 0;
    else
        ++pacer->skipped;

    return paced && lead > 0 ? (uint16_t)lead : 0;
}