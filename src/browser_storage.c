#include "browser_storage.h"
#include "crc32.h"

#include <fileioc.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_SIZE 2048u
#define SNAPSHOT_SIZE 1024u
#define SNAPSHOT_CRC_OFFSET 1020u
#define RECORD_OFFSET 12u
#define RECORD_SIZE 76u
#define RECORD_LIMIT 8u

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

void browser_info_load(const DriveIdentity *identity, uint8_t file_count,
                       uint8_t *locks)
{
    uint8_t handle;
    uint8_t *data;
    size_t size = 20u + file_count;
    unsigned int index;
    bool valid;

    if (!locks)
        return;
    memset(locks, BROWSER_LOCK_UNKNOWN, file_count);
    if (!identity || !identity->size || !file_count)
        return;
    data = malloc(size);
    if (!data)
        return;
    handle = ti_Open("ECDINFO", "r");
    if (!handle)
    {
        free(data);
        return;
    }
    valid = ti_GetSize(handle) == size &&
            ti_Read(data, 1, size, handle) == size;
    if (!ti_Close(handle))
        valid = false;
    if (valid)
    {
        valid = !memcmp(data, "ECFI", 4) && data[4] == 1 &&
                data[5] == file_count && !data[6] && !data[7] &&
                read_u32(data + 8) == identity->size &&
                read_u32(data + 12) == identity->crc32 &&
                read_u32(data + size - 4) == crc32_update(0, data, size - 4);
    }
    if (valid)
    {
        for (index = 0; index < file_count; ++index)
        {
            if ((data[16 + index] & 0x87) != 0x82)
            {
                valid = false;
                break;
            }
        }
    }
    if (valid)
    {
        for (index = 0; index < file_count; ++index)
            locks[index] = (data[16 + index] & 0x40) ? BROWSER_LOCK_YES :
                                                           BROWSER_LOCK_NO;
    }
    free(data);
}

static void write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static bool valid_flags(uint8_t file_count, uint8_t view,
                        const uint8_t *launchers, const uint8_t *hidden)
{
    unsigned int index = file_count / 8u;
    unsigned int remainder = file_count % 8u;

    if (!file_count || (view != BROWSER_ALL_FILES && view != BROWSER_LAUNCHERS))
        return false;
    if (remainder)
    {
        uint8_t mask = (uint8_t)(0xffu << remainder);

        if ((launchers[index] | hidden[index]) & mask)
            return false;
        ++index;
    }
    for (; index < BROWSER_FLAG_BYTES; ++index)
    {
        if (launchers[index] || hidden[index])
            return false;
    }
    return true;
}

static bool valid_snapshot(const uint8_t *snapshot)
{
    unsigned int index;
    unsigned int previous;
    size_t used;

    if (memcmp(snapshot, "ECBP", 4) || snapshot[4] != 1 ||
        !snapshot[5] || snapshot[5] > RECORD_LIMIT || snapshot[6] || snapshot[7] ||
        read_u32(snapshot + SNAPSHOT_CRC_OFFSET) !=
            crc32_update(0, snapshot, SNAPSHOT_CRC_OFFSET))
        return false;
    for (index = 0; index < snapshot[5]; ++index)
    {
        const uint8_t *record = snapshot + RECORD_OFFSET + index * RECORD_SIZE;

        if (!read_u32(record) || record[10] || record[11] ||
            !valid_flags(record[8], record[9], record + 12, record + 44))
            return false;
        for (previous = 0; previous < index; ++previous)
        {
            if (!memcmp(record, snapshot + RECORD_OFFSET + previous * RECORD_SIZE, 9))
                return false;
        }
    }
    used = RECORD_OFFSET + snapshot[5] * RECORD_SIZE;
    for (; used < SNAPSHOT_CRC_OFFSET; ++used)
    {
        if (snapshot[used])
            return false;
    }
    return true;
}

static int newest_snapshot(const uint8_t *data)
{
    bool first = valid_snapshot(data);
    bool second = valid_snapshot(data + SNAPSHOT_SIZE);

    if (first && second)
    {
        uint32_t difference = read_u32(data + SNAPSHOT_SIZE + 8) - read_u32(data + 8);

        return difference && difference < UINT32_C(0x80000000) ? 1 : 0;
    }
    if (first)
        return 0;
    return second ? 1 : -1;
}

