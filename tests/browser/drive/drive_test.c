#include <stdio.h>
#include <string.h>
#include "mocks.h"
#include "drive.h"
#include "crc32.h"
#include "browser_model.h"

#define drive_init() drive_init(CE_PACKAGE_DISK)

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        fprintf(stderr, "FAIL %s%s%s:%d: %s\n", current_test, \
                current_case ? " / " : "", current_case ? current_case : "", \
                __LINE__, #condition); \
        return; \
    } \
} while (0)

static unsigned int checks, failures;
static const char *current_test, *current_case;

typedef struct {
    CpuState cpu;
    uint8_t ram[MEM_SIZE], low[MEM_LOW_SIZE], rom[MEM_SIZE];
    DriveTrace trace[DRIVE_TRACE_MAX];
    uint8_t trace_count;
    unsigned int renders, unhides;
    bool boot_hidden;
} GuestSnapshot;

static GuestSnapshot guest_before;
static MockDisk disk_before;

static void save_guest(void)
{
    memcpy(&guest_before.cpu, &cpu, sizeof(cpu));
    memcpy(guest_before.ram, mock_ram, sizeof(mock_ram));
    memcpy(guest_before.low, mock_low, sizeof(mock_low));
    memcpy(guest_before.rom, mock_rom, sizeof(mock_rom));
    memcpy(guest_before.trace, drive_trace, sizeof(drive_trace));
    guest_before.trace_count = drive_trace_count;
    guest_before.renders = mock.render_requests;
    guest_before.unhides = mock.unhide_calls;
    guest_before.boot_hidden = mock.boot_hidden;
}

static bool guest_unchanged(void)
{
    return memcmp(&guest_before.cpu, &cpu, sizeof(cpu)) == 0 &&
        memcmp(guest_before.ram, mock_ram, sizeof(mock_ram)) == 0 &&
        memcmp(guest_before.low, mock_low, sizeof(mock_low)) == 0 &&
        memcmp(guest_before.rom, mock_rom, sizeof(mock_rom)) == 0 &&
        memcmp(guest_before.trace, drive_trace, sizeof(drive_trace)) == 0 &&
        guest_before.trace_count == drive_trace_count &&
        guest_before.renders == mock.render_requests &&
        guest_before.unhides == mock.unhide_calls &&
        guest_before.boot_hidden == mock.boot_hidden;
}

static void write24(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
}

static void set_entry(unsigned int index, const char *name, uint32_t offset,
                      uint32_t size, uint16_t address)
{
    uint8_t *entry = mock_disk_byte(1 + index * 22);
    size_t length = strlen(name);
    memset(entry, 0xa0, 16);
    memcpy(entry, name, length < 16 ? length : 16);
    write24(entry + 16, offset);
    write24(entry + 19, size);
    *mock_disk_byte(offset) = (uint8_t)address;
    *mock_disk_byte(offset + 1) = (uint8_t)(address >> 8);
}

static void fixture(unsigned int count)
{
    unsigned int index;
    uint32_t offset = 1 + count * 22;
    uint32_t position;
    mock_reset();
    mock_set_size(MOCK_CHUNK_SIZE * 2 + 37);
    for (position = 0; position < mock.disks[0].size; ++position)
        *mock_disk_byte(position) = (uint8_t)(position * 37u ^ (position >> 8));
    *mock_disk_byte(0) = (uint8_t)count;
    for (index = 0; index < count; ++index) {
        char name[17];
        uint8_t body[6] = { (uint8_t)index, 0x42, 0x53, 0x64, 0x75, 0x86 };
        (void)snprintf(name, sizeof(name), "FILE%03u", index);
        set_entry(index, name, offset, 8, (uint16_t)(0x2000 + index * 16));
        mock_disk_write(offset + 2, body, sizeof(body));
        offset += 8;
    }
}

static void metadata_header_boundary(void)
{
    DriveFileInfo info;
    unsigned int index;
    fixture(19);
    set_entry(18, "LAUNCHER", MOCK_CHUNK_SIZE - 1, 8, 0x1234);
    CHECK(drive_init());
    CHECK(drive_file_count() == 19);
    for (index = 0; index < 18; ++index) {
        char expected[17];
        (void)snprintf(expected, sizeof(expected), "FILE%03u", index);
        CHECK(drive_file_info((uint8_t)index, &info));
        CHECK(strcmp(info.name, expected) == 0);
        CHECK(info.size == 8);
        CHECK(info.load_address == 0x2000 + index * 16);
    }
    CHECK(drive_file_info(18, &info));
    CHECK(strcmp(info.name, "LAUNCHER") == 0);
    CHECK(info.size == 8);
    CHECK(info.load_address == 0x1234);
    CHECK(mock.byte_calls == 38);
    CHECK(mock.mapped[0] != mock.mapped[1]);
}

static void metadata_display_names(void)
{
    static const uint8_t raw_name[16] = {
        0xc1, 0xda, 0x01, 0x1f, 0x7f, 0xff, 0x80, 0xdb,
        'Q', 0xa0, 'R', 0xa0, 0xa0, 0xa0, 0xa0, 0xa0
    };
    DriveFileInfo info;
    char small[7];
    char one[3] = { 'L', 'X', 'R' };
    fixture(19);
    set_entry(0, "ABCDEFGHIJKLMNOP", 419, 8, 0x2000);
    memcpy(mock_disk_byte(23), raw_name, sizeof(raw_name));
    memset(mock_disk_byte(45), 0xa0, 16);
    CHECK(drive_init());
    memcpy(&disk_before, &mock.disks[0], sizeof(disk_before));
    CHECK(drive_file_info(0, &info));
    CHECK(strcmp(info.name, "ABCDEFGHIJKLMNOP") == 0);
    CHECK(info.name[16] == '\0');
    CHECK(drive_file_info(1, &info));
    CHECK(strcmp(info.name, "AZ??????Q R") == 0);
    CHECK(drive_file_info(2, &info));
    CHECK(strcmp(info.name, "") == 0);
    memset(small, '!', sizeof(small));
    CHECK(drive_file_name(0, small + 1, 5));
    CHECK(strcmp(small + 1, "ABCD") == 0);
    CHECK(small[0] == '!' && small[6] == '!');
    CHECK(drive_file_name(0, one + 1, 1));
    CHECK(one[0] == 'L' && one[1] == '\0' && one[2] == 'R');
    CHECK(memcmp(&disk_before, &mock.disks[0], sizeof(disk_before)) == 0);
}

