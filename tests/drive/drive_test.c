#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/drive.c"

enum
{
    HOST_PACKAGE_SIZE = 0x30000,
    HOST_MAP_SIZE = 560,
    HOST_NAME_ADDRESS = 0x0200,
    HOST_RETURN_ADDRESS = 0x7535
};

static uint8_t host_ram[MEM_SIZE];
static uint8_t host_low[MEM_LOW_SIZE];
static uint8_t host_rom[MEM_SIZE];
static uint8_t host_package[HOST_PACKAGE_SIZE];
static uint32_t host_package_size;
static uint8_t expected_map[HOST_MAP_SIZE];
static unsigned int failures;

CpuState cpu;
uint8_t *mem_ram = host_ram;
uint8_t *mem_low = host_low;
uint8_t mem_color_ram[0x400];
MemPage mem_rd_pg[256];
MemPage mem_wr_pg[256];

#define EXPECT_EQ(actual, expected) do { \
    unsigned long actual_value = (unsigned long)(actual); \
    unsigned long expected_value = (unsigned long)(expected); \
    if (actual_value != expected_value) { \
        fprintf(stderr, "  line %d: %s = %lu, expected %lu\n", \
                __LINE__, #actual, actual_value, expected_value); \
        ++failures; \
    } \
} while (0)

uint32_t CEPackageSize(const char *package_id)
{
    return strcmp(package_id, CE_PACKAGE_DISK) == 0 ? host_package_size : 0;
}

const uint8_t *CEPackageChunkPtr(const char *package_id, uint32_t offset,
                               uint32_t expected_size)
{
    if (strcmp(package_id, CE_PACKAGE_DISK) != 0 ||
        expected_size != host_package_size || offset >= host_package_size)
        return NULL;
    return host_package + offset;
}

const uint8_t *CEPackageGetDirectChunk(const char *package_id, uint8_t chunk,
                                     uint32_t expected_size)
{
    return CEPackageChunkPtr(package_id, (uint32_t)chunk * 0x8000u, expected_size);
}

uint8_t *mem_kernal_rom_ptr(void)
{
    return host_rom + ADDR_KERNAL;
}

uint8_t mem_read_io(uint16_t address)
{
    fprintf(stderr, "  unexpected IO read: $%04x\n", address);
    ++failures;
    return 0;
}

unsigned int vic_frames(void)
{
    return 100;
}

void vic_set_boot_hidden(bool hidden)
{
    (void)hidden;
}

void vic_request_render(void)
{
}

void jit_invalidate_range(uint16_t addr, unsigned int len)
{
    (void)addr;
    (void)len;
}

static void host_write24(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
}

static void prepare_load(const char *name)
{
    size_t name_size = strlen(name);
    memset(host_low, 0, sizeof(host_low));
    memset(host_ram, 0, sizeof(host_ram));
    memset(host_rom, 0, sizeof(host_rom));
    memset(&cpu, 0, sizeof(cpu));
    for (unsigned int page = 0; page < 256; ++page)
    {
        mem_rd_pg[page].ptr = page < 16 ? host_low : host_ram;
        mem_wr_pg[page].ptr = page < 16 ? host_low : host_ram;
    }
    for (unsigned int page = 0xe0; page < 256; ++page)
        mem_rd_pg[page].ptr = host_rom;
    host_rom[0xf4a5] = DRIVE_TRAP_OPCODE;
    memcpy(host_low + HOST_NAME_ADDRESS, name, name_size);
    host_low[ZP_FNLEN] = (uint8_t)name_size;
    host_low[ZP_FNADR] = (uint8_t)HOST_NAME_ADDRESS;
    host_low[ZP_FNADR + 1] = (uint8_t)(HOST_NAME_ADDRESS >> 8);
    host_low[ZP_SECADR] = 1;
    host_low[0x1fe] = (uint8_t)(HOST_RETURN_ADDRESS - 1);
    host_low[0x1ff] = (uint8_t)((HOST_RETURN_ADDRESS - 1) >> 8);
    memset(host_low + 0x0400, 0x20, HOST_MAP_SIZE);
    host_low[0x03ff] = host_low[0x0630] = 0xa5;
    cpu.pc = 0xf4a6;
    cpu.sp = 0xfd;
    cpu.cf = true;
}

