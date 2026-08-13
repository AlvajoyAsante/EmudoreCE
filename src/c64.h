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

bool c64_init(void);      /* allocate memory, load ROMs, reset chips */
void c64_run(void);       /* main emulator loop, returns on quit */
void c64_shutdown(void);  /* free memory, restore the OS screen */

#endif /* EMUDORE_CE_C64_H */