static void metadata_invalid_arguments(void)
{
    DriveFileInfo info, before;
    char name[17];
    fixture(19);
    CHECK(drive_init());
    memset(&info, 0x6c, sizeof(info));
    memcpy(&before, &info, sizeof(info));
    memset(name, '!', sizeof(name));
    save_guest();
    CHECK(!drive_file_info(19, &info));
    CHECK(!drive_file_info(255, &info));
    CHECK(!drive_file_info(0, NULL));
    CHECK(!drive_file_name(0, NULL, 17));
    CHECK(!drive_file_name(0, name, 0));
    CHECK(!drive_file_name(19, name, sizeof(name)));
    CHECK(!drive_file_name(255, name, sizeof(name)));
    CHECK(memcmp(&before, &info, sizeof(info)) == 0);
    CHECK(name[0] == '!' && name[16] == '!');
    CHECK(mock.byte_calls == 0);
    CHECK(guest_unchanged());
}

static void metadata_invalid_ranges(void)
{
    static const struct {
        const char *name;
        uint32_t offset, size;
    } cases[] = {
        { "zero-size", 419, 0 },
        { "one-byte-size", 419, 1 },
        { "count-byte-overlap", 0, 2 },
        { "directory-overlap", 418, 2 },
        { "offset-after-end", MOCK_CHUNK_SIZE * 2 + 38, 2 },
        { "offset-at-end", MOCK_CHUNK_SIZE * 2 + 37, 2 },
        { "header-past-end", MOCK_CHUNK_SIZE * 2 + 36, 2 },
        { "body-past-end", MOCK_CHUNK_SIZE * 2 + 35, 3 },
        { "maximum-offset", 0xffffff, 2 },
        { "maximum-size", 419, 0xffffff }
    };
    DriveFileInfo info, before;
    size_t index;
    fixture(19);
    CHECK(drive_init());
    memset(&info, 0x6c, sizeof(info));
    memcpy(&before, &info, sizeof(info));
    save_guest();
    for (index = 0; index < ARRAY_COUNT(cases); ++index) {
        uint8_t *entry = mock_disk_byte(1);
        current_case = cases[index].name;
        write24(entry + 16, cases[index].offset);
        write24(entry + 19, cases[index].size);
        CHECK(!drive_file_info(0, &info));
        CHECK(memcmp(&before, &info, sizeof(info)) == 0);
        CHECK(mock.byte_calls == 0);
        CHECK(guest_unchanged());
    }
}

static void metadata_truncated_chunks(void)
{
    unsigned int chunk;
    for (chunk = 0; chunk < 3; ++chunk) {
        DriveFileInfo info, before;
        fixture(19);
        current_case = chunk == 0 ? "low-byte-chunk" :
            chunk == 1 ? "high-byte-chunk" : "last-byte-truncated";
        set_entry(18, "BOUNDARY", chunk == 2 ? mock.disks[0].size - 2 :
                  MOCK_CHUNK_SIZE - 1, 2, 0x4321);
        CHECK(drive_init());
        --mock.disks[0].chunks[chunk].available;
        memset(&info, 0x6c, sizeof(info));
        memcpy(&before, &info, sizeof(info));
        save_guest();
        CHECK(!drive_file_info(18, &info));
        CHECK(memcmp(&before, &info, sizeof(info)) == 0);
        CHECK(mock.byte_calls == 2);
        CHECK(guest_unchanged());
    }
}

static void metadata_header_only_at_end(void)
{
    DriveFileInfo info;
    fixture(19);
    set_entry(18, "EMPTY", mock.disks[0].size - 2, 2, 0xffff);
    CHECK(drive_init());
    CHECK(drive_file_info(18, &info));
    CHECK(info.size == 2 && info.load_address == 0xffff);
}

static void metadata_queries_are_read_only(void)
{
    DriveFileInfo info;
    DriveIdentity identity;
    unsigned int index;
    fixture(19);
    CHECK(drive_init());
    drive_select_file(18);
    cpu.pc = 0x4567;
    cpu.sp = 0x89;
    cpu.a = 0xab;
    cpu.cf = true;
    cpu.cycles = 12345;
    drive_trace_count = 2;
    memset(drive_trace, 0x7b, sizeof(drive_trace));
    mock.boot_hidden = true;
    save_guest();
    memcpy(&disk_before, &mock.disks[0], sizeof(disk_before));
    for (index = 0; index < 19; ++index)
        CHECK(drive_file_info((uint8_t)index, &info));
    CHECK(drive_identity(&identity));
    CHECK(drive_identity(&identity));
    CHECK(guest_unchanged());
    CHECK(mock.io_reads == 0);
    CHECK(memcmp(&disk_before, &mock.disks[0], sizeof(disk_before)) == 0);
}

static void metadata_count_255(void)
{
    DriveFileInfo info;
    fixture(255);
    CHECK(drive_init());
    CHECK(drive_file_count() == 255);
    CHECK(drive_file_info(254, &info));
    CHECK(strcmp(info.name, "FILE254") == 0);
    CHECK(info.size == 8 && info.load_address == 0x2fe0);
    CHECK(!drive_file_info(255, &info));
}

static uint32_t disk_crc_oracle(void)
{
    uint32_t state = UINT32_MAX;
    uint32_t offset;
    for (offset = 0; offset < mock.disks[mock.active_disk].size; ++offset) {
        unsigned int bit;
        state ^= *mock_disk_byte(offset);
        for (bit = 0; bit < 8; ++bit)
            state = state & 1u ? (state >> 1) ^ UINT32_C(0xedb88320) : state >> 1;
    }
    return ~state;
}

static void crc32_standard_vector(void)
{
    static const uint8_t vector[] = "123456789";
    size_t split;
    CHECK(crc32_update(0, NULL, 0) == 0);
    CHECK(crc32_update(0, vector, 9) == UINT32_C(0xcbf43926));
    CHECK(crc32_update(UINT32_C(0x87654321), NULL, 0) == UINT32_C(0x87654321));
    for (split = 0; split <= 9; ++split) {
        uint32_t first = crc32_update(0, vector, split);
        CHECK(crc32_update(first, vector + split, 9 - split) == UINT32_C(0xcbf43926));
    }
}

