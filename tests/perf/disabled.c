#include "perf.h"

#include <stdio.h>
#include <stdlib.h>

#if EMUDORE_PROFILE
#error "disabled requires EMUDORE_PROFILE=0"
#endif

extern unsigned long forbidden_argument(void);

static unsigned int checks_run;
static unsigned int failures;

static void check_equal(unsigned long actual, unsigned long expected,
                        const char *expression, int line)
{
    ++checks_run;
    if (actual != expected)
    {
        ++failures;
        fprintf(stderr, "%s:%d: %s = %lu, expected %lu\n",
                __FILE__, line, expression, actual, expected);
    }
}

#define CHECK_EQ(actual, expected) \
    check_equal((unsigned long)(actual), (unsigned long)(expected), \
                #actual, __LINE__)

int main(void)
{
    volatile unsigned int argument_evaluations = 0U;
    unsigned int summary_index = 0U;
    PerfSummary summaries[2] = {
        {1UL, 1U, 1U, 1UL, 1UL, 1UL},
        {2UL, 2U, 2U, 2UL, 2UL, 2UL}
    };

    perf_reset();
    perf_set_sample(++argument_evaluations);
    CHECK_EQ(argument_evaluations, 0U);
    CHECK_EQ(perf_sampling(), false);
    CHECK_EQ(perf_begin(++argument_evaluations), 0UL);
    CHECK_EQ(argument_evaluations, 0U);
    perf_end(++argument_evaluations, ++argument_evaluations);
    CHECK_EQ(argument_evaluations, 0U);
    CHECK_EQ(perf_render_begin(), 0UL);
    perf_render_end(++argument_evaluations);
    CHECK_EQ(argument_evaluations, 0U);
    perf_frame_complete(++argument_evaluations, ++argument_evaluations);
    CHECK_EQ(argument_evaluations, 0U);
    perf_sample_pc(++argument_evaluations);
    CHECK_EQ(argument_evaluations, 0U);
    perf_note_char_kernel();
    perf_note_sprite_line();

    perf_set_sample(forbidden_argument());
    (void)perf_begin(forbidden_argument());
    perf_end(forbidden_argument(), forbidden_argument());
    perf_render_end(forbidden_argument());
    perf_frame_complete(forbidden_argument(), forbidden_argument());
    perf_sample_pc(forbidden_argument());

    perf_get_summary(&summaries[summary_index++]);
    CHECK_EQ(summary_index, 1U);
    CHECK_EQ(summaries[0].frames, 0U);
    CHECK_EQ(summaries[0].rendered_frames, 0U);
    CHECK_EQ(summaries[0].frame_ticks, 0UL);
    CHECK_EQ(summaries[0].average_ticks, 0UL);
    CHECK_EQ(summaries[0].p95_ticks, 0UL);
    CHECK_EQ(summaries[0].max_ticks, 0UL);
    CHECK_EQ(summaries[1].frames, 2U);
    perf_get_summary(NULL);
    CHECK_EQ(argument_evaluations, 0U);
    printf("disabled: %u checks, %u failures; standalone link without profiler/timer\n",
           checks_run, failures);
    return failures == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}