static void prepare_followup_load(const char *name)
{
    size_t name_size = strlen(name);

    memset(host_low + HOST_NAME_ADDRESS, 0, DIR_NAME_LEN);
    memcpy(host_low + HOST_NAME_ADDRESS, name, name_size);
    host_low[ZP_FNLEN] = (uint8_t)name_size;
    host_low[ZP_FNADR] = (uint8_t)HOST_NAME_ADDRESS;
    host_low[ZP_FNADR + 1] = (uint8_t)(HOST_NAME_ADDRESS >> 8);
    host_low[ZP_SECADR] = 1;
    host_low[0x1fe] = (uint8_t)(HOST_RETURN_ADDRESS - 1);
    host_low[0x1ff] = (uint8_t)((HOST_RETURN_ADDRESS - 1) >> 8);
    host_rom[0xf4a5] = DRIVE_TRAP_OPCODE;
    cpu.pc = 0xf4a6;
    cpu.sp = 0xfd;
    cpu.cf = true;
}

static void prepare_map_package(void)
{
    uint32_t data_offset = 1 + 2 * DIR_ENTRY_SIZE;
    memset(host_package, 0, sizeof(host_package));
    host_package[0] = 2;
    for (unsigned int entry_index = 0; entry_index < 2; ++entry_index)
    {
        uint8_t *entry = host_package + 1 + entry_index * DIR_ENTRY_SIZE;
        memset(entry, 0xa0, DIR_NAME_LEN);
        memcpy(entry, entry_index ? "EVENTA.MAP" : "BOOT", entry_index ? 10 : 4);
        host_write24(entry + DIR_NAME_LEN, data_offset);
        host_write24(entry + DIR_NAME_LEN + 3, HOST_MAP_SIZE + 2);
    }
    host_package[data_offset] = 0;
    host_package[data_offset + 1] = 4;
    for (unsigned int offset = 0; offset < HOST_MAP_SIZE; ++offset)
        expected_map[offset] = (uint8_t)((offset * 37u) ^ (offset >> 3));
    memcpy(host_package + data_offset + 2, expected_map, HOST_MAP_SIZE);
    host_package_size = data_offset + 2 + HOST_MAP_SIZE;
    EXPECT_EQ(drive_init(CE_PACKAGE_DISK), true);
}

static void expect_map_loaded(void)
{
    EXPECT_EQ(cpu.cf, false);
    EXPECT_EQ(cpu.a, 0);
    EXPECT_EQ(cpu.pc, HOST_RETURN_ADDRESS);
    EXPECT_EQ(cpu.sp, 0xff);
    EXPECT_EQ(cpu.x, 0x30);
    EXPECT_EQ(cpu.y, 0x06);
    EXPECT_EQ(memcmp(host_low + 0x0400, expected_map, HOST_MAP_SIZE) == 0, true);
    EXPECT_EQ(host_low[0x03ff], 0xa5);
    EXPECT_EQ(host_low[0x0630], 0xa5);
}

static void expect_kernal_load(const char *name, uint8_t entry_index)
{
    prepare_load(name);
    EXPECT_EQ(drive_trap_load(), true);
    expect_map_loaded();
    EXPECT_EQ(host_low[ZP_EAL], 0x30);
    EXPECT_EQ(host_low[ZP_EAL + 1], 0x06);
    EXPECT_EQ(host_low[ZP_STATUS], 0x40);
    EXPECT_EQ(drive_trace[drive_trace_count - 1].entry, entry_index);
}

static void test_drive_zero_map_load(void)
{
    prepare_map_package();
    expect_kernal_load("0:EVENTA.MAP", 1);
    EXPECT_EQ(drive_trace_count, 1);
    EXPECT_EQ(drive_trace[0].len, 12);
    EXPECT_EQ(memcmp(drive_trace[0].name, "0:E", 3), 0);
    EXPECT_EQ(drive_trace[0].addr, 0x0400);
    EXPECT_EQ(drive_trace[0].end, 0x0630);
    EXPECT_EQ(drive_trace[0].caller, HOST_RETURN_ADDRESS);
}

