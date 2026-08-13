/*
 * emudore-CE: machine glue (C port of c64.cpp).
 *
 * c64_init()/c64_shutdown() wire up the chips; ROMs load from AppVars.
 *
 * IMPORTANT: this file must NOT use dbg_printf()/sprintf anywhere near
 * c64_init's return path. On the eZ80, sprintf loads the debug-port constant
 * into IX, and c64_init returns its bool in A via IX (ld a,ixl / pop ix / ret)
 * -- so a sprintf between the work and the return clobbers IX and returns to a
 * garbage address. All debug output here is kept out of the return path.
 */
#include <stdlib.h>
#include "c64.h"
#include "cpu.h"
#include "memory.h"
#include "cia1.h"
#include "cia2.h"
#include "vic.h"
#include "io.h"
#include "util.h"
#include "CEPackage.h"

/* Load the three C64 ROMs into the ROM image. Returns false on failure.
   No dbg_printf here: this feeds c64_init's return value. */
static bool load_roms(void)
{
    /* BASIC 8 KiB -> $A000, KERNAL 8 KiB -> $E000, CharGen 4 KiB -> $D000 */
    if (!CEPackageRead(CE_PACKAGE_BASIC, mem_basic_rom_ptr(), 0x2000))
        return false;
    if (!CEPackageRead(CE_PACKAGE_KERNAL, mem_kernal_rom_ptr(), 0x2000))
        return false;
    if (!CEPackageRead(CE_PACKAGE_CHARS, mem_chars_rom_ptr(), 0x1000))
        return false;
    return true;
}

static bool c64_ok;

bool c64_ready(void)
{
    return c64_ok;
}

bool c64_boot_memory(void)
{
    c64_ok = false;
    io_init();
    mem_init();
    return mem_ready();
}

bool c64_boot_roms(void)
{
    return load_roms();
}

void c64_mark_ready(void)
{
    c64_ok = true;
}

/* Returns void and stores the result in a static (c64_ok) instead of
   returning it through IX. The ez80 compiler was holding c64_init's bool
   return value in the IX register, but the CEPackageRead argument setup also
   loads constants/pointers into IX, clobbering the return value and the saved
   frame -> crash on `pop ix; ret`. A static result avoids that path. */
void c64_init(void)
{
    if (!c64_boot_memory())
        return;
    if (!c64_boot_roms())
        return;
    cia1_reset();
    cia2_reset();
    vic_reset();
    cpu_reset();
    c64_mark_ready();
}

void c64_run(void)
{
    /* cpu+vic must interleave every instruction (raster timing); CIA timers
       and keypad IO tolerate 8-instruction granularity and are the loop's
       fixed overhead, so they only run on every 8th iteration. */
    unsigned int tick = 0;
    for (;;)
    {
        if (!cpu_emulate())
            break;
        if (!vic_emulate())
            break;
        if ((++tick & 7) == 0)
        {
            cia1_emulate();
            cia2_emulate();
            if (!io_emulate())
                break;   /* CLEAR pressed */
        }
    }
}

void c64_shutdown(void)
{
    mem_shutdown();
    io_shutdown();
}
