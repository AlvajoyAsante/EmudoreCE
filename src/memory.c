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

static uint8_t *mem_ram;
static uint8_t rom_basic[0x2000];
static uint8_t rom_kernal[0x2000];
static uint8_t rom_chars[0x1000];
static uint8_t banks[7];

void mem_init(void)
{
    mem_ram = MEM_RAM_VRAM;
    memset(mem_ram, 0, MEM_SIZE);
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
    if (charen && (loram || hiram))
        banks[MEM_BANK_CHAREN] = MEM_BANK_IO;
    else if (charen && !loram && !hiram)
        banks[MEM_BANK_CHAREN] = MEM_BANK_RAM;
    else
        banks[MEM_BANK_CHAREN] = MEM_BANK_ROM;

    mem_write_byte_no_io(ADDR_MEM_LAYOUT, v);
}

void mem_write_byte_no_io(uint16_t addr, uint8_t v)
{
    mem_ram[addr] = v;
}

void mem_write_byte(uint16_t addr, uint8_t v)
{
    uint16_t page = addr & 0xff00;

    if (page == ADDR_ZERO_PAGE)
    {
        if (addr == ADDR_MEM_LAYOUT)
            mem_setup_banks(v);
        else
            mem_ram[addr] = v;
    }
    else if (page >= ADDR_VIC_FIRST_PAGE && page <= ADDR_VIC_LAST_PAGE)
    {
        if (banks[MEM_BANK_CHAREN] == MEM_BANK_IO)
            vic_write_register(addr & 0x7f, v);
        else
            mem_ram[addr] = v;
    }
    else if (page == ADDR_CIA1_PAGE)
    {
        if (banks[MEM_BANK_CHAREN] == MEM_BANK_IO)
            cia1_write_register(addr & 0x0f, v);
        else
            mem_ram[addr] = v;
    }
    else if (page == ADDR_CIA2_PAGE)
    {
        if (banks[MEM_BANK_CHAREN] == MEM_BANK_IO)
            cia2_write_register(addr & 0x0f, v);
        else
            mem_ram[addr] = v;
    }
    else
    {
        mem_ram[addr] = v;
    }
}

uint8_t mem_read_byte(uint16_t addr)
{
    uint8_t retval = 0;
    uint16_t page = addr & 0xff00;

    if (page >= ADDR_VIC_FIRST_PAGE && page <= ADDR_VIC_LAST_PAGE)
    {
        if (banks[MEM_BANK_CHAREN] == MEM_BANK_IO)
            retval = vic_read_register(addr & 0x7f);
        else if (banks[MEM_BANK_CHAREN] == MEM_BANK_ROM)
            retval = rom_chars[addr - ADDR_CHARS];
        else
            retval = mem_ram[addr];
    }
    else if (page == ADDR_CIA1_PAGE)
    {
        if (banks[MEM_BANK_CHAREN] == MEM_BANK_IO)
            retval = cia1_read_register(addr & 0x0f);
        else
            retval = mem_ram[addr];
    }
    else if (page == ADDR_CIA2_PAGE)
    {
        if (banks[MEM_BANK_CHAREN] == MEM_BANK_IO)
            retval = cia2_read_register(addr & 0x0f);
        else
            retval = mem_ram[addr];
    }
    else if (page >= ADDR_BASIC_FIRST && page <= ADDR_BASIC_LAST)
    {
        retval = (banks[MEM_BANK_BASIC] == MEM_BANK_ROM)
                     ? rom_basic[addr - ADDR_BASIC]
                     : mem_ram[addr];
    }
    else if (page >= ADDR_KERNAL_FIRST)
    {
        retval = (banks[MEM_BANK_KERNAL] == MEM_BANK_ROM)
                     ? rom_kernal[addr - ADDR_KERNAL]
                     : mem_ram[addr];
    }
    else
    {
        retval = mem_ram[addr];
    }
    return retval;
}

uint8_t mem_read_byte_no_io(uint16_t addr)
{
    return mem_ram[addr];
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

uint16_t mem_read_word(uint16_t addr)
{
    return mem_read_byte(addr) | (mem_read_byte(addr + 1) << 8);
}

uint16_t mem_read_word_no_io(uint16_t addr)
{
    return mem_read_byte_no_io(addr) | (mem_read_byte_no_io(addr + 1) << 8);
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