static void test_plain_map_load(void)
{
    prepare_map_package();
    expect_kernal_load("EVENTA.MAP", 1);
}

static void test_prefixed_wildcards(void)
{
    prepare_map_package();
    expect_kernal_load("0:EVENT?.MAP", 1);
    expect_kernal_load("0:EVENT*", 1);
}

static void test_selected_wildcards(void)
{
    prepare_map_package();
    drive_select_file(1);
    expect_kernal_load("*", 1);
    expect_kernal_load("0:*", 1);
}

static void test_unselected_wildcards(void)
{
    prepare_map_package();
    expect_kernal_load("*", 0);
    expect_kernal_load("0:*", 0);
}

static void test_unmatched_names(void)
{
    static const char *const names[] = {
        "0:EVENTB.MAP", "1:EVENTA.MAP", "0EVENTA.MAP",
        "00:EVENTA.MAP", "0:EVENTA", "0"
    };
    prepare_map_package();
    for (unsigned int name_index = 0;
         name_index < sizeof(names) / sizeof(names[0]); ++name_index)
    {
        prepare_load(names[name_index]);
        EXPECT_EQ(drive_trap_load(), true);
        EXPECT_EQ(cpu.cf, true);
        EXPECT_EQ(cpu.a, 4);
        EXPECT_EQ(cpu.pc, HOST_RETURN_ADDRESS);
        EXPECT_EQ(cpu.sp, 0xff);
        EXPECT_EQ(drive_trace[name_index].entry, 0xff);
        for (unsigned int offset = 0; offset < HOST_MAP_SIZE; ++offset)
            EXPECT_EQ(host_low[0x0400 + offset], 0x20);
        EXPECT_EQ(host_low[0x03ff], 0xa5);
        EXPECT_EQ(host_low[0x0630], 0xa5);
    }
}

static void test_prefixed_full_length_name(void)
{
    prepare_map_package();
    memcpy(host_package + 1 + DIR_ENTRY_SIZE, "ABCDEFGHIJKLMNOP", DIR_NAME_LEN);
    expect_kernal_load("0:ABCDEFGHIJKLMNOP", 1);
}

static void test_fastload_drive_zero(void)
{
    prepare_map_package();
    prepare_load("0:EVENTA.MAP");
    host_ram[FASTLOAD_ENTRY] = DRIVE_TRAP2_OPCODE;
    cpu.pc = FASTLOAD_ENTRY + 1;
    EXPECT_EQ(drive_trap_fastload(), true);
    expect_map_loaded();
    EXPECT_EQ(host_low[0xac], 0x30);
    EXPECT_EQ(host_low[0xad], 0x06);
    EXPECT_EQ(host_low[ZP_STATUS], 0);
    EXPECT_EQ(drive_trace[0].entry, 1);
}

static void test_map_load_after_bootstrap(void)
{
    prepare_map_package();
    for (unsigned int load_index = 0; load_index < 8; ++load_index)
        expect_kernal_load("BOOT", 0);
    expect_kernal_load("0:EVENTA.MAP", 1);
    EXPECT_EQ(drive_trace_count, 9);
    EXPECT_EQ(drive_trace[8].entry, 1);
    EXPECT_EQ(drive_trace[8].addr, 0x0400);
    EXPECT_EQ(drive_trace[8].end, 0x0630);
}

