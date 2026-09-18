/*
 * emudore-CE: MOS 6510 state and support for the native eZ80 core and JIT.
 * Opcode dispatch lives in cpu_core.s; no C interpreter is retained here.
 */
#include <stddef.h>
#include <string.h>
#include "cpu.h"
#include "memory.h"
#include "jit.h"

CpuState cpu;
#ifndef NDEBUG
uint32_t cpu_asm_selftest_mismatch;
#endif

/* cpu_core.s hard-codes these offsets; keep them pinned. */
_Static_assert(offsetof(CpuState, pc) == 0, "pc offset");
_Static_assert(offsetof(CpuState, sp) == 2, "sp offset");
_Static_assert(offsetof(CpuState, a) == 3, "a offset");
_Static_assert(offsetof(CpuState, x) == 4, "x offset");
_Static_assert(offsetof(CpuState, y) == 5, "y offset");
_Static_assert(offsetof(CpuState, cf) == 6, "cf offset");
_Static_assert(offsetof(CpuState, zf) == 7, "zf offset");
_Static_assert(offsetof(CpuState, idf) == 8, "idf offset");
_Static_assert(offsetof(CpuState, dmf) == 9, "dmf offset");
_Static_assert(offsetof(CpuState, bcf) == 10, "bcf offset");
_Static_assert(offsetof(CpuState, of) == 11, "of offset");
_Static_assert(offsetof(CpuState, nf) == 12, "nf offset");
_Static_assert(offsetof(CpuState, cycles) == 13, "cycles offset");

/* Native eZ80 core (cpu_core.s), also exercised by the startup self-test. */
extern bool cpu_run_asm(unsigned int until_cycles);

/* Set when cpu_run stops on an illegal opcode; main() reports it after
   the run loop (dbg_printf inside the hot path clobbers the IX frame). */
uint8_t  cpu_illegal_op;
uint16_t cpu_illegal_pc;

/* BRK diagnostics: a runaway program hitting BRK warm-starts BASIC, which
   looks like "nothing happened". main() reports these after the run loop. */
uint16_t cpu_last_brk_pc;
unsigned int cpu_brk_count;
uint16_t cpu_first_brk_pc;
uint8_t  cpu_first_brk_sp;
uint16_t cpu_first_brk_ret; /* return address on the 6502 stack at 1st BRK */

/* Waitloop skip: consumed by the native core after raster busy-polls. */
uint8_t cpu_warp;

/* ROM-JIT gate: nonzero when the loaded KERNAL matched the byte patterns
   the native blocks in cpu_core.s were written against (set by c64.c). */
uint8_t cpu_jit_kernal;
bool cpu_jit_reentry;

/* Stack (page 1) always lives in mem_low: skip the page table and IO checks. */
__attribute__((__always_inline__))
static inline void push(uint8_t v)
{
    mem_low[ADDR_STACK + cpu.sp] = v;
    cpu.sp--;
}

static void tick(uint8_t v)
{
    cpu.cycles += v;
}

static uint8_t cpu_flags(void)
{
    uint8_t v = 0;
    v |= cpu.cf  << 0;
    v |= cpu.zf  << 1;
    v |= cpu.idf << 2;
    v |= cpu.dmf << 3;
    v |= 1 << 4; /* brk/php push bcf set */
    v |= 1 << 5; /* unused, always set */
    v |= cpu.of  << 6;
    v |= cpu.nf  << 7;
    return v;
}

/* BRK support called by the native core; retain post-run diagnostics. */
void cpu_brk_native(void)
{
    cpu_last_brk_pc = (uint16_t)(cpu.pc - 1);
    if (!cpu_brk_count)
    {
        /* first derail: the topmost stack word points at the code that
           JSR'd/IRQ'd into the weeds */
        cpu_first_brk_pc = cpu_last_brk_pc;
        cpu_first_brk_sp = cpu.sp;
        cpu_first_brk_ret = (uint16_t)(mem_low[ADDR_STACK + (uint8_t)(cpu.sp + 1)] |
                                       (mem_low[ADDR_STACK + (uint8_t)(cpu.sp + 2)] << 8));
    }
    ++cpu_brk_count;
    push(((cpu.pc + 1) >> 8) & 0xff);
    push(((cpu.pc + 1) & 0xff));
    push(cpu_flags());
    cpu.pc = mem_read_word(ADDR_IRQ_VEC);
    cpu.idf = true;
    cpu.bcf = true;
    tick(7);
}

