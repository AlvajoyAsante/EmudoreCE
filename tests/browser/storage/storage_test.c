#include "browser_storage.h"
#include "crc32.h"
#include "storage_mock.h"

#include <fileioc.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks;
static unsigned int tests;

#define info_data (mock_files[MOCK_INFO].data)
#define info_size (mock_files[MOCK_INFO].size)
#define info_exists (mock_files[MOCK_INFO].exists)
#define settings_data (mock_files[MOCK_SETTINGS].data)

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "group %u: %s:%d: %s\n", tests + 1, __FILE__, __LINE__, #condition); \
    exit(EXIT_FAILURE); } } while (0)

static uint32_t reference_crc(const uint8_t *data, size_t size)
{
    uint32_t result = UINT32_C(0xffffffff);
    size_t index;
    unsigned int bit;

    for (index = 0; index < size; ++index)
    {
        result ^= data[index];
        for (bit = 0; bit < 8; ++bit)
            result = (result >> 1) ^ ((result & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return result ^ UINT32_C(0xffffffff);
}

static void put_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void info_checksum(void)
{
    put_u32(info_data + info_size - 4, reference_crc(info_data, info_size - 4));
}

static void make_info_count(const DriveIdentity *identity, uint8_t file_count)
{
    unsigned int index;

    memset(info_data, 0, sizeof(info_data));
    info_size = (uint16_t)(20u + file_count);
    info_exists = true;
    memcpy(info_data, "ECFI", 4);
    info_data[4] = 1;
    info_data[5] = file_count;
    put_u32(info_data + 8, identity->size);
    put_u32(info_data + 12, identity->crc32);
    for (index = 0; index < file_count; ++index)
        info_data[16 + index] = index % 2 ? 0x82 : 0xc2;
    info_checksum();
}

static void make_info(const DriveIdentity *identity)
{
    make_info_count(identity, 2);
}

static void expect_locks(const DriveIdentity *identity, uint8_t first,
                         uint8_t second)
{
    uint8_t locks[4] = {0x55, 0x55, 0x55, 0x55};

    browser_info_load(identity, 2, locks + 1);
    CHECK(locks[0] == 0x55 && locks[3] == 0x55);
    CHECK(locks[1] == first && locks[2] == second);
    CHECK(!mock_live_allocations && !mock_open_handles);
}

static void test_crc_vector(void)
{
    const uint8_t vector[] = "123456789";
    uint32_t partial = crc32_update(0, vector, 4);

    CHECK(reference_crc(vector, 9) == UINT32_C(0xcbf43926));
    CHECK(crc32_update(0, vector, 9) == UINT32_C(0xcbf43926));
    CHECK(crc32_update(partial, vector + 4, 5) == UINT32_C(0xcbf43926));
    ++tests;
}

static void test_info_valid(void)
{
    DriveIdentity identity = {UINT32_C(0x12345678), UINT32_C(0x89abcdef)};

    make_info(&identity);
    expect_locks(&identity, BROWSER_LOCK_YES, BROWSER_LOCK_NO);
    ++tests;
}

static void test_info_missing_and_stale(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};

    info_exists = false;
    expect_locks(&identity, BROWSER_LOCK_UNKNOWN, BROWSER_LOCK_UNKNOWN);
    make_info(&identity);
    ++identity.crc32;
    expect_locks(&identity, BROWSER_LOCK_UNKNOWN, BROWSER_LOCK_UNKNOWN);
    ++tests;
}

static void test_info_invalid(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};

    make_info(&identity);
    info_data[17] = 0xc1;
    info_checksum();
    expect_locks(&identity, BROWSER_LOCK_UNKNOWN, BROWSER_LOCK_UNKNOWN);
    make_info(&identity);
    info_data[21] ^= 1;
    expect_locks(&identity, BROWSER_LOCK_UNKNOWN, BROWSER_LOCK_UNKNOWN);
    make_info(&identity);
    --info_size;
    expect_locks(&identity, BROWSER_LOCK_UNKNOWN, BROWSER_LOCK_UNKNOWN);
    ++tests;
}

static void test_info_all_types(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    unsigned int type;

    mock_reset();
    for (type = 0; type < 256; ++type)
    {
        make_info(&identity);
        info_data[17] = (uint8_t)type;
        info_checksum();
        if ((type & 0x87) == 0x82)
            expect_locks(&identity, BROWSER_LOCK_YES,
                         (type & 0x40) ? BROWSER_LOCK_YES : BROWSER_LOCK_NO);
        else
            expect_locks(&identity, BROWSER_LOCK_UNKNOWN, BROWSER_LOCK_UNKNOWN);
    }
    CHECK(!mock_mutations && !mock_writable_opens);
    ++tests;
}

static void test_info_lengths(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    unsigned int length;

    mock_reset();
    for (length = 0; length <= 276; ++length)
    {
        make_info(&identity);
        info_size = (uint16_t)length;
        if (length == 22)
            expect_locks(&identity, BROWSER_LOCK_YES, BROWSER_LOCK_NO);
        else
            expect_locks(&identity, BROWSER_LOCK_UNKNOWN, BROWSER_LOCK_UNKNOWN);
    }
    ++tests;
}

static void test_info_header_and_checksum(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    unsigned int offset;

    mock_reset();
    for (offset = 0; offset < 16; ++offset)
    {
        make_info(&identity);
        info_data[offset] ^= 1;
        info_checksum();
        expect_locks(&identity, BROWSER_LOCK_UNKNOWN, BROWSER_LOCK_UNKNOWN);
    }
    make_info(&identity);
    info_data[4] = 2;
    info_checksum();
    expect_locks(&identity, BROWSER_LOCK_UNKNOWN, BROWSER_LOCK_UNKNOWN);
    make_info(&identity);
    info_data[5] = 0;
    info_checksum();
    expect_locks(&identity, BROWSER_LOCK_UNKNOWN, BROWSER_LOCK_UNKNOWN);
    for (offset = 0; offset < 22; ++offset)
    {
        make_info(&identity);
        info_data[offset] ^= 1;
        expect_locks(&identity, BROWSER_LOCK_UNKNOWN, BROWSER_LOCK_UNKNOWN);
    }
    ++tests;
}

static void test_info_maximum_count(void)
{
    DriveIdentity identity = {UINT32_MAX, 0};
    uint8_t locks[257];
    unsigned int index;

    mock_reset();
    make_info_count(&identity, 255);
    memset(locks, 0x55, sizeof(locks));
    browser_info_load(&identity, 255, locks + 1);
    CHECK(info_size == 275);
    CHECK(locks[0] == 0x55 && locks[256] == 0x55);
    for (index = 0; index < 255; ++index)
        CHECK(locks[index + 1] == (index % 2 ? BROWSER_LOCK_NO : BROWSER_LOCK_YES));
    CHECK(!mock_live_allocations && !mock_open_handles);
    ++tests;
}

static void test_info_archived(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    uint8_t original[22];

    mock_reset();
    make_info(&identity);
    mock_files[MOCK_INFO].archived = true;
    memcpy(original, info_data, sizeof(original));
    expect_locks(&identity, BROWSER_LOCK_YES, BROWSER_LOCK_NO);
    CHECK(mock_archived_reads == 1 && mock_files[MOCK_INFO].archived);
    CHECK(!mock_mutations && !mock_writable_opens);
    CHECK(!memcmp(original, info_data, sizeof(original)));
    ++tests;
}

static void test_info_invalid_arguments(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    uint8_t locks[3] = {0x55, 0x55, 0x55};

    mock_reset();
    browser_info_load(NULL, 2, locks);
    CHECK(locks[0] == BROWSER_LOCK_UNKNOWN && locks[1] == BROWSER_LOCK_UNKNOWN);
    CHECK(locks[2] == 0x55);
    memset(locks, 0x55, sizeof(locks));
    browser_info_load(&identity, 0, locks);
    CHECK(locks[0] == 0x55 && locks[1] == 0x55 && locks[2] == 0x55);
    browser_info_load(&identity, 2, NULL);
    identity.size = 0;
    browser_info_load(&identity, 2, locks);
    CHECK(locks[0] == BROWSER_LOCK_UNKNOWN && locks[1] == BROWSER_LOCK_UNKNOWN);
    CHECK(locks[2] == 0x55);
    CHECK(!mock_calls[MOCK_ALLOCATE] && !mock_calls[MOCK_OPEN]);
    ++tests;
}

static void test_info_io_failures(void)
{
    static const unsigned int operations[] =
        {MOCK_OPEN, MOCK_READ, MOCK_CLOSE, MOCK_ALLOCATE, MOCK_SIZE};
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    unsigned int index;

    for (index = 0; index < sizeof(operations) / sizeof(operations[0]); ++index)
    {
        mock_reset();
        make_info(&identity);
        mock_fail_at[operations[index]] = 1;
        mock_partial_read = 10;
        expect_locks(&identity, BROWSER_LOCK_UNKNOWN, BROWSER_LOCK_UNKNOWN);
        CHECK(!mock_mutations && !mock_writable_opens);
    }
    ++tests;
}

static uint32_t get_u32(const uint8_t *data)
{
    return (uint32_t)data[0] + (uint32_t)data[1] * UINT32_C(256) +
           (uint32_t)data[2] * UINT32_C(65536) +
           (uint32_t)data[3] * UINT32_C(16777216);
}

static void snapshot_checksum(uint8_t *snapshot)
{
    put_u32(snapshot + 1020, reference_crc(snapshot, 1020));
}

static void make_snapshot(uint8_t *snapshot, const DriveIdentity *identity,
                          uint8_t file_count, const BrowserPreferences *preferences,
                          uint32_t generation)
{
    memset(snapshot, 0, 1024);
    memcpy(snapshot, "ECBP", 4);
    snapshot[4] = 1;
    snapshot[5] = 1;
    put_u32(snapshot + 8, generation);
    put_u32(snapshot + 12, identity->size);
    put_u32(snapshot + 16, identity->crc32);
    snapshot[20] = file_count;
    snapshot[21] = preferences->view;
    memcpy(snapshot + 24, preferences->launchers, 32);
    memcpy(snapshot + 56, preferences->hidden, 32);
    snapshot_checksum(snapshot);
}

static void fixture_snapshot(unsigned int slot, const DriveIdentity *identity,
                             uint8_t file_count, const BrowserPreferences *preferences,
                             uint32_t generation)
{
    CHECK(slot < 2);
    mock_files[MOCK_SETTINGS].exists = true;
    mock_files[MOCK_SETTINGS].size = 2048;
    make_snapshot(settings_data + slot * 1024u, identity, file_count, preferences, generation);
}

static void expect_preferences(const DriveIdentity *identity, uint8_t file_count,
                               const BrowserPreferences *expected)
{
    BrowserPreferences actual;

    memset(&actual, 0xa5, sizeof(actual));
    browser_preferences_load(identity, file_count, &actual);
    CHECK(actual.view == expected->view);
    CHECK(!memcmp(actual.launchers, expected->launchers, BROWSER_FLAG_BYTES));
    CHECK(!memcmp(actual.hidden, expected->hidden, BROWSER_FLAG_BYTES));
    CHECK(!mock_live_allocations && !mock_open_handles);
}

static void test_preferences_roundtrip(void)
{
    DriveIdentity identity = {UINT32_C(0x12345678), UINT32_C(0x89abcdef)};
    BrowserPreferences expected = {{0}, {0}, BROWSER_LAUNCHERS};
    BrowserPreferences defaults = {{0}, {0}, BROWSER_ALL_FILES};

    mock_reset();
    expect_preferences(&identity, 19, &defaults);
    expected.launchers[0] = 0x81;
    expected.launchers[2] = 0x04;
    expected.hidden[1] = 0x10;
    CHECK(browser_preferences_save(&identity, 19, &expected) == BROWSER_SAVE_OK);
    CHECK(mock_files[MOCK_SETTINGS].size == 2048);
    CHECK(mock_files[MOCK_SETTINGS].archived);
    CHECK(mock_write_count == 2);
    CHECK(mock_writes[0].offset == 0 && mock_writes[0].written == 2048);
    CHECK(mock_writes[1].offset == 0 && mock_writes[1].written == 1024);
    mock_reset_operations();
    expect_preferences(&identity, 19, &expected);
    CHECK(mock_archived_reads == 1 && !mock_mutations);
    ++tests;
}

static void test_preferences_inactive_and_noop(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences expected = {{1}, {2}, BROWSER_LAUNCHERS};
    uint8_t previous[2048];

    mock_reset();
    CHECK(browser_preferences_save(&identity, 19, &expected) == BROWSER_SAVE_OK);
    memcpy(previous, settings_data, sizeof(previous));
    mock_reset_operations();
    CHECK(browser_preferences_save(&identity, 19, &expected) == BROWSER_SAVE_OK);
    CHECK(!mock_mutations && !mock_writable_opens && !mock_calls[MOCK_ARCHIVE]);
    CHECK(!memcmp(previous, settings_data, sizeof(previous)));
    expected.hidden[0] = 4;
    mock_reset_operations();
    CHECK(browser_preferences_save(&identity, 19, &expected) == BROWSER_SAVE_OK);
    CHECK(mock_write_count == 1);
    CHECK(mock_writes[0].offset == 1024 && mock_writes[0].written == 1024);
    CHECK(!memcmp(previous, settings_data, 1024));
    expect_preferences(&identity, 19, &expected);
    ++tests;
}

static void test_preferences_partial_write(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences previous = {{1}, {2}, BROWSER_ALL_FILES};
    BrowserPreferences changed = {{4}, {8}, BROWSER_LAUNCHERS};
    uint8_t snapshot[1024];

    mock_reset();
    CHECK(browser_preferences_save(&identity, 19, &previous) == BROWSER_SAVE_OK);
    memcpy(snapshot, settings_data, sizeof(snapshot));
    mock_reset_operations();
    mock_fail_at[MOCK_WRITE] = 1;
    mock_partial_write = 500;
    CHECK(browser_preferences_save(&identity, 19, &changed) == BROWSER_SAVE_FAILED);
    CHECK(!memcmp(snapshot, settings_data, sizeof(snapshot)));
    CHECK(!mock_calls[MOCK_ARCHIVE]);
    mock_reset_operations();
    expect_preferences(&identity, 19, &previous);
    ++tests;
}

static void test_preferences_serialization(void)
{
    DriveIdentity identity = {UINT32_C(0x12345678), UINT32_C(0x89abcdef)};
    BrowserPreferences expected = {{0}, {0}, BROWSER_LAUNCHERS};
    uint8_t serialized[2048] = {0};
    uint8_t original_info[22];

    mock_reset();
    make_info(&identity);
    mock_files[MOCK_INFO].archived = true;
    memcpy(original_info, info_data, sizeof(original_info));
    expected.launchers[0] = 0x91;
    expected.launchers[31] = 0x40;
    expected.hidden[0] = 0x02;
    expected.hidden[31] = 0x20;
    make_snapshot(serialized, &identity, 255, &expected, 1);
    CHECK(browser_preferences_save(&identity, 255, &expected) == BROWSER_SAVE_OK);
    CHECK(!memcmp(serialized, settings_data, sizeof(serialized)));
    CHECK(settings_data[12] == 0x78 && settings_data[15] == 0x12);
    CHECK(settings_data[16] == 0xef && settings_data[19] == 0x89);
    CHECK(settings_data[20] == 255 && settings_data[21] == BROWSER_LAUNCHERS);
    CHECK(get_u32(settings_data + 1020) == reference_crc(settings_data, 1020));
    CHECK(!memcmp(original_info, info_data, sizeof(original_info)));
    CHECK(mock_files[MOCK_INFO].archived && info_size == 22);
    ++tests;
}

static void test_preferences_identity_and_count(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    DriveIdentity different = identity;
    BrowserPreferences expected = {{1}, {2}, BROWSER_LAUNCHERS};
    BrowserPreferences defaults = {{0}, {0}, BROWSER_ALL_FILES};

    mock_reset();
    CHECK(browser_preferences_save(&identity, 19, &expected) == BROWSER_SAVE_OK);
    ++different.size;
    expect_preferences(&different, 19, &defaults);
    different = identity;
    ++different.crc32;
    expect_preferences(&different, 19, &defaults);
    expect_preferences(&identity, 18, &defaults);
    expect_preferences(&identity, 20, &defaults);
    CHECK(browser_preferences_save(&different, 19, &expected) == BROWSER_SAVE_OK);
    expect_preferences(&identity, 19, &expected);
    expect_preferences(&different, 19, &expected);
    expected.hidden[0] = 4;
    CHECK(browser_preferences_save(&identity, 20, &expected) == BROWSER_SAVE_OK);
    expect_preferences(&identity, 20, &expected);
    expected.hidden[0] = 2;
    expect_preferences(&identity, 19, &expected);
    CHECK(settings_data[5] == 3);
    ++tests;
}

static void test_preferences_eviction(void)
{
    DriveIdentity identities[9];
    BrowserPreferences expected = {{0}, {0}, BROWSER_LAUNCHERS};
    BrowserPreferences defaults = {{0}, {0}, BROWSER_ALL_FILES};
    unsigned int index;

    mock_reset();
    for (index = 0; index < 9; ++index)
    {
        identities[index].size = UINT32_C(0x10000) + index;
        identities[index].crc32 = UINT32_C(0xabcdef00) + index;
        expected.launchers[0] = (uint8_t)(index + 1);
        CHECK(browser_preferences_save(&identities[index], 8, &expected) == BROWSER_SAVE_OK);
    }
    CHECK(settings_data[5] == 8 && get_u32(settings_data + 8) == 9);
    expect_preferences(&identities[0], 8, &defaults);
    for (index = 1; index < 9; ++index)
    {
        expected.launchers[0] = (uint8_t)(index + 1);
        expect_preferences(&identities[index], 8, &expected);
        CHECK(get_u32(settings_data + 12 + (index - 1) * 76) == identities[index].size);
    }
    ++tests;
}

static void test_preferences_changed_recency(void)
{
    DriveIdentity identities[9];
    BrowserPreferences expected = {{1}, {0}, BROWSER_LAUNCHERS};
    BrowserPreferences defaults = {{0}, {0}, BROWSER_ALL_FILES};
    static const unsigned int final_order[] = {1, 2, 4, 5, 6, 7, 3, 8};
    unsigned int index;

    mock_reset();
    for (index = 0; index < 9; ++index)
    {
        identities[index].size = 1000 + index;
        identities[index].crc32 = 2000 + index;
        if (index < 8)
            CHECK(browser_preferences_save(&identities[index], 8, &expected) == BROWSER_SAVE_OK);
    }
    mock_reset_operations();
    expect_preferences(&identities[0], 8, &expected);
    CHECK(browser_preferences_save(&identities[0], 8, &expected) == BROWSER_SAVE_OK);
    CHECK(!mock_mutations && !mock_writable_opens);
    CHECK(get_u32(settings_data + 1024 + 8) == 8);
    expected.launchers[0] = 2;
    CHECK(browser_preferences_save(&identities[3], 8, &expected) == BROWSER_SAVE_OK);
    CHECK(settings_data[5] == 8 && get_u32(settings_data + 12 + 7 * 76) == identities[3].size);
    CHECK(get_u32(settings_data + 12) == identities[0].size);
    CHECK(browser_preferences_save(&identities[8], 8, &expected) == BROWSER_SAVE_OK);
    CHECK(settings_data[1024 + 5] == 8);
    for (index = 0; index < 8; ++index)
        CHECK(get_u32(settings_data + 1024 + 12 + index * 76) == identities[final_order[index]].size);
    expect_preferences(&identities[0], 8, &defaults);
    expect_preferences(&identities[3], 8, &expected);
    ++tests;
}

static void test_preferences_flag_boundaries(void)
{
    DriveIdentity identity = {UINT32_MAX, 0};
    BrowserPreferences expected;
    BrowserPreferences invalid;
    unsigned int count;

    for (count = 1; count <= 255; ++count)
    {
        mock_reset();
        memset(&expected, 0, sizeof(expected));
        expected.view = (uint8_t)(count % 2);
        expected.launchers[(count - 1) / 8] = (uint8_t)(1u << ((count - 1) % 8));
        expected.hidden[(count - 1) / 8] = expected.launchers[(count - 1) / 8];
        CHECK(browser_preferences_save(&identity, (uint8_t)count, &expected) == BROWSER_SAVE_OK);
        expect_preferences(&identity, (uint8_t)count, &expected);
        mock_reset_operations();
        invalid = expected;
        invalid.launchers[count / 8] |= (uint8_t)(1u << (count % 8));
        CHECK(browser_preferences_save(&identity, (uint8_t)count, &invalid) == BROWSER_SAVE_FAILED);
        invalid = expected;
        invalid.hidden[count / 8] |= (uint8_t)(1u << (count % 8));
        CHECK(browser_preferences_save(&identity, (uint8_t)count, &invalid) == BROWSER_SAVE_FAILED);
        CHECK(!mock_mutations && !mock_calls[MOCK_OPEN] && !mock_calls[MOCK_ALLOCATE]);
    }
    ++tests;
}

static void test_preferences_maximum_flags(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences expected;

    mock_reset();
    memset(&expected, 0xff, sizeof(expected));
    expected.launchers[31] = 0x7f;
    expected.hidden[31] = 0x7f;
    expected.view = BROWSER_LAUNCHERS;
    CHECK(browser_preferences_save(&identity, 255, &expected) == BROWSER_SAVE_OK);
    expect_preferences(&identity, 255, &expected);
    ++tests;
}

static void test_preferences_invalid_arguments(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    DriveIdentity empty = {0, UINT32_C(0x76543210)};
    BrowserPreferences preferences = {{0}, {0}, BROWSER_ALL_FILES};
    BrowserPreferences defaults = preferences;

    mock_reset();
    CHECK(browser_preferences_save(NULL, 19, &preferences) == BROWSER_SAVE_FAILED);
    CHECK(browser_preferences_save(&empty, 19, &preferences) == BROWSER_SAVE_FAILED);
    CHECK(browser_preferences_save(&identity, 0, &preferences) == BROWSER_SAVE_FAILED);
    CHECK(browser_preferences_save(&identity, 19, NULL) == BROWSER_SAVE_FAILED);
    preferences.view = 2;
    CHECK(browser_preferences_save(&identity, 19, &preferences) == BROWSER_SAVE_FAILED);
    preferences.view = 255;
    CHECK(browser_preferences_save(&identity, 19, &preferences) == BROWSER_SAVE_FAILED);
    expect_preferences(NULL, 19, &defaults);
    expect_preferences(&empty, 19, &defaults);
    expect_preferences(&identity, 0, &defaults);
    browser_preferences_load(&identity, 19, NULL);
    CHECK(!mock_mutations && !mock_calls[MOCK_ALLOCATE] && !mock_calls[MOCK_OPEN]);
    ++tests;
}

static void test_snapshot_validation(void)
{
    static const struct
    {
        unsigned int offset;
        uint8_t value;
    } mutations[] = {
        {0, 'X'}, {1, 'X'}, {2, 'X'}, {3, 'X'}, {4, 0}, {4, 2},
        {5, 0}, {5, 9}, {5, 255}, {6, 1}, {7, 1}, {20, 0},
        {21, 2}, {21, 255}, {22, 1}, {23, 1}, {26, 8}, {55, 0x80},
        {58, 8}, {87, 0x80}, {88, 1}, {1019, 1}
    };
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences expected = {{1}, {2}, BROWSER_LAUNCHERS};
    BrowserPreferences defaults = {{0}, {0}, BROWSER_ALL_FILES};
    unsigned int index;

    for (index = 0; index < sizeof(mutations) / sizeof(mutations[0]); ++index)
    {
        mock_reset();
        fixture_snapshot(0, &identity, 19, &expected, 1);
        settings_data[mutations[index].offset] = mutations[index].value;
        snapshot_checksum(settings_data);
        expect_preferences(&identity, 19, &defaults);
        CHECK(!mock_mutations);
    }
    mock_reset();
    fixture_snapshot(0, &identity, 19, &expected, 1);
    put_u32(settings_data + 12, 0);
    snapshot_checksum(settings_data);
    expect_preferences(&identity, 19, &defaults);
    fixture_snapshot(0, &identity, 19, &expected, 1);
    settings_data[1023] ^= 1;
    expect_preferences(&identity, 19, &defaults);
    fixture_snapshot(0, &identity, 19, &expected, 1);
    settings_data[5] = 2;
    memcpy(settings_data + 88, settings_data + 12, 76);
    snapshot_checksum(settings_data);
    expect_preferences(&identity, 19, &defaults);
    put_u32(settings_data + 88, identity.size + 1);
    settings_data[96] = 0;
    snapshot_checksum(settings_data);
    expect_preferences(&identity, 19, &defaults);
    ++tests;
}

static void test_snapshot_decoded_flag_boundaries(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences expected = {{1}, {0}, BROWSER_LAUNCHERS};
    BrowserPreferences defaults = {{0}, {0}, BROWSER_ALL_FILES};
    unsigned int count;
    unsigned int flags;

    for (count = 1; count <= 255; ++count)
    {
        for (flags = 0; flags < 2; ++flags)
        {
            mock_reset();
            fixture_snapshot(0, &identity, (uint8_t)count, &expected, 1);
            settings_data[24 + flags * 32 + count / 8] |= (uint8_t)(1u << (count % 8));
            snapshot_checksum(settings_data);
            expect_preferences(&identity, (uint8_t)count, &defaults);
        }
    }
    ++tests;
}

static void test_snapshot_selection(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences first = {{1}, {2}, BROWSER_ALL_FILES};
    BrowserPreferences second = {{4}, {8}, BROWSER_LAUNCHERS};
    BrowserPreferences defaults = {{0}, {0}, BROWSER_ALL_FILES};

    mock_reset();
    fixture_snapshot(0, &identity, 19, &first, 4);
    fixture_snapshot(1, &identity, 19, &second, 5);
    expect_preferences(&identity, 19, &second);
    settings_data[1024 + 1023] ^= 1;
    expect_preferences(&identity, 19, &first);
    settings_data[1023] ^= 1;
    expect_preferences(&identity, 19, &defaults);
    settings_data[1024 + 1023] ^= 1;
    expect_preferences(&identity, 19, &second);
    fixture_snapshot(0, &identity, 19, &first, 6);
    expect_preferences(&identity, 19, &first);
    fixture_snapshot(0, &identity, 19, &first, 5);
    expect_preferences(&identity, 19, &first);
    fixture_snapshot(0, &identity, 19, &first, UINT32_C(0x80000005));
    expect_preferences(&identity, 19, &first);
    CHECK(!mock_mutations && !mock_writable_opens);
    ++tests;
}

static void test_generation_wrap(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences first = {{1}, {2}, BROWSER_ALL_FILES};
    BrowserPreferences second = {{4}, {8}, BROWSER_LAUNCHERS};
    uint8_t previous[1024];

    mock_reset();
    fixture_snapshot(0, &identity, 19, &first, UINT32_MAX);
    fixture_snapshot(1, &identity, 19, &second, UINT32_MAX - 1);
    expect_preferences(&identity, 19, &first);
    memcpy(previous, settings_data, sizeof(previous));
    CHECK(browser_preferences_save(&identity, 19, &second) == BROWSER_SAVE_OK);
    CHECK(get_u32(settings_data + 1024 + 8) == 0);
    CHECK(!memcmp(previous, settings_data, sizeof(previous)));
    expect_preferences(&identity, 19, &second);
    memcpy(previous, settings_data + 1024, sizeof(previous));
    CHECK(browser_preferences_save(&identity, 19, &first) == BROWSER_SAVE_OK);
    CHECK(get_u32(settings_data + 8) == 1);
    CHECK(!memcmp(previous, settings_data + 1024, sizeof(previous)));
    expect_preferences(&identity, 19, &first);
    mock_reset();
    fixture_snapshot(0, &identity, 19, &first, 0);
    fixture_snapshot(1, &identity, 19, &second, UINT32_MAX);
    expect_preferences(&identity, 19, &first);
    ++tests;
}

static void test_preferences_bad_sizes(void)
{
    static const uint16_t sizes[] = {0, 1, 20, 1024, 2047, 2049, 4096};
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences expected = {{1}, {2}, BROWSER_LAUNCHERS};
    BrowserPreferences defaults = {{0}, {0}, BROWSER_ALL_FILES};
    uint8_t serialized[2048] = {0};
    unsigned int index;

    make_snapshot(serialized, &identity, 19, &expected, 1);
    for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); ++index)
    {
        mock_reset();
        fixture_snapshot(0, &identity, 19, &expected, 50);
        mock_files[MOCK_SETTINGS].size = sizes[index];
        expect_preferences(&identity, 19, &defaults);
        CHECK(browser_preferences_save(&identity, 19, &expected) == BROWSER_SAVE_OK);
        CHECK(mock_write_count == 2 && mock_writes[0].written == 2048);
        CHECK(mock_files[MOCK_SETTINGS].size == 2048);
        CHECK(!memcmp(serialized, settings_data, sizeof(serialized)));
        expect_preferences(&identity, 19, &expected);
    }
    ++tests;
}