static void prepare_geos_d64(void)
{
    memset(host_package, 0, sizeof(host_package));
    host_package_size = D64_RAW_SIZE_35;

    /* Track 18 Sector 0 (BAM) */
    uint32_t bam_off = 91392u;
    host_package[bam_off + 0] = 18;
    host_package[bam_off + 1] = 1;
    host_package[bam_off + 2] = 'A';
    memset(host_package + bam_off + 0x90, 0xa0, 16);
    memcpy(host_package + bam_off + 0x90, "GEOS DISK", 9);
    memcpy(host_package + bam_off + 0xad, "GEOS format V1.0", 16);

    /* Track 18 Sector 1 (Dir) */
    uint32_t dir_off = 91648u;
    host_package[dir_off + 0] = 0;
    host_package[dir_off + 1] = 0xff;

    /* Entry 0: GEOS boot PRG */
    uint8_t *e0 = host_package + dir_off + 2;
    e0[0] = 0x82; /* PRG */
    e0[1] = 1;    /* Track 1 */
    e0[2] = 0;    /* Sector 0 */
    memset(e0 + 3, 0xa0, 16);
    memcpy(e0 + 3, "GEOS", 4);
    e0[28] = 2;   /* 2 blocks */
    e0[29] = 0;

    /* Entry 1: DESKTOP USR/VLIR */
    uint8_t *e1 = host_package + dir_off + 2 + 32;
    e1[0] = 0x83; /* USR */
    e1[1] = 2;    /* Track 2 */
    e1[2] = 0;    /* Sector 0 (Record table) */
    memset(e1 + 3, 0xa0, 16);
    memcpy(e1 + 3, "DESKTOP", 7);
    e1[19] = 3;   /* Info track */
    e1[20] = 0;   /* Info sector */
    e1[21] = 1;   /* VLIR */
    e1[22] = 6;   /* Application */
    e1[28] = 4;
    e1[29] = 0;

    /* Entry 2: GEOWRITE USR/VLIR */
    uint8_t *e2 = host_package + dir_off + 2 + 64;
    e2[0] = 0x83; /* USR */
    e2[1] = 4;    /* Track 4 */
    e2[2] = 0;    /* Sector 0 */
    memset(e2 + 3, 0xa0, 16);
    memcpy(e2 + 3, "GEOWRITE", 8);
    e2[21] = 1;   /* VLIR */
    e2[28] = 8;
    e2[29] = 0;

    /* Track 1 Sector 0 (GEOS PRG first sector) */
    host_package[0] = 1;   /* next track */
    host_package[1] = 1;   /* next sector */
    host_package[2] = 0x01; /* load addr $0801 */
    host_package[3] = 0x08;
    for (unsigned int i = 4; i < 256; ++i)
        host_package[i] = (uint8_t)(0x10 + (i & 0x7f));

    /* Track 1 Sector 1 (GEOS PRG second/last sector) */
    uint32_t sec1_off = 256u;
    host_package[sec1_off + 0] = 0;   /* last sector */
    host_package[sec1_off + 1] = 100; /* ends at byte 100 */
    for (unsigned int i = 2; i <= 100; ++i)
        host_package[sec1_off + i] = (uint8_t)(0x80 + (i & 0x3f));

    /* Track 2 Sector 0 (DESKTOP VLIR index block) */
    uint32_t vlir_idx_off = 21u * 256u;
    host_package[vlir_idx_off + 0] = 2; /* Record 0: Track 2, Sector 1 */
    host_package[vlir_idx_off + 1] = 1;
    host_package[vlir_idx_off + 2] = 2; /* Record 1: Track 2, Sector 2 */
    host_package[vlir_idx_off + 3] = 2;

    /* Track 2 Sector 1 (DESKTOP Record 0) */
    uint32_t rec0_off = 22u * 256u;
    host_package[rec0_off + 0] = 0;
    host_package[rec0_off + 1] = 60;
    memcpy(host_package + rec0_off + 2, "DESKTOP_RECORD_0_DATA", 21);

    /* Track 2 Sector 2 (DESKTOP Record 1) */
    uint32_t rec1_off = 23u * 256u;
    host_package[rec1_off + 0] = 0;
    host_package[rec1_off + 1] = 80;
    memcpy(host_package + rec1_off + 2, "DESKTOP_RECORD_1_DATA", 21);

    EXPECT_EQ(drive_init(CE_PACKAGE_DISK), true);
}

