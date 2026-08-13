/*
 * emudore-CE: machine glue (C port of c64.cpp).
 *
 * The constructor/destructor become c64_init()/c64_shutdown(). The desktop
 * file-loading (Loader) and the r2 Debugger are removed; ROMs are loaded from
 * AppVars here instead.
 */
#include <stdlib.h>
#include "c64.h"
#include "cpu.h"
#include "memory.h"
#include "cia1.h"
#include "cia2.h"
#include "vic.h"
#include "io.h"
#include "util.h"
#include "CEPackage.h"

static bool load_roms(void)
{
    dbg_printf("[c64] rom buffer base=%p\n", (void *)mem_rom_ptr());
    /* BASIC 8 KiB -> $A000, KERNAL 8 KiB -> $E000, CharGen 4 KiB -> $D000 */
    dbg_printf("[c64] loading BASIC\n");
    if (!CEPackageRead(CE_PACKAGE_BASIC, mem_rom_ptr() + ADDR_BASIC, 0x2000))
        return false;
    dbg_printf("[c64] loading KERNAL\n");
    if (!CEPackageRead(CE_PACKAGE_KERNAL, mem_rom_ptr() + ADDR_KERNAL, 0x2000))
        return false;
    dbg_printf("[c64] loading CHARS\n");
    if (!CEPackageRead(CE_PACKAGE_CHARS, mem_rom_ptr() + ADDR_CHARS, 0x1000))
        return false;
    dbg_printf("[c64] ROMs loaded ok\n");
    return true;
}

bool c64_init(void)
{
    io_init();
    mem_init();
    if (!mem_ready())
    {
        dbg_printf("[c64] memory backing store failed\n");
        return false;
    }
    if (!load_roms())
    {
        dbg_printf("[c64] load_roms failed\n");
        return false;
    }
    cia1_reset();
    cia2_reset();
    vic_reset();
    cpu_reset();
    dbg_printf("[c64] init ok, pc=%04x\n", cpu.pc);
    return true;
}

void c64_run(void)
{
    bool running = true;
    unsigned long iter = 0;
    dbg_printf("[c64] run loop start, pc=%04x\n", cpu.pc);
    while (running)
    {
        if (!cia1_emulate())
        {
            dbg_printf("[c64] cia1_emulate false\n");
            break;
        }
        if (!cia2_emulate())
        {
            dbg_printf("[c64] cia2_emulate false\n");
            break;
        }
        if (!cpu_emulate())
        {
            dbg_printf("[c64] cpu_emulate false (bad opcode) pc=%04x\n", cpu.pc);
            break;
        }
        if (!vic_emulate())
        {
            dbg_printf("[c64] vic_emulate false\n");
            break;
        }
        if (!io_emulate())
            break; /* returns false when CLEAR is pressed */
        ++iter;
        if (iter == 1 || (iter % 100000) == 0)
            dbg_printf("[c64] iter=%lu pc=%04x cycles=%u\n", iter, cpu.pc, cpu.cycles);
    }
    dbg_printf("[c64] loop exited after %lu iters\n", iter);
}

void c64_shutdown(void)
{
    mem_shutdown();
    io_shutdown();
}