static void identity_all_chunks_and_cache(void)
{
    DriveIdentity identity, cached;
    uint32_t streaming = 0;
    unsigned int chunk;
    fixture(19);
    CHECK(drive_init());
    CHECK(!drive_identity(NULL));
    CHECK(mock.direct_calls[0] == 1);
    CHECK(drive_identity(&identity));
    CHECK(identity.size == mock.disks[0].size);
    CHECK(identity.crc32 == disk_crc_oracle());
    CHECK(mock.direct_calls[0] == 2 && mock.direct_calls[1] == 1 &&
          mock.direct_calls[2] == 1 && mock.direct_calls[3] == 0);
    for (chunk = 0; chunk < 3; ++chunk)
        streaming = crc32_update(streaming, mock.disks[0].chunks[chunk].data,
                                 mock.disks[0].chunks[chunk].available);
    CHECK(streaming == identity.crc32);
    CHECK(drive_identity(&cached));
    CHECK(cached.size == identity.size && cached.crc32 == identity.crc32);
    CHECK(mock.direct_calls[0] == 2 && mock.direct_calls[1] == 1 &&
          mock.direct_calls[2] == 1 && mock.direct_calls[3] == 0);
    CHECK(mock.byte_calls == 0 && mock.size_calls == 1);
}

static void identity_invalidation_refreshes_every_chunk(void)
{
    static const uint32_t changed_offsets[] = {
        1, 0x1000, MOCK_CHUNK_SIZE + 97, MOCK_CHUNK_SIZE * 2 + 36
    };
    DriveIdentity previous, cached, fresh;
    size_t index;
    fixture(19);
    CHECK(drive_init());
    CHECK(drive_identity(&previous));
    for (index = 0; index < ARRAY_COUNT(changed_offsets); ++index) {
        *mock_disk_byte(changed_offsets[index]) ^= 0x80;
        CHECK(drive_identity(&cached));
        CHECK(cached.crc32 == previous.crc32);
        drive_invalidate();
        CHECK(drive_file_count() == 0);
        CHECK(!drive_identity(&fresh));
        CHECK(drive_init());
        CHECK(drive_identity(&fresh));
        CHECK(fresh.size == previous.size);
        CHECK(fresh.crc32 == disk_crc_oracle());
        CHECK(fresh.crc32 != previous.crc32);
        previous = fresh;
    }
    CHECK(mock.close_calls == 0);
}

static void identity_excludes_physical_padding(void)
{
    DriveIdentity before, after;
    fixture(19);
    CHECK(drive_init());
    CHECK(drive_identity(&before));
    *mock_disk_byte(mock.disks[0].size) ^= 0xff;
    *mock_disk_byte(MOCK_CHUNK_SIZE * 3) ^= 0xff;
    drive_invalidate();
    CHECK(drive_init());
    CHECK(drive_identity(&after));
    CHECK(before.size == after.size && before.crc32 == after.crc32);
    CHECK(mock.direct_calls[3] == 0);
}

static void identity_missing_chunk_is_not_cached(void)
{
    unsigned int chunk;
    for (chunk = 1; chunk < 3; ++chunk) {
        DriveIdentity identity, before;
        size_t available;
        fixture(19);
        CHECK(drive_init());
        available = mock.disks[0].chunks[chunk].available;
        mock.disks[0].chunks[chunk].available = 0;
        memset(&identity, 0x6c, sizeof(identity));
        memcpy(&before, &identity, sizeof(identity));
        CHECK(!drive_identity(&identity));
        CHECK(memcmp(&before, &identity, sizeof(identity)) == 0);
        CHECK(mock.direct_calls[0] == 2 && mock.direct_calls[chunk] == 1);
        mock.disks[0].chunks[chunk].available = available;
        CHECK(drive_identity(&identity));
        CHECK(identity.crc32 == disk_crc_oracle());
        CHECK(mock.direct_calls[0] == 3 && mock.direct_calls[chunk] == 2);
    }
}

static void failed_reinit_clears_disk_state(void)
{
    static const char *const cases[] = {
        "missing-package", "missing-directory-chunk", "empty-directory", "short-directory"
    };
    size_t index;
    for (index = 0; index < ARRAY_COUNT(cases); ++index) {
        DriveFileInfo info, info_before;
        DriveIdentity identity, identity_before;
        char name[17] = "untouched";
        fixture(19);
        current_case = cases[index];
        CHECK(drive_init());
        CHECK(drive_identity(&identity));
        drive_select_file(18);
        if (index == 0)
            mock_set_size(0);
        else if (index == 1)
            mock.disks[0].chunks[0].available = 0;
        else if (index == 2)
            *mock_disk_byte(0) = 0;
        else
            mock_set_size(418);
        memset(&info, 0x6c, sizeof(info));
        memcpy(&info_before, &info, sizeof(info));
        memcpy(&identity_before, &identity, sizeof(identity));
        CHECK(!drive_init());
        CHECK(drive_file_count() == 0);
        CHECK(!drive_file_info(0, &info));
        CHECK(!drive_file_name(0, name, sizeof(name)));
        CHECK(!drive_identity(&identity));
        CHECK(memcmp(&info_before, &info, sizeof(info)) == 0);
        CHECK(memcmp(&identity_before, &identity, sizeof(identity)) == 0);
        CHECK(strcmp(name, "untouched") == 0);
        CHECK(mock.close_calls == 0);
    }
}