static void test_preferences_recover_corruption(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences expected = {{1}, {2}, BROWSER_ALL_FILES};
    BrowserPreferences changed = {{4}, {8}, BROWSER_LAUNCHERS};
    BrowserPreferences defaults = {{0}, {0}, BROWSER_ALL_FILES};
    uint8_t previous[1024];

    mock_reset();
    fixture_snapshot(0, &identity, 19, &expected, 7);
    fixture_snapshot(1, &identity, 19, &changed, 100);
    settings_data[1024 + 6] = 1;
    snapshot_checksum(settings_data + 1024);
    memcpy(previous, settings_data, sizeof(previous));
    CHECK(browser_preferences_save(&identity, 19, &changed) == BROWSER_SAVE_OK);
    CHECK(get_u32(settings_data + 1024 + 8) == 8);
    CHECK(!memcmp(previous, settings_data, sizeof(previous)));
    CHECK(mock_write_count == 1 && mock_writes[0].offset == 1024);
    expect_preferences(&identity, 19, &changed);
    mock_reset();
    mock_files[MOCK_SETTINGS].exists = true;
    mock_files[MOCK_SETTINGS].size = 2048;
    memset(settings_data, 0xff, 2048);
    expect_preferences(&identity, 19, &defaults);
    CHECK(browser_preferences_save(&identity, 19, &changed) == BROWSER_SAVE_OK);
    CHECK(mock_write_count == 2 && mock_writes[0].written == 2048);
    CHECK(get_u32(settings_data + 8) == 1);
    expect_preferences(&identity, 19, &changed);
    ++tests;
}