/* ---- public API -------------------------------------------------------- */

void cpu_reset(void)
{
#ifndef NDEBUG
    memset(cpu_fallback_counts, 0, sizeof(cpu_fallback_counts));
#endif
    cpu.a = cpu.x = cpu.y = cpu.sp = 0;
    cpu.cf = cpu.zf = cpu.idf = cpu.dmf = cpu.bcf = cpu.of = cpu.nf = false;
    cpu.pc = mem_read_word(ADDR_RESET_VEC);
    cpu.cycles = 6;
}

void cpu_irq(void)
{
    if (!cpu.idf)
    {
        push((cpu.pc >> 8) & 0xff);
        push(cpu.pc & 0xff);
        push(cpu_flags() & 0xef);
        cpu.pc = mem_read_word(ADDR_IRQ_VEC);
        cpu.idf = true;
        tick(7);
    }
}

void cpu_nmi(void)
{
    push((cpu.pc >> 8) & 0xff);
    push(cpu.pc & 0xff);
    push(cpu_flags() & 0xef);
    cpu.pc = mem_read_word(ADDR_NMI_VEC);
    tick(7);
}

/* Execute JIT blocks or the native assembly core. */
bool cpu_run(unsigned int until_cycles)
{
    unsigned int native_hits = jit_native_hits;
    while ((int)(cpu.cycles - until_cycles) < 0)
    {
        if (!jit_run_batch(until_cycles))
        {
            uint16_t missed_pc = cpu.pc;
            bool result;
            cpu_jit_reentry = jit_backend_ready() && jit_ready_block_count != 0;
            result = cpu_run_asm(until_cycles);
            cpu_jit_reentry = false;
            if (result && jit_native_hits == native_hits)
                jit_observe_pc(missed_pc);
            return result;
        }
    }
    return true;
}

/* Validate native bank-port RMW handling against fixed expected state. */
#if defined(__clang__)
__attribute__((__minsize__))
#endif
bool cpu_asm_selftest(void)
{
    static const uint8_t program[] = {0xe6, 0x01, 0xc6, 0x01};
    CpuState saved = cpu;
    uint8_t saved_program[sizeof(program)];
    uint8_t saved_port = mem_low[1];
    const uint8_t *basic_page;
    bool ok;

    memcpy(saved_program, mem_low + 0x400, sizeof(program));
    memcpy(mem_low + 0x400, program, sizeof(program));
    mem_setup_banks(7);
    basic_page = mem_rd_pg[0xa0].ptr;
    memset(&cpu, 0, sizeof(cpu));
    cpu.pc = 0x400;
    cpu.sp = 0xfd;
    cpu.a = 0xa5;
    cpu.x = 0x5a;
    cpu.y = 0x81;
    cpu.cf = cpu.idf = cpu.of = true;
    ok = cpu_run_asm(5) && cpu.pc == 0x402 && cpu.cycles == 5 &&
         mem_low[1] == 8 && mem_rd_pg[0xa0].ptr == mem_ram &&
         mem_rd_pg[0xe0].ptr == mem_ram && !cpu.zf && !cpu.nf;
    if (ok)
        ok = cpu_run_asm(10) && cpu.pc == 0x404 && cpu.cycles == 10 &&
             mem_low[1] == 7 && mem_rd_pg[0xa0].ptr == basic_page &&
             cpu.sp == 0xfd && cpu.a == 0xa5 && cpu.x == 0x5a && cpu.y == 0x81 &&
             cpu.cf && cpu.idf && cpu.of && !cpu.dmf && !cpu.zf && !cpu.nf;
#ifndef NDEBUG
    cpu_asm_selftest_mismatch = ok ? 0 : 1;
#endif
    memcpy(mem_low + 0x400, saved_program, sizeof(program));
    mem_setup_banks(saved_port);
    cpu = saved;
    return ok;
}