static void remapped_package_uses_fresh_pointers(void)
{
    DriveFileInfo info;
    DriveIdentity before, after;
    const uint8_t *old_directory;
    unsigned int chunk;
    fixture(19);
    CHECK(drive_init());
    CHECK(drive_identity(&before));
    old_directory = mock.mapped[0];
    memcpy(&mock.disks[1], &mock.disks[0], sizeof(MockDisk));
    mock.active_disk = 1;
    set_entry(0, "RELOCATED", 419, 8, 0x4567);
    *mock_disk_byte(mock.disks[1].size - 1) ^= 0xff;
    CHECK(mock.mapped[0] == old_directory);
    drive_invalidate();
    CHECK(mock.close_calls == 0);
    CEPackageCloseDirect();
    CHECK(mock.close_calls == 1);
    for (chunk = 0; chunk < MOCK_CHUNK_COUNT; ++chunk) {
        CHECK(mock.mapped[chunk] == NULL);
        memset(mock.disks[0].chunks[chunk].data, 0xdd, MOCK_CHUNK_SIZE);
    }
    CHECK(old_directory[0] == 0xdd);
    CHECK(drive_file_count() == 0);
    CHECK(!drive_file_info(0, &info));
    CHECK(!drive_identity(&after));
    CHECK(drive_init());
    CHECK(mock.mapped[0] != old_directory);
    CHECK(drive_file_count() == 19);
    CHECK(drive_file_info(0, &info));
    CHECK(strcmp(info.name, "RELOCATED") == 0 && info.load_address == 0x4567);
    CHECK(drive_identity(&after));
    CHECK(after.size == before.size && after.crc32 != before.crc32);
    CHECK(after.crc32 == disk_crc_oracle());
}

static void rename_entry(unsigned int index, const char *name)
{
    uint8_t *entry = mock_disk_byte(1 + index * 22);
    size_t length = strlen(name);
    memset(entry, 0xa0, 16);
    memcpy(entry, name, length < 16 ? length : 16);
}

static void prepare_load(const uint8_t *name, uint8_t length, uint8_t sa,
                          uint16_t relocation)
{
    if (length)
        memcpy(mock_low + 0x0500, name, length);
    mock_low[0xb7] = length;
    mock_low[0xbb] = 0x00;
    mock_low[0xbc] = 0x05;
    mock_low[0xb9] = sa;
    mock_low[0xc3] = (uint8_t)relocation;
    mock_low[0xc4] = (uint8_t)(relocation >> 8);
    mock_low[0x90] = 0xcc;
    mock_low[0x1fc] = 0x66;
    mock_low[0x1fd] = 0x45;
    cpu.pc = 0xf4a6;
    cpu.sp = 0xfb;
    cpu.a = 0;
    cpu.x = 0x77;
    cpu.y = 0x88;
    cpu.cf = true;
    cpu.cycles = 1000;
    mock_rom[0xf4a5] = 0x85;
    mock_rom[0xf4a6] = 0x93;
    drive_patch_kernal();
}

static void prepare_named_load(const char *name, uint8_t sa, uint16_t relocation)
{
    prepare_load((const uint8_t *)name, (uint8_t)strlen(name), sa, relocation);
}

static bool ram_matches(uint16_t address, const uint8_t *expected, size_t length)
{
    size_t offset;
    for (offset = 0; offset < length; ++offset) {
        if (*mem_host_ptr((uint16_t)(address + offset)) != expected[offset])
            return false;
    }
    return true;
}

static void expect_load_result(unsigned int trace_index, uint8_t entry,
                                uint16_t address, uint16_t end, uint8_t sa,
                                bool fastload)
{
    const DriveTrace *trace;
    unsigned int end_zp = fastload ? 0xac : 0xae;
    CHECK(drive_trace_count == trace_index + 1);
    CHECK(trace_index < DRIVE_TRACE_MAX);
    trace = &drive_trace[trace_index];
    CHECK(trace->caller == 0x4567);
    CHECK(trace->entry == entry);
    CHECK(trace->addr == address && trace->end == end);
    CHECK(trace->sa == (fastload ? 0x60 : sa));
    CHECK(cpu.pc == 0x4567 && cpu.sp == 0xfd);
    CHECK(cpu.a == 0 && !cpu.cf);
    CHECK(cpu.x == (uint8_t)end && cpu.y == (uint8_t)(end >> 8));
    CHECK(cpu.cycles == 1100);
    CHECK(mock_low[end_zp] == (uint8_t)end);
    CHECK(mock_low[end_zp + 1] == (uint8_t)(end >> 8));
    CHECK(mock_low[0x90] == (fastload ? 0 : 0x40));
    if (fastload)
        CHECK(mock_low[0xb8] == sa && mock_low[0xb9] == 0x60);
}

static void launcher_selection_preserves_hidden_support(void)
{
    static const uint8_t launcher[] = { 0xf1, 0x82, 0x73, 0x64, 0x55, 0x46 };
    static const uint8_t support[] = { 3, 0x42, 0x53, 0x64, 0x75, 0x86 };
    BrowserModel model;
    DriveFileInfo info;
    uint32_t original_crc;
    fixture(19);
    set_entry(18, "LAUNCHER", MOCK_CHUNK_SIZE - 1, 8, 0x1234);
    mock_disk_write(MOCK_CHUNK_SIZE + 1, launcher, sizeof(launcher));
    rename_entry(3, "HIDDEN.OBJ");
    CHECK(drive_init());
    original_crc = disk_crc_oracle();
    browser_model_init(&model, drive_file_count(), NULL);
    CHECK(browser_model_move(&model, 19));
    CHECK(browser_model_entry(&model) == 19);
    CHECK(browser_model_launcher(&model));
    CHECK(browser_model_move(&model, -15));
    CHECK(browser_model_entry(&model) == 4);
    CHECK(browser_model_launcher(&model));
    CHECK(browser_model_hide(&model));
    CHECK(browser_flag(model.preferences.hidden, 3));
    CHECK(browser_flag(model.preferences.launchers, 3));
    browser_model_view(&model);
    CHECK(model.preferences.view == BROWSER_LAUNCHERS);
    CHECK(model.count == 2 && model.selected == 1);
    CHECK(browser_model_entry(&model) == 19);
    drive_select_file((uint8_t)(browser_model_entry(&model) - 1));
    CHECK(drive_file_info(18, &info));
    CHECK(info.load_address == 0x1234);
    prepare_named_load("*", 1, 0);
    CHECK(drive_trap_load());
    expect_load_result(0, 18, 0x1234, 0x123a, 1, false);
    CHECK(ram_matches(0x1234, launcher, sizeof(launcher)));
    CHECK(drive_trace[0].len == 1 && drive_trace[0].name[0] == '*');
    prepare_named_load("HIDDEN.OBJ", 1, 0);
    CHECK(drive_trap_load());
    expect_load_result(1, 3, 0x2030, 0x2036, 1, false);
    CHECK(ram_matches(0x2030, support, sizeof(support)));
    CHECK(drive_trace[1].len == 10);
    CHECK(memcmp(drive_trace[1].name, "HID", 3) == 0);
    CHECK(browser_flag(model.preferences.hidden, 3));
    CHECK(drive_file_count() == 19);
    CHECK(disk_crc_oracle() == original_crc);
}

