/*
 * emudore-CE: MOS 6526 CIA #1 (keyboard, joysticks, IRQ timers).
 * C port of cia1.h — the SDL IO pointer becomes a call into io.c.
 */
#ifndef EMUDORE_CE_CIA1_H
#define EMUDORE_CE_CIA1_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Reset CIA #1 to its power-on state.
 *
 * This clears port latches, timer control, IRQ state, and re-arms the
 * wall-clock-backed timing source that drives keyboard repeat and jiffy IRQs.
 */
void cia1_reset(void);
/**
 * Advance CIA #1 timers and deliver IRQs.
 *
 * Timer countdown is based on real elapsed wall-clock time so keyboard and
 * cursor behavior stays stable even when emulation speed varies.
 *
 * @return true in the current implementation.
 */
bool cia1_emulate(void);
/**
 * Write one CIA #1 register.
 *
 * @param r Low 4-bit register number selected by the CPU bus decode.
 * @param v Byte value to store into the emulated register.
 */
void    cia1_write_register(uint8_t r, uint8_t v);
/**
 * Read one CIA #1 register.
 *
 * @param r Low 4-bit register number selected by the CPU bus decode.
 * @return The value currently exposed on the emulated data bus.
 */
uint8_t cia1_read_register(uint8_t r);
/** Inspect a CIA register without feeding the VIC busy-poll detector. */
uint8_t cia1_peek_register(uint8_t r);

#endif /* EMUDORE_CE_CIA1_H */
