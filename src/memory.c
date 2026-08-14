/*
 * emudore-CE: C64 DRAM + ROM banking (C port of memory.cpp).
 *
 * Differences from the desktop version:
 *  - ROMs are loaded from AppVars via CEPackage instead of <fstream> files.
 *  - The debug dump()/print_screen_text() helpers are removed.
 */
#include <string.h>
#include <stdint.h>
#include "memory.h"
#include "cia1.h"
#include "cia2.h"
#include "vic.h"
#include "util.h"

/* The 64 KiB C64 RAM lives in the second half of VRAM (0xD52C00, 76800
   bytes): graphx runs single-buffered so that half is unused, and it is the
   only spare 64 KiB block on the CE (heap is ~60 KiB, AppVars max ~65.5 KiB).
   The ROMs only need their real 20 KiB, kept as static arrays in BSS. */
#define MEM_RAM_VRAM ((uint8_t *)0xD52C00)

uint8_t *mem_ram;
uint8_t *mem_low;
MemPage mem_rd_pg[256];
MemPage mem_wr_pg[256];
static uint8_t ram_low[MEM_LOW_SIZE]; /* hot C64 pages $00-$0F in fast RAM */
static uint8_t rom_basic[0x2000];
static uint8_t rom_kernal[0x2000];
static uint8_t rom_chars[0x1000];
static uint8_t banks[7];

void mem_init(void)
{
    mem_ram = MEM_RAM_VRAM; /* bytes 0..MEM_LOW_SIZE-1 unused (see ram_low) */
    mem_low = ram_low;
    memset(ram_low, 0, sizeof(ram_low));
    memset(mem_ram + MEM_LOW_SIZE, 0, MEM_SIZE - MEM_LOW_SIZE);
    mem_setup_banks(LORAM | HIRAM | CHAREN);
    mem_write_byte_no_io(ADDR_DATA_DIR, 0x2f);
}

int mem_ready(void)
{
    return mem_ram != NULL;
}

void mem_shutdown(void)
{
    mem_ram = NULL;
}

void mem_setup_banks(uint8_t v)
{
    bool hiram  = (v & HIRAM)  != 0;
    bool loram  = (v & LORAM)  != 0;
    bool charen = (v & CHAREN) != 0;
    int i;

    for (i = 0; i < 7; ++i)
        banks[i] = MEM_BANK_RAM;

    if (hiram)
        banks[MEM_BANK_KERNAL] = MEM_BANK_ROM;
    if (loram && hiram)
        banks[MEM_BANK_BASIC] = MEM_BANK_ROM;
    /* PLA rule: with LORAM=0 and HIRAM=0 the $D000 area is RAM no matter
       what CHAREN says; char ROM shows only for CHAREN=0 with a ROM in. */
    if (!loram && !hiram)
        banks[MEM_BANK_CHAREN] = MEM_BANK_RAM;
    else if (charen)
        banks[MEM_BANK_CHAREN] = MEM_BANK_IO;
    else
        banks[MEM_BANK_CHAREN] = MEM_BANK_ROM;

    /* Rebuild the fast page tables (pointers are biased so p[addr] works). */
    for (i = 0; i < 16; ++i)
    {
        mem_rd_pg[i].ptr = mem_low;
        mem_wr_pg[i].ptr = mem_low;
    }
    for (i = 16; i < 256; ++i)
    {
        mem_rd_pg[i].ptr = mem_ram;
        mem_wr_pg[i].ptr = mem_ram;
    }
    if (banks[MEM_BANK_BASIC] == MEM_BANK_ROM)
        for (i = 0xa0; i <= 0xbf; ++i)
            mem_rd_pg[i].ptr = rom_basic - ADDR_BASIC;
    if (banks[MEM_BANK_KERNAL] == MEM_BANK_ROM)
        for (i = 0xe0; i <= 0xff; ++i)
            mem_rd_pg[i].ptr = rom_kernal - ADDR_KERNAL;
    if (banks[MEM_BANK_CHAREN] == MEM_BANK_IO)
    {
        for (i = 0xd0; i <= 0xd3; ++i)
            mem_rd_pg[i].ptr = mem_wr_pg[i].ptr = NULL;   /* VIC  */
        mem_rd_pg[0xdc].ptr = mem_wr_pg[0xdc].ptr = NULL; /* CIA1 */
        mem_rd_pg[0xdd].ptr = mem_wr_pg[0xdd].ptr = NULL; /* CIA2 */
    }
    else if (banks[MEM_BANK_CHAREN] == MEM_BANK_ROM)
    {
        /* char ROM covers the whole $D000-$DFFF window in this mode */
        for (i = 0xd0; i <= 0xdf; ++i)
            mem_rd_pg[i].ptr = rom_chars - ADDR_CHARS;
    }

    mem_write_byte_no_io(ADDR_MEM_LAYOUT, v);
}

/* Slow paths for the NULL (IO) pages of the tables in memory.h. */
uint8_t mem_read_io(uint16_t addr)
{
    uint16_t page = addr & 0xff00;
    if (page == ADDR_CIA1_PAGE)
        return cia1_read_register(addr & 0x0f);
    if (page == ADDR_CIA2_PAGE)
        return cia2_read_register(addr & 0x0f);
    /* VIC registers mirror every 64 bytes across $D000-$D3FF */
    return vic_read_register(addr & 0x3f);
}

void mem_write_io(uint16_t addr, uint8_t v)
{
    uint16_t page = addr & 0xff00;
    if (page == ADDR_CIA1_PAGE)
        cia1_write_register(addr & 0x0f, v);
    else if (page == ADDR_CIA2_PAGE)
        cia2_write_register(addr & 0x0f, v);
    else
        vic_write_register(addr & 0x3f, v);
}

uint8_t *mem_basic_rom_ptr(void)
{
    return rom_basic;
}

uint8_t *mem_kernal_rom_ptr(void)
{
    return rom_kernal;
}

uint8_t *mem_chars_rom_ptr(void)
{
    return rom_chars;
}

uint8_t *mem_ram_ptr(void)
{
    return mem_ram;
}

void mem_write_word(uint16_t addr, uint16_t v)
{
    mem_write_byte(addr, (uint8_t)v);
    mem_write_byte(addr + 1, (uint8_t)(v >> 8));
}

void mem_write_word_no_io(uint16_t addr, uint16_t v)
{
    mem_write_byte_no_io(addr, (uint8_t)v);
    mem_write_byte_no_io(addr + 1, (uint8_t)(v >> 8));
}

/*
 * The VIC has only 14 address lines; CIA2 supplies the top two. The VIC always
 * reads RAM ignoring the bank configuration, except that the character ROM
 * shows through at $1000-$1FFF and $9000-$9FFF.
 */
uint8_t mem_vic_read_byte(uint16_t addr)
{
    uint16_t vic_addr = cia2_vic_base_address() + (addr & 0x3fff);
    if ((vic_addr >= 0x1000 && vic_addr < 0x2000) ||
        (vic_addr >= 0x9000 && vic_addr < 0xa000))
        return rom_chars[vic_addr & 0xfff];
    return mem_read_byte_no_io(vic_addr);
}