static void selected_wildcard_and_invalid_selection(void)
{
    fixture(19);
    CHECK(drive_init());
    drive_select_file(18);
    prepare_named_load("0:*", 1, 0);
    CHECK(drive_trap_load());
    expect_load_result(0, 18, 0x2120, 0x2126, 1, false);
    CHECK(mock_ram[0x2120] == 18);
    drive_select_file(19);
    prepare_named_load("*", 1, 0);
    CHECK(drive_trap_load());
    expect_load_result(1, 0, 0x2000, 0x2006, 1, false);
    CHECK(mock_ram[0x2000] == 0);
    drive_select_file(255);
    prepare_named_load("*", 1, 0);
    CHECK(drive_trap_load());
    expect_load_result(2, 0, 0x2000, 0x2006, 1, false);
}

static void filename_patterns_keep_original_order(void)
{
    static const struct {
        const char *pattern;
        uint8_t entry;
    } cases[] = {
        { "", 0 }, { "0:", 0 }, { "?ART?", 1 }, { "PART*", 1 },
        { "0:PART?", 1 }, { "*IGNORED", 0 }, { "PART", 255 },
        { "PART??", 255 }, { "PARTB?", 255 }, { "partb", 255 },
        { "HIDDEN.OBJ", 3 }, { "MISSING", 255 }
    };
    size_t index;
    for (index = 0; index < ARRAY_COUNT(cases); ++index) {
        uint8_t entry = cases[index].entry;
        fixture(19);
        current_case = cases[index].pattern;
        rename_entry(0, "ZFIRST");
        rename_entry(1, "PARTB");
        rename_entry(2, "PARTA");
        rename_entry(3, "HIDDEN.OBJ");
        CHECK(drive_init());
        drive_select_file(18);
        prepare_named_load(cases[index].pattern, 1, 0);
        CHECK(drive_trap_load());
        if (entry == 255) {
            CHECK(cpu.a == 4 && cpu.cf);
            CHECK(cpu.pc == 0x4567 && cpu.sp == 0xfd);
            CHECK(drive_trace_count == 1 && drive_trace[0].entry == 255);
            CHECK(drive_trace[0].addr == 0 && drive_trace[0].end == 0);
            CHECK(mock_ram[0x2000] == 0xcd && mock_ram[0x2030] == 0xcd);
        } else {
            uint16_t address = (uint16_t)(0x2000 + entry * 16);
            uint8_t expected[] = { entry, 0x42, 0x53, 0x64, 0x75, 0x86 };
            expect_load_result(0, entry, address, (uint16_t)(address + 6), 1, false);
            CHECK(ram_matches(address, expected, sizeof(expected)));
        }
    }
}

static void exact_obj_and_full_length_names(void)
{
    static const char *const patterns[] = {
        ".OBJ", "ABCDEFGHIJKLMNOP", "ABCDEFGHIJKLMNOPQ", "ABCDEFGHIJKLMNOP?",
        "ABCDEFGHIJKLMNOP*"
    };
    static const uint8_t entries[] = { 12, 6, 255, 255, 6 };
    size_t index;
    for (index = 0; index < ARRAY_COUNT(patterns); ++index) {
        fixture(19);
        current_case = patterns[index];
        rename_entry(12, ".OBJ");
        rename_entry(6, "ABCDEFGHIJKLMNOP");
        CHECK(drive_init());
        drive_select_file(18);
        prepare_named_load(patterns[index], 1, 0);
        CHECK(drive_trap_load());
        CHECK(drive_trace_count == 1 && drive_trace[0].entry == entries[index]);
        if (entries[index] == 255) {
            CHECK(cpu.a == 4 && cpu.cf);
        } else {
            uint16_t address = (uint16_t)(0x2000 + entries[index] * 16);
            uint8_t expected[] = { entries[index], 0x42, 0x53, 0x64, 0x75, 0x86 };
            expect_load_result(0, entries[index], address, (uint16_t)(address + 6), 1, false);
            CHECK(ram_matches(address, expected, sizeof(expected)));
        }
    }
}

static void raw_petscii_lookup_survives_display_query(void)
{
    static const uint8_t raw_name[] = { 'R', 0xc1, 0x01, 'W' };
    static const uint8_t expected[] = { 5, 0x42, 0x53, 0x64, 0x75, 0x86 };
    DriveFileInfo info;
    fixture(19);
    rename_entry(5, "XXXX");
    memcpy(mock_disk_byte(1 + 5 * 22), raw_name, sizeof(raw_name));
    CHECK(drive_init());
    drive_select_file(18);
    CHECK(drive_file_info(5, &info));
    CHECK(strcmp(info.name, "RA?W") == 0);
    prepare_load(raw_name, sizeof(raw_name), 1, 0);
    CHECK(drive_trap_load());
    expect_load_result(0, 5, 0x2050, 0x2056, 1, false);
    CHECK(ram_matches(0x2050, expected, sizeof(expected)));
    CHECK(memcmp(drive_trace[0].name, raw_name, 3) == 0);
    CHECK(memcmp(mock_disk_byte(1 + 5 * 22), raw_name, sizeof(raw_name)) == 0);
    prepare_named_load(info.name, 1, 0);
    CHECK(drive_trap_load());
    CHECK(cpu.a == 4 && cpu.cf && drive_trace[1].entry == 255);
}