static void test_preferences_load_failures(void)
{
    static const unsigned int operations[] =
        {MOCK_OPEN, MOCK_READ, MOCK_CLOSE, MOCK_ALLOCATE, MOCK_SIZE};
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences expected = {{1}, {2}, BROWSER_LAUNCHERS};
    BrowserPreferences defaults = {{0}, {0}, BROWSER_ALL_FILES};
    uint8_t original[2048];
    unsigned int index;

    for (index = 0; index < sizeof(operations) / sizeof(operations[0]); ++index)
    {
        mock_reset();
        fixture_snapshot(0, &identity, 19, &expected, 1);
        mock_files[MOCK_SETTINGS].archived = true;
        memcpy(original, settings_data, sizeof(original));
        mock_fail_at[operations[index]] = 1;
        mock_partial_read = 1024;
        expect_preferences(&identity, 19, &defaults);
        CHECK(!mock_mutations && !mock_writable_opens && !mock_calls[MOCK_ARCHIVE]);
        CHECK(mock_files[MOCK_SETTINGS].archived);
        CHECK(!memcmp(original, settings_data, sizeof(original)));
    }
    ++tests;
}

static void test_preferences_save_prewrite_failures(void)
{
    static const struct
    {
        unsigned int operation;
        unsigned int occurrence;
    } faults[] = {
        {MOCK_ALLOCATE, 1}, {MOCK_OPEN, 1}, {MOCK_OPEN, 2},
        {MOCK_READ, 1}, {MOCK_CLOSE, 1}, {MOCK_SEEK, 1}, {MOCK_SIZE, 2}
    };
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences previous = {{1}, {2}, BROWSER_ALL_FILES};
    BrowserPreferences changed = {{4}, {8}, BROWSER_LAUNCHERS};
    uint8_t original[2048];
    unsigned int index;
    unsigned int active;

    for (active = 0; active < 2; ++active)
    {
        for (index = 0; index < sizeof(faults) / sizeof(faults[0]); ++index)
        {
            mock_reset();
            fixture_snapshot(active, &identity, 19, &previous, 8);
            fixture_snapshot(active ^ 1u, &identity, 19, &previous, 7);
            mock_files[MOCK_SETTINGS].archived = true;
            memcpy(original, settings_data, sizeof(original));
            mock_fail_at[faults[index].operation] = faults[index].occurrence;
            mock_partial_read = 1024;
            CHECK(browser_preferences_save(&identity, 19, &changed) == BROWSER_SAVE_FAILED);
            CHECK(!memcmp(original, settings_data, sizeof(original)));
            CHECK(!mock_write_count && !mock_calls[MOCK_ARCHIVE]);
            CHECK(!mock_live_allocations && !mock_open_handles);
            mock_reset_operations();
            expect_preferences(&identity, 19, &previous);
        }
    }
    ++tests;
}

