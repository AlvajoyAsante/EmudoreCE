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
#include <sys/timers.h>
#include "c64.h"
#include "cpu.h"
#include "memory.h"
#include "cia1.h"
#include "cia2.h"
#include "vic.h"
#include "io.h"
#include "perf.h"
#include "util.h"
#include "CEPackage.h"
#include "drive.h"

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

/* ROM-JIT gates: each native block in cpu_core.s is only valid for the
   exact KERNAL bytes it was translated from. bit0 = $E5CD wait-for-key
   (LDA $C6/STA $CC/STA $0292/BEQ), bit1 = $F69B UDTIM jiffy increment
   (LDX #0/INC $A2/BNE/INC $A1/BNE/INC $A0). */
static void detect_jit_blocks(void)
{
    const uint8_t *k = mem_kernal_rom_ptr();
    uint8_t mask = 0;
    if (k[0x05cd] == 0xA5 && k[0x05ce] == 0xC6 &&
        k[0x05cf] == 0x85 && k[0x05d0] == 0xCC &&
        k[0x05d1] == 0x8D && k[0x05d2] == 0x92 &&
        k[0x05d3] == 0x02 &&
        k[0x05d4] == 0xF0 && k[0x05d5] == 0xF7)
        mask |= 1;
    if (k[0x169b] == 0xA2 && k[0x169c] == 0x00 &&
        k[0x169d] == 0xE6 && k[0x169e] == 0xA2 &&
        k[0x169f] == 0xD0 && k[0x16a0] == 0x06 &&
        k[0x16a1] == 0xE6 && k[0x16a2] == 0xA1 &&
        k[0x16a3] == 0xD0 && k[0x16a4] == 0x02 &&
        k[0x16a5] == 0xE6 && k[0x16a6] == 0xA0)
        mask |= 2;
    cpu_jit_kernal = mask;
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
    detect_jit_blocks();
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
    if (drive_init())
        drive_patch_kernal();
    cia1_reset();
    cia2_reset();
    vic_reset();
    cpu_reset();
    c64_mark_ready();
}

/* Total emulated cycles, accumulated per raster line (cpu.cycles itself is
   24-bit and wraps every ~17 s). Read by main()'s exit speed report. */
unsigned long c64_total_cycles;

#ifndef NDEBUG
/* Wall-tick (32 kHz) buckets per subsystem: 0=cpu_run 1=vic 2=io 3=cia+drive.
   Coarse per call but exact in aggregate; names the hog in the exit dump. */
unsigned long c64_prof[4];
unsigned long c64_prof_lines;
#endif

void c64_run(void)
{
    /* cpu_run executes a whole raster line's worth of instructions per call.
       Glue cadence is tuned to its real cost: io_emulate and cia1_emulate
       each pay a ~12 us timer_Get (peripheral-bus reads), so they run every
       8/16 lines (still ~2500/1250 Hz); cheap cycle-based cia2/drive stay
       at 4 lines. */
    unsigned int line = 0;
    unsigned int last_cycles = cpu.cycles;
#if EMUDORE_PROFILE
    uint8_t sample_phase = 0;
    unsigned long sample_start;
#endif
#ifndef NDEBUG
    unsigned int t0, t1;
#endif
    c64_total_cycles = 0;
    perf_reset();
    for (;;)
    {
#if EMUDORE_PROFILE
        perf_set_sample(sample_phase == 0);
        if (++sample_phase == 17)
            sample_phase = 0;
        perf_sample_pc(cpu.pc);
        sample_start = perf_begin(PERF_CPU);
#endif
#ifndef NDEBUG
        t0 = (unsigned int)timer_Get(1);
#endif
        if (!cpu_run(vic_raster_due))
        {
#if EMUDORE_PROFILE
            perf_end(PERF_CPU, sample_start);
#endif
            return;
        }
#if EMUDORE_PROFILE
        perf_end(PERF_CPU, sample_start);
#endif
        c64_total_cycles += (unsigned int)(cpu.cycles - last_cycles);
        last_cycles = cpu.cycles;
#ifndef NDEBUG
        t1 = (unsigned int)timer_Get(1);
        c64_prof[0] += (unsigned int)(t1 - t0);
#endif
    #if EMUDORE_PROFILE
        sample_start = perf_begin(PERF_VIC);
    #endif
        if (!vic_emulate())
        {
    #if EMUDORE_PROFILE
            perf_end(PERF_VIC, sample_start);
    #endif
            return;
        }
    #if EMUDORE_PROFILE
        perf_end(PERF_VIC, sample_start);
    #endif
#ifndef NDEBUG
        c64_prof[1] += (unsigned int)(timer_Get(1) - t1);
        c64_prof_lines++;
#endif
        ++line;
        if ((line & 7) == 0)
        {
    #if EMUDORE_PROFILE
            sample_start = perf_begin(PERF_IO);
    #endif
#ifndef NDEBUG
            t0 = (unsigned int)timer_Get(1);
#endif
            if (!io_emulate())
            {
    #if EMUDORE_PROFILE
            perf_end(PERF_IO, sample_start);
    #endif
                return;   /* CLEAR pressed */
            }
    #if EMUDORE_PROFILE
            perf_end(PERF_IO, sample_start);
    #endif
#ifndef NDEBUG
            c64_prof[2] += (unsigned int)(timer_Get(1) - t0);
#endif
        }
        if ((line & 3) == 0)
        {
    #if EMUDORE_PROFILE
            sample_start = perf_begin(PERF_CIA);
    #endif
#ifndef NDEBUG
            t0 = (unsigned int)timer_Get(1);
#endif
            cia2_emulate();
            drive_tick();
            if ((line & 15) == 0)
                cia1_emulate();
#if EMUDORE_PROFILE
            perf_end(PERF_CIA, sample_start);
#endif
#ifndef NDEBUG
            c64_prof[3] += (unsigned int)(timer_Get(1) - t0);
#endif
        }
    }
}

void c64_shutdown(void)
{
    mem_shutdown();
    io_shutdown();
}