static void browser_index_254_loads_original_entry(void)
{
    static const uint8_t expected[] = { 254, 0x42, 0x53, 0x64, 0x75, 0x86 };
    BrowserModel model;
    fixture(255);
    CHECK(drive_init());
    browser_model_init(&model, drive_file_count(), NULL);
    CHECK(model.count == 256);
    CHECK(browser_model_move(&model, 255));
    CHECK(browser_model_entry(&model) == 255);
    CHECK(browser_model_launcher(&model));
    browser_model_view(&model);
    CHECK(model.count == 2 && browser_model_entry(&model) == 255);
    drive_select_file((uint8_t)(browser_model_entry(&model) - 1));
    prepare_named_load("*", 1, 0);
    CHECK(drive_trap_load());
    expect_load_result(0, 254, 0x2fe0, 0x2fe6, 1, false);
    CHECK(ram_matches(0x2fe0, expected, sizeof(expected)));
}

static void load_crosses_chunk_and_ram_seam(void)
{
    static const uint8_t body[] = { 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xa9 };
    fixture(1);
    set_entry(0, "CROSS", MOCK_CHUNK_SIZE - 4, sizeof(body) + 2, 0x0ffd);
    mock_disk_write(MOCK_CHUNK_SIZE - 2, body, sizeof(body));
    CHECK(drive_init());
    prepare_named_load("CROSS", 1, 0);
    CHECK(drive_trap_load());
    expect_load_result(0, 0, 0x0ffd, 0x1007, 1, false);
    CHECK(ram_matches(0x0ffd, body, sizeof(body)));
    CHECK(mock_low[0x0ffc] == 0 && mock_ram[0x1007] == 0xcd);
    CHECK(mock_ram[0x0ffd] == 0xcd && mock_ram[0x0fff] == 0xcd);
    CHECK(mock.mapped[1] != NULL);
}

static void load_wraps_ram_under_kernal_rom(void)
{
    static const uint8_t body[] = { 0x17, 0x28, 0x39, 0x4a, 0x5b, 0x6c, 0x7d, 0x8e };
    fixture(1);
    set_entry(0, "WRAP", MOCK_CHUNK_SIZE - 1, sizeof(body) + 2, 0xfffd);
    mock_disk_write(MOCK_CHUNK_SIZE + 1, body, sizeof(body));
    CHECK(drive_init());
    prepare_named_load("WRAP", 1, 0);
    save_guest();
    CHECK(drive_trap_load());
    expect_load_result(0, 0, 0xfffd, 0x0005, 1, false);
    CHECK(ram_matches(0xfffd, body, sizeof(body)));
    CHECK(mock_ram[0xfffc] == 0xcd && mock_low[5] == 0);
    CHECK(mock_ram[0] == 0xcd && mock_ram[4] == 0xcd);
    CHECK(memcmp(guest_before.rom, mock_rom, sizeof(mock_rom)) == 0);
    CHECK(mem_rd_pg[0xf4].ptr == mock_rom);
}

static void load_relocation_and_verify_preserve_behavior(void)
{
    static const uint8_t body[] = { 0, 0x42, 0x53, 0x64, 0x75, 0x86 };
    unsigned int verify;
    for (verify = 0; verify < 2; ++verify) {
        fixture(1);
        CHECK(drive_init());
        prepare_named_load("FILE000", 0, 0x0ffe);
        cpu.a = (uint8_t)verify;
        save_guest();
        CHECK(drive_trap_load());
        expect_load_result(0, 0, 0x0ffe, 0x1004, 0, false);
        CHECK(mock_ram[0x2000] == 0xcd);
        if (verify) {
            CHECK(memcmp(guest_before.ram, mock_ram, sizeof(mock_ram)) == 0);
            guest_before.low[0x90] = 0x40;
            guest_before.low[0xae] = 0x04;
            guest_before.low[0xaf] = 0x10;
            CHECK(memcmp(guest_before.low, mock_low, sizeof(mock_low)) == 0);
        } else {
            CHECK(ram_matches(0x0ffe, body, sizeof(body)));
        }
    }
}

static const uint8_t resident_signature[] = {
    0xa9, 0x00, 0x85, 0x93, 0x78, 0xa6, 0xb9, 0x86,
    0xb8, 0xa9, 0x60, 0x85, 0xb9, 0x20, 0xd5, 0xf3
};

static void resident_fastload_serves_named_support(void)
{
    static const uint8_t support[] = { 3, 0x42, 0x53, 0x64, 0x75, 0x86 };
    fixture(19);
    set_entry(18, "RESIDENT", MOCK_CHUNK_SIZE - 1, sizeof(resident_signature) + 2, 0x1a4e);
    mock_disk_write(MOCK_CHUNK_SIZE + 1, resident_signature, sizeof(resident_signature));
    rename_entry(3, "HIDDEN.OBJ");
    CHECK(drive_init());
    drive_select_file(18);
    prepare_named_load("*", 1, 0);
    CHECK(drive_trap_load());
    expect_load_result(0, 18, 0x1a4e, 0x1a5e, 1, false);
    CHECK(mock_ram[0x1a4e] == DRIVE_TRAP2_OPCODE);
    CHECK(memcmp(mock_ram + 0x1a4f, resident_signature + 1, sizeof(resident_signature) - 1) == 0);
    prepare_named_load("HIDDEN.OBJ", 1, 0);
    cpu.pc = 0x1a4f;
    CHECK(drive_trap_fastload());
    expect_load_result(1, 3, 0x2030, 0x2036, 1, true);
    CHECK(ram_matches(0x2030, support, sizeof(support)));
    CHECK(drive_trace[1].len == 10 && memcmp(drive_trace[1].name, "HID", 3) == 0);
    prepare_named_load("HIDDEN.OBJ", 0, 0x0800);
    cpu.pc = 0x1a4f;
    CHECK(drive_trap_fastload());
    expect_load_result(2, 3, 0x0800, 0x0806, 0, true);
    CHECK(ram_matches(0x0800, support, sizeof(support)));
    prepare_named_load("MISSING", 1, 0);
    cpu.pc = 0x1a4f;
    save_guest();
    CHECK(drive_trap_fastload());
    CHECK(cpu.cf && mock_low[0x90] == 0x02);
    CHECK(cpu.pc == 0x4567 && cpu.sp == 0xfd);
    CHECK(drive_trace_count == 4 && drive_trace[3].entry == 255);
    CHECK(memcmp(guest_before.ram, mock_ram, sizeof(mock_ram)) == 0);
}

