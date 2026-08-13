/*
 * emudore-CE: MOS 6526 CIA #1 (keyboard, joysticks, IRQ timers).
 * C port of cia1.h — the SDL IO pointer becomes a call into io.c.
 */
#ifndef EMUDORE_CE_CIA1_H
#define EMUDORE_CE_CIA1_H

#include <stdint.h>
#include <stdbool.h>

void cia1_reset(void);
bool cia1_emulate(void);
void    cia1_write_register(uint8_t r, uint8_t v);
uint8_t cia1_read_register(uint8_t r);

#endif /* EMUDORE_CE_CIA1_H */
