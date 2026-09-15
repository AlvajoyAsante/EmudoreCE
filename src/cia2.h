/*
 * emudore-CE: MOS 6526 CIA #2 (serial bus, VIC banking, NMI timers).
 * C port of cia2.h.
 */
#ifndef EMUDORE_CE_CIA2_H
#define EMUDORE_CE_CIA2_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Reset CIA #2 to its power-on state.
 *
 * This clears the VIC-bank control port, timer latches, and NMI-related IRQ
 * state so later raster and serial-side behavior starts from known values.
 */
void cia2_reset(void);
/**
 * Advance CIA #2 timers and deliver NMIs when they expire.
 *
 * Unlike CIA #1, this chip is stepped from emulated CPU cycles because its
 * responsibilities here are tied to machine timing rather than wall-clock IO.
 *
 * @return true in the current implementation.
 */
bool cia2_emulate(void);
/**
 * Write one CIA #2 register.
 *
 * @param r Low 4-bit register number selected by the CPU bus decode.
 * @param v Byte value to store into the emulated register.
 */
void     cia2_write_register(uint8_t r, uint8_t v);
/**
 * Read one CIA #2 register.
 *
 * @param r Low 4-bit register number selected by the CPU bus decode.
 * @return The value currently exposed on the emulated data bus.
 */
uint8_t  cia2_read_register(uint8_t r);
/** Inspect a CIA register without feeding the VIC busy-poll detector. */
uint8_t  cia2_peek_register(uint8_t r);
/**
 * Decode the current VIC-II 16 KiB base address.
 *
 * The C64 inverts CIA #2 PRA bits 0-1 before they select the active VIC bank.
 *
 * @return Base address of the currently visible VIC bank.
 */
uint16_t cia2_vic_base_address(void);

#endif /* EMUDORE_CE_CIA2_H */
