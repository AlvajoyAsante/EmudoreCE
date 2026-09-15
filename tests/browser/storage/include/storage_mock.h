#ifndef STORAGE_MOCK_H
#define STORAGE_MOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { MOCK_INFO, MOCK_SETTINGS, MOCK_FILE_COUNT };
enum
{
    MOCK_OPEN,
    MOCK_READ,
    MOCK_WRITE,
    MOCK_SEEK,
    MOCK_CLOSE,
    MOCK_ARCHIVE,
    MOCK_ALLOCATE,
    MOCK_SIZE,
    MOCK_OPERATION_COUNT
};

typedef struct
{
    uint8_t data[4096];
    uint16_t size;
    bool exists;
    bool archived;
} MockFile;

typedef struct
{
    size_t offset;
    size_t requested;
    size_t written;
} MockWrite;

extern MockFile mock_files[MOCK_FILE_COUNT];
extern unsigned int mock_calls[MOCK_OPERATION_COUNT];
extern unsigned int mock_fail_at[MOCK_OPERATION_COUNT];
extern MockWrite mock_writes[32];
extern unsigned int mock_write_count;
extern unsigned int mock_mutations;
extern unsigned int mock_writable_opens;
extern unsigned int mock_archived_reads;
extern unsigned int mock_open_handles;
extern unsigned int mock_live_allocations;
extern size_t mock_peak_heap;
extern size_t mock_partial_read;
extern size_t mock_partial_write;
extern unsigned int mock_drop_write_at;
extern unsigned int mock_corrupt_write_at;
extern unsigned int mock_override_read_at;
extern uint8_t mock_read_override[2048];

void mock_reset(void);
void mock_reset_operations(void);
void *storage_test_malloc(size_t size);
void storage_test_free(void *pointer);

#endif