static bool prepare_actual_geos_d64(void)
{
    FILE *file;
    long length;

    memset(host_package, 0, sizeof(host_package));
    file = fopen("../GEOS64.d64", "rb");
    if (!file)
    {
        fprintf(stderr, "  cannot load real GEOS64.d64 fixture\n");
        ++failures;
        return false;
    }
    if (fseek(file, 0, SEEK_END) ||
        (length = ftell(file)) != D64_RAW_SIZE_35 ||
        fseek(file, 0, SEEK_SET) ||
        fread(host_package, 1, (size_t)length, file) != (size_t)length ||
        fclose(file))
    {
        fprintf(stderr, "  cannot load real GEOS64.d64 fixture\n");
        ++failures;
        return false;
    }
    host_package_size = (uint32_t)length;
    return true;
}

static void test_raw_d64_geos_detection(void)
{
    prepare_geos_d64();
    EXPECT_EQ(drive_is_d64(), true);
    EXPECT_EQ(drive_is_geos(), true);
    EXPECT_EQ(drive_file_count(), 3);

    char name[32];
    EXPECT_EQ(drive_file_name(0, name, sizeof(name)), 1);
    EXPECT_EQ(strcmp(name, "GEOS"), 0);
    EXPECT_EQ(drive_file_name(1, name, sizeof(name)), 1);
    EXPECT_EQ(strcmp(name, "DESKTOP"), 0);
    EXPECT_EQ(drive_file_name(2, name, sizeof(name)), 1);
    EXPECT_EQ(strcmp(name, "GEOWRITE"), 0);

    DriveFileInfo info;
    EXPECT_EQ(drive_file_info(0, &info), true);
    EXPECT_EQ(info.load_address, 0x0801);
    EXPECT_EQ(strcmp(info.name, "GEOS"), 0);
}

static void test_raw_d64_sector_origin(void)
{
    uint8_t sector[256];
    prepare_geos_d64();
    EXPECT_EQ(d64_sector_offset(1, 0), 0);
    EXPECT_EQ(drive_read_sector(1, 0, sector), true);
    EXPECT_EQ(memcmp(sector, host_package, sizeof(sector)), 0);
    EXPECT_EQ(sector[0], 1);
    EXPECT_EQ(sector[1], 1);
    EXPECT_EQ(sector[2], 0x01);
    EXPECT_EQ(sector[3], 0x08);
    printf("  track 1 sector 0: offset=%lu bytes=%02x %02x %02x %02x\n",
           (unsigned long)d64_sector_offset(1, 0),
           sector[0], sector[1], sector[2], sector[3]);
}

static void test_raw_d64_prg_load(void)
{
    prepare_geos_d64();
    prepare_load("GEOS");
    EXPECT_EQ(drive_trap_load(), true);

    EXPECT_EQ(cpu.cf, false);
    EXPECT_EQ(cpu.a, 0);
    EXPECT_EQ(cpu.pc, HOST_RETURN_ADDRESS);
    EXPECT_EQ(cpu.sp, 0xff);

    /* Verify first sector payload loaded to $0801 in low memory */
    for (unsigned int i = 4; i < 256; ++i)
        EXPECT_EQ(host_low[0x0801 + (i - 4)], (uint8_t)(0x10 + (i & 0x7f)));

    /* Verify second sector payload */
    for (unsigned int i = 2; i <= 100; ++i)
        EXPECT_EQ(host_low[0x0801 + 252 + (i - 2)], (uint8_t)(0x80 + (i & 0x3f)));

    /* A generic raw GEOS file load must not install the kernel driver. */
    EXPECT_EQ(host_ram[0x9d80], 0);
    EXPECT_EQ(host_ram[0x9d8f], 0);
}

