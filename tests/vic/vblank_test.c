#include <stdio.h>
#include "../../src/vblank.h"

static unsigned int failures;
static unsigned int checks;

#define EXPECT(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

int main(void)
{
    VBlankPacer pacer;
    uint32_t now = 100;
    uint32_t total_wait = 0;
    uint32_t start;
    unsigned int rendered = 0;
    unsigned int previous_render = 0;

    vblank_reset(&pacer, now);
    for (unsigned int frame = 0; frame < VBLANK_FPS; ++frame)
    {
        uint16_t wait = vblank_advance(&pacer, now, true, true, false);
        EXPECT(wait == 655 || wait == 656);
        EXPECT(pacer.render);
        total_wait += wait;
        now += wait;
        EXPECT(pacer.deadline == now);
    }
    EXPECT(total_wait == VBLANK_TIMER_HZ);

    now = UINT32_MAX - 300u;
    start = now;
    vblank_reset(&pacer, now);
    for (unsigned int frame = 0; frame < VBLANK_FPS; ++frame)
        now += vblank_advance(&pacer, now, true, false, false);
    EXPECT((uint32_t)(now - start) == VBLANK_TIMER_HZ);
    EXPECT(pacer.render);

    now = 0;
    vblank_reset(&pacer, now);
    for (unsigned int frame = 1; frame <= 12; ++frame)
    {
        now += 2000;
        EXPECT(vblank_advance(&pacer, now, true, true, false) == 0);
        EXPECT(pacer.skipped <= VBLANK_MAX_SKIPPED);
        if (pacer.render)
        {
            EXPECT(frame - previous_render == 3);
            previous_render = frame;
            ++rendered;
        }
    }
    EXPECT(rendered == 4);

    vblank_reset(&pacer, 0);
    EXPECT(vblank_advance(&pacer, 700, true, true, false) == 0);
    EXPECT(!pacer.render);
    now = 1000;
    now += vblank_advance(&pacer, now, true, true, false);
    EXPECT(!pacer.render);
    now += vblank_advance(&pacer, now, true, true, false);
    EXPECT(pacer.render);

    vblank_reset(&pacer, 0);
    vblank_advance(&pacer, 1000, true, true, false);
    EXPECT(!pacer.render);
    vblank_advance(&pacer, 2000, true, true, true);
    EXPECT(pacer.render && pacer.skipped == 0);
    vblank_advance(&pacer, 3000, true, false, false);
    EXPECT(pacer.render && pacer.skipped == 0);

    vblank_reset(&pacer, 10);
    EXPECT(vblank_advance(&pacer, 11, false, false, false) == 0);
    EXPECT(pacer.render && pacer.deadline == 11);
    EXPECT(vblank_advance(&pacer, 100000, true, true, false) == 0);
    EXPECT(pacer.deadline == 100000 && !pacer.render);
    EXPECT(vblank_advance(&pacer, 100001, true, true, false) <= 656);

    vblank_reset(&pacer, 100000);
    EXPECT(vblank_advance(&pacer, 0, true, false, false) <= 656);
    EXPECT(pacer.deadline <= 656);

    printf("VBlank pacing: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}