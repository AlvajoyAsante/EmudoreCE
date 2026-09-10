#include "perf.h"

#if EMUDORE_PROFILE

#include <limits.h>
#include <string.h>
#include <sys/timers.h>

PerfStats perf_stats;

static bool sampling_active;
static bool scope_sampled[PERF_SCOPE_COUNT];
static unsigned long scope_render_ticks[PERF_SCOPE_COUNT];
static bool render_sampled;
static unsigned long frame_tick_ring[PERF_FRAME_WINDOW_SIZE];
static uint8_t frame_render_ring[PERF_FRAME_WINDOW_SIZE];
static uint8_t frame_window_next;
static uint8_t frame_window_count;
static unsigned long previous_frame_tick;
static bool frame_clock_ready;

void perf_reset(void)
{
    memset(&perf_stats, 0, sizeof(perf_stats));
    memset(scope_sampled, 0, sizeof(scope_sampled));
    sampling_active = false;
    render_sampled = false;
    frame_window_next = 0;
    frame_window_count = 0;
    previous_frame_tick = 0UL;
    frame_clock_ready = false;
}

void perf_set_sample(bool active)
{
    sampling_active = active;
}

bool perf_sampling(void)
{
    return sampling_active;
}

unsigned long perf_begin(PerfScope scope)
{
    ++perf_stats.total_calls[scope];
    scope_sampled[scope] = sampling_active;
    if (!scope_sampled[scope])
        return 0UL;

    ++perf_stats.sampled_calls[scope];
    scope_render_ticks[scope] = perf_stats.render_ticks;
    return (unsigned long)timer_Get(1);
}

void perf_end(PerfScope scope, unsigned long start)
{
    unsigned long elapsed_ticks;
    unsigned long nested_render_ticks;

    if (!scope_sampled[scope])
        return;

    elapsed_ticks = (uint32_t)((uint32_t)timer_Get(1) - (uint32_t)start);
    scope_sampled[scope] = false;
    nested_render_ticks = perf_stats.render_ticks - scope_render_ticks[scope];
    perf_stats.ticks[scope] += elapsed_ticks - nested_render_ticks;
}

unsigned long perf_render_begin(void)
{
    ++perf_stats.render_calls;
    render_sampled = sampling_active;
    if (!render_sampled)
        return 0UL;

    ++perf_stats.sampled_render_calls;
    return (unsigned long)timer_Get(1);
}

void perf_render_end(unsigned long start)
{
    unsigned long elapsed_ticks;

    if (!render_sampled)
        return;

    elapsed_ticks = (uint32_t)((uint32_t)timer_Get(1) - (uint32_t)start);
    render_sampled = false;
    perf_stats.render_ticks += elapsed_ticks;
}

void perf_frame_complete(bool rendered, bool hidden)
{
    unsigned long current_tick;
    unsigned long elapsed_ticks;

    ++perf_stats.frames;
    if (hidden)
    {
        ++perf_stats.hidden_frames;
        frame_window_next = 0;
        frame_window_count = 0;
        frame_clock_ready = false;
        return;
    }
    if (rendered)
        ++perf_stats.rendered_frames;

    current_tick = (unsigned long)timer_Get(1);
    if (!frame_clock_ready)
    {
        previous_frame_tick = current_tick;
        frame_clock_ready = true;
        return;
    }

    elapsed_ticks = (uint32_t)((uint32_t)current_tick - (uint32_t)previous_frame_tick);
    previous_frame_tick = current_tick;
    if (elapsed_ticks == 0UL)
        return;

    frame_tick_ring[frame_window_next] = elapsed_ticks;
    frame_render_ring[frame_window_next] = rendered;
    ++frame_window_next;
    frame_window_next &= (uint8_t)(PERF_FRAME_WINDOW_SIZE - 1);
    if (frame_window_count < PERF_FRAME_WINDOW_SIZE)
        ++frame_window_count;
}

void perf_sample_pc(uint16_t pc)
{
    uint8_t page;

    if (!sampling_active)
        return;

    page = (uint8_t)(pc >> 8);
    if (perf_stats.pc_pages[page] != UINT16_MAX)
        ++perf_stats.pc_pages[page];
}

void perf_get_summary(PerfSummary *summary)
{
    unsigned long ordered_ticks[PERF_FRAME_WINDOW_SIZE];
    unsigned long sample_ticks;
    unsigned long average_remainder = 0UL;
    uint8_t sample_count = frame_window_count;
    uint8_t sample_index;
    uint8_t insertion_index;
    unsigned int percentile_index;

    if (!summary)
        return;

    *summary = (PerfSummary){0};
    if (sample_count == 0)
        return;

    summary->frames = sample_count;
    for (sample_index = 0; sample_index < sample_count; ++sample_index)
    {
        sample_ticks = frame_tick_ring[sample_index];
        if (sample_ticks > ULONG_MAX - summary->frame_ticks)
            summary->frame_ticks = ULONG_MAX;
        else
            summary->frame_ticks += sample_ticks;
        summary->rendered_frames += frame_render_ring[sample_index];
        summary->average_ticks += sample_ticks / sample_count;
        average_remainder += sample_ticks % sample_count;

        insertion_index = sample_index;
        while (insertion_index != 0 &&
               ordered_ticks[insertion_index - 1] > sample_ticks)
        {
            ordered_ticks[insertion_index] = ordered_ticks[insertion_index - 1];
            --insertion_index;
        }
        ordered_ticks[insertion_index] = sample_ticks;
    }

    summary->average_ticks += average_remainder / sample_count;
    summary->max_ticks = ordered_ticks[sample_count - 1];
    percentile_index = (sample_count * 95U + 99U) / 100U - 1U;
    summary->p95_ticks = ordered_ticks[percentile_index];
}

#endif