/*
 * emudore-CE: C64 DRAM + ROM banking (C port of memory.cpp).
 *
 * Differences from the desktop version:
 *  - ROMs are loaded from AppVars via CEPackage instead of <fstream> files.
 *  - The debug dump()/print_screen_text() helpers are removed.
 */
#include <string.h>
#include <stdint.h>
#include <fileioc.h>
#include <ti/vars.h>
#include "memory.h"
#include "cia1.h"
#include "cia2.h"
#include "vic.h"
#include "util.h"

/* The C64 needs 64 KiB of RAM plus a 64 KiB shadow "ROM" image (a write to a
   ROM-mapped address still lands in the hidden RAM underneath, which some
   graphic modes rely on). That's 128 KiB, far more than the default BSS can
   hold, and hardcoding an address collides with graphx. So we allocate it as
   a runtime AppVar: the OS gives us a contiguous block in RAM and we just
   read it via ti_GetDataPtr. No hardcoded addresses, no BSS/stack changes. */
#define MEM_APPVAR_NAME "EM64MEM"
#define MEM_TOTAL_SIZE  (2u * MEM_SIZE)  /* RAM then ROM, one block */

static uint8_t mem_handle;   /* open AppVar handle, 0 = not open */
static uint8_t *mem_ram;     /* -> AppVar data + 0          */
static uint8_t *mem_rom;     /* -> AppVar data + MEM_SIZE   */
static uint8_t banks[7];

void mem_init(void)
{
    /* (Re)create the backing AppVar in RAM at exactly the size we need. */
    mem_handle = ti_OpenVar(MEM_APPVAR_NAME, "w+", OS_TYPE_APPVAR);
    if (!mem_handle)
    {
        dbg_printf("[mem] ti_OpenVar(%s) failed\n", MEM_APPVAR_NAME);
        return;
    }
    if (ti_Resize(MEM_TOTAL_SIZE, mem_handle) != (int)MEM_TOTAL_SIZE)
    {
        dbg_printf("[mem] ti_Resize(%u) failed\n", (unsigned)MEM_TOTAL_SIZE);
        ti_Close(mem_handle);
        mem_handle = 0;
        return;
    }
    mem_ram = (uint8_t *)ti_GetDataPtr(mem_handle);
    if (!mem_ram)
    {
        dbg_printf("[mem] ti_GetDataPtr NULL\n");
        ti_Close(mem_handle);
        mem_handle = 0;
        return;
    }
    mem_rom = mem_ram + MEM_SIZE;
    dbg_printf("[mem] ram=%p rom=%p\n", (void *)mem_ram, (void *)mem_rom);

    memset(mem_ram, 0, MEM_SIZE);
    memset(mem_rom, 0, MEM_SIZE);
    mem_setup_banks(LORAM | HIRAM | CHAREN);
    mem_write_byte_no_io(ADDR_DATA_DIR, 0x2f);
    dbg_printf("[mem] banks set up\n");
}

/* True once the backing AppVar was created and mapped. */
int mem_ready(void)
{
    return mem_handle != 0 && mem_ram != NULL;
}

void mem_shutdown(void)
{
    if (mem_handle)
    {
        ti_Close(mem_handle);
        mem_handle = 0;
    }
    /* Remove the backing AppVar so we don't leave a 128 KiB file behind. */
    ti_DeleteVar(MEM_APPVAR_NAME, OS_TYPE_APPVAR);
    mem_ram = mem_rom = NULL;
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
            retval = mem_rom[addr];
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
        retval = (banks[MEM_BANK_BASIC] == MEM_BANK_ROM) ? mem_rom[addr]
                                                         : mem_ram[addr];
    }
    else if (page >= ADDR_KERNAL_FIRST && page <= ADDR_KERNAL_LAST)
    {
        retval = (banks[MEM_BANK_KERNAL] == MEM_BANK_ROM) ? mem_rom[addr]
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

uint8_t mem_read_byte_rom(uint16_t addr)
{
    return mem_rom[addr];
}

uint8_t *mem_rom_ptr(void)
{
    return mem_rom;
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
        return mem_rom[ADDR_CHARS + (vic_addr & 0xfff)];
    return mem_read_byte_no_io(vic_addr);
}