static void test_actual_geos_bootstrap_once(void)
{
    uint8_t record[256];
    uint8_t kernal_tail[0x88];
    unsigned int i;

    memset(host_low, 0, sizeof(host_low));
    memset(host_ram, 0, sizeof(host_ram));
    memset(host_rom, 0, sizeof(host_rom));
    if (!prepare_actual_geos_d64())
        return;

    EXPECT_EQ(drive_init(CE_PACKAGE_DISK), true);
    EXPECT_EQ(drive_is_d64(), true);
    EXPECT_EQ(drive_is_geos(), true);
    EXPECT_EQ(host_ram[0x9d80], 0);

    prepare_load("GEOS");
    EXPECT_EQ(drive_trap_load(), true);
    EXPECT_EQ(cpu.cf, false);
    EXPECT_EQ(drive_trace_count, 1);
    EXPECT_EQ(drive_trace[0].track, 17);
    EXPECT_EQ(drive_trace[0].sector, 0);
    EXPECT_EQ(host_ram[0x9d80], 0);

    for (i = 0; i < sizeof(kernal_tail); ++i)
        host_rom[0xff78 + i] = kernal_tail[i] = (uint8_t)(0x40 + i);

    prepare_followup_load("GEOS BOOT");
    EXPECT_EQ(drive_trap_load(), true);
    EXPECT_EQ(cpu.cf, false);
    EXPECT_EQ(drive_trace_count, 2);
    EXPECT_EQ(drive_trace[1].track, 1);
    EXPECT_EQ(drive_trace[1].sector, 16);
    EXPECT_EQ(host_ram[0x1000], 0x4c);
    EXPECT_EQ(host_ram[0x1001], 0x03);
    EXPECT_EQ(host_ram[0x1002], 0xc0);
    EXPECT_EQ(host_ram[0x9d80], DRIVE_GEOS_TRAP_OPCODE);
    EXPECT_EQ(host_ram[0x9d81], 0);
    EXPECT_EQ(host_ram[0x9d8f], DRIVE_GEOS_TRAP_OPCODE);
    EXPECT_EQ(host_ram[0x9d90], 5);

    EXPECT_EQ(drive_read_sector(1, 10, record), true);
    EXPECT_EQ(memcmp(host_ram + 0x9000, record + 2, 64), 0);
        EXPECT_EQ(host_ram[0xc000], 0x4c);
        EXPECT_EQ(host_ram[0xc001], 0x0f);
        EXPECT_EQ(host_ram[0xc002], 0xc0);
    EXPECT_EQ(host_ram[0xc003], 0x4c);
        EXPECT_EQ(memcmp(host_ram + 0xff78, kernal_tail, sizeof(kernal_tail)), 0);

    host_ram[0x1000] = 0xea;
    host_ram[0x1001] = 0xea;
    host_ram[0x1002] = 0xea;
    prepare_followup_load("DESK TOP");
    EXPECT_EQ(drive_trap_load(), true);
    EXPECT_EQ(cpu.cf, false);
    EXPECT_EQ(drive_trace_count, 3);
    EXPECT_EQ(host_ram[0x1000], 0xea);
    EXPECT_EQ(host_ram[0x1001], 0xea);
    EXPECT_EQ(host_ram[0x1002], 0xea);
}

