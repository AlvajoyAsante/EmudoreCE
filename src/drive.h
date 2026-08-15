/*
 * emudore-CE: virtual 1541 "fast load" drive.
 *
 * Serves KERNAL LOAD requests straight out of the DSK AppVar package
 * (a D64's PRG files packed by tools/d64_to_appvar.py). The KERNAL image
 * gets a trap opcode at $F4A5 (the default ILOAD vector target); cpu.c
 * dispatches that opcode to drive_trap_load().
 */
#ifndef EMUDORE_CE_DRIVE_H
#define EMUDORE_CE_DRIVE_H

#include <stdbool.h>
#include <stdint.h>

/* 6502 JAM opcodes used as traps; never produced by real code. */
#define DRIVE_TRAP_OPCODE  0x02 /* KERNAL LOAD ($F4A5)                  */
#define DRIVE_TRAP2_OPCODE 0x12 /* resident game fastloader (signature) */

/* Validates/maps the DSK AppVar package. false = no disk present (fine,
   the machine just boots to BASIC). */
bool drive_init(void);

/* Directory helpers for the startup menu. */
uint8_t drive_file_count(void);
int drive_file_name(uint8_t index, char *buffer, uint8_t bufferSize);
void drive_select_file(uint8_t index);

/* Plants the trap opcode at $F4A5 in the loaded KERNAL image. Call after
   c64_boot_roms() and only when drive_init() succeeded. */
void drive_patch_kernal(void);

/* Executes a trapped KERNAL LOAD. Returns false if the trap opcode was
   fetched anywhere else (treat as an illegal instruction). */
bool drive_trap_load(void);

/* Executes a trapped resident fastloader call (see drive.c signature). */
bool drive_trap_fastload(void);

/* Auto-types LOAD"*",8,1 into the keyboard buffer once BASIC reaches the
   READY prompt. Cheap no-op after it has fired; call from the run loop. */
void drive_tick(void);

/* Diagnostics: every trapped LOAD is recorded here (no dbg_printf allowed
   inside the CPU loop); main() prints the ring after c64_run returns. */
typedef struct
{
    uint16_t caller;   /* return address of the JSR $FFD5           */
    uint16_t addr;     /* first byte written                        */
    uint16_t end;      /* one past the last byte written            */
    uint8_t entry;     /* matched directory index, 0xFF = not found */
    uint8_t sa;        /* secondary address ($B9)                   */
    uint8_t len;       /* filename length                           */
    uint8_t name[3];   /* first filename bytes (PETSCII)            */
} DriveTrace;

#define DRIVE_TRACE_MAX 8
extern DriveTrace drive_trace[DRIVE_TRACE_MAX];
extern uint8_t drive_trace_count; /* total traps (ring keeps the first 8) */

#endif /* EMUDORE_CE_DRIVE_H */
