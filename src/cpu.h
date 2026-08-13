/*
 * emudore-CE: MOS 6510 CPU (C port of cpu.h).
 *
 * All of the original C++ inline member functions become ordinary functions
 * that operate on a single global CpuState. Register/flag accessors become
 * plain macros so the instruction implementations stay readable.
 */
#ifndef EMUDORE_CE_CPU_H
#define EMUDORE_CE_CPU_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    /* registers */
    uint16_t pc;
    uint8_t sp, a, x, y;
    /* status flags */
    bool cf, zf, idf, dmf, bcf, of, nf;
    /* clock */
    unsigned int cycles;
} CpuState;

extern CpuState cpu;

void cpu_reset(void);
/* Execute one instruction. Returns false on an illegal opcode. */
bool cpu_emulate(void);
void cpu_irq(void);
void cpu_nmi(void);

/* flag helpers used inside the instruction implementations */
#define SET_ZF(val) (cpu.zf = ((uint8_t)(val) == 0))
#define SET_NF(val) (cpu.nf = (((uint8_t)(val) & 0x80) != 0))

#endif /* EMUDORE_CE_CPU_H */
