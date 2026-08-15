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
   /* 6510 programmer-visible registers */
    uint16_t pc;
    uint8_t sp, a, x, y;
   /* Individual processor status flags, kept split for fast C access:
      cf carry, zf zero, idf interrupt-disable, dmf decimal mode,
      bcf break-command shadow, of overflow, nf negative/sign. */
    bool cf, zf, idf, dmf, bcf, of, nf;
   /* Total elapsed CPU cycles modulo the host unsigned int width. */
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

/** Reset registers, flags, vectors and runtime diagnostics. */
void cpu_reset(void);
/**
 * Execute CPU work until the requested cycle target is reached.
 *
 * The comparison is wrap-safe so callers may pass future deadlines even when
 * the cycle counter is near overflow.
 *
 * @param until_cycles Inclusive cycle target for the current batch.
 * @return false on an illegal opcode or trap failure; true otherwise.
 */
bool cpu_run(unsigned int until_cycles);
/**
 * Execute one opcode with the portable C core.
 *
 * @return false on an illegal opcode or trap failure; true otherwise.
 */
bool cpu_step_slow(void);
/**
 * Self-test the hybrid eZ80 assembly core against the C core.
 *
 * @return true when both cores produced identical machine state and the asm
 *         core is now enabled for later cpu_run() calls.
 */
bool cpu_asm_selftest(void);
/** Force all future execution through the C core only. */
void cpu_asm_disable(void);
/**
 * Run one CPU batch with the pure C core only.
 *
 * @param until_cycles Inclusive cycle target for the batch.
 * @return false on an illegal opcode or trap failure; true otherwise.
 */
bool cpu_run_c(unsigned int until_cycles);
/**
 * Compare C-core and asm-core execution during KERNAL boot.
 *
 * @param machine_reset Callback that restores RAM and chips to a deterministic
 *        pre-run state before each comparison attempt.
 * @param report Callback receiving the first divergent budget and both CPU
 *        states, or a zero budget when no divergence was found.
 */
void cpu_asm_boot_probe(void (*machine_reset)(void),
                        void (*report)(unsigned int budget,
                                       const CpuState *c_state,
                                       const CpuState *a_state));
/** Queue or deliver a maskable IRQ on the emulated 6510. */
void cpu_irq(void);
/** Queue or deliver a non-maskable interrupt on the emulated 6510. */
void cpu_nmi(void);

/* flag helpers used inside the instruction implementations */
#define SET_ZF(val) (cpu.zf = ((uint8_t)(val) == 0))
#define SET_NF(val) (cpu.nf = (((uint8_t)(val) & 0x80) != 0))

#endif /* EMUDORE_CE_CPU_H */