static void test_geos_driver_traps(void)
{
    prepare_geos_d64();
    drive_install_geos_driver();

    /* Test InitForIO ($9D80) */
    cpu.pc = 0x9d81;
    host_low[0x1fe] = (uint8_t)(0x5000 - 1);
    host_low[0x1ff] = (uint8_t)((0x5000 - 1) >> 8);
    cpu.sp = 0xfd;
    EXPECT_EQ(drive_trap_geos(), true);
    EXPECT_EQ(cpu.a, 0);
    EXPECT_EQ(cpu.cf, false);
    EXPECT_EQ(cpu.zf, true);
    EXPECT_EQ(cpu.pc, 0x5000);

    /* Test EnterTurbo ($9D8C) */
    cpu.pc = 0x9d8d;
    host_low[0x1fe] = (uint8_t)(0x5001 - 1);
    host_low[0x1ff] = (uint8_t)((0x5001 - 1) >> 8);
    cpu.sp = 0xfd;
    EXPECT_EQ(drive_trap_geos(), true);
    EXPECT_EQ(cpu.a, 0);
    EXPECT_EQ(cpu.x, 0);
    EXPECT_EQ(cpu.pc, 0x5001);

    /* Test ReadBlock ($9D8F): read Track 1 Sector 0 into $4000 */
    host_low[0x04] = 1; /* r1L = Track 1 */
    host_low[0x05] = 0; /* r1H = Sector 0 */
    host_low[0x0a] = 0x00; /* r4L */
    host_low[0x0b] = 0x40; /* r4H = $4000 */
    cpu.pc = 0x9d90;
    host_low[0x1fe] = (uint8_t)(0x5002 - 1);
    host_low[0x1ff] = (uint8_t)((0x5002 - 1) >> 8);
    cpu.sp = 0xfd;
    EXPECT_EQ(drive_trap_geos(), true);
    EXPECT_EQ(cpu.a, 0);
    EXPECT_EQ(cpu.pc, 0x5002);
    /* Verify the direct sector read path was used (not intermediate copy) */
    EXPECT_EQ(host_ram[0x4000], 1);     /* Sector header byte 0 */
    EXPECT_EQ(host_ram[0x4001], 1);     /* Sector header byte 1 */
    EXPECT_EQ(host_ram[0x4002], 0x01);   /* Sector header byte 2 */
    EXPECT_EQ(host_ram[0x4003], 0x08);   /* Sector header byte 3 */

    /* Test ReadLink ($9D98): Track 1 Sector 0 -> next is (1, 1) */
    host_low[0x04] = 1;
    host_low[0x05] = 0;
    cpu.pc = 0x9d99;
    host_low[0x1fe] = (uint8_t)(0x5003 - 1);
    host_low[0x1ff] = (uint8_t)((0x5003 - 1) >> 8);
    cpu.sp = 0xfd;
    EXPECT_EQ(drive_trap_geos(), true);
    EXPECT_EQ(cpu.a, 0);
    EXPECT_EQ(host_low[0x04], 1); /* next track */
    EXPECT_EQ(host_low[0x05], 1); /* next sector */

    /* Test WriteBlock ($9D92) and readback */
    uint8_t write_pattern[256];
    for (unsigned int i = 0; i < 256; ++i)
        write_pattern[i] = (uint8_t)(0x33 ^ i);
    memcpy(host_ram + 0x6000, write_pattern, 256);
    host_low[0x04] = 10;
    host_low[0x05] = 5;
    host_low[0x0a] = 0x00;
    host_low[0x0b] = 0x60;
    cpu.pc = 0x9d93;
    host_low[0x1fe] = (uint8_t)(0x5004 - 1);
    host_low[0x1ff] = (uint8_t)((0x5004 - 1) >> 8);
    cpu.sp = 0xfd;
    EXPECT_EQ(drive_trap_geos(), true);
    EXPECT_EQ(cpu.a, 0);

    /* Read block back into $7000 */
    memset(host_ram + 0x7000, 0, 256);
    host_low[0x04] = 10;
    host_low[0x05] = 5;
    host_low[0x0a] = 0x00;
    host_low[0x0b] = 0x70;
    cpu.pc = 0x9d90;
    host_low[0x1fe] = (uint8_t)(0x5005 - 1);
    host_low[0x1ff] = (uint8_t)((0x5005 - 1) >> 8);
    cpu.sp = 0xfd;
    EXPECT_EQ(drive_trap_geos(), true);
    EXPECT_EQ(cpu.a, 0);
    EXPECT_EQ(memcmp(host_ram + 0x7000, write_pattern, 256), 0);

    /* Test GetDirHead ($9D9B) */
    cpu.pc = 0x9d9c;
    host_low[0x1fe] = (uint8_t)(0x5006 - 1);
    host_low[0x1ff] = (uint8_t)((0x5006 - 1) >> 8);
    cpu.sp = 0xfd;
    EXPECT_EQ(drive_trap_geos(), true);
    EXPECT_EQ(cpu.a, 0);
    EXPECT_EQ(memcmp(host_ram + 0x8200 + 0xad, "GEOS format V1.0", 16), 0);
}

