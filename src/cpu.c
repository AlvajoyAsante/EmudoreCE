/*
 * emudore-CE: MOS 6510 CPU (C port of cpu.cpp).
 *
 * The C++ class becomes a single global CpuState. Register/flag accessors
 * (a(), cf(), ...) become the macros below so the instruction bodies read the
 * same as the original. Debug-dump helpers (dump_regs) are removed.
 *
 * Current limitations (carried over from the original):
 *  - Illegal instructions are not implemented
 *  - Page-crossing excess cycles are not counted
 *  - Some architectural bugs are not emulated
 */
#include <stddef.h>
#include <string.h>
#include "cpu.h"
#include "memory.h"
#include "vic.h"
#include "util.h"
#include "drive.h"

CpuState cpu;

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

/* eZ80 hybrid core (cpu_core.s); used only after the self-test passes. */
extern bool cpu_run_asm(unsigned int until_cycles);
static bool cpu_asm_ok;

void cpu_asm_disable(void)
{
    cpu_asm_ok = false;
}

/* Set when cpu_emulate stops on an illegal opcode; main() reports it after
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

/* register/flag accessors as macros (read) and assignment helpers (write) */
#define A()  (cpu.a)
#define X()  (cpu.x)
#define Y()  (cpu.y)
#define SP() (cpu.sp)
#define PC() (cpu.pc)

/* ---- helpers ---------------------------------------------------------- */
/* always_inline: -Oz outlines these otherwise, adding 1-3 calls per emulated
   instruction in the hottest loop of the program. */

/* Waitloop skip (TIBoyce technique): raster busy-polls and jmp-to-self spins
   only exit via an external event (raster advance / IRQ), so the rest of the
   line batch is dead time - warp the cycle counter to the batch end. */
uint8_t cpu_warp;

/* ROM-JIT gate: nonzero when the loaded KERNAL matched the byte patterns
   the native blocks in cpu_core.s were written against (set by c64.c). */
uint8_t cpu_jit_kernal;
static unsigned int run_until;

__attribute__((__always_inline__))
static inline void warp_to_batch_end(void)
{
    /* until - 1: the instruction's own tick() lands the counter past the
       batch end, matching the asm core's "countdown = 0" warp exactly */
    unsigned int target = run_until - 1;
    if ((int)(target - cpu.cycles) > 0)
        cpu.cycles = target;
}

__attribute__((__always_inline__))
static inline uint8_t load_byte(uint16_t addr)
{
    uint8_t v = mem_read_byte(addr);
    if (cpu_warp)
    {
        cpu_warp = 0;
        warp_to_batch_end();
    }
    return v;
}

/* Stack (page 1) and zero page always live in mem_low: skip the page table
   and IO checks entirely. */
__attribute__((__always_inline__))
static inline void push(uint8_t v)
{
    mem_low[ADDR_STACK + cpu.sp] = v;
    cpu.sp--;
}

__attribute__((__always_inline__))
static inline uint8_t pop(void)
{
    ++cpu.sp;
    return mem_low[ADDR_STACK + cpu.sp];
}

/* Code fetch: the PC never points at an IO page in real code, so the NULL
   (IO) table check is skipped. */
__attribute__((__always_inline__))
static inline uint8_t fetch_op(void)
{
    U16B a;
    a.w = cpu.pc++;
    return mem_rd_pg[a.b.hi].ptr[a.w];
}

__attribute__((__always_inline__))
static inline uint16_t fetch_opw(void)
{
    U16B v;
    v.b.lo = fetch_op();
    v.b.hi = fetch_op();
    return v.w;
}

/* Zero-page operand reads (never IO/ROM). */
__attribute__((__always_inline__))
static inline uint8_t zp_read(void)
{
    return mem_low[fetch_op()];
}

__attribute__((__always_inline__))
static inline uint8_t zpx_read(void)
{
    return mem_low[(uint8_t)(fetch_op() + cpu.x)];
}

__attribute__((__always_inline__))
static inline uint8_t zpy_read(void)
{
    return mem_low[(uint8_t)(fetch_op() + cpu.y)];
}

/* 16-bit pointer fetch from zero page (wraps at $FF). */
__attribute__((__always_inline__))
static inline uint16_t zp_word(uint8_t zp)
{
    U16B r;
    r.b.lo = mem_low[zp];
    r.b.hi = mem_low[(uint8_t)(zp + 1)];
    return r.w;
}

