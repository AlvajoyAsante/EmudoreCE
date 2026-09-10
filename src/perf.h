#ifndef EMUDORE_CE_PERF_H
#define EMUDORE_CE_PERF_H

#include <stdbool.h>
#include <stdint.h>

#ifndef EMUDORE_PROFILE
#define EMUDORE_PROFILE 0
#endif

#define PERF_FRAME_WINDOW_SIZE 32

typedef enum {
    PERF_CPU,
    PERF_VIC,
    PERF_IO,
    PERF_CIA,
    PERF_SCOPE_COUNT
} PerfScope;

typedef struct {
    unsigned long total_calls[PERF_SCOPE_COUNT];
    unsigned long sampled_calls[PERF_SCOPE_COUNT];
    unsigned long ticks[PERF_SCOPE_COUNT];
    unsigned long render_ticks;
    unsigned long render_calls;
    unsigned long sampled_render_calls;
    unsigned long frames;
    unsigned long rendered_frames;
    unsigned long hidden_frames;
    unsigned long char_kernel_calls;
    unsigned long sprite_lines;
    uint16_t pc_pages[256];
} PerfStats;

typedef struct {
    unsigned long frame_ticks;
    unsigned int frames;
    unsigned int rendered_frames;
    unsigned long average_ticks;
    unsigned long p95_ticks;
    unsigned long max_ticks;
} PerfSummary;

#if EMUDORE_PROFILE

extern PerfStats perf_stats;

void perf_reset(void);
void perf_set_sample(bool active);
bool perf_sampling(void);
unsigned long perf_begin(PerfScope scope);
void perf_end(PerfScope scope, unsigned long start);
unsigned long perf_render_begin(void);
void perf_render_end(unsigned long start);
void perf_frame_complete(bool rendered, bool hidden);
void perf_sample_pc(uint16_t pc);
void perf_get_summary(PerfSummary *summary);
void perf_report(void);

#define perf_note_char_kernel() ((void)++perf_stats.char_kernel_calls)
#define perf_note_sprite_line() ((void)++perf_stats.sprite_lines)

#else

#define perf_reset() ((void)0)
#define perf_set_sample(active) ((void)sizeof((bool)(active)))
#define perf_sampling() (false)
#define perf_begin(scope) ((void)sizeof((PerfScope)(scope)), 0UL)
#define perf_end(scope, start) \
    ((void)sizeof((PerfScope)(scope)), (void)sizeof((unsigned long)(start)))
#define perf_render_begin() (0UL)
#define perf_render_end(start) ((void)sizeof((unsigned long)(start)))
#define perf_frame_complete(rendered, hidden) \
    ((void)sizeof((bool)(rendered)), (void)sizeof((bool)(hidden)))
#define perf_sample_pc(pc) ((void)sizeof((uint16_t)(pc)))
#define perf_note_char_kernel() ((void)0)
#define perf_note_sprite_line() ((void)0)
#define perf_report() ((void)0)

static inline void perf_get_summary(PerfSummary *summary)
{
    if (summary)
        *summary = (PerfSummary){0};
}

#endif

#endif