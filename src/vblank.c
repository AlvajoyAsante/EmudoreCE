#include "vblank.h"


/**
 * @brief Reset the vblank pacer state with a new base timestamp.
 *
 * Initializes all internal state variables to their default values based on
 * the provided starting time. This function is typically called at system boot
 * or when resetting timing synchronization.
 *
 * @param pacer Pointer to the VBlankPacer structure to reset
 * @param now   Current timestamp in timer ticks to use as base reference
 */
void vblank_reset(VBlankPacer *pacer, uint32_t now)
{
    pacer->deadline = now;
    pacer->remainder = 0;
    pacer->skipped = 0;
    pacer->render = true;
}


/**
 * @brief Advance the vblank pacer state based on current time and frame conditions.
 *
 * Updates the internal timing state and determines whether a frame should be
 * rendered or skipped based on timing constraints. This function manages frame
 * rate limiting and handles cases where rendering is delayed due to timing issues.
 *
 * @param pacer      Pointer to the VBlankPacer structure to advance
 * @param now        Current timestamp in timer ticks
 * @param paced      Whether this is a paced (timed) frame update
 * @param automatic  Whether the frame is automatically generated (vs. manual)
 * @param requested  Whether rendering was specifically requested by user input
 *
 * @return Number of frames that were skipped since last call to vblank_advance
 */
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