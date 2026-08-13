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

/* non-zero once RAM/ROM backing buffers were allocated */
int mem_ready(void);

/* direct access to the ROM/RAM images, used by the ROM loader */
uint8_t *mem_basic_rom_ptr(void);
uint8_t *mem_kernal_rom_ptr(void);
uint8_t *mem_chars_rom_ptr(void);
uint8_t *mem_ram_ptr(void);

uint8_t  mem_read_byte(uint16_t addr);
uint8_t  mem_read_byte_no_io(uint16_t addr);
void     mem_write_byte(uint16_t addr, uint8_t v);
void     mem_write_byte_no_io(uint16_t addr, uint8_t v);
uint16_t mem_read_word(uint16_t addr);
uint16_t mem_read_word_no_io(uint16_t addr);
void     mem_write_word(uint16_t addr, uint16_t v);
void     mem_write_word_no_io(uint16_t addr, uint16_t v);

/* VIC-II view of memory (always RAM except character ROM windows). */
uint8_t  mem_vic_read_byte(uint16_t addr);

#endif /* EMUDORE_CE_MEMORY_H */