static void resident_trap_requires_complete_signature(void)
{
    size_t position;
    for (position = 0; position < sizeof(resident_signature); ++position) {
        uint8_t damaged[sizeof(resident_signature)];
        fixture(1);
        memcpy(damaged, resident_signature, sizeof(damaged));
        damaged[position] ^= 1;
        set_entry(0, "RESIDENT", 23, sizeof(damaged) + 2, 0x1a4e);
        mock_disk_write(25, damaged, sizeof(damaged));
        CHECK(drive_init());
        prepare_named_load("RESIDENT", 1, 0);
        CHECK(drive_trap_load());
        CHECK(ram_matches(0x1a4e, damaged, sizeof(damaged)));
        CHECK(mock_ram[0x1a4e] != DRIVE_TRAP2_OPCODE);
    }
}

static void unknown_traps_do_not_touch_guest(void)
{
    unsigned int variant;
    for (variant = 0; variant < 6; ++variant) {
        fixture(19);
        CHECK(drive_init());
        prepare_named_load("*", 1, 0);
        if (variant == 0)
            cpu.pc = 0xf4a7;
        else if (variant == 1)
            mem_rd_pg[0xf4].ptr = NULL;
        else if (variant == 2)
            mock_rom[0xf4a5] = 0xea;
        else if (variant == 3)
            mem_rd_pg[0xf4].ptr = mock_ram;
        else if (variant == 4) {
            cpu.pc = 0x1a50;
            mock_ram[0x1a4e] = DRIVE_TRAP2_OPCODE;
        } else {
            cpu.pc = 0x1a4f;
            mock_ram[0x1a4e] = 0xa9;
        }
        save_guest();
        CHECK(!(variant < 4 ? drive_trap_load() : drive_trap_fastload()));
        CHECK(guest_unchanged());
        CHECK(mock.byte_calls == 0 && mock.io_reads == 0);
    }
}

static void missing_file_returns_error_without_copy(void)
{
    fixture(19);
    CHECK(drive_init());
    prepare_named_load("ABSENT", 1, 0);
    save_guest();
    CHECK(drive_trap_load());
    CHECK(cpu.a == 4 && cpu.cf);
    CHECK(cpu.pc == 0x4567 && cpu.sp == 0xfd && cpu.cycles == 1000);
    CHECK(drive_trace_count == 1 && drive_trace[0].entry == 255);
    CHECK(drive_trace[0].caller == 0x4567);
    CHECK(drive_trace[0].addr == 0 && drive_trace[0].end == 0);
    guest_before.low[0x90] = 0;
    CHECK(memcmp(guest_before.low, mock_low, sizeof(mock_low)) == 0);
    CHECK(memcmp(guest_before.ram, mock_ram, sizeof(mock_ram)) == 0);
    mock.frames = 1;
    drive_tick();
    CHECK(mock.unhide_calls == 1 && mock.render_requests == 1);
}

static void disabled_drive_reports_device_not_present(void)
{
    DriveIdentity identity;
    DriveFileInfo info;
    fixture(19);
    CHECK(drive_init());
    CHECK(drive_identity(&identity));
    drive_select_file(18);
    drive_disable();
    CHECK(drive_file_count() == 0);
    CHECK(!drive_file_info(0, &info));
    CHECK(!drive_identity(&identity));
    CHECK(mock.close_calls == 0);
    prepare_named_load("*", 1, 0);
    CHECK(mock_rom[0xf4a5] == DRIVE_TRAP_OPCODE);
    save_guest();
    CHECK(drive_trap_load());
    CHECK(cpu.a == 5 && cpu.cf);
    CHECK(cpu.pc == 0x4567 && cpu.sp == 0xfd && cpu.cycles == 1000);
    CHECK(drive_trace_count == 0);
    guest_before.low[0x90] = 0;
    CHECK(memcmp(guest_before.low, mock_low, sizeof(mock_low)) == 0);
    CHECK(memcmp(guest_before.ram, mock_ram, sizeof(mock_ram)) == 0);
    cpu.pc = 0x1a4f;
    mock_ram[0x1a4e] = DRIVE_TRAP2_OPCODE;
    save_guest();
    CHECK(!drive_trap_fastload());
    mock.frames = 1000;
    drive_tick();
    CHECK(guest_unchanged());
}

static void init_resets_startup_without_changing_later_loads(void)
{
    static const uint8_t load_command[] = {
        0x4c, 0xcf, 0x22, 0x2a, 0x22, 0x2c, 0x38, 0x2c, 0x31, 0x0d
    };
    static const uint8_t run_command[] = { 0x52, 0x55, 0x4e, 0x0d };
    static const uint8_t support[] = { 3, 0x42, 0x53, 0x64, 0x75, 0x86 };
    unsigned int renders;
    fixture(19);
    rename_entry(3, "HIDDEN.OBJ");
    CHECK(drive_init());
    drive_select_file(18);
    mock_rom[0xe5cd] = 0xa5;
    mock_rom[0xe5ce] = 0xc6;
    mock_rom[0xe5cf] = 0x85;
    mock_rom[0xe5d0] = 0xcc;
    mock_rom[0xe5d4] = 0xf0;
    mock_rom[0xe5d5] = 0xf7;
    prepare_named_load("*", 1, 0);
    mock.frames = 5;
    mock.boot_hidden = true;
    cpu.pc = 0xe5cd;
    drive_tick();
    CHECK(mock_low[0xc6] == sizeof(load_command));
    CHECK(memcmp(mock_low + 0x0277, load_command, sizeof(load_command)) == 0);
    CHECK(mock.render_requests == 1);
    mock_low[0xc6] = 0;
    prepare_named_load("*", 1, 0);
    CHECK(drive_trap_load());
    expect_load_result(0, 18, 0x2120, 0x2126, 1, false);
    cpu.pc = 0xe5cd;
    drive_tick();
    CHECK(mock_low[0xc6] == sizeof(run_command));
    CHECK(memcmp(mock_low + 0x0277, run_command, sizeof(run_command)) == 0);
    CHECK(mock.render_requests == 2 && mock.unhide_calls == 0);
    CHECK(drive_init());
    CHECK(drive_trace_count == 0);
    CHECK(drive_file_count() == 19);
    mock_low[0xc6] = 0;
    mock.frames = 6;
    cpu.pc = 0xe5cd;
    drive_tick();
    CHECK(mock_low[0xc6] == 0);
    mock.frames = 17;
    drive_tick();
    CHECK(mock.unhide_calls == 0 && mock.boot_hidden);
    CHECK(mock.render_requests == 2);
    mock.frames = 60;
    cpu.pc = 0x2000;
    drive_tick();
    CHECK(mock_low[0xc6] == sizeof(load_command));
    CHECK(memcmp(mock_low + 0x0277, load_command, sizeof(load_command)) == 0);
    mock_low[0xc6] = 0;
    prepare_named_load("*", 1, 0);
    CHECK(drive_trap_load());
    expect_load_result(0, 0, 0x2000, 0x2006, 1, false);
    CHECK(mock_ram[0x2000] == 0);
    cpu.pc = 0xe5cd;
    drive_tick();
    CHECK(mock_low[0xc6] == sizeof(run_command));
    CHECK(memcmp(mock_low + 0x0277, run_command, sizeof(run_command)) == 0);
    mock_low[0xc6] = 0;
    prepare_named_load("HIDDEN.OBJ", 1, 0);
    CHECK(drive_trap_load());
    expect_load_result(1, 3, 0x2030, 0x2036, 1, false);
    CHECK(ram_matches(0x2030, support, sizeof(support)));
    renders = mock.render_requests;
    mock.frames = 71;
    drive_tick();
    CHECK(mock_low[0xc6] == 0 && mock.render_requests == renders);
    CHECK(mock.unhide_calls == 0);
    mock.frames = 72;
    drive_tick();
    CHECK(mock.unhide_calls == 1 && !mock.boot_hidden);
    CHECK(mock.render_requests == renders + 1);
    mock.frames = 73;
    drive_tick();
    CHECK(mock.unhide_calls == 1 && mock_low[0xc6] == 0);
}

