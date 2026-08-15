/*
 * emudore-CE: virtual 1541 "fast load" drive (see drive.h).
 *
 * IMPORTANT: no dbg_printf()/sprintf anywhere in this file. drive_init()
 * returns a bool (in A via IX on the eZ80) and drive_trap_load() runs inside
 * the CPU hot loop; sprintf loads the debug-port constant into IX and
 * clobbers frames/return values (see the c64.c lore).
 */
#include <stdint.h>
#include <string.h>
#include "drive.h"
#include "cpu.h"
#include "memory.h"
#include "vic.h"
#include "CEPackage.h"

/* DSK payload layout (see tools/d64_to_appvar.py):
   u8 count, then count * 22-byte entries { name[16] PETSCII $A0-padded,
   u24 data offset, u24 data size }, then the raw PRG blobs. */
#define DIR_ENTRY_SIZE 22
#define DIR_NAME_LEN   16
#define CHUNK_PAYLOAD  0x8000u

/* KERNAL zero-page/OS locations used by LOAD */
#define ZP_STATUS   0x90 /* ST io status word            */
#define ZP_VERIFY   0x93 /* load(0) / verify(1) flag     */
#define ZP_EAL      0xAE /* end-of-load address (2 bytes)*/
#define ZP_SECADR   0xB9 /* secondary address            */
#define ZP_FNLEN    0xB7 /* filename length              */
#define ZP_FNADR    0xBB /* filename pointer (2 bytes)   */
#define ZP_MEMUSS   0xC3 /* relocation target (2 bytes)  */
#define ZP_NDX      0xC6 /* keyboard buffer count        */
#define ZP_BLNSW    0xCC /* 0 = cursor blinking (input)  */
#define ADDR_KEYBUF 0x0277
#define KEYBUF_MAX  10

static uint32_t disk_size;       /* 0 = no disk package present */
static const uint8_t *disk_dir;  /* chunk 0: count byte + directory entries */
static uint8_t selected_file;
static uint8_t autostart_done;

DriveTrace drive_trace[DRIVE_TRACE_MAX];
uint8_t drive_trace_count;

bool drive_init(void)
{
    uint8_t count;

    selected_file = 0xff;
    disk_size = CEPackageSize(CE_PACKAGE_DISK);
    if (!disk_size)
        return false;

    /* Uncompressed packages map straight into the archived AppVar; the
       directory always fits in chunk 0 (144 * 22 + 1 < 32 KiB). */
    disk_dir = CEPackageGetDirectChunk(CE_PACKAGE_DISK, 0, disk_size);
    if (!disk_dir)
    {
        disk_size = 0;
        return false;
    }

    count = disk_dir[0];
    if (!count || 1 + (uint32_t)count * DIR_ENTRY_SIZE > disk_size)
    {
        disk_size = 0;
        return false;
    }
    return true;
}

uint8_t drive_file_count(void)
{
    return disk_dir ? disk_dir[0] : 0;
}

int drive_file_name(uint8_t index, char *buffer, uint8_t bufferSize)
{
    const uint8_t *entry;
    uint8_t i;
    uint8_t count = drive_file_count();

    if (!buffer || !bufferSize || index >= count)
        return 0;

    entry = disk_dir + 1 + (uint32_t)index * DIR_ENTRY_SIZE;
    for (i = 0; i < DIR_NAME_LEN && i + 1 < bufferSize; ++i)
    {
        uint8_t c = entry[i];
        buffer[i] = (c == 0xa0) ? ' ' : (char)c;
    }
    while (i > 0 && buffer[i - 1] == ' ')
        --i;
    buffer[i] = '\0';
    return 1;
}

void drive_select_file(uint8_t index)
{
    selected_file = index < drive_file_count() ? index : 0xff;
}

void drive_patch_kernal(void)
{
    /* $F4A5 (KERNAL offset $14A5) is STA $93, the first instruction of the
       default ILOAD routine. Verify before patching so a different KERNAL
       image is left untouched. */
    uint8_t *k = mem_kernal_rom_ptr();
    if (disk_size && k[0x14a5] == 0x85 && k[0x14a6] == 0x93)
        k[0x14a5] = DRIVE_TRAP_OPCODE;
}

