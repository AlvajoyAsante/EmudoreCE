/*
 * emudore-CE entry point.
 *
 * Boots the C64 from ROM AppVars (BASIC / KERNAL / character generator) and
 * runs the emulator until CLEAR is pressed.
 *
 * The boot path is deliberately minimal to save stack: the CE's stack is small
 * and the sprintf-based dbg_printf() / graphx text calls each use a lot of it,
 * which was overflowing into BSS and crashing the c64_init() call. So the boot
 * screen is a single status line and we validate the ROMs quietly.
 */
#include <graphx.h>
#include <keypadc.h>
#include <stdint.h>
#include <string.h>
#include <debug.h>
#include <sys/timers.h>

#include "c64.h"
#include "cpu.h"
#include "cia1.h"
#include "cia2.h"
#include "vic.h"
#include "perf.h"
#include "io.h"
#include "memory.h"
#include "CEPackage.h"
#include "drive.h"

/* File-scope palette (2 entries is enough for a status line). */
static const uint16_t bootPalette[] = {
    gfx_RGBTo1555(0, 0, 0),       /* 0 = black background */
    gfx_RGBTo1555(255, 255, 255)  /* 1 = white text       */
};

/* The ROM-status mask MUST be static (memory-backed), not a local. The eZ80
   compiler was keeping a local romMask in the IX register across the
   CEPackageValidate() calls, but those (and graphx) do NOT preserve IX, so
   IX's upper bytes got clobbered and c64_init() later dereferenced a garbage
   24-bit address (the 0x7b4c82 / 0x0017 crashes). Keeping it in memory stops
   the compiler from allocating it to a register. */
static int romMask;

/* Wall-clock ticks (32 kHz timer 1) spent inside c64_run; static for the
   same IX-register reason as romMask. */
static unsigned long speed_ticks;

static void print_centered(const char *s, uint8_t y);

static uint8_t count_bits(uint8_t value)
{
    uint8_t count = 0;
    while (value)
    {
        count += value & 1;
        value >>= 1;
    }
    return count;
}

