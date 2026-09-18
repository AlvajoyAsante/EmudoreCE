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

/* 1541 D64 disk geometry */
#define D64_NUM_TRACKS  35
#define D64_RAW_SIZE_35 174848u
#define D64_RAW_SIZE_40 196608u
#define D64_MAX_SECTORS 768u
#define D64_DIR_TRACK   18
#define D64_DIR_SECTOR  1
#define D64_BAM_TRACK   18
#define D64_BAM_SECTOR  0
#define MAX_D64_FILES   144

static const uint16_t d64_track_start_sector[42] = {
    0,   0,  21,  42,  63,  84, 105, 126, 147, 168, 189,
  210, 231, 252, 273, 294, 315, 336, 357, 376, 395, 414,
  433, 452, 471, 490, 508, 526, 544, 562, 580, 598, 615,
  632, 649, 666, 683, 700, 717, 734, 751, 768
};

static const uint8_t d64_track_sector_count[42] = {
    0,  21,  21,  21,  21,  21,  21,  21,  21,  21,  21,
   21,  21,  21,  21,  21,  21,  21,  19,  19,  19,  19,
   19,  19,  19,  18,  18,  18,  18,  18,  18,  17,  17,
   17,  17,  17,  17,  17,  17,  17,  17,   0
};

typedef struct
{
    uint8_t track;       /* starting data track */
    uint8_t sector;      /* starting data sector */
    uint8_t dir_track;   /* directory sector track */
    uint8_t dir_sector;  /* directory sector sector */
    uint8_t slot;        /* slot 0..7 in directory sector */
    uint8_t type;        /* file type byte */
    uint8_t geos_struct; /* GEOS structure: 0=Seq, 1=VLIR */
    uint8_t geos_type;   /* GEOS file type */
    uint16_t blocks;     /* size in blocks */
} RawD64Entry;

#define DRIVE_SECTOR_CACHE_MAX 4
typedef struct
{
    uint8_t track;
    uint8_t sector;
    bool valid;
    uint8_t data[256];
} DriveSectorCache;

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

static DriveSectorCache sector_cache[DRIVE_SECTOR_CACHE_MAX];
static RawD64Entry d64_entries[MAX_D64_FILES];
static uint8_t d64_entry_count;
static bool disk_is_raw_d64;
static bool disk_is_geos;
static bool geos_bootstrapped;

DriveTrace drive_trace[DRIVE_TRACE_MAX];
uint8_t drive_trace_count;
unsigned int drive_geos_call_count;

static uint32_t d64_sector_offset(uint8_t track, uint8_t sector)
{
    if (track == 0 || track > 40 || sector >= d64_track_sector_count[track])
        return 0xfffffffful;
    return (uint32_t)(d64_track_start_sector[track] + sector) * 256u;
}

bool drive_write_sector(uint8_t track, uint8_t sector, const uint8_t *source)
{
    uint8_t i;
    if (!source || track == 0 || track > 40)
        return false;
    for (i = 0; i < DRIVE_SECTOR_CACHE_MAX; ++i)
    {
        if (sector_cache[i].valid && sector_cache[i].track == track && sector_cache[i].sector == sector)
        {
            memcpy(sector_cache[i].data, source, 256);
            return true;
        }
    }
    for (i = 0; i < DRIVE_SECTOR_CACHE_MAX; ++i)
    {
        if (!sector_cache[i].valid)
        {
            sector_cache[i].valid = true;
            sector_cache[i].track = track;
            sector_cache[i].sector = sector;
            memcpy(sector_cache[i].data, source, 256);
            return true;
        }
    }
    sector_cache[0].valid = true;
    sector_cache[0].track = track;
    sector_cache[0].sector = sector;
    memcpy(sector_cache[0].data, source, 256);
    return true;
}

/* Borrowed, read-only sector view: consume before writes, disk changes or
   CEPackageCloseDirect(). Reads never populate/evict the modified-sector
   overlay. Aligned 256-byte sectors fit wholly within 32 KiB package chunks. */