static void test_preferences_save_postwrite_failures(void)
{
    static const struct
    {
        unsigned int operation;
        unsigned int occurrence;
    } faults[] = {
        {MOCK_SEEK, 2}, {MOCK_READ, 2}, {MOCK_SIZE, 3}, {MOCK_CLOSE, 2}
    };
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences previous = {{1}, {2}, BROWSER_ALL_FILES};
    BrowserPreferences changed = {{4}, {8}, BROWSER_LAUNCHERS};
    uint8_t original[1024];
    unsigned int index;
    unsigned int active;

    for (active = 0; active < 2; ++active)
    {
        for (index = 0; index < sizeof(faults) / sizeof(faults[0]); ++index)
        {
            mock_reset();
            fixture_snapshot(active, &identity, 19, &previous, 8);
            fixture_snapshot(active ^ 1u, &identity, 19, &previous, 7);
            memcpy(original, settings_data + active * 1024u, sizeof(original));
            mock_fail_at[faults[index].operation] = faults[index].occurrence;
            mock_partial_read = 1023;
            CHECK(browser_preferences_save(&identity, 19, &changed) == BROWSER_SAVE_FAILED);
            CHECK(!memcmp(original, settings_data + active * 1024u, sizeof(original)));
            CHECK(!mock_live_allocations && !mock_open_handles);
            CHECK(mock_calls[MOCK_ARCHIVE] == (faults[index].operation == MOCK_CLOSE ? 1u : 0u));
            mock_reset_operations();
            expect_preferences(&identity, 19, &changed);
        }
    }
    ++tests;
}

