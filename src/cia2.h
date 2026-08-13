/*
 * emudore-CE: MOS 6526 CIA #2 (serial bus, VIC banking, NMI timers).
 * C port of cia2.h.
 */
#ifndef EMUDORE_CE_CIA2_H
#define EMUDORE_CE_CIA2_H

#include <stdint.h>
#include <stdbool.h>

void cia2_reset(void);
bool cia2_emulate(void);
void     cia2_write_register(uint8_t r, uint8_t v);
uint8_t  cia2_read_register(uint8_t r);
uint16_t cia2_vic_base_address(void);

#endif /* EMUDORE_CE_CIA2_H */
