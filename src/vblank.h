#ifndef EMUDORE_VBLANK_H
#define EMUDORE_VBLANK_H

#include <stdbool.h>
#include <stdint.h>

#define VBLANK_TIMER_HZ 32768u
#define VBLANK_FPS 50u
#define VBLANK_MAX_SKIPPED 2u

/**
 * @brief Structure representing a vblank pacer for timing control.
 *
 * This structure maintains state information for managing frame pacing and
 * rendering decisions based on timing constraints. It tracks the deadline for
 * the next frame, remaining time budget, number of skipped frames, and whether
 * rendering is required for the current frame.
 */
typedef struct
{
    uint32_t deadline;      /**< Deadline timestamp (in timer ticks) for next frame */
    uint8_t remainder;      /**< Fractional part of timing calculation */
    uint8_t skipped;        /**< Number of consecutive frames skipped due to timing issues */
    bool render;            /**< Flag indicating if current frame should be rendered */
} VBlankPacer;

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
void vblank_reset(VBlankPacer *pacer, uint32_t now);

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
                        bool paced, bool automatic, bool requested);

#endif