static void test_preferences_all_partial_writes(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences previous = {{1}, {2}, BROWSER_ALL_FILES};
    BrowserPreferences changed = {{4}, {8}, BROWSER_LAUNCHERS};
    uint8_t original[1024];
    uint8_t intended[1024];
    unsigned int active;
    size_t written;

    make_snapshot(intended, &identity, 19, &changed, 9);
    for (active = 0; active < 2; ++active)
    {
        for (written = 0; written < 1024; ++written)
        {
            bool complete;

            mock_reset();
            fixture_snapshot(active, &identity, 19, &previous, 8);
            fixture_snapshot(active ^ 1u, &identity, 19, &previous, 7);
            memcpy(original, settings_data + active * 1024u, sizeof(original));
            mock_fail_at[MOCK_WRITE] = 1;
            mock_partial_write = written;
            CHECK(browser_preferences_save(&identity, 19, &changed) == BROWSER_SAVE_FAILED);
            CHECK(!memcmp(original, settings_data + active * 1024u, sizeof(original)));
            CHECK(mock_files[MOCK_SETTINGS].size == 2048);
            CHECK(mock_write_count == 1 && mock_writes[0].written == written);
            CHECK(mock_writes[0].offset == (active ^ 1u) * 1024u);
            CHECK(!mock_calls[MOCK_ARCHIVE]);
            CHECK(!mock_live_allocations && !mock_open_handles);
            complete = !memcmp(intended, settings_data + (active ^ 1u) * 1024u, sizeof(intended));
            mock_reset_operations();
            expect_preferences(&identity, 19, complete ? &changed : &previous);
        }
    }
    ++tests;
}