/* Zero-page store: direct, but $0001 still switches banks. */
__attribute__((__always_inline__))
static inline void zp_write(uint8_t zp, uint8_t v)
{
    mem_low[zp] = v;
    if (zp == ADDR_MEM_LAYOUT)
        mem_setup_banks(v);
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

static void set_flags(uint8_t v)
{
    cpu.cf  = ISSET_BIT(v, 0);
    cpu.zf  = ISSET_BIT(v, 1);
    cpu.idf = ISSET_BIT(v, 2);
    cpu.dmf = ISSET_BIT(v, 3);
    cpu.of  = ISSET_BIT(v, 6);
    cpu.nf  = ISSET_BIT(v, 7);
}

/* ---- addressing modes ------------------------------------------------- */
/* always_inline: one of these runs per memory-operand instruction. */

#define AM_INLINE __attribute__((__always_inline__)) static inline

AM_INLINE uint16_t addr_zero(void)  { return fetch_op(); }
AM_INLINE uint16_t addr_zerox(void) { return (fetch_op() + cpu.x) & 0xff; }
AM_INLINE uint16_t addr_abs(void)   { return fetch_opw(); }
AM_INLINE uint16_t addr_absx(void)  { return fetch_opw() + cpu.x; }
AM_INLINE uint16_t addr_absy(void)  { return fetch_opw() + cpu.y; }
AM_INLINE uint16_t addr_indx(void)  { return zp_word((uint8_t)(fetch_op() + cpu.x)); }
AM_INLINE uint16_t addr_indy(void)  { return zp_word(fetch_op()) + cpu.y; }

/* ---- data handling / memory ------------------------------------------- */

static void sta(uint16_t addr, uint8_t cycles) { mem_write_byte(addr, cpu.a); tick(cycles); }
static void stx(uint16_t addr, uint8_t cycles) { mem_write_byte(addr, cpu.x); tick(cycles); }
static void sty(uint16_t addr, uint8_t cycles) { mem_write_byte(addr, cpu.y); tick(cycles); }

static void lda(uint8_t v, uint8_t cycles) { cpu.a = v; SET_ZF(cpu.a); SET_NF(cpu.a); tick(cycles); }
static void ldx(uint8_t v, uint8_t cycles) { cpu.x = v; SET_ZF(cpu.x); SET_NF(cpu.x); tick(cycles); }
static void ldy(uint8_t v, uint8_t cycles) { cpu.y = v; SET_ZF(cpu.y); SET_NF(cpu.y); tick(cycles); }

static void txs(void) { cpu.sp = cpu.x; tick(2); }
static void tsx(void) { cpu.x = cpu.sp; SET_ZF(cpu.x); SET_NF(cpu.x); tick(2); }
static void tax(void) { cpu.x = cpu.a; SET_ZF(cpu.x); SET_NF(cpu.x); tick(2); }
static void txa(void) { cpu.a = cpu.x; SET_ZF(cpu.a); SET_NF(cpu.a); tick(2); }
static void tay(void) { cpu.y = cpu.a; SET_ZF(cpu.y); SET_NF(cpu.y); tick(2); }
static void tya(void) { cpu.a = cpu.y; SET_ZF(cpu.a); SET_NF(cpu.a); tick(2); }
static void pha(void) { push(cpu.a); tick(3); }
static void pla(void) { cpu.a = pop(); SET_ZF(cpu.a); SET_NF(cpu.a); tick(4); }

/* ---- logic ------------------------------------------------------------ */

static void ora(uint8_t v, uint8_t cycles)  { cpu.a |= v; SET_ZF(cpu.a); SET_NF(cpu.a); tick(cycles); }
static void _and(uint8_t v, uint8_t cycles) { cpu.a &= v; SET_ZF(cpu.a); SET_NF(cpu.a); tick(cycles); }
static void eor(uint8_t v, uint8_t cycles)  { cpu.a ^= v; SET_ZF(cpu.a); SET_NF(cpu.a); tick(cycles); }

static void bit(uint16_t addr, uint8_t cycles)
{
    uint8_t t = load_byte(addr);
    cpu.of = (t & 0x40) != 0;
    SET_NF(t);
    SET_ZF(t & cpu.a);
    tick(cycles);
}

static uint8_t rol(uint8_t v)
{
    uint16_t t = (v << 1) | (uint8_t)cpu.cf;
    cpu.cf = (t & 0x100) != 0;
    SET_ZF(t);
    SET_NF(t);
    return (uint8_t)t;
}

static uint8_t ror(uint8_t v)
{
    uint16_t t = (v >> 1) | (uint8_t)(cpu.cf << 7);
    cpu.cf = (v & 0x1) != 0;
    SET_ZF(t);
    SET_NF(t);
    return (uint8_t)t;
}

static uint8_t lsr(uint8_t v)
{
    uint8_t t = v >> 1;
    cpu.cf = (v & 0x1) != 0;
    SET_ZF(t);
    SET_NF(t);
    return t;
}

static uint8_t asl(uint8_t v)
{
    uint8_t t = (v << 1) & 0xff;
    cpu.cf = (v & 0x80) != 0;
    SET_ZF(t);
    SET_NF(t);
    return t;
}

static void rol_a(void) { cpu.a = rol(cpu.a); tick(2); }
static void ror_a(void) { cpu.a = ror(cpu.a); tick(2); }
static void lsr_a(void) { cpu.a = lsr(cpu.a); tick(2); }
static void asl_a(void) { cpu.a = asl(cpu.a); tick(2); }

/* Read-modify-write instructions perform a read then two writes (original
   byte, then modified byte) because some code uses that side effect, e.g.
   ASL $d019 acknowledges a VIC interrupt on the first write. */
static void rol_mem(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    mem_write_byte(addr, rol(v));
    tick(cycles);
}
static void ror_mem(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    mem_write_byte(addr, ror(v));
    tick(cycles);
}
static void lsr_mem(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    mem_write_byte(addr, lsr(v));
    tick(cycles);
}
static void asl_mem(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    mem_write_byte(addr, asl(v));
    tick(cycles);
}

/* ---- arithmetic ------------------------------------------------------- */

static void inc(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    v++;
    mem_write_byte(addr, v);
    SET_ZF(v);
    SET_NF(v);
    tick(cycles);
}
static void dec(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    v--;
    mem_write_byte(addr, v);
    SET_ZF(v);
    SET_NF(v);
    tick(cycles);
}

static void inx(void) { cpu.x += 1; SET_ZF(cpu.x); SET_NF(cpu.x); tick(2); }
static void iny(void) { cpu.y += 1; SET_ZF(cpu.y); SET_NF(cpu.y); tick(2); }
static void dex(void) { cpu.x -= 1; SET_ZF(cpu.x); SET_NF(cpu.x); tick(2); }
static void dey(void) { cpu.y -= 1; SET_ZF(cpu.y); SET_NF(cpu.y); tick(2); }

static void adc(uint8_t v, uint8_t cycles)
{
    uint16_t t;
    if (cpu.dmf)
    {
        t = (cpu.a & 0xf) + (v & 0xf) + (cpu.cf ? 1 : 0);
        if (t > 0x09) t += 0x6;
        t += (cpu.a & 0xf0) + (v & 0xf0);
        if ((t & 0x1f0) > 0x90) t += 0x60;
    }
    else
    {
        t = cpu.a + v + (cpu.cf ? 1 : 0);
    }
    cpu.cf = (t > 0xff);
    t &= 0xff;
    cpu.of = !((cpu.a ^ v) & 0x80) && ((cpu.a ^ t) & 0x80);
    SET_ZF(t);
    SET_NF(t);
    cpu.a = (uint8_t)t;
    tick(cycles);
}

static void sbc(uint8_t v, uint8_t cycles)
{
    uint16_t t;
    if (cpu.dmf)
    {
        t = (cpu.a & 0xf) - (v & 0xf) - (cpu.cf ? 0 : 1);
        if ((t & 0x10) != 0)
            t = ((t - 0x6) & 0xf) | ((cpu.a & 0xf0) - (v & 0xf0) - 0x10);
        else
            t = (t & 0xf) | ((cpu.a & 0xf0) - (v & 0xf0));
        if ((t & 0x100) != 0)
            t -= 0x60;
    }
    else
    {
        t = cpu.a - v - (cpu.cf ? 0 : 1);
    }
    cpu.cf = (t < 0x100);
    t &= 0xff;
    cpu.of = ((cpu.a ^ t) & 0x80) && ((cpu.a ^ v) & 0x80);
    SET_ZF(t);
    SET_NF(t);
    cpu.a = (uint8_t)t;
    tick(cycles);
}

/* ---- flag access ------------------------------------------------------ */

static void sei(void) { cpu.idf = true;  tick(2); }
static void cli(void) { cpu.idf = false; tick(2); }
static void sec(void) { cpu.cf  = true;  tick(2); }
static void clc(void) { cpu.cf  = false; tick(2); }
static void sed(void) { cpu.dmf = true;  tick(2); }
static void cld(void) { cpu.dmf = false; tick(2); }
static void clv(void) { cpu.of  = false; tick(2); }

static void php(void) { push(cpu_flags()); tick(3); }
static void plp(void) { set_flags(pop()); tick(4); }

/* ---- control flow ----------------------------------------------------- */

static void cmp(uint8_t v, uint8_t cycles)
{
    uint16_t t = cpu.a - v;
    cpu.cf = (t < 0x100);
    t &= 0xff;
    SET_ZF(t);
    SET_NF(t);
    tick(cycles);
}
static void cpx(uint8_t v, uint8_t cycles)
{
    uint16_t t = cpu.x - v;
    cpu.cf = (t < 0x100);
    t &= 0xff;
    SET_ZF(t);
    SET_NF(t);
    tick(cycles);
}
static void cpy(uint8_t v, uint8_t cycles)
{
    uint16_t t = cpu.y - v;
    cpu.cf = (t < 0x100);
    t &= 0xff;
    SET_ZF(t);
    SET_NF(t);
    tick(cycles);
}

/* ---- undocumented opcodes (stable subset, used by real games) ---------- */

static void lax(uint8_t v, uint8_t cycles)
{
    cpu.a = cpu.x = v;
    SET_ZF(v);
    SET_NF(v);
    tick(cycles);
}
static void sax(uint16_t addr, uint8_t cycles)
{
    mem_write_byte(addr, cpu.a & cpu.x);
    tick(cycles);
}
/* RMW combos keep the documented double-write behaviour (IO side effects) */
static void slo(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    v = asl(v);
    mem_write_byte(addr, v);
    ora(v, cycles);
}
static void rla(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    v = rol(v);
    mem_write_byte(addr, v);
    _and(v, cycles);
}
static void sre(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    v = lsr(v);
    mem_write_byte(addr, v);
    eor(v, cycles);
}
static void rra(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    v = ror(v);
    mem_write_byte(addr, v);
    adc(v, cycles);
}
static void dcp(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    v--;
    mem_write_byte(addr, v);
    cmp(v, cycles);
}
static void isc(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    v++;
    mem_write_byte(addr, v);
    sbc(v, cycles);
}
static void anc(uint8_t v)
{
    _and(v, 2);
    cpu.cf = cpu.nf;
}
static void alr(uint8_t v)
{
    cpu.a = lsr(cpu.a & v);
    tick(2);
}
static void arr(uint8_t v)
{
    cpu.a = (uint8_t)(((cpu.a & v) >> 1) | ((uint8_t)cpu.cf << 7));
    SET_ZF(cpu.a);
    SET_NF(cpu.a);
    cpu.cf = (cpu.a & 0x40) != 0;
    cpu.of = (((cpu.a >> 6) ^ (cpu.a >> 5)) & 1) != 0;
    tick(2);
}
static void sbx(uint8_t v)
{
    uint8_t t = cpu.a & cpu.x;
    cpu.cf = t >= v;
    cpu.x = (uint8_t)(t - v);
    SET_ZF(cpu.x);
    SET_NF(cpu.x);
    tick(2);
}
/* multi-byte NOPs still consume their operand */
static void nop_arg(uint16_t addr, uint8_t cycles)
{
    (void)addr;
    tick(cycles);
}

/* JSR pushes the address of its own last byte, not the next instruction. */
static void jsr(void)
{
    uint16_t addr = addr_abs();
    push(((cpu.pc - 1) >> 8) & 0xff);
    push(((cpu.pc - 1) & 0xff));
    cpu.pc = addr;
    tick(6);
}

static void jmp(void)
{
    uint16_t t = addr_abs();
    if (t == (uint16_t)(cpu.pc - 3))
        warp_to_batch_end(); /* jmp-to-self: only an IRQ exits, skip ahead */
    cpu.pc = t;
    tick(3);
}
static void jmp_ind(void) { cpu.pc = mem_read_word(addr_abs()); tick(3); }
static void rts(void)     { cpu.pc = (uint16_t)(pop() + (pop() << 8)) + 1; tick(6); }

/* Branches cost 2 cycles when not taken, +1 when taken, and +1 more when
   the destination crosses a page. Raster handlers depend on this timing. */
__attribute__((__always_inline__))
static inline void branch(bool condition)
{
    U16B old_pc, new_pc;
    int8_t offset = (int8_t)fetch_op();

    tick(2);
    if (!condition)
        return;

    old_pc.w = cpu.pc;
    cpu.pc = (uint16_t)(cpu.pc + offset);
    new_pc.w = cpu.pc;
    tick((uint8_t)(old_pc.b.hi == new_pc.b.hi ? 1 : 2));
}

static void bne(void) { branch(!cpu.zf); }
static void beq(void) { branch(cpu.zf); }
static void bcs(void) { branch(cpu.cf); }
static void bcc(void) { branch(!cpu.cf); }
static void bpl(void) { branch(!cpu.nf); }
static void bmi(void) { branch(cpu.nf); }
static void bvc(void) { branch(!cpu.of); }
static void bvs(void) { branch(cpu.of); }

/* ---- misc -------------------------------------------------------------- */

static void nop(void) { tick(2); }

static void brk(void)
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

static void rti(void)
{
    set_flags(pop());
    cpu.pc = pop() + (pop() << 8);
    tick(7);
}

/* ---- public API -------------------------------------------------------- */

void cpu_reset(void)
{
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

__attribute__((__always_inline__))
static inline bool cpu_step(void)
{
    uint8_t insn = fetch_op();
    bool retval = true;

    switch (insn)
    {
    case 0x00: brk(); break;
    case 0x01: ora(load_byte(addr_indx()), 6); break;
    case 0x05: ora(zp_read(), 3); break;
    case 0x06: asl_mem(addr_zero(), 5); break;
    case 0x08: php(); break;
    case 0x09: ora(fetch_op(), 2); break;
    case 0x0A: asl_a(); break;
    case 0x0D: ora(load_byte(addr_abs()), 4); break;
    case 0x0E: asl_mem(addr_abs(), 6); break;
    case 0x10: bpl(); break;
    case 0x11: ora(load_byte(addr_indy()), 5); break;
    case 0x15: ora(zpx_read(), 4); break;
    case 0x16: asl_mem(addr_zerox(), 6); break;
    case 0x18: clc(); break;
    case 0x19: ora(load_byte(addr_absy()), 4); break;
    case 0x1D: ora(load_byte(addr_absx()), 4); break;
    case 0x1E: asl_mem(addr_absx(), 7); break;
    case 0x20: jsr(); break;
    case 0x21: _and(load_byte(addr_indx()), 6); break;
    case 0x24: bit(addr_zero(), 3); break;
    case 0x25: _and(zp_read(), 3); break;
    case 0x26: rol_mem(addr_zero(), 5); break;
    case 0x28: plp(); break;
    case 0x29: _and(fetch_op(), 2); break;
    case 0x2A: rol_a(); break;
    case 0x2C: bit(addr_abs(), 4); break;
    case 0x2D: _and(load_byte(addr_abs()), 4); break;
    case 0x2E: rol_mem(addr_abs(), 6); break;
    case 0x30: bmi(); break;
    case 0x31: _and(load_byte(addr_indy()), 5); break;
    case 0x35: _and(zpx_read(), 4); break;
    case 0x36: rol_mem(addr_zerox(), 6); break;
    case 0x38: sec(); break;
    case 0x39: _and(load_byte(addr_absy()), 4); break;
    case 0x3D: _and(load_byte(addr_absx()), 4); break;
    case 0x3E: rol_mem(addr_absx(), 7); break;
    case 0x40: rti(); break;
    case 0x41: eor(load_byte(addr_indx()), 6); break;
    case 0x45: eor(zp_read(), 3); break;
    case 0x46: lsr_mem(addr_zero(), 5); break;
    case 0x48: pha(); break;
    case 0x49: eor(fetch_op(), 2); break;
    case 0x4A: lsr_a(); break;
    case 0x4C: jmp(); break;
    case 0x4D: eor(load_byte(addr_abs()), 4); break;
    case 0x4E: lsr_mem(addr_abs(), 6); break;
    case 0x50: bvc(); break;
    case 0x51: eor(load_byte(addr_indy()), 5); break;
    case 0x55: eor(zpx_read(), 4); break;
    case 0x56: lsr_mem(addr_zerox(), 6); break;
    case 0x58: cli(); break;
    case 0x59: eor(load_byte(addr_absy()), 4); break;
    case 0x5D: eor(load_byte(addr_absx()), 4); break;
    case 0x5E: lsr_mem(addr_absx(), 7); break;
    case 0x60: rts(); break;
    case 0x61: adc(load_byte(addr_indx()), 6); break;
    case 0x65: adc(zp_read(), 3); break;
    case 0x66: ror_mem(addr_zero(), 5); break;
    case 0x68: pla(); break;
    case 0x69: adc(fetch_op(), 2); break;
    case 0x6A: ror_a(); break;
    case 0x6C: jmp_ind(); break;
    case 0x6D: adc(load_byte(addr_abs()), 4); break;
    case 0x6E: ror_mem(addr_abs(), 6); break;
    case 0x70: bvs(); break;
    case 0x71: adc(load_byte(addr_indy()), 5); break;
    case 0x75: adc(zpx_read(), 4); break;
    case 0x76: ror_mem(addr_zerox(), 6); break;
    case 0x78: sei(); break;
    case 0x79: adc(load_byte(addr_absy()), 4); break;
    case 0x7D: adc(load_byte(addr_absx()), 4); break;
    case 0x7E: ror_mem(addr_absx(), 7); break;
    case 0x81: sta(addr_indx(), 6); break;
    case 0x84: zp_write(fetch_op(), cpu.y); tick(3); break;
    case 0x85: zp_write(fetch_op(), cpu.a); tick(3); break;
    case 0x86: zp_write(fetch_op(), cpu.x); tick(3); break;
    case 0x88: dey(); break;
    case 0x8A: txa(); break;
    case 0x8C: sty(addr_abs(), 4); break;
    case 0x8D: sta(addr_abs(), 4); break;
    case 0x8E: stx(addr_abs(), 4); break;
    case 0x90: bcc(); break;
    case 0x91: sta(addr_indy(), 6); break;
    case 0x94: zp_write((uint8_t)(fetch_op() + cpu.x), cpu.y); tick(4); break;
    case 0x95: zp_write((uint8_t)(fetch_op() + cpu.x), cpu.a); tick(4); break;
    case 0x96: zp_write((uint8_t)(fetch_op() + cpu.y), cpu.x); tick(4); break;
    case 0x98: tya(); break;
    case 0x99: sta(addr_absy(), 5); break;
    case 0x9A: txs(); break;
    case 0x9D: sta(addr_absx(), 5); break;
    case 0xA0: ldy(fetch_op(), 2); break;
    case 0xA1: lda(load_byte(addr_indx()), 6); break;
    case 0xA2: ldx(fetch_op(), 2); break;
    case 0xA4: ldy(zp_read(), 3); break;
    case 0xA5: lda(zp_read(), 3); break;
    case 0xA6: ldx(zp_read(), 3); break;
    case 0xA8: tay(); break;
    case 0xA9: lda(fetch_op(), 2); break;
    case 0xAA: tax(); break;
    case 0xAC: ldy(load_byte(addr_abs()), 4); break;
    case 0xAD: lda(load_byte(addr_abs()), 4); break;
    case 0xAE: ldx(load_byte(addr_abs()), 4); break;
    case 0xB0: bcs(); break;
    case 0xB1: lda(load_byte(addr_indy()), 5); break;
    case 0xB4: ldy(zpx_read(), 4); break;
    case 0xB5: lda(zpx_read(), 4); break;
    case 0xB6: ldx(zpy_read(), 4); break;
    case 0xB8: clv(); break;
    case 0xB9: lda(load_byte(addr_absy()), 4); break;
    case 0xBA: tsx(); break;
    case 0xBC: ldy(load_byte(addr_absx()), 4); break;
    case 0xBD: lda(load_byte(addr_absx()), 4); break;
    case 0xBE: ldx(load_byte(addr_absy()), 4); break;
    case 0xC0: cpy(fetch_op(), 2); break;
    case 0xC1: cmp(load_byte(addr_indx()), 6); break;
    case 0xC4: cpy(zp_read(), 3); break;
    case 0xC5: cmp(zp_read(), 3); break;
    case 0xC6: dec(addr_zero(), 5); break;
    case 0xC8: iny(); break;
    case 0xC9: cmp(fetch_op(), 2); break;
    case 0xCA: dex(); break;
    case 0xCC: cpy(load_byte(addr_abs()), 4); break;
    case 0xCD: cmp(load_byte(addr_abs()), 4); break;
    case 0xCE: dec(addr_abs(), 6); break;
    case 0xD0: bne(); break;
    case 0xD1: cmp(load_byte(addr_indy()), 5); break;
    case 0xD5: cmp(zpx_read(), 4); break;
    case 0xD6: dec(addr_zerox(), 6); break;
    case 0xD8: cld(); break;
    case 0xD9: cmp(load_byte(addr_absy()), 4); break;
    case 0xDD: cmp(load_byte(addr_absx()), 4); break;
    case 0xDE: dec(addr_absx(), 7); break;
    case 0xE0: cpx(fetch_op(), 2); break;
    case 0xE1: sbc(load_byte(addr_indx()), 6); break;
    case 0xE4: cpx(zp_read(), 3); break;
    case 0xE5: sbc(zp_read(), 3); break;
    case 0xE6: inc(addr_zero(), 5); break;
    case 0xE8: inx(); break;
    case 0xE9: sbc(fetch_op(), 2); break;
    case 0xEA: nop(); break;
    case 0xEC: cpx(load_byte(addr_abs()), 4); break;
    case 0xED: sbc(load_byte(addr_abs()), 4); break;
    case 0xEE: inc(addr_abs(), 6); break;
    case 0xF0: beq(); break;
    case 0xF1: sbc(load_byte(addr_indy()), 5); break;
    case 0xF5: sbc(zpx_read(), 4); break;
    case 0xF6: inc(addr_zerox(), 6); break;
    case 0xF8: sed(); break;
    case 0xF9: sbc(load_byte(addr_absy()), 4); break;
    case 0xFD: sbc(load_byte(addr_absx()), 4); break;
    case 0xFE: inc(addr_absx(), 7); break;
    /* ---- undocumented opcodes ---- */
    case 0xA3: lax(load_byte(addr_indx()), 6); break;
    case 0xA7: lax(zp_read(), 3); break;
    case 0xAB: lax(fetch_op(), 2); break;
    case 0xAF: lax(load_byte(addr_abs()), 4); break;
    case 0xB3: lax(load_byte(addr_indy()), 5); break;
    case 0xB7: lax(zpy_read(), 4); break;
    case 0xBF: lax(load_byte(addr_absy()), 4); break;
    case 0x83: sax(addr_indx(), 6); break;
    case 0x87: sax(addr_zero(), 3); break;
    case 0x8F: sax(addr_abs(), 4); break;
    case 0x97: sax((uint16_t)((fetch_op() + cpu.y) & 0xff), 4); break;
    case 0x03: slo(addr_indx(), 8); break;
    case 0x07: slo(addr_zero(), 5); break;
    case 0x0F: slo(addr_abs(), 6); break;
    case 0x13: slo(addr_indy(), 8); break;
    case 0x17: slo(addr_zerox(), 6); break;
    case 0x1B: slo(addr_absy(), 7); break;
    case 0x1F: slo(addr_absx(), 7); break;
    case 0x23: rla(addr_indx(), 8); break;
    case 0x27: rla(addr_zero(), 5); break;
    case 0x2F: rla(addr_abs(), 6); break;
    case 0x33: rla(addr_indy(), 8); break;
    case 0x37: rla(addr_zerox(), 6); break;
    case 0x3B: rla(addr_absy(), 7); break;
    case 0x3F: rla(addr_absx(), 7); break;
    case 0x43: sre(addr_indx(), 8); break;
    case 0x47: sre(addr_zero(), 5); break;
    case 0x4F: sre(addr_abs(), 6); break;
    case 0x53: sre(addr_indy(), 8); break;
    case 0x57: sre(addr_zerox(), 6); break;
    case 0x5B: sre(addr_absy(), 7); break;
    case 0x5F: sre(addr_absx(), 7); break;
    case 0x63: rra(addr_indx(), 8); break;
    case 0x67: rra(addr_zero(), 5); break;
    case 0x6F: rra(addr_abs(), 6); break;
    case 0x73: rra(addr_indy(), 8); break;
    case 0x77: rra(addr_zerox(), 6); break;
    case 0x7B: rra(addr_absy(), 7); break;
    case 0x7F: rra(addr_absx(), 7); break;
    case 0xC3: dcp(addr_indx(), 8); break;
    case 0xC7: dcp(addr_zero(), 5); break;
    case 0xCF: dcp(addr_abs(), 6); break;
    case 0xD3: dcp(addr_indy(), 8); break;
    case 0xD7: dcp(addr_zerox(), 6); break;
    case 0xDB: dcp(addr_absy(), 7); break;
    case 0xDF: dcp(addr_absx(), 7); break;
    case 0xE3: isc(addr_indx(), 8); break;
    case 0xE7: isc(addr_zero(), 5); break;
    case 0xEF: isc(addr_abs(), 6); break;
    case 0xF3: isc(addr_indy(), 8); break;
    case 0xF7: isc(addr_zerox(), 6); break;
    case 0xFB: isc(addr_absy(), 7); break;
    case 0xFF: isc(addr_absx(), 7); break;
    case 0x0B: case 0x2B: anc(fetch_op()); break;
    case 0x4B: alr(fetch_op()); break;
    case 0x6B: arr(fetch_op()); break;
    case 0xCB: sbx(fetch_op()); break;
    case 0xEB: sbc(fetch_op(), 2); break;
    case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
        nop(); break;
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:
        nop_arg(fetch_op(), 2); break;
    case 0x04: case 0x44: case 0x64:
        nop_arg(addr_zero(), 3); break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4:
        nop_arg(addr_zerox(), 4); break;
    case 0x0C:
        nop_arg(addr_abs(), 4); break;
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
        nop_arg(addr_absx(), 4); break;
    case DRIVE_TRAP_OPCODE: /* KERNAL LOAD trap planted at $F4A5 by drive.c */
        if (drive_trap_load())
            break;
        /* trap opcode fetched anywhere else: treat as the JAM it is */
        cpu_illegal_op = insn;
        cpu_illegal_pc = cpu.pc;
        retval = false;
        break;
    case DRIVE_TRAP2_OPCODE: /* resident game fastloader trap (drive.c) */
        if (drive_trap_fastload())
            break;
        cpu_illegal_op = insn;
        cpu_illegal_pc = cpu.pc;
        retval = false;
        break;
    default:
        cpu_illegal_op = insn;
        cpu_illegal_pc = cpu.pc;
        retval = false;
    }
    return retval;
}

/* The asm core executes the hot opcodes; the C loop is the validated
   fallback and shares cpu_step_slow's single copy of the opcode switch
   (inlining it twice costs ~10 KiB of flash). */
bool cpu_run(unsigned int until_cycles)
{
    run_until = until_cycles;
    if (cpu_asm_ok)
        return cpu_run_asm(until_cycles);
    do
    {
        if (!cpu_step_slow())
            return false;
    } while ((int)(cpu.cycles - until_cycles) < 0);
    return true;
}

/* One full C instruction step: the asm core's fallback for opcodes it does
   not implement (illegals, BCD, RMW combos, drive traps). */
bool cpu_step_slow(void)
{
    return cpu_step();
}

/* Pure C batch, available regardless of cpu_asm_ok (divergence probe). */
bool cpu_run_c(unsigned int until_cycles)
{
    run_until = until_cycles;
    do
    {
        if (!cpu_step_slow())
            return false;
    } while ((int)(cpu.cycles - until_cycles) < 0);
    return true;
}

/* Runs the loaded KERNAL reset sequence through BOTH cores with increasing
   cycle budgets (no chip emulation: fully deterministic) and reports the
   first architectural divergence. Requires ROMs loaded; trashes RAM. */
void cpu_asm_boot_probe(void (*machine_reset)(void),
                        void (*report)(unsigned int budget,
                                       const CpuState *c_state,
                                       const CpuState *a_state))
{
    unsigned int budget;
    CpuState c_state;

    for (budget = 8; budget <= 30000; budget += (budget < 1200 ? 2 : 200))
    {
        machine_reset();
        cpu_run_c(budget);
        c_state = cpu;

        machine_reset();
        cpu_run_asm(budget);

        if (c_state.pc != cpu.pc || c_state.sp != cpu.sp ||
            c_state.a != cpu.a || c_state.x != cpu.x || c_state.y != cpu.y ||
            c_state.cycles != cpu.cycles ||
            c_state.cf != cpu.cf || c_state.zf != cpu.zf ||
            c_state.nf != cpu.nf || c_state.of != cpu.of ||
            c_state.idf != cpu.idf || c_state.dmf != cpu.dmf)
        {
            report(budget, &c_state, &cpu);
            return;
        }
    }
    report(0, &cpu, &cpu); /* no divergence found */
}

/* Runs the same 6502 program through the C core and the asm core and
   compares every register, flag, the cycle counter and touched memory.
   Requires mem_init() done; scribbles $0400-$05FF + a few ZP cells. */
bool cpu_asm_selftest(void)
{
    static const uint8_t prog[] = {
        0xA9, 0x05,             /* 0400 LDA #$05        */
        0x85, 0x10,             /* 0402 STA $10         */
        0xA6, 0x10,             /* 0404 LDX $10         */
        0xA0, 0x07,             /* 0406 LDY #$07        */
        0xE8,                   /* 0408 INX             */
        0x88,                   /* 0409 DEY             */
        0x18,                   /* 040A CLC             */
        0x69, 0x10,             /* 040B ADC #$10        */
        0xC9, 0x15,             /* 040D CMP #$15        */
        0xF0, 0x02,             /* 040F BEQ $0413       */
        0xA9, 0xFF,             /* 0411 LDA #$FF        */
        0x38,                   /* 0413 SEC             */
        0xE9, 0x06,             /* 0414 SBC #$06        */
        0x8D, 0x00, 0x05,       /* 0416 STA $0500       */
        0xAD, 0x00, 0x05,       /* 0419 LDA $0500       */
        0x0A,                   /* 041C ASL A           */
        0x2A,                   /* 041D ROL A           */
        0x48,                   /* 041E PHA             */
        0x20, 0xA0, 0x04,       /* 041F JSR $04A0       */
        0x68,                   /* 0422 PLA             */
        0xE6, 0x10,             /* 0423 INC $10         */
        0xC6, 0x11,             /* 0425 DEC $11         */
        0xA2, 0x04,             /* 0427 LDX #$04        */
        0xCA,                   /* 0429 DEX             */
        0xD0, 0xFD,             /* 042A BNE $0429       */
        0x99, 0x01, 0x05,       /* 042C STA $0501,Y     */
        0xB1, 0x12,             /* 042F LDA ($12),Y     */
        0x91, 0x12,             /* 0431 STA ($12),Y     */
        0xEE, 0x00, 0x05,       /* 0433 INC $0500 (C-fallback bridge test) */
        0xA9, 0x37,             /* 0436 LDA #$37        */
        0x85, 0x01,             /* 0438 STA $01  (bank-latch C call)       */
        0xA9, 0x03,             /* 043A LDA #$03        */
        0x8D, 0x20, 0xD0,       /* 043C STA $D020 (IO-write C call)        */
        0xAD, 0x20, 0xD0,       /* 043F LDA $D020 (IO-read C call)         */
        0x85, 0x15,             /* 0442 STA $15         */
        0x38,                   /* 0444 SEC             */
        0x08,                   /* 0445 PHP             */
        0x18,                   /* 0446 CLC             */
        0x28,                   /* 0447 PLP (carry restored from stack)    */
        0xA2, 0x02,             /* 0448 LDX #$02        */
        0xA1, 0x10,             /* 044A LDA ($10,X)     */
        0x81, 0x10,             /* 044C STA ($10,X)     */
        0xA2, 0x06,             /* 044E LDX #$06        */
        0x16, 0x10,             /* 0450 ASL $10,X       */
        0x1E, 0x00, 0x05,       /* 0452 ASL $0500,X     */
        0x6C, 0x18, 0x00,       /* 0455 JMP ($0018)     */
        0xA9, 0x04,             /* 0458 LDA #$04 (RTI target hi)           */
        0x48,                   /* 045A PHA             */
        0xA9, 0x62,             /* 045B LDA #$62 (RTI target lo = $0462)   */
        0x48,                   /* 045D PHA             */
        0x08,                   /* 045E PHP             */
        0x40,                   /* 045F RTI             */
        0xA9, 0xEE,             /* 0460 LDA #$EE (skipped by RTI)          */
        0x24, 0x16,             /* 0462 BIT $16 (lazy-NZ/V split check)    */
        0x4C, 0x64, 0x04        /* 0464 JMP $0464 (spin until budget)      */
    };
    static const uint8_t sub[] = {
        0xA9, 0x22,             /* 04A0 LDA #$22        */
        0x85, 0x14,             /* 04A2 STA $14         */
        0x60                    /* 04A4 RTS             */
    };
    CpuState ref;
    uint8_t ref_mem[8];
    int pass;
    bool ok = true;

    for (pass = 0; pass < 2 && ok; ++pass)
    {
        memset(mem_low + 0x0400, 0, 0x0200);
        memcpy(mem_low + 0x0400, prog, sizeof(prog));
        memcpy(mem_low + 0x04A0, sub, sizeof(sub));
        mem_low[0x10] = 0; mem_low[0x11] = 3;
        mem_low[0x12] = 0x00; mem_low[0x13] = 0x05;
        mem_low[0x14] = 0; mem_low[0x15] = 0;
        mem_low[0x16] = 0x41;                 /* ASL $10,X target */
        mem_low[0x18] = 0x58; mem_low[0x19] = 0x04; /* JMP ($0018) -> $0458 */
        vic_write_register(0x20, 0); /* deterministic border for readback */
        mem_setup_banks(LORAM | HIRAM | CHAREN);
        memset(&cpu, 0, sizeof(cpu));
        cpu.pc = 0x0400;
        cpu.sp = 0xfd;

        if (pass == 0)
        {
            cpu_asm_ok = false;
            cpu_run(300);
            ref = cpu;
            ref_mem[0] = mem_low[0x10]; ref_mem[1] = mem_low[0x11];
            ref_mem[2] = mem_low[0x14]; ref_mem[3] = mem_low[0x500];
            ref_mem[4] = mem_low[0x506]; ref_mem[5] = mem_low[0x507];
            ref_mem[6] = mem_low[0x15]; ref_mem[7] = mem_low[0x16];
        }
        else
        {
            cpu_run_asm(300);
            ok = ref.pc == cpu.pc && ref.sp == cpu.sp && ref.a == cpu.a &&
                 ref.x == cpu.x && ref.y == cpu.y && ref.cycles == cpu.cycles &&
                 ref.cf == cpu.cf && ref.zf == cpu.zf && ref.nf == cpu.nf &&
                 ref.of == cpu.of &&
                 ref_mem[0] == mem_low[0x10] && ref_mem[1] == mem_low[0x11] &&
                 ref_mem[2] == mem_low[0x14] && ref_mem[3] == mem_low[0x500] &&
                 ref_mem[4] == mem_low[0x506] && ref_mem[5] == mem_low[0x507] &&
                 ref_mem[6] == mem_low[0x15] && ref_mem[7] == mem_low[0x16];
        }
    }

    /* clean the scratch so boot starts from zeroed RAM as before */
    memset(mem_low + 0x0400, 0, 0x0200);
    mem_low[0x10] = mem_low[0x11] = mem_low[0x12] = 0;
    mem_low[0x13] = mem_low[0x14] = mem_low[0x15] = 0;
    mem_low[0x16] = mem_low[0x18] = mem_low[0x19] = 0;
    vic_write_register(0x20, 0);
    mem_setup_banks(LORAM | HIRAM | CHAREN);
    memset(&cpu, 0, sizeof(cpu));

    cpu_asm_ok = ok;
    return ok;
}
