/*
 * emudore-CE: machine glue (C port of c64.h).
 *
 * The desktop version also had a BASIC/PRG Loader and a Debugger; neither is
 * brought over (the calculator version boots from ROM AppVars and has no
 * debugger UI), so this header only wires up the core chips.
 */
#ifndef EMUDORE_CE_C64_H
#define EMUDORE_CE_C64_H

#include <stdbool.h>

#ifndef EMUDORE_CPU_HOT_PROFILE
#define EMUDORE_CPU_HOT_PROFILE 0
#endif

/**
 * Bring up the emulated machine.
 *
 * Initialization is staged so platform services come up before the chips:
 * IO and RAM backing are initialized first, then ROM AppVars are loaded,
 * then the CIAs, VIC and CPU are reset into their power-on state.
 *
 * On failure, the machine stays in the not-ready state and c64_ready()
 * continues to return false.
 */
void c64_init(void);
/**
 * Report whether the machine finished initialization successfully.
 *
 * @return true after c64_init() reached the chip-reset stage successfully;
 *         false if initialization has not run yet or failed partway through.
 */
bool c64_ready(void);
/**
 * Initialize the CE-specific IO and memory backends.
 *
 * This is the first half of c64_init() and is exposed separately so boot code
 * can stage diagnostics around the most failure-prone setup work.
 *
 * @return true when the calculator-side services and RAM backing are ready.
 */
bool c64_boot_memory(void);
/**
 * Load the C64 ROM images from AppVars.
 *
 * BASIC, KERNAL and character ROM payloads are copied into their dedicated
 * backing arrays. Any machine-specific ROM patching happens as part of this
 * step so later reset code sees the final image.
 *
 * @return true when all required ROM packages were found and loaded.
 */
bool c64_boot_roms(void);
/**
 * Mark the machine ready.
 *
 * This is a small helper used by staged boot code once all reset-time work
 * has completed and the run loop may safely start.
 */
void c64_mark_ready(void);
/**
 * Run the main emulation loop.
 *
 * The loop batches CPU execution to VIC raster deadlines, advances the VIC,
 * scans keypad-driven IO, and services the CIAs and virtual drive.
 * It returns when a fatal core condition stops execution or the user quits.
 */
void c64_run(void);
/**
 * Shut the machine down and release calculator-facing resources.
 *
 * This restores the display/IO state and clears memory-side pointers so later
 * accidental access fails cleanly instead of touching stale backing storage.
 */
void c64_shutdown(void);

#if !defined(NDEBUG) || EMUDORE_CPU_HOT_PROFILE
/* Total emulated cycles across c64_run, for the debug exit speed report. */
extern unsigned long c64_total_cycles;
#endif

#ifndef NDEBUG
/* Wall-tick profile buckets: 0=cpu_run 1=vic 2=io 3=cia+drive. */
extern unsigned long c64_prof[4];
extern unsigned long c64_prof_lines;
#endif

#if !defined(NDEBUG) || EMUDORE_CPU_HOT_PROFILE
extern uint16_t c64_hot_pc[2];
extern uint16_t c64_hot_pc_votes[2];
extern uint16_t c64_hot_pc_samples[2];
extern uint8_t c64_hot_bytes[2][8];
extern uint16_t c64_hot_branch_pc[2];
extern uint8_t c64_hot_branch_bytes[2][16];
#endif

#endif /* EMUDORE_CE_C64_H */