static char *append_u8(char *dst, uint8_t value)
{
    char digits[3];
    uint8_t length = 0;

    do
    {
        digits[length++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (value && length < sizeof(digits));

    while (length)
        *dst++ = digits[--length];
    return dst;
}

static void print_count_line(const char *label, uint8_t value, uint8_t total,
                             uint8_t y)
{
    char line[24];
    char *p = line;

    while (*label)
        *p++ = *label++;
    *p++ = ' ';
    p = append_u8(p, value);
    if (total)
    {
        *p++ = '/';
        p = append_u8(p, total);
    }
    *p = '\0';
    print_centered(line, y);
}

/* Returns 0 = quit, 1 = file selected (run it), 2 = boot BASIC without disk. */
static int show_file_menu(uint8_t romCount, uint8_t fileCount)
{
    uint8_t selected = 0;
    /* entry 0 = "BASIC (no disk)", entries 1..fileCount = disk files */
    uint8_t visible = fileCount < 8 ? (uint8_t)(fileCount + 1) : 9;
    uint8_t keyHeld = 0;
    uint8_t redraw = 1;

    if (!fileCount)
        return 2;

    for (;;)
    {
        uint8_t i;
        char name[17];

        if (redraw)
        {
            gfx_FillScreen(0);
            print_centered("emudore file menu", 12);
            print_count_line("ROMs", romCount, 3, 28);
            print_count_line("FILES", fileCount, 0, 40);
            print_centered("UP/DOWN choose, ENTER load", 54);

            for (i = 0; i < visible; ++i)
            {
                char line[20];
                char *p = line;
                const char *src;

                if (i == 0)
                {
                    name[0] = '\0';
                    src = "BASIC (no disk)";
                }
                else
                {
                    if (!drive_file_name((uint8_t)(i - 1), name, sizeof(name)))
                        continue;
                    src = name;
                }
                *p++ = (i == selected) ? '>' : ' ';
                *p++ = ' ';
                for (; *src && (p - line) < (int)sizeof(line) - 1; ++src)
                    *p++ = *src;
                *p = '\0';
                gfx_PrintStringXY(line, 16, (uint8_t)(76 + i * 16));
            }

            redraw = 0;
        }

        kb_Scan();
        if (kb_IsDown(kb_KeyClear))
            return 0;
        if (kb_IsDown(kb_KeyUp))
        {
            if (!keyHeld && selected > 0)
            {
                --selected;
                redraw = 1;
            }
            keyHeld = 1;
            continue;
        }
        if (kb_IsDown(kb_KeyDown))
        {
            if (!keyHeld && selected + 1 < visible)
            {
                ++selected;
                redraw = 1;
            }
            keyHeld = 1;
            continue;
        }
        if (kb_IsDown(kb_KeyEnter))
        {
            if (selected == 0)
                return 2;
            drive_select_file((uint8_t)(selected - 1));
            return 1;
        }
        keyHeld = 0;
    }
}

static void print_centered(const char *s, uint8_t y)
{
    gfx_PrintStringXY(s, (GFX_LCD_WIDTH - gfx_GetStringWidth(s)) / 2, y);
}

#ifndef NDEBUG
/* boot-probe plumbing: reset RAM + chips to a deterministic pre-run state */
static void probe_reset(void)
{
    mem_init();
    cia1_reset();
    cia2_reset();
    vic_reset();
    cpu_reset();
}

static void probe_report(unsigned int budget,
                         const CpuState *c_state, const CpuState *a_state)
{
    if (!budget)
    {
        dbg_printf("[probe] no divergence up to 30000 cycles\n");
        return;
    }
    dbg_printf("[probe] DIVERGED at budget %u:\n", budget);
    dbg_printf("[probe]  C : pc=%04x sp=%02x a=%02x x=%02x y=%02x cyc=%u "
               "c%dz%dn%dv%di%dd%d\n",
               c_state->pc, c_state->sp, c_state->a, c_state->x, c_state->y,
               c_state->cycles, c_state->cf, c_state->zf, c_state->nf,
               c_state->of, c_state->idf, c_state->dmf);
    dbg_printf("[probe]  ASM: pc=%04x sp=%02x a=%02x x=%02x y=%02x cyc=%u "
               "c%dz%dn%dv%di%dd%d\n",
               a_state->pc, a_state->sp, a_state->a, a_state->x, a_state->y,
               a_state->cycles, a_state->cf, a_state->zf, a_state->nf,
               a_state->of, a_state->idf, a_state->dmf);
}
#endif

int main(void)
{
    uint8_t romCount;
    uint8_t fileCount;

    gfx_Begin();
    /* Single-buffered on purpose: the second VRAM half (0xD52C00) holds the
       C64's 64 KiB RAM (see memory.c), so it must never be blitted/drawn. */
    gfx_SetPalette(bootPalette, sizeof(bootPalette), 0);
    gfx_FillScreen(0);
    gfx_SetTextFGColor(1);
    print_centered("emudore CE booting...", 110);

    {
        uint8_t rows = vic_row_asm_selftest();
        (void)rows; /* only used by dbg_printf, which is empty in NDEBUG */
        dbg_printf("[boot] asm rows: hires=%s mc=%s chars=%s\n",
                   (rows & 1) ? "OK" : "FAILED",
               (rows & 2) ? "OK" : "FAILED",
               (rows & 4) ? "OK" : "FAILED");
    }

    /* Validate the three ROM AppVars. bit0=BASIC bit1=KERNAL bit2=CHARGEN. */
    romMask = 0;
    if (CEPackageValidate(CE_PACKAGE_BASIC, 0x2000))  romMask |= 1;
    if (CEPackageValidate(CE_PACKAGE_KERNAL, 0x2000)) romMask |= 2;
    if (CEPackageValidate(CE_PACKAGE_CHARS, 0x1000))  romMask |= 4;

    dbg_printf("[boot] EMUDORE romMask=%d\n", romMask);
    romCount = count_bits((uint8_t)romMask);

    if (romMask != 7)
    {
        print_centered("ROM AppVars missing!", 130);
        while (!kb_AnyKey())
            kb_Scan();
        gfx_End();
        return 1;
    }

    fileCount = drive_init() ? drive_file_count() : 0;
    if (fileCount)
    {
        int choice = show_file_menu(romCount, fileCount);
        if (!choice)
        {
            gfx_End();
            return 0;
        }
        if (choice == 2)
        {
            /* plain BASIC boot: no KERNAL autoload; a manual LOAD would
               otherwise hit the unpatched IEC path and hang for minutes */
            drive_disable();
            fileCount = 0;
        }
    }

    dbg_printf("[boot] init_io\n");
    io_init();

    dbg_printf("[boot] init_mem\n");
    mem_init();
    if (!mem_ready())
    {
        dbg_printf("[boot] init_mem FAILED\n");
        print_centered("mem init failed", 130);
        while (!kb_AnyKey())
            kb_Scan();
        gfx_End();
        return 1;
    }

    dbg_printf("[boot] cpu core: %s\n",
               cpu_asm_selftest() ? "asm (self-test OK)"
                                  : "C (asm self-test FAILED)");
    kb_Scan();
    if (kb_IsDown(kb_KeyMode))
    {
        cpu_asm_disable();
        dbg_printf("[boot] MODE held: forcing C cpu core\n");
    }

    dbg_printf("[boot] load_roms\n");
    if (!c64_boot_roms())
    {
        dbg_printf("[boot] load_roms FAILED\n");
        print_centered("ROM load failed", 130);
        while (!kb_AnyKey())
            kb_Scan();
        gfx_End();
        return 1;
    }

    /* KERNAL reset vector must be $FCE2; anything else means the ROM bytes
       did not land where the CPU will fetch them. */
    dbg_printf("[boot] resetvec=%02x%02x\n",
               mem_kernal_rom_ptr()[0x1ffd], mem_kernal_rom_ptr()[0x1ffc]);

#ifndef NDEBUG
    /* Dual-core boot divergence probe: pinpoints the first opcode where the
       asm core's architectural state departs from the C core's. */
    cpu_asm_boot_probe(probe_reset, probe_report);
#endif

    /* The LOAD trap is planted in every mode: with a disk it serves files
       from the DSK AppVar (drive_tick auto-types LOAD"*",8,1 at READY, a
       menu-chosen file resolves the wildcard), without one it fails fast
       with DEVICE NOT PRESENT (the real IEC path hangs on an empty bus). */
    drive_patch_kernal();
    if (fileCount)
        dbg_printf("[boot] disk appvar found\n");
    else
        dbg_printf("[boot] no disk appvar\n");

    dbg_printf("[boot] reset cia1\n");
    cia1_reset();
    dbg_printf("[boot] reset cia2\n");
    cia2_reset();
    dbg_printf("[boot] reset vic\n");
    vic_reset();
    dbg_printf("[boot] reset cpu\n");
    cpu_reset();
    c64_mark_ready();

#ifndef NDEBUG
    {
        /* Timebase calibration: count timer-1 (32K crystal) ticks across
           24M CPU-clock ticks of timer 2 (0.5 s at 48 MHz). Expected ~16384;
           a big deviation means every wall-clock measurement is scaled.
           timer_Get spins until two consecutive reads match - never true on
           a CPU-clocked timer - so timer 2 is read only while stopped. */
        unsigned long t1a, t2;
        timer_Disable(2);
        timer_Set(2, 0);
        t1a = timer_Get(1);
        do
        {
            volatile unsigned int spin = 4000;
            timer_Enable(2, TIMER_CPU, TIMER_NOINT, TIMER_UP);
            while (spin--)
                ;
            timer_Disable(2); /* pause is ~1% of the spin window */
            t2 = timer_Get(2);
        } while (t2 < 24000000UL);
        dbg_printf("[cal] timer1 ticks per 0.5s: %lu (expect ~16384)\n",
                   timer_Get(1) - t1a);
    }

    {
        /* Microbench 1: raw timer_Get cost (it reads peripheral-bus regs,
           which may be far slower than RAM and inflate every profile
           bucket, including this profiler's own overhead). */
        unsigned long tb0, tb1;
        unsigned int i;
        volatile unsigned long sink = 0;
        tb0 = timer_Get(1);
        for (i = 0; i < 10000; ++i)
            sink += timer_Get(1);
        tb1 = timer_Get(1);
        (void)sink;
        dbg_printf("[bench] 10000x timer_Get: %lu ticks\n", tb1 - tb0);

        /* Microbench 2: asm core alone on a 3-instruction RAM loop
           (INX/DEX/JMP $0400, 8 cycles/iter, no IO, no warp, no per-line
           glue) -> the core's true cyc/s ceiling. */
        mem_low[0x400] = 0xE8;
        mem_low[0x401] = 0xCA;
        mem_low[0x402] = 0x4C;
        mem_low[0x403] = 0x00;
        mem_low[0x404] = 0x04;
        cpu.pc = 0x0400;
        tb0 = timer_Get(1);
        cpu_run(cpu.cycles + 650000UL);
        tb1 = timer_Get(1);
        dbg_printf("[bench] 650k cyc asm loop: %lu ticks = %lu cyc/s\n",
                   tb1 - tb0,
                   (unsigned long)(((uint64_t)650000UL << 15) / (tb1 - tb0)));

        /* Microbench 3: raw VRAM fill (one full 320x240 frame per pass).
           Splits "VRAM is inherently slow under LCD DMA" from "the char
           renderer wastes work": a full-frame render measures ~120 ms. */
        tb0 = timer_Get(1);
        for (i = 0; i < 10; ++i)
            memset(gfx_vram, 0, GFX_LCD_WIDTH * GFX_LCD_HEIGHT);
        tb1 = timer_Get(1);
        dbg_printf("[bench] 10x 76800B VRAM memset: %lu ticks\n", tb1 - tb0);

        mem_low[0x400] = mem_low[0x401] = mem_low[0x402] = 0;
        mem_low[0x403] = mem_low[0x404] = 0;
        cpu_reset(); /* re-fetch the reset vector for the real boot */
    }
#endif

    dbg_printf("[boot] run\n");
    gfx_FillScreen(0); /* boot text must not linger through skipped frames */
    if (fileCount)
    {
        /* Skip straight to the program: render nothing until the autoload
           finishes (drive.c lifts the veil ~12 frames after the LOAD). */
        vic_set_boot_hidden(true);
        print_centered("loading...", 110);
    }
    io_reload_palette(); /* nothing after this may touch the palette */
    speed_ticks = timer_Get(1); /* timer 1 running since cia1_reset */
    c64_run();
    speed_ticks = timer_Get(1) - speed_ticks;

    if (cpu_illegal_op)
        dbg_printf("[exit] illegal opcode %02x at %04x\n",
                   cpu_illegal_op, cpu_illegal_pc);
    else
        dbg_printf("[exit] quit key%s\n",
                   vic_bad_mode ? " (illegal gfx mode seen, rendered black)"
                                : "");

    {
        /* cycles/sec = cycles * 32768 / ticks; PAL C64 = 985248 cyc/s */
        unsigned long cps = speed_ticks
            ? (unsigned long)(((uint64_t)c64_total_cycles << 15) / speed_ticks)
            : 0;
        (void)cps;
        dbg_printf("[speed] %lu cyc in %lu ticks = %lu cyc/s (%lu%% of PAL)\n",
                   c64_total_cycles, speed_ticks, cps, cps / 9852);
    }

#ifndef NDEBUG
    dbg_printf("[prof] lines=%lu cpu=%lu vic=%lu io=%lu cia=%lu ticks\n",
               c64_prof_lines, c64_prof[0], c64_prof[1],
               c64_prof[2], c64_prof[3]);
#endif
    dbg_printf("[cpu] fallbacks=%u last-op=%02x\n",
               cpu_fallbacks, cpu_fallback_op);
    dbg_printf("[jit] mask=%02x e5cd=%u udtim=%u\n",
               cpu_jit_kernal, cpu_jit_hits, cpu_jit_hits2);
#ifndef NDEBUG
    dbg_printf("[vprof] renders=%lu flush=%lu char=%lu rows=%lu plines=%lu\n",
               vic_prof[2], vic_prof[0], vic_prof[1],
               vic_prof[3], vic_prof[4]);
#endif

    /* drive/BRK diagnostics gathered during the run */
    {
        static VicDebug vd;
        uint8_t i, n = drive_trace_count < DRIVE_TRACE_MAX
                     ? drive_trace_count : DRIVE_TRACE_MAX;
        dbg_printf("[drive] %u traps, %u BRKs (last at %04x)\n",
                   (unsigned)drive_trace_count, cpu_brk_count, cpu_last_brk_pc);
        if (cpu_brk_count)
            dbg_printf("[brk] first at %04x sp=%02x stack-ret=%04x\n",
                       cpu_first_brk_pc, cpu_first_brk_sp, cpu_first_brk_ret);
        for (i = 0; i < n; ++i)
        {
            DriveTrace *t = &drive_trace[i];
            (void)t;
            dbg_printf("[drive] #%u from=%04x name='%c%c%c' len=%u sa=%u "
                       "entry=%u addr=%04x end=%04x\n",
                       (unsigned)i, t->caller,
                       t->name[0], t->name[1], t->name[2],
                       (unsigned)t->len, (unsigned)t->sa,
                       (unsigned)t->entry, t->addr, t->end);
        }
        vic_debug_snapshot(&vd);
        dbg_printf("[vic] mode=%u cr1=%02x cr2=%02x d018=%02x base=%04x "
                   "scr=%04x chr=%04x bmp=%04x border=%u bg0=%u spr=%02x "
                   "rirq=%d irqen=%02x\n",
                   (unsigned)vd.mode, vd.cr1, vd.cr2, vd.mem_ptrs,
                   vd.vic_base, vd.screen_mem, vd.char_mem, vd.bitmap_mem,
                   (unsigned)vd.border, (unsigned)vd.bg0,
                   vd.sprite_en, vd.raster_irq, vd.irq_en);
        dbg_printf("[cpu] pc=%04x $01=%02x $d011-area irqs pending\n",
                   cpu.pc, *mem_host_ptr(1));
        dbg_printf("[pal] 01=%04x 21=%04x 31=%04x 40=%04x 44=%04x\n",
                   io_palette_probe(0x01), io_palette_probe(0x21),
                   io_palette_probe(0x31), io_palette_probe(0x40),
                   io_palette_probe(0x44));
        dbg_printf("[spr] en=%02x dh=%02x dw=%02x prio=%02x mc=%02x msbx=%02x\n",
                   vd.sprite_en, vd.spr_dh, vd.spr_dw, vd.spr_prio,
                   vd.spr_mc, vd.spr_msbx);
        dbg_printf("[spr] shared=%02x,%02x\n", vd.spr_shared[0], vd.spr_shared[1]);
        for (i = 0; i < 8; ++i)
            dbg_printf("[spr] #%u x=%u y=%u ptr=%02x color=%02x\n", (unsigned)i,
                       (unsigned)vd.spr_x[i], (unsigned)vd.spr_y[i],
                       vd.spr_ptr[i], vd.spr_color[i]);
        dbg_printf("[wtrace] last %u VIC writes (line reg val):\n",
                   (unsigned)VIC_WTRACE_MAX);
        for (i = 0; i < VIC_WTRACE_MAX; ++i)
        {
            const VicWriteTrace *t =
                &vic_wtrace[(vic_wtrace_pos + i) & (VIC_WTRACE_MAX - 1)];
            (void)t;
            dbg_printf("%3u %02x %02x%c", t->line, t->reg, t->val,
                       ((i & 3) == 3) ? '\n' : ' ');
        }
        {
            uint8_t *c = mem_host_ptr(0xc000);
            uint8_t *d = mem_host_ptr(0xd000);
            (void)c; (void)d;
            dbg_printf("[mem] c000=%02x%02x%02x%02x%02x%02x%02x%02x "
                       "d000=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                       c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
                       d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        }
#ifndef NDEBUG
        {
            uint16_t table = (uint16_t)(vd.vic_base |
                ((vd.screen_mem + VIC_SPRITE_PTRS_OFFSET) & 0x3fff));
            dbg_printf("[sptr] table=%04x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                       table, vd.spr_ptr[0], vd.spr_ptr[1], vd.spr_ptr[2], vd.spr_ptr[3],
                       vd.spr_ptr[4], vd.spr_ptr[5], vd.spr_ptr[6], vd.spr_ptr[7]);
            for (i = 0; i < 8; ++i)
            {
                uint16_t address = (uint16_t)(vd.spr_ptr[i] * VIC_SPRITE_SIZE);
                uint8_t offset;
                if (!(vd.sprite_en & (1u << i)))
                    continue;
                dbg_printf("[sprdata] #%u addr=%04x ", (unsigned)i,
                           (uint16_t)(vd.vic_base | address));
                for (offset = 0; offset < 63; ++offset)
                    dbg_printf("%02x", mem_vic_read_byte(address + offset));
                dbg_printf("\n");
            }
        }
        {
            int panel_y, panel_x;
            for (panel_y = 156; panel_y < 220; panel_y += 4)
            {
                const uint8_t *panel_row = io_text_row(panel_y + IO_CROP_Y);
                if (!panel_row)
                    continue;
                dbg_printf("[panel] y=%d x=224 ", panel_y);
                for (panel_x = 224; panel_x < 320; ++panel_x)
                    dbg_printf("%02x", panel_row[panel_x]);
                dbg_printf("\n");
            }
        }
#endif
        {
            /* framebuffer slices through the play area: raw palette indices
               incl. tag bits (0x20=fg text, 0x10=sprite) */
            int yy, xx;
            for (yy = 90; yy <= 130; yy += 20)
            {
                uint8_t *r = io_text_row(yy + IO_CROP_Y);
                if (!r)
                    continue;
                dbg_printf("[pix] y=%d ", yy);
                for (xx = 0; xx < 96; ++xx)
                    dbg_printf("%02x", r[xx]);
                dbg_printf("\n");
            }
        }
    }

    c64_shutdown();
    perf_report();
    gfx_End();
    return 0;
}
