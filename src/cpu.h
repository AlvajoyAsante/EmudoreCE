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

/* diagnostics: last illegal opcode that stopped cpu_emulate (0 = none) */
extern uint8_t  cpu_illegal_op;
extern uint16_t cpu_illegal_pc;

void cpu_reset(void);
/* Execute instructions until cpu.cycles reaches until_cycles (wrap-safe).
   Returns false on an illegal opcode. */
bool cpu_run(unsigned int until_cycles);
void cpu_irq(void);
void cpu_nmi(void);

/* flag helpers used inside the instruction implementations */
#define SET_ZF(val) (cpu.zf = ((uint8_t)(val) == 0))
#define SET_NF(val) (cpu.nf = (((uint8_t)(val) & 0x80) != 0))

#endif /* EMUDORE_CE_CPU_H */