static int find_record(const uint8_t *snapshot, const DriveIdentity *identity,
                       uint8_t file_count)
{
    unsigned int index;

    for (index = 0; index < snapshot[5]; ++index)
    {
        const uint8_t *record = snapshot + RECORD_OFFSET + index * RECORD_SIZE;

        if (read_u32(record) == identity->size &&
            read_u32(record + 4) == identity->crc32 && record[8] == file_count)
            return (int)index;
    }
    return -1;
}

typedef enum
{
    SETTINGS_MISSING,
    SETTINGS_BAD_SIZE,
    SETTINGS_READ_FAILED,
    SETTINGS_READ_OK
} SettingsReadResult;

static SettingsReadResult read_settings(uint8_t *data)
{
    uint8_t handle = ti_Open("ECBRSET", "r");
    SettingsReadResult result;

    if (!handle)
        return SETTINGS_MISSING;
    if (ti_GetSize(handle) != SETTINGS_SIZE)
        result = SETTINGS_BAD_SIZE;
    else
        result = ti_Read(data, 1, SETTINGS_SIZE, handle) == SETTINGS_SIZE ?
                     SETTINGS_READ_OK : SETTINGS_READ_FAILED;
    if (!ti_Close(handle))
        result = SETTINGS_READ_FAILED;
    return result;
}

void browser_preferences_load(const DriveIdentity *identity, uint8_t file_count,
                              BrowserPreferences *preferences)
{
    uint8_t *data;
    int active;
    int found;

    if (!preferences)
        return;
    memset(preferences, 0, sizeof(*preferences));
    preferences->view = BROWSER_ALL_FILES;
    if (!identity || !identity->size || !file_count)
        return;
    data = malloc(SETTINGS_SIZE);
    if (!data)
        return;
    if (read_settings(data) == SETTINGS_READ_OK &&
        (active = newest_snapshot(data)) >= 0)
    {
        const uint8_t *snapshot = data + (unsigned int)active * SNAPSHOT_SIZE;

        found = find_record(snapshot, identity, file_count);
        if (found >= 0)
        {
            const uint8_t *record = snapshot + RECORD_OFFSET + (unsigned int)found * RECORD_SIZE;

            preferences->view = record[9];
            memcpy(preferences->launchers, record + 12, BROWSER_FLAG_BYTES);
            memcpy(preferences->hidden, record + 44, BROWSER_FLAG_BYTES);
        }
    }
    free(data);
}

static void update_snapshot(uint8_t *snapshot, const DriveIdentity *identity,
                            uint8_t file_count, const BrowserPreferences *preferences,
                            uint32_t generation)
{
    unsigned int count = snapshot[5];
    int found = find_record(snapshot, identity, file_count);
    uint8_t *record;
    size_t used;

    if (found >= 0)
    {
        record = snapshot + RECORD_OFFSET + (unsigned int)found * RECORD_SIZE;
        memmove(record, record + RECORD_SIZE,
                (count - (unsigned int)found - 1u) * RECORD_SIZE);
        --count;
    }
    else if (count == RECORD_LIMIT)
    {
        memmove(snapshot + RECORD_OFFSET, snapshot + RECORD_OFFSET + RECORD_SIZE,
                (RECORD_LIMIT - 1u) * RECORD_SIZE);
        --count;
    }
    record = snapshot + RECORD_OFFSET + count * RECORD_SIZE;
    write_u32(record, identity->size);
    write_u32(record + 4, identity->crc32);
    record[8] = file_count;
    record[9] = preferences->view;
    record[10] = 0;
    record[11] = 0;
    memcpy(record + 12, preferences->launchers, BROWSER_FLAG_BYTES);
    memcpy(record + 44, preferences->hidden, BROWSER_FLAG_BYTES);
    memcpy(snapshot, "ECBP", 4);
    snapshot[4] = 1;
    snapshot[5] = (uint8_t)(count + 1u);
    snapshot[6] = 0;
    snapshot[7] = 0;
    write_u32(snapshot + 8, generation);
    used = RECORD_OFFSET + (count + 1u) * RECORD_SIZE;
    memset(snapshot + used, 0, SNAPSHOT_CRC_OFFSET - used);
    write_u32(snapshot + SNAPSHOT_CRC_OFFSET,
              crc32_update(0, snapshot, SNAPSHOT_CRC_OFFSET));
}

