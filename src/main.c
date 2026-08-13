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

int main(void)
{
    gfx_Begin();
    /* Single-buffered on purpose: the second VRAM half (0xD52C00) holds the
       C64's 64 KiB RAM (see memory.c), so it must never be blitted/drawn. */
    gfx_SetPalette(bootPalette, sizeof(bootPalette), 0);
    gfx_FillScreen(0);
    gfx_SetTextFGColor(1);
    print_centered("emudore CE booting...", 110);

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
    c64_run();

    if (cpu_illegal_op)
        dbg_printf("[exit] illegal opcode %02x at %04x\n",
                   cpu_illegal_op, cpu_illegal_pc);
    else if (vic_bad_mode)
        dbg_printf("[exit] unsupported gfx mode %d\n", vic_bad_mode);
    else
        dbg_printf("[exit] quit key\n");

    c64_shutdown();
    gfx_End();
    return 0;
}
