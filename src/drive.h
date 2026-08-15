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

/**
 * Initialize the virtual drive from the DSK AppVar package.
 *
 * This validates the packaged D64-derived directory and maps chunk 0 so the
 * loader can serve PRG data directly from archived storage.
 *
 * @return true when a valid disk package is present; false when no disk is
 *         available, which is still a normal boot-to-BASIC case.
 */
bool drive_init(void);

/**
 * Return how many PRG entries are available in the current disk package.
 *
 * @return Directory entry count, or 0 when no disk package was loaded.
 */
uint8_t drive_file_count(void);
/**
 * Copy one menu-safe filename out of the disk directory.
 *
 * @param index Zero-based PRG directory index.
 * @param buffer Destination text buffer.
 * @param bufferSize Size of buffer in bytes, including the terminator.
 * @return Non-zero on success, zero when the index is invalid or the buffer is
 *         unusable.
 */
int drive_file_name(uint8_t index, char *buffer, uint8_t bufferSize);
/**
 * Select which directory entry later autoload or trap requests should use.
 *
 * @param index Zero-based PRG directory index.
 */
void drive_select_file(uint8_t index);

/**
 * Patch the loaded KERNAL image to trap LOAD requests.
 *
 * Call this only after c64_boot_roms() and only when drive_init() succeeded.
 */
void drive_patch_kernal(void);

/**
 * Service a trapped KERNAL LOAD.
 *
 * @return true when the trap was recognized and handled; false when the same
 *         opcode appeared somewhere else and should be treated as illegal.
 */
bool drive_trap_load(void);

/**
 * Service a trapped game-specific resident fastloader call.
 *
 * @return true when the trap matched the supported signature; false otherwise.
 */
bool drive_trap_fastload(void);

/**
 * Advance drive-side background work.
 *
 * This mainly watches for the BASIC READY prompt and injects the staged
 * LOAD/RUN command sequence once the machine is ready to accept it.
 */
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
