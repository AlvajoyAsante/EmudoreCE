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
#include "CEPackage.h"

/* File-scope palette (2 entries is enough for a status line). */
static const uint16_t bootPalette[] = {
    gfx_RGBTo1555(0, 0, 0),       /* 0 = black background */
    gfx_RGBTo1555(255, 255, 255)  /* 1 = white text       */
};

int main(void)
{
    int romMask;

    gfx_Begin();
    gfx_SetDrawBuffer();
    gfx_SetPalette(bootPalette, sizeof(bootPalette), 0);
    gfx_FillScreen(0);
    gfx_SetTextFGColor(1);
    gfx_PrintStringXY("emudore CE booting...", 10, 110);
    gfx_BlitBuffer();

    /* Validate the three ROM AppVars. bit0=BASIC bit1=KERNAL bit2=CHARGEN. */
    romMask = 0;
    if (CEPackageValidate(CE_PACKAGE_BASIC, 0x2000))  romMask |= 1;
    if (CEPackageValidate(CE_PACKAGE_KERNAL, 0x2000)) romMask |= 2;
    if (CEPackageValidate(CE_PACKAGE_CHARS, 0x1000))  romMask |= 4;

    dbg_printf("[boot] EMUDORE romMask=%d\n", romMask);

    if (romMask != 7)
    {
        gfx_PrintStringXY("ROM AppVars missing!", 10, 130);
        gfx_BlitBuffer();
        while (!kb_AnyKey())
            kb_Scan();
        gfx_End();
        return 1;
    }

    dbg_printf("[boot] c64_init\n");
    if (!c64_init())
    {
        dbg_printf("[boot] c64_init FAILED\n");
        gfx_PrintStringXY("init failed (RAM?)", 10, 130);
        gfx_BlitBuffer();
        while (!kb_AnyKey())
            kb_Scan();
        gfx_End();
        return 1;
    }

    dbg_printf("[boot] run\n");
    c64_run();

    dbg_printf("[exit] done\n");
    c64_shutdown();
    gfx_End();
    return 0;
}
