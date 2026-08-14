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
#include <debug.h>

#include "c64.h"
#include "cpu.h"
#include "cia1.h"
#include "cia2.h"
#include "vic.h"
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
    gfx_Begin();
    /* Single-buffered on purpose: the second VRAM half (0xD52C00) holds the
       C64's 64 KiB RAM (see memory.c), so it must never be blitted/drawn. */
    gfx_SetPalette(bootPalette, sizeof(bootPalette), 0);
    gfx_FillScreen(0);
    gfx_SetTextFGColor(1);
    print_centered("emudore CE booting...", 110);

    {
        uint8_t rows = vic_row_asm_selftest();
        dbg_printf("[boot] asm rows: hires=%s mc=%s\n",
                   (rows & 1) ? "OK" : "FAILED",
                   (rows & 2) ? "OK" : "FAILED");
    }

    /* Validate the three ROM AppVars. bit0=BASIC bit1=KERNAL bit2=CHARGEN. */
    romMask = 0;
    if (CEPackageValidate(CE_PACKAGE_BASIC, 0x2000))  romMask |= 1;
    if (CEPackageValidate(CE_PACKAGE_KERNAL, 0x2000)) romMask |= 2;
    if (CEPackageValidate(CE_PACKAGE_CHARS, 0x1000))  romMask |= 4;

    dbg_printf("[boot] EMUDORE romMask=%d\n", romMask);

    if (romMask != 7)
    {
        print_centered("ROM AppVars missing!", 130);
        while (!kb_AnyKey())
            kb_Scan();
        gfx_End();
        return 1;
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

    /* Optional disk package (ECDSKnn AppVars): patch the LOAD trap and let
       drive_tick auto-type LOAD"*",8,1 at the READY prompt. */
    if (drive_init())
    {
        drive_patch_kernal();
        dbg_printf("[boot] disk appvar found\n");
    }
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

    dbg_printf("[boot] run\n");
    gfx_FillScreen(0); /* boot text must not linger through skipped frames */
    io_reload_palette(); /* nothing after this may touch the palette */
    c64_run();

    if (cpu_illegal_op)
        dbg_printf("[exit] illegal opcode %02x at %04x\n",
                   cpu_illegal_op, cpu_illegal_pc);
    else
        dbg_printf("[exit] quit key%s\n",
                   vic_bad_mode ? " (illegal gfx mode seen, rendered black)"
                                : "");

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
        for (i = 0; i < 8; ++i)
            dbg_printf("[spr] #%u x=%u y=%u\n", (unsigned)i,
                       (unsigned)vd.spr_x[i], (unsigned)vd.spr_y[i]);
        dbg_printf("[wtrace] last %u VIC writes (line reg val):\n",
                   (unsigned)VIC_WTRACE_MAX);
        for (i = 0; i < VIC_WTRACE_MAX; ++i)
        {
            const VicWriteTrace *t =
                &vic_wtrace[(vic_wtrace_pos + i) & (VIC_WTRACE_MAX - 1)];
            dbg_printf("%3u %02x %02x%c", t->line, t->reg, t->val,
                       ((i & 3) == 3) ? '\n' : ' ');
        }
        {
            uint8_t *c = mem_host_ptr(0xc000);
            uint8_t *d = mem_host_ptr(0xd000);
            dbg_printf("[mem] c000=%02x%02x%02x%02x%02x%02x%02x%02x "
                       "d000=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                       c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
                       d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        }
        {
            /* sprite pointer table (VIC bank 3, screen $C000) */
            uint8_t *p = mem_host_ptr(0xc3f8);
            dbg_printf("[sptr] %02x %02x %02x %02x %02x %02x %02x %02x\n",
                       p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
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
    gfx_End();
    return 0;
}
