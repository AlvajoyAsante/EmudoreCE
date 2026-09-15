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
#include "crc32.h"
#include "jit.h"

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
static const char *disk_package;
static uint8_t selected_file;
uint8_t drive_autostart_stage;
static unsigned int unhide_frame; /* frame at which to lift the hidden boot */
static bool ready_pc_detect;     /* KERNAL wait-loop bytes verified */
static DriveIdentity disk_identity;
static bool identity_valid;

DriveTrace drive_trace[DRIVE_TRACE_MAX];
uint8_t drive_trace_count;

void drive_invalidate(void)
{
    disk_dir = NULL;
    disk_size = 0;
    disk_package = NULL;
    selected_file = 0xff;
    identity_valid = false;
}

bool drive_init(const char *package_id)
{
    uint8_t count;

    drive_invalidate();
    drive_autostart_stage = 0;
    unhide_frame = 0;
    ready_pc_detect = false;
    drive_trace_count = 0;
    if (!package_id)
        return false;
    disk_size = CEPackageSize(package_id);
    if (!disk_size)
        return false;

    /* Uncompressed packages map straight into the archived AppVar; the
       directory always fits in chunk 0 (144 * 22 + 1 < 32 KiB). */
    disk_dir = CEPackageGetDirectChunk(package_id, 0, disk_size);
    if (!disk_dir)
    {
        disk_size = 0;
        return false;
    }

    count = disk_dir[0];
    if (!count || 1 + (uint32_t)count * DIR_ENTRY_SIZE > disk_size)
    {
        drive_invalidate();
        return false;
    }
    disk_package = package_id;
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
        if (c == 0xa0)
            c = ' ';
        else if (c >= 0xc1 && c <= 0xda)
            c -= 0x80;
        else if (c < 0x20 || c > 0x7e)
            c = '?';
        buffer[i] = (char)c;
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

void drive_disable(void)
{
    drive_invalidate();
}

void drive_suppress_autoload(void)
{
    drive_autostart_stage = 2;
    ready_pc_detect = false;
    unhide_frame = 0;
}

void drive_patch_kernal(void)
{
    /* $F4A5 (KERNAL offset $14A5) is STA $93, the first instruction of the
       default ILOAD routine. Verify before patching so a different KERNAL
       image is left untouched. Planted even with no disk: the trap then
       answers DEVICE NOT PRESENT instantly instead of hanging for minutes
       in the KERNAL's IEC delay loops with nothing on the bus. */
    uint8_t *k = mem_kernal_rom_ptr();
    if (k[0x14a5] == 0x85 && k[0x14a6] == 0x93)
        k[0x14a5] = DRIVE_TRAP_OPCODE;
    /* $E5CD wait-for-key loop: LDA $C6 / STA $CC / STA $0292 / BEQ $E5CD.
       If present, drive_tick detects the READY prompt by pc instead of the
       fixed 60-frame safety wait. ($E5CD - $E000 = offset $05CD.) */
    ready_pc_detect = k[0x05cd] == 0xa5 && k[0x05ce] == 0xc6 &&
                      k[0x05cf] == 0x85 && k[0x05d0] == 0xcc &&
                      k[0x05d4] == 0xf0 && k[0x05d5] == 0xf7;
}

static uint32_t read24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

bool drive_file_info(uint8_t index, DriveFileInfo *info)
{
    const uint8_t *entry;
    const uint8_t *low;
    const uint8_t *high;
    uint32_t offset;
    uint32_t size;
    if (!info || !disk_size || index >= drive_file_count())
        return false;
    entry = disk_dir + 1 + (uint32_t)index * DIR_ENTRY_SIZE;
    offset = read24(entry + DIR_NAME_LEN);
    size = read24(entry + DIR_NAME_LEN + 3);
    if (offset < 1 + (uint32_t)drive_file_count() * DIR_ENTRY_SIZE ||
        offset > disk_size || size < 2 || size > disk_size - offset)
        return false;
    low = CEPackageChunkPtr(disk_package, offset, disk_size);
    high = CEPackageChunkPtr(disk_package, offset + 1, disk_size);
    if (!low || !high)
        return false;
    drive_file_name(index, info->name, sizeof(info->name));
    info->size = size;
    info->load_address = (uint16_t)*low | ((uint16_t)*high << 8);
    return true;
}

bool drive_identity(DriveIdentity *identity)
{
    uint32_t offset;
    uint32_t crc = 0;
    if (!identity || !disk_size || !disk_dir)
        return false;
    if (!identity_valid)
    {
        for (offset = 0; offset < disk_size; offset += CHUNK_PAYLOAD)
        {
            uint32_t remaining = disk_size - offset;
            size_t length = remaining > CHUNK_PAYLOAD ? CHUNK_PAYLOAD : remaining;
            const uint8_t *data = CEPackageGetDirectChunk(
                disk_package, (uint8_t)(offset / CHUNK_PAYLOAD), disk_size);
            if (!data)
                return false;
            crc = crc32_update(crc, data, length);
        }
        disk_identity.size = disk_size;
        disk_identity.crc32 = crc;
        identity_valid = true;
    }
    *identity = disk_identity;
    return true;
}

/* One byte of the disk payload (offsets can straddle chunk boundaries). */
static uint8_t disk_byte(uint32_t offset)
{
    const uint8_t *p = CEPackageChunkPtr(disk_package, offset, disk_size);
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

    if (name_len >= 2 && mem_read_byte(name_addr) == '0' &&
        mem_read_byte((uint16_t)(name_addr + 1)) == ':')
    {
        name_addr = (uint16_t)(name_addr + 2);
        name_len -= 2;
    }

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
        const uint8_t *src = CEPackageChunkPtr(disk_package, offset, disk_size);
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
            jit_invalidate_range(addr, span);
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
    if (cpu.pc != 0xf4a6 ||
        mem_rd_pg[0xf4].ptr == NULL ||
        mem_rd_pg[0xf4].ptr[0xf4a5] != DRIVE_TRAP_OPCODE)
        return false;

    if (!disk_size)
    {
        mem_low[ZP_STATUS] = 0;
        cpu.a = 5; /* DEVICE NOT PRESENT, without the IEC timeout crawl */
        cpu.cf = true;
        do_rts();
        return true;
    }

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
        unhide_frame = vic_frames() + 1;
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
    /* covers both RUN-typed and $0302-autostart games */
    unhide_frame = vic_frames() + 12;
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

    if (unhide_frame && (int)(vic_frames() - unhide_frame) >= 0)
    {
        unhide_frame = 0;
        vic_set_boot_hidden(false);
        vic_request_render();
    }

    /* Stage 2: after the load, queue RUN so single-file games self-start. */
    if (drive_autostart_stage == 1 && drive_trace_count == 1)
    {
        /* The keyboard bytes become idle before LOAD has returned to BASIC.
           Queue RUN only after the verified READY loop is actually reached. */
        if (mem_low[ZP_BLNSW] != 0 || mem_low[ZP_NDX] != 0)
            return;
        if (ready_pc_detect && (cpu.pc < 0xe5cd || cpu.pc > 0xe5d5))
            return;
        if (!ready_pc_detect && vic_frames() < 120)
            return;
        for (i = 0; i < 4; ++i)
            mem_low[ADDR_KEYBUF + i] = run_cmd[i];
        mem_low[ZP_NDX] = 4;
        drive_autostart_stage = 2;
        vic_request_render();
        return;
    }
    if (drive_autostart_stage)
        return;

    /* Wait for the READY prompt: keyboard buffer idle and the pc parked in
       the $E5CD wait-for-key loop (first entered exactly at READY). This
       replaces the old fixed 60-frame (~1.2M cycle) safety wait; the frame
       gates remain as fallback for unverified KERNAL images / pc sampling. */
    if (mem_low[ZP_BLNSW] != 0 || mem_low[ZP_NDX] != 0)
        return;
    if (ready_pc_detect)
    {
        if ((cpu.pc < 0xe5cd || cpu.pc > 0xe5d5) && vic_frames() < 120)
            return;
    }
    else if (vic_frames() < 60)
        return;

    for (i = 0; i < KEYBUF_MAX; ++i)
        mem_low[ADDR_KEYBUF + i] = command[i];
    mem_low[ZP_NDX] = KEYBUF_MAX;
    drive_autostart_stage = 1;
    vic_request_render();
}
