#include "storage_mock.h"

#include <assert.h>
#include <fileioc.h>
#include <stdlib.h>
#include <string.h>

MockFile mock_files[MOCK_FILE_COUNT];
unsigned int mock_calls[MOCK_OPERATION_COUNT];
unsigned int mock_fail_at[MOCK_OPERATION_COUNT];
MockWrite mock_writes[32];
unsigned int mock_write_count;
unsigned int mock_mutations;
unsigned int mock_writable_opens;
unsigned int mock_archived_reads;
unsigned int mock_open_handles;
unsigned int mock_live_allocations;
size_t mock_peak_heap;
size_t mock_partial_read;
size_t mock_partial_write;
unsigned int mock_drop_write_at;
unsigned int mock_corrupt_write_at;
unsigned int mock_override_read_at;
uint8_t mock_read_override[2048];

static uint8_t open_handle;
static size_t position;
static bool writable;
static uint8_t *allocation;
static size_t allocation_size;

static bool operation_fails(unsigned int operation)
{
    ++mock_calls[operation];
    return mock_calls[operation] == mock_fail_at[operation];
}

void mock_reset_operations(void)
{
    assert(!mock_open_handles && !mock_live_allocations);
    memset(mock_calls, 0, sizeof(mock_calls));
    memset(mock_fail_at, 0, sizeof(mock_fail_at));
    memset(mock_writes, 0, sizeof(mock_writes));
    mock_write_count = 0;
    mock_mutations = 0;
    mock_writable_opens = 0;
    mock_archived_reads = 0;
    mock_partial_read = 0;
    mock_partial_write = 0;
    mock_drop_write_at = 0;
    mock_corrupt_write_at = 0;
    mock_override_read_at = 0;
    memset(mock_read_override, 0, sizeof(mock_read_override));
}

void mock_reset(void)
{
    mock_reset_operations();
    memset(mock_files, 0, sizeof(mock_files));
}

void *storage_test_malloc(size_t size)
{
    assert(!allocation && !mock_live_allocations && size <= 2048);
    if (operation_fails(MOCK_ALLOCATE))
        return NULL;
    allocation = malloc(size + 32);
    assert(allocation);
    memset(allocation, 0xa5, size + 32);
    allocation_size = size;
    ++mock_live_allocations;
    if (size > mock_peak_heap)
        mock_peak_heap = size;
    return allocation + 16;
}

void storage_test_free(void *pointer)
{
    size_t index;

    assert(allocation && pointer == allocation + 16 && mock_live_allocations == 1);
    for (index = 0; index < 16; ++index)
    {
        assert(allocation[index] == 0xa5);
        assert(allocation[16 + allocation_size + index] == 0xa5);
    }
    free(allocation);
    allocation = NULL;
    --mock_live_allocations;
}

uint8_t ti_Open(const char *name, const char *mode)
{
    unsigned int file_index;
    MockFile *file;

    assert(!mock_open_handles);
    assert(!strcmp(name, "ECDINFO") || !strcmp(name, "ECBRSET"));
    file_index = !strcmp(name, "ECDINFO") ? MOCK_INFO : MOCK_SETTINGS;
    assert(file_index != MOCK_INFO || !strcmp(mode, "r"));
    assert(!strcmp(mode, "r") || !strcmp(mode, "r+") ||
           !strcmp(mode, "w+") || !strcmp(mode, "a+"));
    if (operation_fails(MOCK_OPEN))
        return 0;
    file = &mock_files[file_index];
    if (mode[0] == 'r' && !file->exists)
        return 0;
    writable = strcmp(mode, "r") != 0;
    if (writable)
    {
        ++mock_writable_opens;
        if (mode[0] == 'w' || !file->exists)
        {
            memset(file->data, 0xcc, sizeof(file->data));
            file->exists = true;
            file->size = 0;
            ++mock_mutations;
        }
        if (file->archived)
            ++mock_mutations;
        file->archived = false;
    }
    position = mode[0] == 'a' ? file->size : 0;
    open_handle = (uint8_t)(file_index + 1);
    ++mock_open_handles;
    return open_handle;
}

int ti_Close(uint8_t handle)
{
    bool failed;

    assert(mock_open_handles == 1 && handle == open_handle);
    failed = operation_fails(MOCK_CLOSE);
    --mock_open_handles;
    open_handle = 0;
    return failed ? 0 : 1;
}

uint16_t ti_GetSize(uint8_t handle)
{
    assert(mock_open_handles == 1 && handle == open_handle);
    if (operation_fails(MOCK_SIZE))
        return 0;
    return mock_files[handle - 1].size;
}

size_t ti_Read(void *data, size_t size, size_t count, uint8_t handle)
{
    MockFile *file;
    size_t available;

    assert(mock_open_handles == 1 && handle == open_handle && size == 1);
    file = &mock_files[handle - 1];
    assert(position <= file->size);
    if (operation_fails(MOCK_READ) && count > mock_partial_read)
        count = mock_partial_read;
    available = file->size - position;
    if (count > available)
        count = available;
    if (mock_calls[MOCK_READ] == mock_override_read_at)
    {
        assert(count <= sizeof(mock_read_override));
        memcpy(data, mock_read_override, count);
    }
    else
        memcpy(data, file->data + position, count);
    if (file->archived)
        ++mock_archived_reads;
    position += count;
    return count;
}

size_t ti_Write(const void *data, size_t size, size_t count, uint8_t handle)
{
    MockFile *file;
    MockWrite *write;

    assert(mock_open_handles == 1 && handle == open_handle && writable && size == 1);
    assert(handle == MOCK_SETTINGS + 1 && mock_write_count < 32);
    file = &mock_files[handle - 1];
    assert(!file->archived);
    write = &mock_writes[mock_write_count++];
    write->offset = position;
    write->requested = count;
    if (operation_fails(MOCK_WRITE) && count > mock_partial_write)
        count = mock_partial_write;
    write->written = count;
    assert(position + count <= sizeof(file->data));
    if (mock_calls[MOCK_WRITE] == mock_drop_write_at)
        return count;
    if (count)
    {
        ++mock_mutations;
        memcpy(file->data + position, data, count);
        if (mock_calls[MOCK_WRITE] == mock_corrupt_write_at)
            file->data[position + count / 2] ^= 1;
    }
    position += count;
    if (position > file->size)
        file->size = (uint16_t)position;
    return count;
}

int ti_Seek(int offset, unsigned int origin, uint8_t handle)
{
    long destination;

    assert(mock_open_handles == 1 && handle == open_handle);
    if (operation_fails(MOCK_SEEK))
        return EOF;
    if (origin == SEEK_SET)
        destination = offset;
    else if (origin == SEEK_CUR)
        destination = (long)position + offset;
    else
    {
        assert(origin == SEEK_END);
        destination = (long)mock_files[handle - 1].size + offset;
    }
    if (destination < 0 || destination > mock_files[handle - 1].size)
        return EOF;
    position = (size_t)destination;
    return 0;
}

int (ti_SetArchiveStatus)(uint8_t archive, uint8_t handle)
{
    assert(mock_open_handles == 1 && handle == open_handle);
    assert(handle == MOCK_SETTINGS + 1 && archive == 1);
    if (operation_fails(MOCK_ARCHIVE))
        return 0;
    mock_files[handle - 1].archived = true;
    ++mock_mutations;
    return 1;
}