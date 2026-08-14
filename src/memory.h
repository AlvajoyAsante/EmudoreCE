/*
 * emudore-CE: C64 DRAM + ROM banking (C port of memory.h).
 *
 * load_rom()/load_ram() read files via <fstream> on the desktop version;
 * here ROMs come from AppVars through CEPackage instead, so they are removed.
 * The debug dump helpers are also removed.
 */
#ifndef EMUDORE_CE_MEMORY_H
#define EMUDORE_CE_MEMORY_H

#include <stdint.h>

#define MEM_SIZE 0x10000

/* memory addresses */
#define ADDR_BASIC      0xa000
#define ADDR_KERNAL     0xe000
#define ADDR_STACK      0x0100
#define ADDR_SCREEN     0x0400
#define ADDR_CHARS      0xd000
#define ADDR_BITMAP     0x0000
#define ADDR_COLOR_RAM  0xd800
#define ADDR_RESET_VEC  0xfffc
#define ADDR_IRQ_VEC    0xfffe
#define ADDR_NMI_VEC    0xfffa
#define ADDR_DATA_DIR   0x0000
#define ADDR_MEM_LAYOUT 0x0001

/* memory layout regions */
#define ADDR_ZERO_PAGE       0x0000
#define ADDR_VIC_FIRST_PAGE  0xd000
#define ADDR_VIC_LAST_PAGE   0xd300
#define ADDR_CIA1_PAGE       0xdc00
#define ADDR_CIA2_PAGE       0xdd00
#define ADDR_BASIC_FIRST     0xa000
#define ADDR_BASIC_LAST      0xbfff
#define ADDR_KERNAL_FIRST    0xe000
#define ADDR_KERNAL_LAST     0xffff

/* 6510 memory-layout latch bits (port $0001) */
#define LORAM  (1 << 0)
#define HIRAM  (1 << 1)
#define CHAREN (1 << 2)

/* bank configuration values stored in banks[] */
enum
{
    MEM_BANK_ROM,
    MEM_BANK_RAM,
    MEM_BANK_IO
};
enum
{
    MEM_BANK_BASIC  = 3,
    MEM_BANK_CHAREN = 5,
    MEM_BANK_KERNAL = 6
};

void mem_init(void);
void mem_shutdown(void);
void mem_setup_banks(uint8_t v);

/* non-zero once RAM/RAM backing buffers were allocated */
int mem_ready(void);

/* direct access to the ROM/RAM images, used by the ROM loader */
uint8_t *mem_basic_rom_ptr(void);
uint8_t *mem_kernal_rom_ptr(void);
uint8_t *mem_chars_rom_ptr(void);
uint8_t *mem_ram_ptr(void);

/* Page-table dispatch: one biased host pointer per 256-byte C64 page
   (p[addr] hits the right byte directly). NULL = IO page, take the slow
   path. Rebuilt by mem_setup_banks on every $0001 write.
   Entries are padded to 4 bytes: indexing by *4 is an inline shift, while a
   3-byte pointer stride made ez80-clang call the __imulu helper per access. */
typedef union
{
    uint8_t *ptr;
    uint32_t pad_;
} MemPage;

/* Byte view of a 16-bit address: reading .b.hi replaces `addr >> 8`, which
   ez80-clang otherwise lowers to a __ishru helper CALL per access. */
typedef union
{
    uint16_t w;
    struct { uint8_t lo, hi; } b;
} U16B;

extern uint8_t *mem_ram;
extern uint8_t *mem_low; /* fast BSS copy of C64 $0000-$0FFF (see memory.c) */
extern uint8_t mem_color_ram[0x400]; /* dedicated SRAM, distinct from the
                                        RAM under $D800 like real hardware */
extern MemPage mem_rd_pg[256];
extern MemPage mem_wr_pg[256];
uint8_t mem_read_io(uint16_t addr);
void    mem_write_io(uint16_t addr, uint8_t v);

/* C64 pages $00-$0F (ZP, stack, OS workspace, default screen) live in BSS
   RAM instead of VRAM: they are the hottest data and VRAM pays LCD-DMA
   contention wait states on every access. */
#define MEM_LOW_SIZE 0x1000

__attribute__((__always_inline__))
static inline uint8_t *mem_host_ptr(uint16_t addr)
{
    return (addr < MEM_LOW_SIZE ? mem_low : mem_ram) + addr;
}

/* Inlined hot paths: every 6502 instruction does 2-4 of these. always_inline
   because -Oz otherwise outlines them into local functions (call overhead). */
__attribute__((__always_inline__))
static inline uint8_t mem_read_byte(uint16_t addr)
{
    U16B a;
    const uint8_t *p;
    a.w = addr;
    p = mem_rd_pg[a.b.hi].ptr;
    return p ? p[addr] : mem_read_io(addr);
}

__attribute__((__always_inline__))
static inline void mem_write_byte(uint16_t addr, uint8_t v)
{
    U16B a;
    uint8_t *p;
    a.w = addr;
    p = mem_wr_pg[a.b.hi].ptr;
    if (p)
    {
        p[addr] = v;
        if (addr == ADDR_MEM_LAYOUT)
            mem_setup_banks(v);
    }
    else
        mem_write_io(addr, v);
}

static inline uint8_t mem_read_byte_no_io(uint16_t addr)
{
    return *mem_host_ptr(addr);
}

static inline void mem_write_byte_no_io(uint16_t addr, uint8_t v)
{
    *mem_host_ptr(addr) = v;
}

__attribute__((__always_inline__))
static inline uint16_t mem_read_word(uint16_t addr)
{
    U16B r;
    r.b.lo = mem_read_byte(addr);
    r.b.hi = mem_read_byte((uint16_t)(addr + 1));
    return r.w;
}

static inline uint16_t mem_read_word_no_io(uint16_t addr)
{
    U16B r;
    r.b.lo = mem_read_byte_no_io(addr);
    r.b.hi = mem_read_byte_no_io((uint16_t)(addr + 1));
    return r.w;
}

void mem_write_word(uint16_t addr, uint16_t v);
void mem_write_word_no_io(uint16_t addr, uint16_t v);

/* VIC-II view of memory (always RAM except character ROM windows). */
uint8_t  mem_vic_read_byte(uint16_t addr);

#endif /* EMUDORE_CE_MEMORY_H */