static void test_preferences_write_verification(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences previous = {{1}, {2}, BROWSER_ALL_FILES};
    BrowserPreferences changed = {{4}, {8}, BROWSER_LAUNCHERS};
    uint8_t original[1024];
    unsigned int fault;

    for (fault = 0; fault < 4; ++fault)
    {
        mock_reset();
        fixture_snapshot(0, &identity, 19, &previous, 8);
        fixture_snapshot(1, &identity, 19, &previous, 7);
        memcpy(original, settings_data, sizeof(original));
        if (fault == 0)
            mock_drop_write_at = 1;
        else if (fault == 1)
            mock_corrupt_write_at = 1;
        else
        {
            mock_override_read_at = 2;
            make_snapshot(mock_read_override, &identity, 19,
                          fault == 2 ? &changed : &previous, fault == 2 ? 8 : 9);
        }
        CHECK(browser_preferences_save(&identity, 19, &changed) == BROWSER_SAVE_FAILED);
        CHECK(!mock_calls[MOCK_ARCHIVE]);
        CHECK(!memcmp(original, settings_data, sizeof(original)));
        CHECK(!mock_live_allocations && !mock_open_handles);
        mock_reset_operations();
        expect_preferences(&identity, 19, fault < 2 ? &previous : &changed);
    }
    ++tests;
}

