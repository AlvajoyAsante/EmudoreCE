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
#include <string.h>
#include "c64.h"
#include "cpu.h"
#include "memory.h"
#include "cia1.h"
#include "cia2.h"
#include "vic.h"
#include "io.h"
#include "util.h"
#include "CEPackage.h"

/* The KERNAL RAM test ($FD6C-$FD87) walks all 40 KiB byte-by-byte (~1.3M
   emulated cycles of boot time). Replace it with "top of RAM = $A000" using
   the ROM's own SETTOP call. Bytes are verified first so a different KERNAL
   image is left untouched. */
static void patch_kernal_ram_test(void)
{
    static const uint8_t patch[] = {
        0xA2, 0x00,       /* LDX #$00       */
        0xA0, 0xA0,       /* LDY #$A0       */
        0x18,             /* CLC            */
        0x20, 0x2D, 0xFE, /* JSR $FE2D  set MEMSIZ = $A000 */
        0x4C, 0x90, 0xFD  /* JMP $FD90  continue past the test */
    };
    uint8_t *k = mem_kernal_rom_ptr();
    if (k[0x1d6c] == 0xE6 && k[0x1d6d] == 0xC2 &&  /* INC $C2      */
        k[0x1d6e] == 0xB1 && k[0x1d6f] == 0xC1 &&  /* LDA ($C1),Y  */
        k[0x1d8d] == 0x20 && k[0x1d8e] == 0x2D &&  /* JSR $FE2D    */
        k[0x1d8f] == 0xFE)
        memcpy(&k[0x1d6c], patch, sizeof(patch));
}

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
    patch_kernal_ram_test();
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
    /* The CPU runs in a tight batch until the next raster line is due (the
       per-instruction vic/cia/io calls were pure overhead). Comparisons are
       wrap-safe: cpu.cycles is a 24-bit int on the eZ80 and wraps every ~17
       emulated seconds. */
    unsigned int line = 0;
    for (;;)
    {
        while ((int)(cpu.cycles - vic_raster_due) < 0)
            if (!cpu_emulate())
                return;
        if (!vic_emulate())
            return;
        cia1_emulate();
        cia2_emulate();
        if ((++line & 7) == 0 && !io_emulate())
            return;   /* CLEAR pressed */
    }
}

void c64_shutdown(void)
{
    mem_shutdown();
    io_shutdown();
}
