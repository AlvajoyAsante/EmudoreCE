#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cemu_throttle.h"
#include "asic.h"
#include "mem.h"
#include "cpu.h"
#include "emu.h"
#include "schedule.h"

void gui_console_clear(void) {}
void gui_console_printf(const char *format, ...) { (void)format; }
void gui_console_err_printf(const char *format, ...) { (void)format; }
asic_rev_t gui_handle_reset(const boot_ver_t *version, asic_rev_t loaded,
                           asic_rev_t suggested, emu_device_t device, bool *python)
{
    (void)version;
    (void)loaded;
    (void)suggested;
    (void)device;
    (void)python;
    return ASIC_REV_PRE_A;
}

static void prepare_halt(void)
{
    mem_poke_byte(0xd08000, 0x76);
    cpu.ADL = true;
    cpu.halted = false;
    cpu.IEF1 = cpu.IEF2 = cpu.IEF_wait = cpu.NMI = false;
    cpu_flush(0xd08000, true);
    cpu_clear_signals();
    if (!emu_set_run_rate(1000))
        exit(1);
}

int main(void)
{
    CemuThrottleSnapshot start, first, second, reset, final;
    double simulated;
    double wall;
    double ratio;
    asic_init();
    mem.flash.size = SIZE_FLASH_MIN;
    mem.flash.block = malloc(mem.flash.size);
    if (!mem.flash.block)
        return 1;
    memset(mem.flash.block, 0xff, mem.flash.size);
    asic_reset();
    prepare_halt();
    cemu_throttle_snapshot(&start);
    cemu_throttle_run(1000);
    cemu_throttle_snapshot(&first);
    cemu_throttle_run(1000);
    cemu_throttle_snapshot(&second);
    simulated = (double)(second.simulated_ticks - start.simulated_ticks) / 48000000.0;
    wall = second.wall_seconds - start.wall_seconds;
    ratio = simulated / wall;
    printf("CALIBRATION simulated_seconds=%.6f wall_seconds=%.6f achieved_percent=%.3f\n",
           simulated, wall, ratio * 100.0);
    if (simulated < 1.999 || simulated > 2.001 || ratio > 1.356 ||
        first.simulated_ticks < 47999000 || !cpu.halted || cpu.haltCycles < 95000000)
        return 1;
    if (ratio < 1.30)
    {
        puts("HOST_UNDERSPEED: cap enforced, host did not sustain 135%");
        return 2;
    }
    asic_reset();
    prepare_halt();
    cemu_throttle_snapshot(&reset);
    if (reset.simulated_ticks != second.simulated_ticks ||
        reset.wall_seconds < second.wall_seconds)
        return 1;
    if (cemu_throttle_run_until_halt(1000) != 1)
        return 1;
    cemu_throttle_snapshot(&final);
    if (final.simulated_ticks - reset.simulated_ticks > 48100 ||
        final.simulated_ticks - reset.simulated_ticks < 47900)
        return 1;
    asic_free();
    puts("PASS 135% throttle, HALT elapsed time, scheduler rollover, reset and early-stop accounting");
    return 0;
}