static const uint8_t *drive_sector_ptr(uint8_t track, uint8_t sector)
{
    uint32_t offset;
    uint8_t i;

    if (!disk_size || !disk_package)
        return NULL;

    for (i = 0; i < DRIVE_SECTOR_CACHE_MAX; ++i)
    {
        if (sector_cache[i].valid && sector_cache[i].track == track && sector_cache[i].sector == sector)
            return sector_cache[i].data;
    }

    offset = d64_sector_offset(track, sector);
    if (offset == 0xfffffffful || offset + 256 > disk_size)
        return NULL;

    return CEPackageChunkPtr(disk_package, offset, disk_size);
}

bool drive_read_sector(uint8_t track, uint8_t sector, uint8_t *destination)
{
    const uint8_t *src;
    if (!destination)
        return false;
    src = drive_sector_ptr(track, sector);
    if (!src)
        return false;

    memcpy(destination, src, 256);
    return true;
}

bool drive_is_d64(void)
{
    return disk_is_raw_d64;
}

bool drive_is_geos(void)
{
    return disk_is_geos;
}

void drive_install_geos_driver(void)
{
    uint8_t i;
    for (i = 0; i <= 0x0c; ++i)
    {
        uint16_t addr = (uint16_t)(0x9d80 + (uint16_t)i * 3);
        *mem_host_ptr(addr) = DRIVE_GEOS_TRAP_OPCODE;
        *mem_host_ptr((uint16_t)(addr + 1)) = i;
        *mem_host_ptr((uint16_t)(addr + 2)) = 0x60; /* RTS */
        jit_invalidate_range(addr, 3);
    }
    *mem_host_ptr(0x8489) = 1; /* _driveType = 1541 */
    if (mem_low[0xba] == 0)
        mem_low[0xba] = 8;
}

void drive_invalidate(void)
{
    uint8_t i;
    disk_dir = NULL;
    disk_size = 0;
    disk_package = NULL;
    selected_file = 0xff;
    identity_valid = false;
    disk_is_raw_d64 = false;
    disk_is_geos = false;
    geos_bootstrapped = false;
    d64_entry_count = 0;
    for (i = 0; i < DRIVE_SECTOR_CACHE_MAX; ++i)
        sector_cache[i].valid = false;
}

static void scan_raw_d64(void)
{
    uint8_t dir_track = D64_DIR_TRACK;
    uint8_t dir_sector = D64_DIR_SECTOR;
    uint8_t sector_buf[256];
    uint8_t visited[40];
    uint8_t visit_count = 0;
    uint8_t i;

    d64_entry_count = 0;

    while (dir_track != 0 && visit_count < 35 && d64_entry_count < MAX_D64_FILES)
    {
        for (i = 0; i < visit_count; ++i)
            if (visited[i] == dir_sector && dir_track == D64_DIR_TRACK)
                return;
        visited[visit_count++] = dir_sector;

        if (!drive_read_sector(dir_track, dir_sector, sector_buf))
            break;

        for (i = 0; i < 8 && d64_entry_count < MAX_D64_FILES; ++i)
        {
            const uint8_t *entry = sector_buf + 2 + i * 32;
            uint8_t file_type = entry[0];
            uint8_t actual_type = file_type & 0x07;
            if ((file_type & 0x80) && actual_type != 0 && actual_type <= 4)
            {
                RawD64Entry *e = &d64_entries[d64_entry_count];
                e->track = entry[1];
                e->sector = entry[2];
                e->dir_track = dir_track;
                e->dir_sector = dir_sector;
                e->slot = i;
                e->type = file_type;
                e->geos_struct = entry[21];
                e->geos_type = entry[22];
                e->blocks = (uint16_t)entry[28] | ((uint16_t)entry[29] << 8);
                ++d64_entry_count;
            }
        }

        dir_track = sector_buf[0];
        dir_sector = sector_buf[1];
    }
}

