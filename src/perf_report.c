#include "perf.h"

#if EMUDORE_PROFILE

#undef NDEBUG
#include <debug.h>
#include <graphx.h>
#include <keypadc.h>

#include "cpu.h"
#include "vic.h"

static PerfSummary summary;
static unsigned long emulated_tenths, rendered_tenths;
static unsigned long average_ms, p95_ms, maximum_ms;
static uint64_t sampled_ticks;
static unsigned int sampled_cpu_percent, sampled_render_percent;

static void print_rate(const char *label, unsigned long tenths, uint8_t row)
{
    gfx_SetTextXY(12, row);
    gfx_PrintString(label);
    gfx_PrintUInt((unsigned int)(tenths / 10), 1);
    gfx_PrintChar('.');
    gfx_PrintUInt((unsigned int)(tenths % 10), 1);
}

static void print_value(const char *label, unsigned long value, uint8_t row)
{
    gfx_SetTextXY(12, row);
    gfx_PrintString(label);
    gfx_PrintUInt((unsigned int)value, 1);
}

void perf_report(void)
{
    unsigned int scope, page;
    unsigned int offset;

    perf_get_summary(&summary);
    emulated_tenths = summary.frame_ticks
        ? (unsigned long)((uint64_t)summary.frames * 327680u / summary.frame_ticks) : 0;
    rendered_tenths = summary.frame_ticks
        ? (unsigned long)((uint64_t)summary.rendered_frames * 327680u / summary.frame_ticks) : 0;
    average_ms = (unsigned long)((uint64_t)summary.average_ticks * 1000u / 32768u);
    p95_ms = (unsigned long)((uint64_t)summary.p95_ticks * 1000u / 32768u);
    maximum_ms = (unsigned long)((uint64_t)summary.max_ticks * 1000u / 32768u);
    sampled_ticks = perf_stats.render_ticks;
    for (scope = 0; scope < PERF_SCOPE_COUNT; ++scope)
        sampled_ticks += perf_stats.ticks[scope];
    sampled_cpu_percent = sampled_ticks
        ? (unsigned int)((uint64_t)perf_stats.ticks[PERF_CPU] * 100u / sampled_ticks) : 0;
    sampled_render_percent = sampled_ticks
        ? (unsigned int)((uint64_t)perf_stats.render_ticks * 100u / sampled_ticks) : 0;

    dbg_printf("[perf] recent=%u rendered=%u ticks=%lu fps10=%lu renderfps10=%lu avg_ms=%lu p95_ms=%lu max_ms=%lu\n",
               summary.frames, summary.rendered_frames, summary.frame_ticks,
               emulated_tenths, rendered_tenths, average_ms, p95_ms, maximum_ms);
    dbg_printf("[perf] frames=%lu rendered=%lu hidden=%lu chars=%lu sprite_lines=%lu asm_rows=%u skip=%u\n",
               perf_stats.frames, perf_stats.rendered_frames, perf_stats.hidden_frames,
               perf_stats.char_kernel_calls, perf_stats.sprite_lines,
               (unsigned int)vic_asm_status(), (unsigned int)vic_frame_skip_enabled());
    for (scope = 0; scope < PERF_SCOPE_COUNT; ++scope)
        dbg_printf("[perf-sample] scope=%u calls=%lu sampled=%lu exclusive_ticks=%lu\n",
                   scope, perf_stats.total_calls[scope],
                   perf_stats.sampled_calls[scope], perf_stats.ticks[scope]);
    dbg_printf("[perf-render] calls=%lu sampled=%lu ticks=%lu\n",
               perf_stats.render_calls, perf_stats.sampled_render_calls,
               perf_stats.render_ticks);
    dbg_printf("[perf-cpu] fallbacks=%u opcode=%02x kernal=%02x key=%u jiffy=%u\n",
               cpu_fallbacks, cpu_fallback_op, cpu_jit_kernal,
               cpu_jit_hits, cpu_jit_hits2);
    for (page = 0; page < 256; ++page)
        if (perf_stats.pc_pages[page])
            dbg_printf("[perf-pc] page=%02x samples=%u\n",
                       page, (unsigned int)perf_stats.pc_pages[page]);
    for (page = 0x01; page <= 0x08; page += 0x07)
    {
        uint16_t hottest_count = 0;
        uint8_t hottest_offset = 0;
        for (offset = 0; offset < 256; ++offset)
            if (perf_stats.pc_hot_offsets[page >> 3][offset] > hottest_count)
            {
                hottest_count = perf_stats.pc_hot_offsets[page >> 3][offset];
                hottest_offset = (uint8_t)offset;
            }
        if (hottest_count)
            dbg_printf("[perf-pc-hot] pc=%02x%02x samples=%u\n", page,
                       hottest_offset, (unsigned int)hottest_count);
    }

    gfx_FillScreen(0);
    gfx_SetTextFGColor(1);
    gfx_SetTextBGColor(0);
    gfx_SetTextXY(12, 12);
    gfx_PrintString("Performance: recent frames");
    print_rate("Emulated FPS: ", emulated_tenths, 34);
    print_rate("Rendered FPS: ", rendered_tenths, 50);
    print_value("Frame count: ", summary.frames, 70);
    print_value("Average ms: ", average_ms, 86);
    print_value("95th pct ms: ", p95_ms, 102);
    print_value("Maximum ms: ", maximum_ms, 118);
    gfx_SetTextXY(12, 140);
    gfx_PrintString((vic_asm_status() & 4) ? "Character kernel: ASM" : "Character kernel: C");
    gfx_SetTextXY(12, 156);
    gfx_PrintString(vic_frame_skip_enabled() ? "Frame skipping: ON" : "Frame skipping: OFF");
    gfx_SetTextXY(12, 178);
    gfx_PrintString("Sampled time, including loading:");
    print_value("CPU: ", sampled_cpu_percent, 194);
    gfx_PrintString("%  Render: ");
    gfx_PrintUInt(sampled_render_percent, 1);
    gfx_PrintChar('%');
    gfx_SetTextXY(12, 220);
    gfx_PrintString("ENTER to close");
    do
    {
        kb_Scan();
    } while (!kb_IsDown(kb_KeyEnter));
}

#endif