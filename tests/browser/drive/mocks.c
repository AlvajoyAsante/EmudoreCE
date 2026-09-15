#include <assert.h>
#include <string.h>
#include "mocks.h"
#include "drive.h"

MockDrive mock;
CpuState cpu;
MockMainMemory mock_ram_backing, mock_rom_backing;
MockLowMemory mock_low_backing;
uint8_t *mem_ram = mock_ram;
uint8_t *mem_low = mock_low;
MemPage mem_rd_pg[256];
MemPage mem_wr_pg[256];

void mock_reset(void)
{
    unsigned int page;
    unsigned int disk;
    unsigned int chunk;
    drive_invalidate();
    memset(&mock, 0, sizeof(mock));
    memset(&cpu, 0, sizeof(cpu));
    memset(&mock_ram_backing, 0xa5, sizeof(mock_ram_backing));
    memset(&mock_low_backing, 0xa5, sizeof(mock_low_backing));
    memset(&mock_rom_backing, 0xa5, sizeof(mock_rom_backing));
    memset(mock_ram, 0xcd, sizeof(mock_ram));
    memset(mock_low, 0, sizeof(mock_low));
    memset(mock_rom, 0xea, sizeof(mock_rom));
    for (disk = 0; disk < 2; ++disk) {
        for (chunk = 0; chunk < MOCK_CHUNK_COUNT; ++chunk) {
            memset(mock.disks[disk].chunks[chunk].before, 0xa5, MOCK_GUARD_SIZE);
            memset(mock.disks[disk].chunks[chunk].after, 0x5a, MOCK_GUARD_SIZE);
        }
    }
    for (page = 0; page < 256; ++page) {
        uint8_t *backing = page < MEM_LOW_SIZE / 256 ? mock_low : mock_ram;
        mem_wr_pg[page].ptr = backing;
        mem_rd_pg[page].ptr = page >= ADDR_KERNAL / 256 ? mock_rom : backing;
    }
}

bool mock_guards_intact(void)
{
    unsigned int position, disk, chunk;
    for (position = 0; position < MOCK_GUARD_SIZE; ++position) {
        if (mock_ram_backing.before[position] != 0xa5 ||
            mock_ram_backing.after[position] != 0xa5 ||
            mock_low_backing.before[position] != 0xa5 ||
            mock_low_backing.after[position] != 0xa5 ||
            mock_rom_backing.before[position] != 0xa5 ||
            mock_rom_backing.after[position] != 0xa5)
            return false;
        for (disk = 0; disk < 2; ++disk) {
            for (chunk = 0; chunk < MOCK_CHUNK_COUNT; ++chunk) {
                if (mock.disks[disk].chunks[chunk].before[position] != 0xa5 ||
                    mock.disks[disk].chunks[chunk].after[position] != 0x5a)
                    return false;
            }
        }
    }
    return true;
}

void mock_set_size(uint32_t size)
{
    unsigned int chunk;
    MockDisk *disk = &mock.disks[mock.active_disk];
    assert(size <= MOCK_DISK_CAPACITY);
    disk->size = size;
    for (chunk = 0; chunk < MOCK_CHUNK_COUNT; ++chunk) {
        uint32_t offset = (uint32_t)chunk * MOCK_CHUNK_SIZE;
        uint32_t remaining = size > offset ? size - offset : 0;
        disk->chunks[chunk].available = remaining > MOCK_CHUNK_SIZE
            ? MOCK_CHUNK_SIZE : remaining;
    }
}

uint8_t *mock_disk_byte(uint32_t offset)
{
    assert(offset < MOCK_DISK_CAPACITY);
    return &mock.disks[mock.active_disk].chunks[offset / MOCK_CHUNK_SIZE]
        .data[offset % MOCK_CHUNK_SIZE];
}

void mock_disk_write(uint32_t offset, const uint8_t *data, size_t size)
{
    size_t position;
    assert(offset <= MOCK_DISK_CAPACITY);
    assert(size <= MOCK_DISK_CAPACITY - offset);
    for (position = 0; position < size; ++position)
        *mock_disk_byte(offset + (uint32_t)position) = data[position];
}

uint32_t CEPackageSize(const char *packageId)
{
    assert(strcmp(packageId, CE_PACKAGE_DISK) == 0);
    ++mock.size_calls;
    return mock.disks[mock.active_disk].size;
}

static const uint8_t *map_chunk(const char *packageId, unsigned int chunk,
                                uint32_t expectedSize)
{
    MockDisk *disk = &mock.disks[mock.active_disk];
    uint32_t offset = (uint32_t)chunk * MOCK_CHUNK_SIZE;
    uint32_t remaining;
    assert(strcmp(packageId, CE_PACKAGE_DISK) == 0);
    if (chunk >= MOCK_CHUNK_COUNT || expectedSize != disk->size ||
        offset >= disk->size)
        return NULL;
    remaining = disk->size - offset;
    if (remaining > MOCK_CHUNK_SIZE)
        remaining = MOCK_CHUNK_SIZE;
    if (disk->chunks[chunk].available < remaining)
        return NULL;
    if (!mock.mapped[chunk])
        mock.mapped[chunk] = disk->chunks[chunk].data;
    return mock.mapped[chunk];
}

const uint8_t *CEPackageGetDirectChunk(const char *packageId, uint8_t chunk,
                                      uint32_t expectedSize)
{
    assert(chunk < MOCK_CHUNK_COUNT);
    ++mock.direct_calls[chunk];
    return map_chunk(packageId, chunk, expectedSize);
}

const uint8_t *CEPackageChunkPtr(const char *packageId, uint32_t offset,
                                uint32_t expectedSize)
{
    const uint8_t *data;
    ++mock.byte_calls;
    if (offset >= expectedSize)
        return NULL;
    data = map_chunk(packageId, offset / MOCK_CHUNK_SIZE, expectedSize);
    return data ? data + offset % MOCK_CHUNK_SIZE : NULL;
}

void CEPackageCloseDirect(void)
{
    ++mock.close_calls;
    memset(mock.mapped, 0, sizeof(mock.mapped));
}

uint8_t *mem_kernal_rom_ptr(void)
{
    return mock_rom + ADDR_KERNAL;
}

uint8_t mem_read_io(uint16_t addr)
{
    ++mock.io_reads;
    return *mem_host_ptr(addr);
}

unsigned int vic_frames(void)
{
    return mock.frames;
}

void vic_request_render(void)
{
    ++mock.render_requests;
}

void vic_set_boot_hidden(bool hidden)
{
    mock.boot_hidden = hidden;
    ++mock.unhide_calls;
}