#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/drive.c"

enum
{
    HOST_PACKAGE_SIZE = 0x10000,
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
    EXPECT_EQ(drive_init(), true);
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
        {"map-load-after-bootstrap", test_map_load_after_bootstrap}
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