bool drive_init(const char *package_id)
{
    uint8_t count;
    uint8_t bam_buf[256];

    drive_invalidate();
    drive_autostart_stage = 0;
    unhide_frame = 0;
    ready_pc_detect = false;
    drive_trace_count = 0;
    drive_geos_call_count = 0;
    if (!package_id)
        return false;
    disk_size = CEPackageSize(package_id);
    if (!disk_size)
        return false;

    disk_package = package_id;

    /* Check for raw D64 image: 35-track (174848 B) or 40-track (196608 B) */
    if (disk_size >= D64_RAW_SIZE_35 && drive_read_sector(D64_BAM_TRACK, D64_BAM_SECTOR, bam_buf))
    {
        if (bam_buf[0] == 18 && bam_buf[1] == 1)
        {
            disk_is_raw_d64 = true;
            if (bam_buf[0xad] == 'G' && bam_buf[0xae] == 'E' &&
                bam_buf[0xaf] == 'O' && bam_buf[0xb0] == 'S')
            {
                disk_is_geos = true;
            }
            scan_raw_d64();
            return true;
        }
    }

    /* Uncompressed packages map straight into the archived AppVar; the
       directory always fits in chunk 0 (144 * 22 + 1 < 32 KiB). */
    disk_dir = CEPackageGetDirectChunk(package_id, 0, disk_size);
    if (!disk_dir)
    {
        drive_invalidate();
        return false;
    }

    count = disk_dir[0];
    if (!count || 1 + (uint32_t)count * DIR_ENTRY_SIZE > disk_size)
    {
        drive_invalidate();
        return false;
    }
    return true;
}

uint8_t drive_file_count(void)
{
    if (disk_is_raw_d64)
        return d64_entry_count;
    return disk_dir ? disk_dir[0] : 0;
}