static uint32_t read24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/* One byte of the disk payload (offsets can straddle chunk boundaries). */
static uint8_t disk_byte(uint32_t offset)
{
    const uint8_t *p = CEPackageChunkPtr(CE_PACKAGE_DISK, offset, disk_size);
    return p ? *p : 0;
}

/* CBM filename match: '*' matches the rest, '?' matches any single
   character; shorter patterns must end where the $A0 padding starts. */
static bool name_matches(const uint8_t *entry_name, uint16_t name_addr,
                         uint8_t name_len)
{
    uint8_t i;

    if (!name_len)
        return true; /* empty name: first file */

    for (i = 0; i < name_len; ++i)
    {
        uint8_t c = mem_read_byte(name_addr + i);
        if (c == '*')
            return true;
        if (i >= DIR_NAME_LEN)
            return false;
        if (c == '?' && entry_name[i] != 0xa0)
            continue;
        if (c != entry_name[i])
            return false;
    }
    return name_len >= DIR_NAME_LEN || entry_name[name_len] == 0xa0;
}

static const uint8_t *find_entry(uint16_t name_addr, uint8_t name_len)
{
    const uint8_t *entry = disk_dir + 1;
    uint8_t count = disk_dir[0];
    uint8_t i;

    if (selected_file < count && name_len == 1 && mem_read_byte(name_addr) == '*')
        return entry + (uint32_t)selected_file * DIR_ENTRY_SIZE;

    for (i = 0; i < count; ++i, entry += DIR_ENTRY_SIZE)
        if (name_matches(entry, name_addr, name_len))
            return entry;
    return NULL;
}

/* Copy file bytes from the archived AppVar into C64 RAM as block copies
   (memcpy lowers to LDIR), split only at chunk boundaries, the mem_low/
   mem_ram backing seam at $1000, and the 16-bit address wrap. A real LOAD
   always lands in RAM regardless of ROM banking. */
static void disk_copy_to_ram(uint32_t offset, uint16_t addr, uint32_t count)
{
    while (count)
    {
        const uint8_t *src = CEPackageChunkPtr(CE_PACKAGE_DISK, offset, disk_size);
        unsigned int run = CHUNK_PAYLOAD - (unsigned int)(offset & (CHUNK_PAYLOAD - 1));
        if (!src)
            return; /* cannot happen for a validated package */
        if ((uint32_t)run > count)
            run = (unsigned int)count;
        offset += run;
        count -= run;
        while (run)
        {
            unsigned int span = run;
            if (addr < MEM_LOW_SIZE)
            {
                unsigned int seam = MEM_LOW_SIZE - addr;
                if (span > seam)
                    span = seam;
            }
            else
            {
                unsigned int wrap = (unsigned int)(0x10000ul - addr);
                if (span > wrap)
                    span = wrap;
            }
            memcpy(mem_host_ptr(addr), src, span);
            addr = (uint16_t)(addr + span);
            src += span;
            run -= span;
        }
    }
}

static void do_rts(void)
{
    uint8_t lo, hi;
    ++cpu.sp;
    lo = mem_low[ADDR_STACK + cpu.sp];
    ++cpu.sp;
    hi = mem_low[ADDR_STACK + cpu.sp];
    cpu.pc = (uint16_t)(((uint16_t)hi << 8 | lo) + 1);
}

static void trace_load(uint16_t name_addr, uint8_t entry_index,
                       uint16_t addr, uint16_t end)
{
    DriveTrace *t;
    uint8_t i;

    if (drive_trace_count < DRIVE_TRACE_MAX)
    {
        t = &drive_trace[drive_trace_count];
        t->caller = cpu.pc; /* do_rts already ran: pc = LOAD caller */
        t->addr = addr;
        t->end = end;
        t->entry = entry_index;
        t->sa = mem_low[ZP_SECADR];
        t->len = mem_low[ZP_FNLEN];
        for (i = 0; i < 3; ++i)
            t->name[i] = i < t->len ? mem_read_byte(name_addr + i) : ' ';
    }
    ++drive_trace_count;
}

