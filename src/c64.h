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

void c64_init(void);    /* allocate memory, load ROMs, reset chips */
bool c64_ready(void);   /* true if c64_init succeeded */
bool c64_boot_memory(void); /* io_init + mem_init + mem_ready */
bool c64_boot_roms(void);   /* load BASIC/KERNAL/CHARGEN */
void c64_mark_ready(void);  /* set internal ready flag once resets finish */
void c64_run(void);     /* main emulator loop, returns on quit */
void c64_shutdown(void);/* free memory, restore the OS screen */

#endif /* EMUDORE_CE_C64_H */