static void failed_reinit_resets_diagnostics(void)
{
    fixture(1);
    CHECK(drive_init());
    prepare_named_load("FILE000", 1, 0);
    CHECK(drive_trap_load());
    CHECK(drive_trace_count == 1);
    mock_set_size(0);
    CHECK(!drive_init());
    CHECK(drive_file_count() == 0);
    CHECK(drive_trace_count == 0);
}

typedef struct {
    const char *name;
    void (*run)(void);
} Test;

int main(int argc, char **argv)
{
    static const Test tests[] = {
        { "metadata-header-boundary", metadata_header_boundary },
        { "metadata-display-names", metadata_display_names },
        { "metadata-invalid-arguments", metadata_invalid_arguments },
        { "metadata-invalid-ranges", metadata_invalid_ranges },
        { "metadata-truncated-chunks", metadata_truncated_chunks },
        { "metadata-header-only-at-end", metadata_header_only_at_end },
        { "metadata-queries-are-read-only", metadata_queries_are_read_only },
        { "metadata-count-255", metadata_count_255 },
        { "crc32-standard-vector", crc32_standard_vector },
        { "identity-all-chunks-and-cache", identity_all_chunks_and_cache },
        { "identity-invalidation-refreshes-every-chunk", identity_invalidation_refreshes_every_chunk },
        { "identity-excludes-physical-padding", identity_excludes_physical_padding },
        { "identity-missing-chunk-is-not-cached", identity_missing_chunk_is_not_cached },
        { "failed-reinit-clears-disk-state", failed_reinit_clears_disk_state },
        { "remapped-package-uses-fresh-pointers", remapped_package_uses_fresh_pointers },
        { "launcher-selection-preserves-hidden-support", launcher_selection_preserves_hidden_support },
        { "selected-wildcard-and-invalid-selection", selected_wildcard_and_invalid_selection },
        { "filename-patterns-keep-original-order", filename_patterns_keep_original_order },
        { "exact-obj-and-full-length-names", exact_obj_and_full_length_names },
        { "raw-petscii-lookup-survives-display-query", raw_petscii_lookup_survives_display_query },
        { "browser-index-254-loads-original-entry", browser_index_254_loads_original_entry },
        { "load-crosses-chunk-and-ram-seam", load_crosses_chunk_and_ram_seam },
        { "load-wraps-ram-under-kernal-rom", load_wraps_ram_under_kernal_rom },
        { "load-relocation-and-verify-preserve-behavior", load_relocation_and_verify_preserve_behavior },
        { "resident-fastload-serves-named-support", resident_fastload_serves_named_support },
        { "resident-trap-requires-complete-signature", resident_trap_requires_complete_signature },
        { "unknown-traps-do-not-touch-guest", unknown_traps_do_not_touch_guest },
        { "missing-file-returns-error-without-copy", missing_file_returns_error_without_copy },
        { "disabled-drive-reports-device-not-present", disabled_drive_reports_device_not_present },
        { "init-resets-startup-without-changing-later-loads", init_resets_startup_without_changing_later_loads },
        { "failed-reinit-resets-diagnostics", failed_reinit_resets_diagnostics }
    };
    unsigned int selected = 0;
    unsigned int failed_tests = 0;
    size_t index;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (index = 0; index < ARRAY_COUNT(tests); ++index)
            puts(tests[index].name);
        return 0;
    }
    if (argc > 2) {
        fprintf(stderr, "usage: %s [test-name|--list]\n", argv[0]);
        return 2;
    }
    mock_reset();
    for (index = 0; index < ARRAY_COUNT(tests); ++index) {
        unsigned int before = failures;
        if (argc == 2 && strcmp(argv[1], tests[index].name) != 0)
            continue;
        current_test = tests[index].name;
        current_case = NULL;
        ++selected;
        tests[index].run();
        ++checks;
        if (!mock_guards_intact()) {
            ++failures;
            fprintf(stderr, "FAIL %s: mock memory guard overwritten\n", current_test);
        }
        if (failures != before)
            ++failed_tests;
        printf("%s %s\n", failures == before ? "PASS" : "FAIL", current_test);
    }
    if (!selected) {
        fprintf(stderr, "No tests matched.\n");
        return 2;
    }
        printf("%u tests, %u passed, %u failed; %u checks, %u failures\n",
            selected, selected - failed_tests, failed_tests, checks, failures);
    return failures ? 1 : 0;
}