/* Rally Driver-style resident IEC fastloader: uploads a program into the
   1541 (M-W/M-E) and clocks bytes in by bit-banging $DD00 - unservable
   without a drive CPU. Its resident copy sits at a fixed address, so when
   its entry signature appears in RAM (files can reload it) we plant a trap
   opcode on it and serve the load ourselves. */
#define FASTLOAD_ENTRY 0x1a4e
static const uint8_t fastload_sig[16] = {
    0xa9, 0x00, 0x85, 0x93,       /* LDA #$00, STA $93       */
    0x78,                         /* SEI                     */
    0xa6, 0xb9, 0x86, 0xb8,       /* LDX $B9, STX $B8        */
    0xa9, 0x60, 0x85, 0xb9,       /* LDA #$60, STA $B9       */
    0x20, 0xd5, 0xf3              /* JSR $F3D5 (open w/name) */
};

/* Plant the fastloader trap whenever the signature is present in RAM
   (called after every served load: parts can overwrite/reload the loader). */
static void plant_fastload_trap(void)
{
    uint8_t i;
    uint8_t first = *mem_host_ptr(FASTLOAD_ENTRY);

    if (first != fastload_sig[0] && first != DRIVE_TRAP2_OPCODE)
        return;
    for (i = 1; i < sizeof(fastload_sig); ++i)
        if (*mem_host_ptr((uint16_t)(FASTLOAD_ENTRY + i)) != fastload_sig[i])
            return;
    *mem_host_ptr(FASTLOAD_ENTRY) = DRIVE_TRAP2_OPCODE;
}

bool drive_trap_load(void)
{
    uint16_t addr, end, name_addr;
    uint32_t offset, size;
    const uint8_t *entry;
    bool verify;

    /* Only honour the trap when it was fetched from the patched KERNAL at
       $F4A5 (pc already advanced past the opcode) with the ROM banked in. */
    if (cpu.pc != 0xf4a6 || !disk_size ||
        mem_rd_pg[0xf4].ptr == NULL ||
        mem_rd_pg[0xf4].ptr[0xf4a5] != DRIVE_TRAP_OPCODE)
        return false;

    verify = cpu.a != 0; /* $F4A5 is entered with the verify flag in A */
    name_addr = (uint16_t)(mem_low[ZP_FNADR] | (mem_low[ZP_FNADR + 1] << 8));
    entry = find_entry(name_addr, mem_low[ZP_FNLEN]);

    if (!entry)
    {
        mem_low[ZP_STATUS] = 0;
        cpu.a = 4; /* FILE NOT FOUND */
        cpu.cf = true;
        do_rts();
        trace_load(name_addr, 0xff, 0, 0);
        return true;
    }

    offset = read24(entry + DIR_NAME_LEN);
    size = read24(entry + DIR_NAME_LEN + 3);

    /* secondary address 0 relocates to MEMUSS, otherwise the file's own
       load address (first two payload bytes) is used */
    addr = (uint16_t)(disk_byte(offset) | (disk_byte(offset + 1) << 8));
    if (mem_low[ZP_SECADR] == 0)
        addr = (uint16_t)(mem_low[ZP_MEMUSS] | (mem_low[ZP_MEMUSS + 1] << 8));

    if (!verify)
        disk_copy_to_ram(offset + 2, addr, size - 2);

    end = (uint16_t)(addr + (size - 2));
    mem_low[ZP_EAL] = (uint8_t)end;
    mem_low[ZP_EAL + 1] = (uint8_t)(end >> 8);
    mem_low[ZP_STATUS] = 0x40; /* EOF: the whole file was read */
    cpu.x = (uint8_t)end;
    cpu.y = (uint8_t)(end >> 8);
    cpu.a = 0;
    cpu.cf = false;
    cpu.cycles += 100;
    do_rts();
    trace_load(name_addr, (uint8_t)((entry - (disk_dir + 1)) / DIR_ENTRY_SIZE),
               addr, end);
    plant_fastload_trap();
    return true;
}