int drive_file_name(uint8_t index, char *buffer, uint8_t bufferSize)
{
    const uint8_t *entry;
    uint8_t i;
    uint8_t count = drive_file_count();
    uint8_t sector_buf[256];

    if (!buffer || !bufferSize || index >= count)
        return 0;

    if (disk_is_raw_d64)
    {
        RawD64Entry *e = &d64_entries[index];
        if (!drive_read_sector(e->dir_track, e->dir_sector, sector_buf))
            return 0;
        entry = sector_buf + 2 + e->slot * 32 + 3;
    }
    else
    {
        entry = disk_dir + 1 + (uint32_t)index * DIR_ENTRY_SIZE;
    }

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
    uint8_t *k = mem_kernal_rom_ptr();
    if (k[0x14a5] == 0x85 && k[0x14a6] == 0x93)
        k[0x14a5] = DRIVE_TRAP_OPCODE;
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
    if (!info || !disk_size || index >= drive_file_count())
        return false;

    if (disk_is_raw_d64)
    {
        RawD64Entry *e = &d64_entries[index];
        uint8_t sector_buf[256];
        drive_file_name(index, info->name, sizeof(info->name));
        info->size = (uint32_t)e->blocks * 254u;
        info->load_address = 0x0801;
        if (e->track != 0 && drive_read_sector(e->track, e->sector, sector_buf))
        {
            info->load_address = (uint16_t)sector_buf[2] | ((uint16_t)sector_buf[3] << 8);
        }
        return true;
    }

    const uint8_t *entry = disk_dir + 1 + (uint32_t)index * DIR_ENTRY_SIZE;
    uint32_t offset = read24(entry + DIR_NAME_LEN);
    uint32_t size = read24(entry + DIR_NAME_LEN + 3);
    const uint8_t *low;
    const uint8_t *high;
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
    if (!identity || !disk_size || (!disk_dir && !disk_is_raw_d64))
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
        uint8_t c = mem_read_byte((uint16_t)(name_addr + i));
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

static int find_raw_d64_entry(uint16_t name_addr, uint8_t name_len)
{
    uint8_t count = d64_entry_count;
    uint8_t i;
    uint8_t last_track = 0;
    uint8_t last_sector = 0;
    const uint8_t *sector_data = NULL;

    if (name_len >= 2 && mem_read_byte(name_addr) == '0' &&
        mem_read_byte((uint16_t)(name_addr + 1)) == ':')
    {
        name_addr = (uint16_t)(name_addr + 2);
        name_len -= 2;
    }

    if (selected_file < count && name_len == 1 && mem_read_byte(name_addr) == '*')
        return selected_file;

    for (i = 0; i < count; ++i)
    {
        RawD64Entry *e = &d64_entries[i];
        if (e->dir_track != last_track || e->dir_sector != last_sector)
        {
            sector_data = drive_sector_ptr(e->dir_track, e->dir_sector);
            last_track = e->dir_track;
            last_sector = e->dir_sector;
        }
        if (sector_data)
        {
            const uint8_t *entry_name = sector_data + 2 + e->slot * 32 + 3;
            if (name_matches(entry_name, name_addr, name_len))
                return i;
        }
    }
    return -1;
}

static bool raw_entry_is_named(const RawD64Entry *entry, const char *expected)
{
    uint8_t sector_buf[256];
    const uint8_t *name;
    uint8_t i;

    if (!entry || !expected ||
        !drive_read_sector(entry->dir_track, entry->dir_sector, sector_buf))
        return false;

    name = sector_buf + 2 + entry->slot * 32 + 3;
    for (i = 0; i < DIR_NAME_LEN && expected[i]; ++i)
        if (name[i] != (uint8_t)expected[i])
            return false;
    return expected[i] == '\0' && (i == DIR_NAME_LEN || name[i] == 0xa0);
}

static void disk_copy_to_ram(uint32_t offset, uint16_t addr, uint32_t count)
{
    while (count)
    {
        const uint8_t *src = CEPackageChunkPtr(disk_package, offset, disk_size);
        unsigned int run = CHUNK_PAYLOAD - (unsigned int)(offset & (CHUNK_PAYLOAD - 1));
        if (!src)
            return;
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

/* Copy host data into guest RAM without crossing a backing-store boundary. */
static void copy_to_guest_ram(uint16_t addr, const uint8_t *src, uint16_t count)
{
    while (count)
    {
        unsigned int span = count;
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
        count = (uint16_t)(count - span);
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
                       uint16_t addr, uint16_t end,
                       uint8_t track, uint8_t sector)
{
    DriveTrace *t;
    uint8_t i;

    if (drive_trace_count < DRIVE_TRACE_MAX)
    {
        t = &drive_trace[drive_trace_count];
        t->caller = cpu.pc;
        t->addr = addr;
        t->end = end;
        t->entry = entry_index;
        t->sa = mem_low[ZP_SECADR];
        t->len = mem_low[ZP_FNLEN];
        for (i = 0; i < 3; ++i)
            t->name[i] = i < t->len ? mem_read_byte((uint16_t)(name_addr + i)) : ' ';
        t->track = track;
        t->sector = sector;
    }
    ++drive_trace_count;
}

#define FASTLOAD_ENTRY 0x1a4e
static const uint8_t fastload_sig[16] = {
    0xa9, 0x00, 0x85, 0x93,       /* LDA #$00, STA $93       */
    0x78,                         /* SEI                     */
    0xa6, 0xb9, 0x86, 0xb8,       /* LDX $B9, STX $B8        */
    0xa9, 0x60, 0x85, 0xb9,       /* LDA #$60, STA $B9       */
    0x20, 0xd5, 0xf3              /* JSR $F3D5 (open w/name) */
};

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

static bool load_raw_d64_file(uint8_t start_track, uint8_t start_sector,
                              uint16_t load_override, bool verify,
                              uint16_t *out_end)
{
    uint8_t track = start_track;
    uint8_t sector = start_sector;
    const uint8_t *sector_buf;
    uint16_t addr = 0;
    bool first = true;
    uint8_t visited[D64_MAX_SECTORS / 8] = {0};

    while (track != 0)
    {
        uint32_t sector_offset = d64_sector_offset(track, sector);
        uint16_t sector_index;
        uint8_t mask;

        if (sector_offset == 0xfffffffful)
            return false;
        sector_index = (uint16_t)(sector_offset >> 8);
        mask = (uint8_t)(1u << (sector_index & 7));
        if (visited[sector_index >> 3] & mask)
            return false;
        visited[sector_index >> 3] |= mask;

        sector_buf = drive_sector_ptr(track, sector);
        if (!sector_buf)
            return false;

        uint8_t next_track = sector_buf[0];
        uint8_t next_sector = sector_buf[1];
        uint16_t data_len;
        const uint8_t *data_ptr;

        if (first && load_override == 0xFFFFu)
        {
            first = false;
            uint16_t file_load_addr = (uint16_t)sector_buf[2] | ((uint16_t)sector_buf[3] << 8);
            addr = file_load_addr;
            data_ptr = sector_buf + 4;
            data_len = (next_track == 0) ? (next_sector >= 4 ? next_sector - 3 : 0) : 252;
        }
        else
        {
            if (first)
            {
                first = false;
                addr = load_override;
            }
            data_ptr = sector_buf + 2;
            data_len = (next_track == 0) ? (next_sector >= 2 ? next_sector - 1 : 0) : 254;
        }

        if (!verify && data_len > 0)
            copy_to_guest_ram(addr, data_ptr, data_len);

        addr = (uint16_t)(addr + data_len);
        track = next_track;
        sector = next_sector;
    }

    if (out_end)
        *out_end = addr;
    return true;
}

static bool drive_bootstrap_geos(void)
{
    uint8_t i;
    uint8_t vlir_buf[256];
    uint16_t record_end;
    uint16_t kernal_end;
    int kernal_idx = -1;

    if (geos_bootstrapped)
        return true;

    for (i = 0; i < d64_entry_count; ++i)
    {
        RawD64Entry *e = &d64_entries[i];
        if (e->geos_struct == 1 && raw_entry_is_named(e, "GEOS KERNAL"))
        {
            kernal_idx = i;
            break;
        }
    }

    if (kernal_idx < 0 ||
        !drive_read_sector(d64_entries[kernal_idx].track,
                           d64_entries[kernal_idx].sector, vlir_buf) ||
                vlir_buf[2] == 0 || vlir_buf[4] == 0 || vlir_buf[6] == 0 ||
        !load_raw_d64_file(vlir_buf[2], vlir_buf[3], 0x9000, false, &record_end) ||
          record_end == 0x9000 ||
          !load_raw_d64_file(vlir_buf[4], vlir_buf[5], 0xc000, false, NULL) ||
             !load_raw_d64_file(vlir_buf[6], vlir_buf[7], 0xd000, false, &kernal_end) ||
             kernal_end < ADDR_KERNAL)
        return false;

         /* The first three GEOS KERNAL VLIR records are the resident images for
                 $9000, $C000, and RAM beneath I/O at $D000. */
     /* GEOS maps out KERNAL ROM but its final resident record ends before
         $10000, leaving the KERNAL entry tail it samples in RAM uninitialized. */
     memcpy(mem_host_ptr(kernal_end), mem_kernal_rom_ptr() + kernal_end - ADDR_KERNAL,
              0x10000u - kernal_end);
     jit_invalidate_range(kernal_end, 0x10000u - kernal_end);
    drive_install_geos_driver();

    /* Point $1000 to JMP $C003 for GEOS entry */
    *mem_host_ptr(0x1000) = 0x4c;
    *mem_host_ptr(0x1001) = 0x03;
    *mem_host_ptr(0x1002) = 0xc0;
    jit_invalidate_range(0x1000, 3);
    geos_bootstrapped = true;
    return true;
}

bool drive_trap_load(void)
{
    uint16_t addr, end, name_addr;
    uint32_t offset, size;
    const uint8_t *entry;
    bool verify;

    if (cpu.pc != 0xf4a6 ||
        mem_rd_pg[0xf4].ptr == NULL ||
        mem_rd_pg[0xf4].ptr[0xf4a5] != DRIVE_TRAP_OPCODE)
        return false;

    if (!disk_size)
    {
        mem_low[ZP_STATUS] = 0;
        cpu.a = 5;
        cpu.cf = true;
        do_rts();
        return true;
    }

    verify = cpu.a != 0;
    name_addr = (uint16_t)(mem_low[ZP_FNADR] | (mem_low[ZP_FNADR + 1] << 8));

    if (disk_is_raw_d64)
    {
        int entry_idx = find_raw_d64_entry(name_addr, mem_low[ZP_FNLEN]);
        if (entry_idx < 0)
        {
            mem_low[ZP_STATUS] = 0;
            cpu.a = 4;
            cpu.cf = true;
            do_rts();
            trace_load(name_addr, 0xff, 0, 0, 0, 0);
            unhide_frame = vic_frames() + 1;
            return true;
        }

        RawD64Entry *e = &d64_entries[entry_idx];
        uint16_t load_override = 0xFFFFu;
        bool geos_boot_load = disk_is_geos && !geos_bootstrapped &&
                      raw_entry_is_named(e, "GEOS BOOT");
        if (mem_low[ZP_SECADR] == 0)
            load_override = (uint16_t)(mem_low[ZP_MEMUSS] | (mem_low[ZP_MEMUSS + 1] << 8));

                    /* The GEOS launcher jumps through $1000 after GEOS BOOT returns.
                         Replace that entry with the installed resident KERNAL before
                         returning, so GEOS BOOT's real-1541 polling code is bypassed. */
          if (!load_raw_d64_file(e->track, e->sector, load_override, verify, &end) ||
                        (geos_boot_load && !drive_bootstrap_geos()))
        {
            mem_low[ZP_STATUS] = 0;
            cpu.a = 4;
            cpu.cf = true;
            do_rts();
            trace_load(name_addr, 0xff, 0, 0, e->track, e->sector);
            unhide_frame = vic_frames() + 1;
            return true;
        }

        mem_low[ZP_EAL] = (uint8_t)end;
        mem_low[ZP_EAL + 1] = (uint8_t)(end >> 8);
        mem_low[ZP_STATUS] = 0x40;
        cpu.x = (uint8_t)end;
        cpu.y = (uint8_t)(end >> 8);
        cpu.a = 0;
        cpu.cf = false;
        cpu.cycles += 100;
        do_rts();
        trace_load(name_addr, (uint8_t)entry_idx,
               load_override != 0xFFFFu ? load_override : (uint16_t)(end - (e->blocks * 254)), end,
               e->track, e->sector);
        plant_fastload_trap();
        unhide_frame = vic_frames() + 12;
        return true;
    }

    entry = find_entry(name_addr, mem_low[ZP_FNLEN]);

    if (!entry)
    {
        mem_low[ZP_STATUS] = 0;
        cpu.a = 4;
        cpu.cf = true;
        do_rts();
        trace_load(name_addr, 0xff, 0, 0, 0, 0);
        unhide_frame = vic_frames() + 1;
        return true;
    }

    offset = read24(entry + DIR_NAME_LEN);
    size = read24(entry + DIR_NAME_LEN + 3);

    addr = (uint16_t)(disk_byte(offset) | (disk_byte(offset + 1) << 8));
    if (mem_low[ZP_SECADR] == 0)
        addr = (uint16_t)(mem_low[ZP_MEMUSS] | (mem_low[ZP_MEMUSS + 1] << 8));

    if (!verify)
        disk_copy_to_ram(offset + 2, addr, size - 2);

    end = (uint16_t)(addr + (size - 2));
    mem_low[ZP_EAL] = (uint8_t)end;
    mem_low[ZP_EAL + 1] = (uint8_t)(end >> 8);
    mem_low[ZP_STATUS] = 0x40;
    cpu.x = (uint8_t)end;
    cpu.y = (uint8_t)(end >> 8);
    cpu.a = 0;
    cpu.cf = false;
    cpu.cycles += 100;
    do_rts();
    trace_load(name_addr, (uint8_t)((entry - (disk_dir + 1)) / DIR_ENTRY_SIZE),
               addr, end, 0, 0);
    plant_fastload_trap();
    unhide_frame = vic_frames() + 12;
    return true;
}

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

    if (disk_is_raw_d64)
    {
        int entry_idx = find_raw_d64_entry(name_addr, mem_low[ZP_FNLEN]);
        if (entry_idx < 0)
        {
            mem_low[ZP_STATUS] = 0x02;
            cpu.cf = true;
            do_rts();
            trace_load(name_addr, 0xff, 0, 0, 0, 0);
            return true;
        }

        RawD64Entry *e = &d64_entries[entry_idx];
        uint16_t load_override = (sa == 0) ? (uint16_t)(mem_low[ZP_MEMUSS] | (mem_low[ZP_MEMUSS + 1] << 8)) : 0xFFFFu;
        if (!load_raw_d64_file(e->track, e->sector, load_override, false, &end))
        {
            mem_low[ZP_STATUS] = 0x02;
            cpu.cf = true;
            do_rts();
            trace_load(name_addr, 0xff, 0, 0, e->track, e->sector);
            return true;
        }

        mem_low[0xac] = (uint8_t)end;
        mem_low[0xad] = (uint8_t)(end >> 8);
        mem_low[0xb8] = sa;
        mem_low[ZP_SECADR] = 0x60;
        mem_low[ZP_STATUS] = 0;
        cpu.x = (uint8_t)end;
        cpu.y = (uint8_t)(end >> 8);
        cpu.a = 0;
        cpu.cf = false;
        cpu.cycles += 100;
        do_rts();
        trace_load(name_addr, (uint8_t)entry_idx,
               load_override != 0xFFFFu ? load_override : (uint16_t)(end - (e->blocks * 254)), end,
               e->track, e->sector);
        plant_fastload_trap();
        return true;
    }

    entry = find_entry(name_addr, mem_low[ZP_FNLEN]);

    if (!entry)
    {
        mem_low[ZP_STATUS] = 0x02;
        cpu.cf = true;
        do_rts();
        trace_load(name_addr, 0xff, 0, 0, 0, 0);
        return true;
    }

    offset = read24(entry + DIR_NAME_LEN);
    size = read24(entry + DIR_NAME_LEN + 3);

    addr = (uint16_t)(disk_byte(offset) | (disk_byte(offset + 1) << 8));
    if (sa == 0)
        addr = (uint16_t)(mem_low[ZP_MEMUSS] | (mem_low[ZP_MEMUSS + 1] << 8));

    disk_copy_to_ram(offset + 2, addr, size - 2);

    end = (uint16_t)(addr + (size - 2));
    mem_low[0xac] = (uint8_t)end;
    mem_low[0xad] = (uint8_t)(end >> 8);
    mem_low[0xb8] = sa;
    mem_low[ZP_SECADR] = 0x60;
    mem_low[ZP_STATUS] = 0;
    cpu.x = (uint8_t)end;
    cpu.y = (uint8_t)(end >> 8);
    cpu.a = 0;
    cpu.cf = false;
    cpu.cycles += 100;
    do_rts();
    trace_load(name_addr, (uint8_t)((entry - (disk_dir + 1)) / DIR_ENTRY_SIZE),
               addr, end, 0, 0);
    plant_fastload_trap();
    return true;
}

bool drive_trap_geos(void)
{
    uint8_t fn;
    uint16_t pc = cpu.pc;

    if (pc < 0x9d81 || pc > 0x9da6)
        return false;

    ++drive_geos_call_count;
    fn = *mem_host_ptr(pc);

    switch (fn)
    {
    case 0: /* InitForIO ($9D80) */
    case 1: /* DoneWithIO ($9D83) */
    case 2: /* ExitTurbo ($9D86) */
    case 3: /* PurgeTurbo ($9D89) */
        cpu.a = 0;
        cpu.cf = false;
        cpu.zf = true;
        do_rts();
        return true;

    case 4: /* EnterTurbo ($9D8C) */
        cpu.a = 0;
        cpu.x = 0;
        cpu.cf = false;
        cpu.zf = true;
        do_rts();
        return true;

    case 5: /* ReadBlock ($9D8F) */
    {
        uint8_t track = mem_low[0x04];  /* r1L */
        uint8_t sector = mem_low[0x05]; /* r1H */
        uint16_t buffer = (uint16_t)(mem_low[0x0a] | ((uint16_t)mem_low[0x0b] << 8)); /* r4 */
        const uint8_t *sector_data;

        sector_data = drive_sector_ptr(track, sector);
        if (!sector_data)
        {
            cpu.a = 1;
            cpu.cf = true;
            cpu.zf = false;
            do_rts();
            return true;
        }

        if (buffer < MEM_LOW_SIZE)
        {
            unsigned int seam = MEM_LOW_SIZE - buffer;
            if (seam >= 256)
                memcpy(mem_low + buffer, sector_data, 256);
            else
            {
                memcpy(mem_low + buffer, sector_data, seam);
                memcpy(mem_ram + MEM_LOW_SIZE, sector_data + seam, 256 - seam);
            }
        }
        else
        {
            memcpy(mem_ram + buffer, sector_data, 256);
        }
        jit_invalidate_range(buffer, 256);

        if ((buffer <= 0x9d80 && buffer + 256 > 0x9d80) ||
            (buffer >= 0x9d80 && buffer < 0x9e00))
        {
            drive_install_geos_driver();
        }

        cpu.a = 0;
        cpu.x = 0;
        cpu.cf = false;
        cpu.zf = true;
        cpu.cycles += 50;
        do_rts();
        return true;
    }

    case 6: /* WriteBlock ($9D92) */
    {
        uint8_t track = mem_low[0x04];
        uint8_t sector = mem_low[0x05];
        uint16_t buffer = (uint16_t)(mem_low[0x0a] | ((uint16_t)mem_low[0x0b] << 8));
        uint8_t sec_data[256];

        if (buffer < MEM_LOW_SIZE)
        {
            unsigned int seam = MEM_LOW_SIZE - buffer;
            if (seam >= 256)
                memcpy(sec_data, mem_low + buffer, 256);
            else
            {
                memcpy(sec_data, mem_low + buffer, seam);
                memcpy(sec_data + seam, mem_ram + MEM_LOW_SIZE, 256 - seam);
            }
        }
        else
        {
            memcpy(sec_data, mem_ram + buffer, 256);
        }

        drive_write_sector(track, sector, sec_data);
        cpu.a = 0;
        cpu.x = 0;
        cpu.cf = false;
        cpu.zf = true;
        cpu.cycles += 50;
        do_rts();
        return true;
    }

    case 7: /* VerifyBlock ($9D95) */
        cpu.a = 0;
        cpu.x = 0;
        cpu.cf = false;
        cpu.zf = true;
        do_rts();
        return true;

    case 8: /* ReadLink ($9D98) */
    {
        uint8_t track = mem_low[0x04];
        uint8_t sector = mem_low[0x05];
        const uint8_t *sector_data;

        sector_data = drive_sector_ptr(track, sector);
        if (!sector_data)
        {
            cpu.a = 1;
            cpu.cf = true;
            cpu.zf = false;
            do_rts();
            return true;
        }

        mem_low[0x04] = sector_data[0];
        mem_low[0x05] = sector_data[1];
        cpu.a = 0;
        cpu.cf = false;
        cpu.zf = true;
        cpu.cycles += 20;
        do_rts();
        return true;
    }

    case 9: /* GetDirHead ($9D9B) */
    {
        const uint8_t *sector_data = drive_sector_ptr(D64_BAM_TRACK, D64_BAM_SECTOR);
        if (!sector_data)
        {
            cpu.a = 1;
            cpu.cf = true;
            cpu.zf = false;
            do_rts();
            return true;
        }
        memcpy(mem_host_ptr(0x8200), sector_data, 256);
        jit_invalidate_range(0x8200, 256);
        cpu.a = 0;
        cpu.x = 0;
        cpu.cf = false;
        cpu.zf = true;
        cpu.cycles += 50;
        do_rts();
        return true;
    }

    case 10: /* PutDirHead ($9D9E) */
    {
        const uint8_t *sector_data = mem_host_ptr(0x8200);
        drive_write_sector(D64_BAM_TRACK, D64_BAM_SECTOR, sector_data);
        cpu.a = 0;
        cpu.x = 0;
        cpu.cf = false;
        cpu.zf = true;
        cpu.cycles += 50;
        do_rts();
        return true;
    }

    case 11: /* SetGEOSDisk ($9DA1) */
    {
        uint8_t sec_data[256];
        if (drive_read_sector(D64_BAM_TRACK, D64_BAM_SECTOR, sec_data))
        {
            memcpy(mem_host_ptr(0x8200), sec_data, 256);
            jit_invalidate_range(0x8200, 256);
        }
        cpu.a = 0;
        cpu.x = 0;
        cpu.cf = false;
        cpu.zf = true;
        cpu.cycles += 50;
        do_rts();
        return true;
    }

    default:
        cpu.a = 0;
        cpu.x = 0;
        cpu.cf = false;
        cpu.zf = true;
        do_rts();
        return true;
    }
}

void drive_tick(void)
{
    static const uint8_t command[KEYBUF_MAX] = {
        0x4c, 0xcf, 0x22, 0x2a, 0x22, 0x2c, 0x38, 0x2c, 0x31, 0x0d
    };
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

    if (drive_autostart_stage == 1 && drive_trace_count == 1)
    {
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
