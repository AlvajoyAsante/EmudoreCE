/*
 * emudore-CE: MOS 6510 state and native CPU/JIT support interface.
 */
#ifndef EMUDORE_CE_CPU_H
#define EMUDORE_CE_CPU_H

#include <stdint.h>
void cpu_decimal_adc(uint8_t operand);
void cpu_decimal_sbc(uint8_t operand);
void cpu_brk_native(void);
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

/* diagnostics: last illegal opcode that stopped cpu_run (0 = none) */
extern uint8_t  cpu_illegal_op;
extern uint16_t cpu_illegal_pc;

/* TIBoyce-style waitloop skip: set by vic on raster-register busy polls,
   consumed by the native core's IO-read path (ends the line batch early) */
extern uint8_t cpu_warp;

/* ROM-JIT gate: KERNAL bytes verified against the native blocks (c64.c). */
extern uint8_t cpu_jit_kernal;

/* asm-core fallback diagnostics (cpu_core.s): total C-step bridges and the
   most recent opcode that took one. */
extern unsigned int cpu_fallbacks;
extern uint8_t cpu_fallback_op;
#ifndef NDEBUG
/* Per-opcode C fallback counts for selecting the next asm-core handler. */
extern uint16_t cpu_fallback_counts[256];
/* Nonzero when the most recent assembly startup self-test failed. */
extern uint32_t cpu_asm_selftest_mismatch;
#endif

/* Native block hit counters (KERNAL wait-key, UDTIM, Swirler, dynamic RAM). */
extern unsigned int cpu_jit_hits;
extern unsigned int cpu_jit_hits2;
extern unsigned int cpu_jit_hits3;
/* Generic DEX/BNE and LDA/STA/INY/BNE dynamic blocks, respectively. */
extern unsigned int cpu_jit_hits4;
extern unsigned int cpu_jit_hits5;
extern unsigned int cpu_jit_copy_attempts;
extern uint8_t cpu_jit_copy_stage;

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
 * Validate native bank-port read-modify-write handling against expected state.
 * Requires initialized memory; restores CPU state, scratch bytes and banking.
 *
 * @return true when the native startup checks pass.
 */
bool cpu_asm_selftest(void);
/** Queue or deliver a maskable IRQ on the emulated 6510. */
void cpu_irq(void);
/** Queue or deliver a non-maskable interrupt on the emulated 6510. */
void cpu_nmi(void);

/* flag helpers used inside the instruction implementations */
#define SET_ZF(val) (cpu.zf = ((uint8_t)(val) == 0))
#define SET_NF(val) (cpu.nf = (((uint8_t)(val) & 0x80) != 0))

#endif /* EMUDORE_CE_CPU_H */
