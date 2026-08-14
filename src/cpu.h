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

/* TIBoyce-style waitloop skip: set by vic on raster-register busy polls,
   consumed by both CPU cores' IO-read paths (ends the line batch early) */
extern uint8_t cpu_warp;

/* diagnostics: BRK executions (runaway code warm-starting BASIC) */
extern uint16_t cpu_last_brk_pc;
extern unsigned int cpu_brk_count;
extern uint16_t cpu_first_brk_pc;
extern uint8_t  cpu_first_brk_sp;
extern uint16_t cpu_first_brk_ret;

void cpu_reset(void);
/* Execute instructions until cpu.cycles reaches until_cycles (wrap-safe).
   Returns false on an illegal opcode. */
bool cpu_run(unsigned int until_cycles);
/* One C-core instruction step; the asm core's fallback bridge. */
bool cpu_step_slow(void);
/* Validate the eZ80 hybrid core against the C core (needs mem_init done);
   enables the asm core only when both produce identical results. */
bool cpu_asm_selftest(void);
/* Force the C core (diagnostic override, e.g. MODE held at boot). */
void cpu_asm_disable(void);
/* Pure C batch (divergence probe). */
bool cpu_run_c(unsigned int until_cycles);
/* Dual-core KERNAL-boot divergence probe (debug builds). */
void cpu_asm_boot_probe(void (*machine_reset)(void),
                        void (*report)(unsigned int budget,
                                       const CpuState *c_state,
                                       const CpuState *a_state));
void cpu_irq(void);
void cpu_nmi(void);

/* flag helpers used inside the instruction implementations */
#define SET_ZF(val) (cpu.zf = ((uint8_t)(val) == 0))
#define SET_NF(val) (cpu.nf = (((uint8_t)(val) & 0x80) != 0))

#endif /* EMUDORE_CE_CPU_H */