static void test_preferences_archive_failure(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences previous = {{1}, {2}, BROWSER_ALL_FILES};
    BrowserPreferences changed = {{4}, {8}, BROWSER_LAUNCHERS};
    unsigned int existing;

    for (existing = 0; existing < 2; ++existing)
    {
        mock_reset();
        if (existing)
        {
            fixture_snapshot(0, &identity, 19, &previous, 5);
            mock_files[MOCK_SETTINGS].archived = true;
        }
        mock_fail_at[MOCK_ARCHIVE] = 1;
        CHECK(browser_preferences_save(&identity, 19, &changed) == BROWSER_SAVE_RAM_ONLY);
        CHECK(mock_files[MOCK_SETTINGS].exists && mock_files[MOCK_SETTINGS].size == 2048);
        CHECK(!mock_files[MOCK_SETTINGS].archived);
        CHECK(mock_calls[MOCK_ARCHIVE] == 1 && !mock_live_allocations && !mock_open_handles);
        mock_reset_operations();
        expect_preferences(&identity, 19, &changed);
        mock_fail_at[MOCK_ARCHIVE] = 1;
        CHECK(browser_preferences_save(&identity, 19, &changed) == BROWSER_SAVE_OK);
        CHECK(!mock_calls[MOCK_ARCHIVE] && !mock_mutations && !mock_writable_opens);
        CHECK(!mock_files[MOCK_SETTINGS].archived);
    }
    ++tests;
}

