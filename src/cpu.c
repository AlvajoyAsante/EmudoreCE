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
#include "cpu.h"
#include "memory.h"
#include "util.h"

CpuState cpu;

/* register/flag accessors as macros (read) and assignment helpers (write) */
#define A()  (cpu.a)
#define X()  (cpu.x)
#define Y()  (cpu.y)
#define SP() (cpu.sp)
#define PC() (cpu.pc)

/* ---- helpers ---------------------------------------------------------- */

static uint8_t load_byte(uint16_t addr)
{
    return mem_read_byte(addr);
}

static void push(uint8_t v)
{
    mem_write_byte(ADDR_STACK + cpu.sp, v);
    cpu.sp--;
}

static uint8_t pop(void)
{
    return load_byte((uint16_t)(++cpu.sp + ADDR_STACK));
}

static uint8_t fetch_op(void)
{
    return load_byte(cpu.pc++);
}

static uint16_t fetch_opw(void)
{
    uint16_t v = mem_read_word(cpu.pc);
    cpu.pc += 2;
    return v;
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

static uint16_t addr_zero(void)  { return fetch_op(); }
static uint16_t addr_zerox(void) { return (fetch_op() + cpu.x) & 0xff; }
static uint16_t addr_zeroy(void) { return (fetch_op() + cpu.y) & 0xff; }
static uint16_t addr_abs(void)   { return fetch_opw(); }
static uint16_t addr_absx(void)  { return fetch_opw() + cpu.x; }
static uint16_t addr_absy(void)  { return fetch_opw() + cpu.y; }
static uint16_t addr_indx(void)  { return mem_read_word((addr_zero() + cpu.x) & 0xff); }
static uint16_t addr_indy(void)  { return mem_read_word(addr_zero()) + cpu.y; }

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
    (void)cycles;
}
static void dec(uint16_t addr, uint8_t cycles)
{
    uint8_t v = load_byte(addr);
    mem_write_byte(addr, v);
    v--;
    mem_write_byte(addr, v);
    SET_ZF(v);
    SET_NF(v);
    (void)cycles;
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

/* JSR pushes the address of its own last byte, not the next instruction. */
static void jsr(void)
{
    uint16_t addr = addr_abs();
    push(((cpu.pc - 1) >> 8) & 0xff);
    push(((cpu.pc - 1) & 0xff));
    cpu.pc = addr;
    tick(6);
}

static void jmp(void)     { cpu.pc = addr_abs(); tick(3); }
static void jmp_ind(void) { cpu.pc = mem_read_word(addr_abs()); tick(3); }
static void rts(void)     { cpu.pc = (uint16_t)(pop() + (pop() << 8)) + 1; tick(6); }

static void bne(void) { uint16_t a = (int8_t)fetch_op() + cpu.pc; if (!cpu.zf) cpu.pc = a; tick(2); }
static void beq(void) { uint16_t a = (int8_t)fetch_op() + cpu.pc; if (cpu.zf)  cpu.pc = a; tick(2); }
static void bcs(void) { uint16_t a = (int8_t)fetch_op() + cpu.pc; if (cpu.cf)  cpu.pc = a; tick(2); }
static void bcc(void) { uint16_t a = (int8_t)fetch_op() + cpu.pc; if (!cpu.cf) cpu.pc = a; tick(2); }
static void bpl(void) { uint16_t a = (int8_t)fetch_op() + cpu.pc; if (!cpu.nf) cpu.pc = a; tick(2); }
static void bmi(void) { uint16_t a = (int8_t)fetch_op() + cpu.pc; if (cpu.nf)  cpu.pc = a; tick(2); }
static void bvc(void) { uint16_t a = (int8_t)fetch_op() + cpu.pc; if (!cpu.of) cpu.pc = a; tick(2); }
static void bvs(void) { uint16_t a = (int8_t)fetch_op() + cpu.pc; if (cpu.of)  cpu.pc = a; tick(2); }

/* ---- misc -------------------------------------------------------------- */

static void nop(void) { tick(2); }

static void brk(void)
{
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

bool cpu_emulate(void)
{
    uint8_t insn = fetch_op();
    bool retval = true;

    switch (insn)
    {
    case 0x00: brk(); break;
    case 0x01: ora(load_byte(addr_indx()), 6); break;
    case 0x05: ora(load_byte(addr_zero()), 3); break;
    case 0x06: asl_mem(addr_zero(), 5); break;
    case 0x08: php(); break;
    case 0x09: ora(fetch_op(), 2); break;
    case 0x0A: asl_a(); break;
    case 0x0D: ora(load_byte(addr_abs()), 4); break;
    case 0x0E: asl_mem(addr_abs(), 6); break;
    case 0x10: bpl(); break;
    case 0x11: ora(load_byte(addr_indy()), 5); break;
    case 0x15: ora(load_byte(addr_zerox()), 4); break;
    case 0x16: asl_mem(addr_zerox(), 6); break;
    case 0x18: clc(); break;
    case 0x19: ora(load_byte(addr_absy()), 4); break;
    case 0x1D: ora(load_byte(addr_absx()), 4); break;
    case 0x1E: asl_mem(addr_absx(), 7); break;
    case 0x20: jsr(); break;
    case 0x21: _and(load_byte(addr_indx()), 6); break;
    case 0x24: bit(addr_zero(), 3); break;
    case 0x25: _and(load_byte(addr_zero()), 3); break;
    case 0x26: rol_mem(addr_zero(), 5); break;
    case 0x28: plp(); break;
    case 0x29: _and(fetch_op(), 2); break;
    case 0x2A: rol_a(); break;
    case 0x2C: bit(addr_abs(), 4); break;
    case 0x2D: _and(load_byte(addr_abs()), 4); break;
    case 0x2E: rol_mem(addr_abs(), 6); break;
    case 0x30: bmi(); break;
    case 0x31: _and(load_byte(addr_indy()), 5); break;
    case 0x35: _and(load_byte(addr_zerox()), 4); break;
    case 0x36: rol_mem(addr_zerox(), 6); break;
    case 0x38: sec(); break;
    case 0x39: _and(load_byte(addr_absy()), 4); break;
    case 0x3D: _and(load_byte(addr_absx()), 4); break;
    case 0x3E: rol_mem(addr_absx(), 7); break;
    case 0x40: rti(); break;
    case 0x41: eor(load_byte(addr_indx()), 6); break;
    case 0x45: eor(load_byte(addr_zero()), 3); break;
    case 0x46: lsr_mem(addr_zero(), 5); break;
    case 0x48: pha(); break;
    case 0x49: eor(fetch_op(), 2); break;
    case 0x4A: lsr_a(); break;
    case 0x4C: jmp(); break;
    case 0x4D: eor(load_byte(addr_abs()), 4); break;
    case 0x4E: lsr_mem(addr_abs(), 6); break;
    case 0x50: bvc(); break;
    case 0x51: eor(load_byte(addr_indy()), 5); break;
    case 0x55: eor(load_byte(addr_zerox()), 4); break;
    case 0x56: lsr_mem(addr_zerox(), 6); break;
    case 0x58: cli(); break;
    case 0x59: eor(load_byte(addr_absy()), 4); break;
    case 0x5D: eor(load_byte(addr_absx()), 4); break;
    case 0x5E: lsr_mem(addr_absx(), 7); break;
    case 0x60: rts(); break;
    case 0x61: adc(load_byte(addr_indx()), 6); break;
    case 0x65: adc(load_byte(addr_zero()), 3); break;
    case 0x66: ror_mem(addr_zero(), 5); break;
    case 0x68: pla(); break;
    case 0x69: adc(fetch_op(), 2); break;
    case 0x6A: ror_a(); break;
    case 0x6C: jmp_ind(); break;
    case 0x6D: adc(load_byte(addr_abs()), 4); break;
    case 0x6E: ror_mem(addr_abs(), 6); break;
    case 0x70: bvs(); break;
    case 0x71: adc(load_byte(addr_indy()), 5); break;
    case 0x75: adc(load_byte(addr_zerox()), 4); break;
    case 0x76: ror_mem(addr_zerox(), 6); break;
    case 0x78: sei(); break;
    case 0x79: adc(load_byte(addr_absy()), 4); break;
    case 0x7D: adc(load_byte(addr_absx()), 4); break;
    case 0x7E: ror_mem(addr_absx(), 7); break;
    case 0x81: sta(addr_indx(), 6); break;
    case 0x84: sty(addr_zero(), 3); break;
    case 0x85: sta(addr_zero(), 3); break;
    case 0x86: stx(addr_zero(), 3); break;
    case 0x88: dey(); break;
    case 0x8A: txa(); break;
    case 0x8C: sty(addr_abs(), 4); break;
    case 0x8D: sta(addr_abs(), 4); break;
    case 0x8E: stx(addr_abs(), 4); break;
    case 0x90: bcc(); break;
    case 0x91: sta(addr_indy(), 6); break;
    case 0x94: sty(addr_zerox(), 4); break;
    case 0x95: sta(addr_zerox(), 4); break;
    case 0x96: stx(addr_zeroy(), 4); break;
    case 0x98: tya(); break;
    case 0x99: sta(addr_absy(), 5); break;
    case 0x9A: txs(); break;
    case 0x9D: sta(addr_absx(), 5); break;
    case 0xA0: ldy(fetch_op(), 2); break;
    case 0xA1: lda(load_byte(addr_indx()), 6); break;
    case 0xA2: ldx(fetch_op(), 2); break;
    case 0xA4: ldy(load_byte(addr_zero()), 3); break;
    case 0xA5: lda(load_byte(addr_zero()), 3); break;
    case 0xA6: ldx(load_byte(addr_zero()), 3); break;
    case 0xA8: tay(); break;
    case 0xA9: lda(fetch_op(), 2); break;
    case 0xAA: tax(); break;
    case 0xAC: ldy(load_byte(addr_abs()), 4); break;
    case 0xAD: lda(load_byte(addr_abs()), 4); break;
    case 0xAE: ldx(load_byte(addr_abs()), 4); break;
    case 0xB0: bcs(); break;
    case 0xB1: lda(load_byte(addr_indy()), 5); break;
    case 0xB4: ldy(load_byte(addr_zerox()), 4); break;
    case 0xB5: lda(load_byte(addr_zerox()), 4); break;
    case 0xB6: ldx(load_byte(addr_zeroy()), 4); break;
    case 0xB8: clv(); break;
    case 0xB9: lda(load_byte(addr_absy()), 4); break;
    case 0xBA: tsx(); break;
    case 0xBC: ldy(load_byte(addr_absx()), 4); break;
    case 0xBD: lda(load_byte(addr_absx()), 4); break;
    case 0xBE: ldx(load_byte(addr_absy()), 4); break;
    case 0xC0: cpy(fetch_op(), 2); break;
    case 0xC1: cmp(load_byte(addr_indx()), 6); break;
    case 0xC4: cpy(load_byte(addr_zero()), 3); break;
    case 0xC5: cmp(load_byte(addr_zero()), 3); break;
    case 0xC6: dec(addr_zero(), 5); break;
    case 0xC8: iny(); break;
    case 0xC9: cmp(fetch_op(), 2); break;
    case 0xCA: dex(); break;
    case 0xCC: cpy(load_byte(addr_abs()), 4); break;
    case 0xCD: cmp(load_byte(addr_abs()), 4); break;
    case 0xCE: dec(addr_abs(), 6); break;
    case 0xD0: bne(); break;
    case 0xD1: cmp(load_byte(addr_indy()), 5); break;
    case 0xD5: cmp(load_byte(addr_zerox()), 4); break;
    case 0xD6: dec(addr_zerox(), 6); break;
    case 0xD8: cld(); break;
    case 0xD9: cmp(load_byte(addr_absy()), 4); break;
    case 0xDD: cmp(load_byte(addr_absx()), 4); break;
    case 0xDE: dec(addr_absx(), 7); break;
    case 0xE0: cpx(fetch_op(), 2); break;
    case 0xE1: sbc(load_byte(addr_indx()), 6); break;
    case 0xE4: cpx(load_byte(addr_zero()), 3); break;
    case 0xE5: sbc(load_byte(addr_zero()), 3); break;
    case 0xE6: inc(addr_zero(), 5); break;
    case 0xE8: inx(); break;
    case 0xE9: sbc(fetch_op(), 2); break;
    case 0xEA: nop(); break;
    case 0xEC: cpx(load_byte(addr_abs()), 4); break;
    case 0xED: sbc(load_byte(addr_abs()), 4); break;
    case 0xEE: inc(addr_abs(), 6); break;
    case 0xF0: beq(); break;
    case 0xF1: sbc(load_byte(addr_indy()), 5); break;
    case 0xF5: sbc(load_byte(addr_zerox()), 4); break;
    case 0xF6: inc(addr_zerox(), 6); break;
    case 0xF8: sed(); break;
    case 0xF9: sbc(load_byte(addr_absy()), 4); break;
    case 0xFD: sbc(load_byte(addr_absx()), 4); break;
    case 0xFE: inc(addr_absx(), 7); break;
    default:
        D("Unknown instruction: %X at %04x\n", insn, cpu.pc);
        retval = false;
    }
    return retval;
}
