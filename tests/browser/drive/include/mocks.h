#ifndef BROWSER_DRIVE_MOCKS_H
#define BROWSER_DRIVE_MOCKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "CEPackage.h"
#include "cpu.h"
#include "memory.h"
#include "vic.h"

enum {
    MOCK_CHUNK_SIZE = 0x8000,
    MOCK_CHUNK_COUNT = 4,
    MOCK_GUARD_SIZE = 16,
    MOCK_DISK_CAPACITY = MOCK_CHUNK_SIZE * MOCK_CHUNK_COUNT
};

typedef struct {
    uint8_t before[MOCK_GUARD_SIZE];
    uint8_t data[MOCK_CHUNK_SIZE];
    uint8_t after[MOCK_GUARD_SIZE];
    size_t available;
} MockChunk;

typedef struct {
    uint32_t size;
    MockChunk chunks[MOCK_CHUNK_COUNT];
} MockDisk;

typedef struct {
    MockDisk disks[2];
    unsigned int active_disk;
    const uint8_t *mapped[MOCK_CHUNK_COUNT];
    unsigned int direct_calls[MOCK_CHUNK_COUNT];
    unsigned int byte_calls, size_calls, close_calls;
    unsigned int frames, render_requests, unhide_calls, io_reads;
    bool boot_hidden;
} MockDrive;

typedef struct {
    uint8_t before[MOCK_GUARD_SIZE];
    uint8_t data[MEM_SIZE];
    uint8_t after[MOCK_GUARD_SIZE];
} MockMainMemory;

typedef struct {
    uint8_t before[MOCK_GUARD_SIZE];
    uint8_t data[MEM_LOW_SIZE];
    uint8_t after[MOCK_GUARD_SIZE];
} MockLowMemory;

extern MockDrive mock;
extern MockMainMemory mock_ram_backing, mock_rom_backing;
extern MockLowMemory mock_low_backing;

#define mock_ram (mock_ram_backing.data)
#define mock_low (mock_low_backing.data)
#define mock_rom (mock_rom_backing.data)

void mock_reset(void);
bool mock_guards_intact(void);
void mock_set_size(uint32_t size);
uint8_t *mock_disk_byte(uint32_t offset);
void mock_disk_write(uint32_t offset, const uint8_t *data, size_t size);

#endif