static void test_preferences_noop_mutations(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences expected = {{1}, {2}, BROWSER_LAUNCHERS};
    uint8_t original[2048];
    unsigned int archived;
    unsigned int older_bad;

    for (archived = 0; archived < 2; ++archived)
    {
        for (older_bad = 0; older_bad < 2; ++older_bad)
        {
            mock_reset();
            fixture_snapshot(0, &identity, 19, &expected, 4);
            fixture_snapshot(1, &identity, 19, &expected, 5);
            if (older_bad)
                settings_data[1023] ^= 1;
            mock_files[MOCK_SETTINGS].archived = archived != 0;
            memcpy(original, settings_data, sizeof(original));
            mock_fail_at[MOCK_OPEN] = 2;
            mock_fail_at[MOCK_WRITE] = 1;
            mock_fail_at[MOCK_ARCHIVE] = 1;
            mock_fail_at[MOCK_SEEK] = 1;
            CHECK(browser_preferences_save(&identity, 19, &expected) == BROWSER_SAVE_OK);
            CHECK(!mock_mutations && !mock_writable_opens && !mock_calls[MOCK_ARCHIVE]);
            CHECK(!mock_calls[MOCK_WRITE] && !mock_calls[MOCK_SEEK]);
            CHECK(mock_calls[MOCK_OPEN] == 1 && mock_calls[MOCK_READ] == 1);
            CHECK(!memcmp(original, settings_data, sizeof(original)));
            CHECK(mock_files[MOCK_SETTINGS].archived == (archived != 0));
            CHECK(!mock_live_allocations && !mock_open_handles);
        }
    }
    ++tests;
}

static void setup_invalid_settings(unsigned int kind)
{
    mock_reset();
    if (kind)
    {
        mock_files[MOCK_SETTINGS].exists = true;
        mock_files[MOCK_SETTINGS].size = kind == 1 ? 17 : 2048;
        mock_files[MOCK_SETTINGS].archived = true;
        memset(settings_data, 0xcc, mock_files[MOCK_SETTINGS].size);
    }
}

static void test_preferences_initialize_failures(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences expected = {{1}, {2}, BROWSER_LAUNCHERS};
    BrowserPreferences defaults = {{0}, {0}, BROWSER_ALL_FILES};
    uint8_t intended[1024];
    unsigned int baseline[MOCK_OPERATION_COUNT];
    unsigned int kind;
    unsigned int operation;
    unsigned int occurrence;

    make_snapshot(intended, &identity, 19, &expected, 1);
    for (kind = 0; kind < 3; ++kind)
    {
        setup_invalid_settings(kind);
        CHECK(browser_preferences_save(&identity, 19, &expected) == BROWSER_SAVE_OK);
        memcpy(baseline, mock_calls, sizeof(baseline));
        for (operation = 0; operation < MOCK_OPERATION_COUNT; ++operation)
        {
            for (occurrence = 1; occurrence <= baseline[operation]; ++occurrence)
            {
                BrowserSaveResult result;
                BrowserSaveResult wanted = operation == MOCK_ARCHIVE ?
                                               BROWSER_SAVE_RAM_ONLY : BROWSER_SAVE_FAILED;
                bool complete;

                if ((operation == MOCK_OPEN && occurrence == 1 && kind == 0) ||
                    (operation == MOCK_SIZE && occurrence == 1) ||
                    (operation == MOCK_SIZE && occurrence == 2 && kind != 0))
                    continue;
                setup_invalid_settings(kind);
                mock_fail_at[operation] = occurrence;
                mock_partial_write = 500;
                mock_partial_read = 500;
                result = browser_preferences_save(&identity, 19, &expected);
                if (result != wanted)
                    fprintf(stderr, "initialize kind %u operation %u call %u: result %d\n",
                            kind, operation, occurrence, (int)result);
                CHECK(result == wanted);
                CHECK(!mock_live_allocations && !mock_open_handles);
                complete = mock_files[MOCK_SETTINGS].size == 2048 &&
                           !memcmp(intended, settings_data, sizeof(intended));
                mock_reset_operations();
                expect_preferences(&identity, 19, complete ? &expected : &defaults);
            }
        }
    }
    ++tests;
}

static void test_preferences_initialize_verification(void)
{
    DriveIdentity identity = {69829, UINT32_C(0x76543210)};
    BrowserPreferences expected = {{1}, {2}, BROWSER_LAUNCHERS};
    BrowserPreferences defaults = {{0}, {0}, BROWSER_ALL_FILES};
    unsigned int corruption;

    for (corruption = 0; corruption < 2; ++corruption)
    {
        mock_reset();
        if (corruption)
            mock_corrupt_write_at = 1;
        else
            mock_drop_write_at = 1;
        CHECK(browser_preferences_save(&identity, 19, &expected) == BROWSER_SAVE_FAILED);
        CHECK(!mock_calls[MOCK_ARCHIVE]);
        CHECK(!mock_live_allocations && !mock_open_handles);
        mock_reset_operations();
        expect_preferences(&identity, 19, &defaults);
    }
    ++tests;
}

int main(void)
{
    test_crc_vector();
    test_info_valid();
    test_info_missing_and_stale();
    test_info_invalid();
    test_info_all_types();
    test_info_lengths();
    test_info_header_and_checksum();
    test_info_maximum_count();
    test_info_archived();
    test_info_invalid_arguments();
    test_info_io_failures();
        test_preferences_roundtrip();
        test_preferences_inactive_and_noop();
        test_preferences_partial_write();
    test_preferences_serialization();
    test_preferences_identity_and_count();
    test_preferences_eviction();
    test_preferences_changed_recency();
    test_preferences_flag_boundaries();
    test_preferences_maximum_flags();
    test_preferences_invalid_arguments();
    test_snapshot_validation();
    test_snapshot_decoded_flag_boundaries();
    test_snapshot_selection();
    test_generation_wrap();
    test_preferences_bad_sizes();
    test_preferences_recover_corruption();
    test_preferences_load_failures();
    test_preferences_save_prewrite_failures();
    test_preferences_save_postwrite_failures();
    test_preferences_all_partial_writes();
    test_preferences_write_verification();
    test_preferences_archive_failure();
    test_preferences_noop_mutations();
    test_preferences_initialize_failures();
    test_preferences_initialize_verification();
    printf("storage: %u test groups, %u checks passed; peak scratch heap %zu bytes\n",
            tests, checks, mock_peak_heap);
    return EXIT_SUCCESS;
}