/* Serve a call into the game's resident fastloader at $1A4E. Mimics the
   routine's exit state: data stored, end address in $AC/$AD and X/Y,
   $B8 = entry SA, $B9 = $60, ST clear, carry clear. */
bool drive_trap_fastload(void)
{
    uint16_t addr, end, name_addr;
    uint32_t offset, size;
    const uint8_t *entry;
    uint8_t sa;

    if (cpu.pc != FASTLOAD_ENTRY + 1 || !disk_size ||
        *mem_host_ptr(FASTLOAD_ENTRY) != DRIVE_TRAP2_OPCODE)
        return false;

    sa = mem_low[ZP_SECADR];
    name_addr = (uint16_t)(mem_low[ZP_FNADR] | (mem_low[ZP_FNADR + 1] << 8));
    entry = find_entry(name_addr, mem_low[ZP_FNLEN]);

    if (!entry)
    {
        /* game disks always hit; report DEVICE-style failure and bail */
        mem_low[ZP_STATUS] = 0x02;
        cpu.cf = true;
        do_rts();
        trace_load(name_addr, 0xff, 0, 0);
        return true;
    }

    offset = read24(entry + DIR_NAME_LEN);
    size = read24(entry + DIR_NAME_LEN + 3);

    /* SA 0 relocates to MEMUSS ($C3/$C4), like the real routine's $1A1D */
    addr = (uint16_t)(disk_byte(offset) | (disk_byte(offset + 1) << 8));
    if (sa == 0)
        addr = (uint16_t)(mem_low[ZP_MEMUSS] | (mem_low[ZP_MEMUSS + 1] << 8));

    disk_copy_to_ram(offset + 2, addr, size - 2);

    end = (uint16_t)(addr + (size - 2));
    mem_low[0xac] = (uint8_t)end;        /* running store pointer */
    mem_low[0xad] = (uint8_t)(end >> 8);
    mem_low[0xb8] = sa;                  /* routine stashes SA in $B8 */
    mem_low[ZP_SECADR] = 0x60;
    mem_low[ZP_STATUS] = 0;
    cpu.x = (uint8_t)end;
    cpu.y = (uint8_t)(end >> 8);
    cpu.a = 0;
    cpu.cf = false;
    cpu.cycles += 100;
    do_rts();
    trace_load(name_addr, (uint8_t)((entry - (disk_dir + 1)) / DIR_ENTRY_SIZE),
               addr, end);
    plant_fastload_trap();
    return true;
}

void drive_tick(void)
{
    /* PETSCII for lO"*",8,1 + RETURN: the L / shift-O keyword abbreviation
       keeps the command within the 10-byte keyboard buffer. */
    static const uint8_t command[KEYBUF_MAX] = {
        0x4c, 0xcf, 0x22, 0x2a, 0x22, 0x2c, 0x38, 0x2c, 0x31, 0x0d
    };
    /* RUN + RETURN, typed after a single-file load finishes. */
    static const uint8_t run_cmd[4] = { 0x52, 0x55, 0x4e, 0x0d };
    uint8_t i;

    if (!disk_size)
        return;

    /* Stage 2: after the load, queue RUN so single-file games self-start. */
    if (autostart_done == 1 && drive_trace_count == 1)
    {
        /* Wait until BASIC is back at the READY input loop. */
        if (mem_low[ZP_BLNSW] != 0 || mem_low[ZP_NDX] != 0)
            return;
        for (i = 0; i < 4; ++i)
            mem_low[ADDR_KEYBUF + i] = run_cmd[i];
        mem_low[ZP_NDX] = 4;
        autostart_done = 2;
        vic_request_render();
        return;
    }
    if (autostart_done)
        return;

    /* Wait for the READY prompt: input loop reached (cursor blink enabled),
       keyboard buffer idle, and a safety margin past the reset. */
    if (vic_frames() < 60 || mem_low[ZP_BLNSW] != 0 || mem_low[ZP_NDX] != 0)
        return;

    for (i = 0; i < KEYBUF_MAX; ++i)
        mem_low[ADDR_KEYBUF + i] = command[i];
    mem_low[ZP_NDX] = KEYBUF_MAX;
    autostart_done = 1;
    vic_request_render();
}
