#ifndef EMUDORE_PROFILE
#define EMUDORE_PROFILE 1
#endif

#include "perf.h"
#include "../vic/include/sys/timers.h"

#include <stdio.h>
#include <stdlib.h>

#if !EMUDORE_PROFILE
#error "test_perf requires EMUDORE_PROFILE=1"
#endif

static uint32_t mock_ticks;
static unsigned int timer_reads;
static unsigned int invalid_timer_reads;
static unsigned int tests_run;
static unsigned int checks_run;
static unsigned int failures;
static const char *test_name;

static void check_equal(unsigned long actual, unsigned long expected,
                        const char *expression, int line)
{
    ++checks_run;
    if (actual != expected)
    {
        ++failures;
        fprintf(stderr, "%s:%d: %s: %s = %lu, expected %lu\n",
                __FILE__, line, test_name, expression, actual, expected);
    }
}

#define CHECK_EQ(actual, expected) \
    check_equal((unsigned long)(actual), (unsigned long)(expected), \
                #actual, __LINE__)

uint32_t timer_Get(unsigned int timer)
{
    ++timer_reads;
    if (timer != 1U)
        ++invalid_timer_reads;
    return mock_ticks;
}

static void check_summary(unsigned int frames, unsigned int rendered_frames,
                          unsigned long frame_ticks, unsigned long average_ticks,
                          unsigned long p95_ticks, unsigned long max_ticks)
{
    PerfSummary summary = {1UL, 1U, 1U, 1UL, 1UL, 1UL};
    unsigned int previous_reads = timer_reads;

    perf_get_summary(&summary);
    CHECK_EQ(summary.frames, frames);
    CHECK_EQ(summary.rendered_frames, rendered_frames);
    CHECK_EQ(summary.frame_ticks, frame_ticks);
    CHECK_EQ(summary.average_ticks, average_ticks);
    CHECK_EQ(summary.p95_ticks, p95_ticks);
    CHECK_EQ(summary.max_ticks, max_ticks);
    CHECK_EQ(timer_reads, previous_reads);
}

static void test_unsampled_counts(void)
{
    unsigned int scope_index;
    unsigned int render_index;
    unsigned long start;

    CHECK_EQ(perf_sampling(), false);
    for (scope_index = 0; scope_index < PERF_SCOPE_COUNT; ++scope_index)
    {
        PerfScope scope = (PerfScope)scope_index;
        start = perf_begin(scope);
        CHECK_EQ(start, 0UL);
        mock_ticks += 10U;
        perf_end(scope, start);
        CHECK_EQ(perf_stats.total_calls[scope], 1UL);
        CHECK_EQ(perf_stats.sampled_calls[scope], 0UL);
        CHECK_EQ(perf_stats.ticks[scope], 0UL);
    }
    for (render_index = 0; render_index < 3U; ++render_index)
    {
        start = perf_render_begin();
        CHECK_EQ(start, 0UL);
        mock_ticks += 10U;
        perf_render_end(start);
    }
    CHECK_EQ(perf_stats.render_calls, 3UL);
    CHECK_EQ(perf_stats.sampled_render_calls, 0UL);
    CHECK_EQ(perf_stats.render_ticks, 0UL);
    CHECK_EQ(timer_reads, 0U);
}

static void test_exclusive_scope_render(void)
{
    unsigned int scope_index;
    unsigned long scope_start;
    unsigned long render_start;

    perf_set_sample(true);
    for (scope_index = 0; scope_index < PERF_SCOPE_COUNT; ++scope_index)
    {
        PerfScope scope = (PerfScope)scope_index;
        scope_start = perf_begin(scope);
        mock_ticks += 20U;
        render_start = perf_render_begin();
        mock_ticks += 30U;
        perf_render_end(render_start);
        mock_ticks += 50U;
        perf_end(scope, scope_start);
        CHECK_EQ(perf_stats.total_calls[scope], 1UL);
        CHECK_EQ(perf_stats.sampled_calls[scope], 1UL);
        CHECK_EQ(perf_stats.ticks[scope], 70UL);
        CHECK_EQ(perf_stats.render_ticks, 30UL * (scope_index + 1U));
    }
    CHECK_EQ(perf_stats.render_calls, PERF_SCOPE_COUNT);
    CHECK_EQ(perf_stats.sampled_render_calls, PERF_SCOPE_COUNT);
    CHECK_EQ(timer_reads, 4U * PERF_SCOPE_COUNT);
}

static void test_sampling_phases(void)
{
    unsigned int scope_index;
    unsigned int phase;
    unsigned int sampled_renders = 0U;
    unsigned long scope_start;
    unsigned long render_start;

    for (scope_index = 0; scope_index < PERF_SCOPE_COUNT; ++scope_index)
    {
        PerfScope scope = (PerfScope)scope_index;
        for (phase = 1U; phase <= 3U; ++phase)
        {
            perf_set_sample(true);
            CHECK_EQ(perf_sampling(), true);
            scope_start = perf_begin(scope);
            mock_ticks += 4U;
            render_start = perf_render_begin();
            perf_set_sample(false);
            CHECK_EQ(perf_sampling(), false);
            mock_ticks += 6U;
            perf_render_end(render_start);
            mock_ticks += 10U;
            perf_end(scope, scope_start);
            ++sampled_renders;
            CHECK_EQ(perf_stats.ticks[scope], 14UL * phase);
            CHECK_EQ(perf_stats.render_ticks, 6UL * sampled_renders);

            mock_ticks += 100U;
            perf_render_end(render_start);
            perf_end(scope, scope_start);
            CHECK_EQ(timer_reads, 4U * sampled_renders);

            scope_start = perf_begin(scope);
            render_start = perf_render_begin();
            CHECK_EQ(scope_start, 0UL);
            CHECK_EQ(render_start, 0UL);
            perf_set_sample(true);
            mock_ticks += 50U;
            perf_render_end(render_start);
            perf_end(scope, scope_start);
            CHECK_EQ(perf_stats.total_calls[scope], 2UL * phase);
            CHECK_EQ(perf_stats.sampled_calls[scope], phase);
            CHECK_EQ(perf_stats.ticks[scope], 14UL * phase);
            CHECK_EQ(perf_stats.render_calls, 2UL * sampled_renders);
            CHECK_EQ(perf_stats.sampled_render_calls, sampled_renders);
            CHECK_EQ(perf_stats.render_ticks, 6UL * sampled_renders);
            CHECK_EQ(timer_reads, 4U * sampled_renders);
        }
    }
}

static void test_zero_timestamps(void)
{
    unsigned long scope_start;
    unsigned long render_start;

    perf_set_sample(true);
    scope_start = perf_begin(PERF_CPU);
    render_start = perf_render_begin();
    CHECK_EQ(scope_start, 0UL);
    CHECK_EQ(render_start, 0UL);
    mock_ticks = 30U;
    perf_render_end(render_start);
    mock_ticks = 100U;
    perf_end(PERF_CPU, scope_start);
    CHECK_EQ(perf_stats.ticks[PERF_CPU], 70UL);
    CHECK_EQ(perf_stats.render_ticks, 30UL);
    CHECK_EQ(timer_reads, 4U);
}

static void test_timer_wrap(void)
{
    unsigned long scope_start;
    unsigned long render_start;

    perf_set_sample(true);
    mock_ticks = UINT32_MAX - 49U;
    scope_start = perf_begin(PERF_CPU);
    perf_frame_complete(false, false);
    mock_ticks = UINT32_MAX - 29U;
    render_start = perf_render_begin();
    mock_ticks = 0U;
    perf_render_end(render_start);
    mock_ticks = 50U;
    perf_end(PERF_CPU, scope_start);
    perf_frame_complete(true, false);
    CHECK_EQ(perf_stats.ticks[PERF_CPU], 70UL);
    CHECK_EQ(perf_stats.render_ticks, 30UL);
    check_summary(1U, 1U, 100UL, 100UL, 100UL, 100UL);
    CHECK_EQ(timer_reads, 6U);
}

static void test_empty_summary(void)
{
    check_summary(0U, 0U, 0UL, 0UL, 0UL, 0UL);
    perf_get_summary(NULL);
    CHECK_EQ(perf_stats.frames, 0UL);
    CHECK_EQ(timer_reads, 0U);
}

static void test_visible_frames(void)
{
    CHECK_EQ(perf_sampling(), false);
    perf_frame_complete(true, false);
    check_summary(0U, 0U, 0UL, 0UL, 0UL, 0UL);
    mock_ticks = 10U;
    perf_frame_complete(false, false);
    check_summary(1U, 0U, 10UL, 10UL, 10UL, 10UL);
    mock_ticks = 40U;
    perf_frame_complete(true, false);
    mock_ticks = 60U;
    perf_frame_complete(false, false);
    check_summary(3U, 1U, 60UL, 20UL, 30UL, 30UL);
    CHECK_EQ(perf_stats.frames, 4UL);
    CHECK_EQ(perf_stats.rendered_frames, 2UL);
    CHECK_EQ(perf_stats.hidden_frames, 0UL);
    CHECK_EQ(timer_reads, 4U);
}

static void test_hidden_frames_reset_window(void)
{
    mock_ticks = 100U;
    perf_frame_complete(true, false);
    mock_ticks = 110U;
    perf_frame_complete(true, false);
    check_summary(1U, 1U, 10UL, 10UL, 10UL, 10UL);
    mock_ticks = 5000U;
    perf_frame_complete(true, true);
    check_summary(0U, 0U, 0UL, 0UL, 0UL, 0UL);
    mock_ticks = 6000U;
    perf_frame_complete(false, true);
    CHECK_EQ(perf_stats.frames, 4UL);
    CHECK_EQ(perf_stats.rendered_frames, 2UL);
    CHECK_EQ(perf_stats.hidden_frames, 2UL);
    CHECK_EQ(timer_reads, 2U);
    mock_ticks = 10000U;
    perf_frame_complete(true, false);
    check_summary(0U, 0U, 0UL, 0UL, 0UL, 0UL);
    mock_ticks = 10007U;
    perf_frame_complete(false, false);
    mock_ticks = 10018U;
    perf_frame_complete(true, false);
    check_summary(2U, 1U, 18UL, 9UL, 11UL, 11UL);
    CHECK_EQ(perf_stats.frames, 7UL);
    CHECK_EQ(perf_stats.rendered_frames, 4UL);
    CHECK_EQ(perf_stats.hidden_frames, 2UL);
    CHECK_EQ(timer_reads, 5U);
}

static void test_frame_ring_and_p95(void)
{
    unsigned int duration;

    CHECK_EQ(PERF_FRAME_WINDOW_SIZE, 32U);
    perf_frame_complete(true, false);
    for (duration = 40U; duration != 0U; --duration)
    {
        mock_ticks += duration;
        perf_frame_complete(duration % 3U == 0U, false);
        if (duration == 21U)
            check_summary(20U, 7U, 610UL, 30UL, 39UL, 40UL);
    }
    check_summary(32U, 10U, 528UL, 16UL, 31UL, 32UL);
    check_summary(32U, 10U, 528UL, 16UL, 31UL, 32UL);
    CHECK_EQ(perf_stats.frames, 41UL);
    CHECK_EQ(perf_stats.rendered_frames, 14UL);
    CHECK_EQ(timer_reads, 41U);
}

static void test_zero_duration_discard(void)
{
    perf_frame_complete(true, false);
    perf_frame_complete(false, false);
    check_summary(0U, 0U, 0UL, 0UL, 0UL, 0UL);
    mock_ticks = 10U;
    perf_frame_complete(false, false);
    perf_frame_complete(true, false);
    check_summary(1U, 0U, 10UL, 10UL, 10UL, 10UL);
    mock_ticks = 30U;
    perf_frame_complete(true, false);
    check_summary(2U, 1U, 30UL, 15UL, 20UL, 20UL);
    CHECK_EQ(perf_stats.frames, 5UL);
    CHECK_EQ(perf_stats.rendered_frames, 3UL);
    CHECK_EQ(timer_reads, 5U);
}

static void test_pc_pages_and_saturation(void)
{
    perf_sample_pc(0x1200U);
    CHECK_EQ(perf_stats.pc_pages[0x12], 0U);
    perf_set_sample(true);
    perf_sample_pc(0x1200U);
    perf_sample_pc(0x12FFU);
    perf_sample_pc(0x00FFU);
    perf_sample_pc(0x0100U);
    CHECK_EQ(perf_stats.pc_pages[0x12], 2U);
    CHECK_EQ(perf_stats.pc_pages[0x11], 0U);
    CHECK_EQ(perf_stats.pc_pages[0x13], 0U);
    CHECK_EQ(perf_stats.pc_pages[0], 1U);
    CHECK_EQ(perf_stats.pc_pages[1], 1U);
    perf_stats.pc_pages[0xFF] = UINT16_MAX - 1U;
    perf_sample_pc(0xFF00U);
    CHECK_EQ(perf_stats.pc_pages[0xFF], UINT16_MAX);
    perf_sample_pc(0xFFFFU);
    CHECK_EQ(perf_stats.pc_pages[0xFF], UINT16_MAX);
    perf_set_sample(false);
    perf_sample_pc(0x1200U);
    CHECK_EQ(perf_stats.pc_pages[0x12], 2U);
    CHECK_EQ(timer_reads, 0U);
}

static void test_char_sprite_counters(void)
{
    perf_note_char_kernel();
    perf_note_char_kernel();
    perf_note_sprite_line();
    perf_set_sample(true);
    perf_note_char_kernel();
    perf_note_sprite_line();
    perf_note_sprite_line();
    perf_set_sample(false);
    perf_note_sprite_line();
    CHECK_EQ(perf_stats.char_kernel_calls, 3UL);
    CHECK_EQ(perf_stats.sprite_lines, 4UL);
    CHECK_EQ(timer_reads, 0U);
    perf_reset();
    CHECK_EQ(perf_stats.char_kernel_calls, 0UL);
    CHECK_EQ(perf_stats.sprite_lines, 0UL);
    CHECK_EQ(timer_reads, 0U);
}

static void run_test(const char *name, void (*test)(void))
{
    unsigned int previous_failures = failures;

    test_name = name;
    mock_ticks = 0U;
    timer_reads = 0U;
    invalid_timer_reads = 0U;
    perf_reset();
    ++tests_run;
    test();
    CHECK_EQ(invalid_timer_reads, 0U);
    printf("%s %s\n", failures == previous_failures ? "PASS" : "FAIL", name);
}

int main(void)
{
    run_test("unsampled totals without timer reads", test_unsampled_counts);
    run_test("CPU/VIC/IO/CIA 100 ticks minus 30 render ticks", test_exclusive_scope_render);
    run_test("repeated sampling phases and duplicate ends", test_sampling_phases);
    run_test("zero timestamps are sampled", test_zero_timestamps);
    run_test("32-bit timer wrap for scopes/render/frames", test_timer_wrap);
    run_test("empty and null summaries", test_empty_summary);
    run_test("every visible frame and initial boundary", test_visible_frames);
    run_test("hidden reset and first visible boundary", test_hidden_frames_reset_window);
    run_test("32-frame overwrite and nearest-rank p95", test_frame_ring_and_p95);
    run_test("zero-duration frames discarded", test_zero_duration_discard);
    run_test("sampled PC pages saturate at UINT16_MAX", test_pc_pages_and_saturation);
    run_test("character and sprite counters", test_char_sprite_counters);
    printf("perf: %u tests, %u checks, %u failures\n", tests_run, checks_run, failures);
    return failures == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}