BrowserSaveResult browser_preferences_save(const DriveIdentity *identity,
                                          uint8_t file_count,
                                          const BrowserPreferences *preferences)
{
    uint8_t *data;
    uint8_t *intended;
    uint8_t *verified;
    uint8_t handle = 0;
    int active = -1;
    unsigned int target = 0;
    uint32_t generation = 1;
    SettingsReadResult state;
    BrowserSaveResult result = BROWSER_SAVE_FAILED;

    if (!identity || !identity->size || !preferences ||
        !valid_flags(file_count, preferences->view,
                     preferences->launchers, preferences->hidden))
        return BROWSER_SAVE_FAILED;
    data = malloc(SETTINGS_SIZE);
    if (!data)
        return BROWSER_SAVE_FAILED;
    state = read_settings(data);
    if (state == SETTINGS_READ_FAILED)
        goto done;
    if (state == SETTINGS_READ_OK)
        active = newest_snapshot(data);
    if (active >= 0)
    {
        const uint8_t *snapshot = data + (unsigned int)active * SNAPSHOT_SIZE;
        int found = find_record(snapshot, identity, file_count);

        if (found >= 0)
        {
            const uint8_t *record = snapshot + RECORD_OFFSET + (unsigned int)found * RECORD_SIZE;

            if (record[9] == preferences->view &&
                !memcmp(record + 12, preferences->launchers, BROWSER_FLAG_BYTES) &&
                !memcmp(record + 44, preferences->hidden, BROWSER_FLAG_BYTES))
            {
                result = BROWSER_SAVE_OK;
                goto done;
            }
        }
        generation = read_u32(snapshot + 8) + UINT32_C(1);
        target = (unsigned int)active ^ 1u;
        memcpy(data + target * SNAPSHOT_SIZE, snapshot, SNAPSHOT_SIZE);
        handle = ti_Open("ECBRSET", "r+");
        if (!handle || ti_GetSize(handle) != SETTINGS_SIZE)
            goto done;
    }
    else
    {
        unsigned int index;

        handle = ti_Open("ECBRSET", state == SETTINGS_MISSING ? "a+" : "w+");
        if (!handle || ti_GetSize(handle) != 0 || ti_Seek(0, SEEK_SET, handle) == EOF)
            goto done;
        memset(data, 0, SETTINGS_SIZE);
        if (ti_Write(data, 1, SETTINGS_SIZE, handle) != SETTINGS_SIZE ||
            ti_GetSize(handle) != SETTINGS_SIZE ||
            ti_Seek(0, SEEK_SET, handle) == EOF ||
            ti_Read(data, 1, SETTINGS_SIZE, handle) != SETTINGS_SIZE)
            goto done;
        for (index = 0; index < SETTINGS_SIZE; ++index)
        {
            if (data[index])
                goto done;
        }
    }
    intended = data + target * SNAPSHOT_SIZE;
    verified = data + (target ^ 1u) * SNAPSHOT_SIZE;
    update_snapshot(intended, identity, file_count, preferences, generation);
    if (ti_Seek((int)(target * SNAPSHOT_SIZE), SEEK_SET, handle) == EOF ||
        ti_Write(intended, 1, SNAPSHOT_SIZE, handle) != SNAPSHOT_SIZE ||
        ti_GetSize(handle) != SETTINGS_SIZE ||
        ti_Seek((int)(target * SNAPSHOT_SIZE), SEEK_SET, handle) == EOF ||
        ti_Read(verified, 1, SNAPSHOT_SIZE, handle) != SNAPSHOT_SIZE ||
        memcmp(intended, verified, SNAPSHOT_SIZE) || !valid_snapshot(verified) ||
        read_u32(verified + 8) != generation)
        goto done;
    result = ti_SetArchiveStatus(true, handle) ? BROWSER_SAVE_OK : BROWSER_SAVE_RAM_ONLY;

done:
    if (handle && !ti_Close(handle))
        result = BROWSER_SAVE_FAILED;
    free(data);
    return result;
}