static void test_geos_vlir_file_access(void)
{
    prepare_geos_d64();
    drive_install_geos_driver();

    /* Read DESKTOP VLIR Record Table (Track 2 Sector 0) into $8000 */
    host_low[0x04] = 2; /* Track 2 */
    host_low[0x05] = 0; /* Sector 0 */
    host_low[0x0a] = 0x00;
    host_low[0x0b] = 0x80; /* $8000 */
    cpu.pc = 0x9d90;
    host_low[0x1fe] = (uint8_t)(0x5010 - 1);
    host_low[0x1ff] = (uint8_t)((0x5010 - 1) >> 8);
    cpu.sp = 0xfd;
    EXPECT_EQ(drive_trap_geos(), true);
    EXPECT_EQ(cpu.a, 0);

    /* Record 0 is at (Track 2, Sector 1) */
    uint8_t rec0_track = host_ram[0x8000];
    uint8_t rec0_sector = host_ram[0x8001];
    EXPECT_EQ(rec0_track, 2);
    EXPECT_EQ(rec0_sector, 1);

    /* Read Record 0 into $8100 */
    host_low[0x04] = rec0_track;
    host_low[0x05] = rec0_sector;
    host_low[0x0a] = 0x00;
    host_low[0x0b] = 0x81;
    cpu.pc = 0x9d90;
    host_low[0x1fe] = (uint8_t)(0x5011 - 1);
    host_low[0x1ff] = (uint8_t)((0x5011 - 1) >> 8);
    cpu.sp = 0xfd;
    EXPECT_EQ(drive_trap_geos(), true);
    EXPECT_EQ(cpu.a, 0);
    EXPECT_EQ(memcmp(host_ram + 0x8102, "DESKTOP_RECORD_0_DATA", 21), 0);

    /* Record 1 is at (Track 2, Sector 2) */
    uint8_t rec1_track = host_ram[0x8002];
    uint8_t rec1_sector = host_ram[0x8003];
    EXPECT_EQ(rec1_track, 2);
    EXPECT_EQ(rec1_sector, 2);

    /* Read Record 1 into $8100 */
    host_low[0x04] = rec1_track;
    host_low[0x05] = rec1_sector;
    host_low[0x0a] = 0x00;
    host_low[0x0b] = 0x81;
    cpu.pc = 0x9d90;
    host_low[0x1fe] = (uint8_t)(0x5012 - 1);
    host_low[0x1ff] = (uint8_t)((0x5012 - 1) >> 8);
    cpu.sp = 0xfd;
    EXPECT_EQ(drive_trap_geos(), true);
    EXPECT_EQ(cpu.a, 0);
    EXPECT_EQ(memcmp(host_ram + 0x8102, "DESKTOP_RECORD_1_DATA", 21), 0);
}

int main(void)
{
    static const struct
    {
        const char *name;
        void (*run)(void);
    } tests[] = {
        {"drive-zero-map-load", test_drive_zero_map_load},
        {"plain-map-load", test_plain_map_load},
        {"prefixed-wildcards", test_prefixed_wildcards},
        {"selected-wildcards", test_selected_wildcards},
        {"unselected-wildcards", test_unselected_wildcards},
        {"unmatched-names", test_unmatched_names},
        {"prefixed-full-length-name", test_prefixed_full_length_name},
        {"fastload-drive-zero", test_fastload_drive_zero},
        {"map-load-after-bootstrap", test_map_load_after_bootstrap},
        {"raw-d64-geos-detection", test_raw_d64_geos_detection},
        {"raw-d64-sector-origin", test_raw_d64_sector_origin},
        {"raw-d64-prg-load", test_raw_d64_prg_load},
        {"actual-geos-bootstrap-once", test_actual_geos_bootstrap_once},
        {"geos-driver-traps", test_geos_driver_traps},
        {"geos-vlir-file-access", test_geos_vlir_file_access}
    };
    unsigned int passed = 0;
    unsigned int test_count = sizeof(tests) / sizeof(tests[0]);
    for (unsigned int test_index = 0; test_index < test_count; ++test_index)
    {
        unsigned int before = failures;
        printf("RUN  %s\n", tests[test_index].name);
        tests[test_index].run();
        if (failures == before)
            ++passed;
        printf("%s %s\n", failures == before ? "PASS" : "FAIL", tests[test_index].name);
    }
    printf("%u tests: %u passed, %u failed\n", test_count, passed, test_count - passed);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}