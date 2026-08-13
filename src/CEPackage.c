#include "CEPackage.h"

#include <fileioc.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <debug.h>

/* dbg_printf() is sprintf() into the debug port, which pulls in the whole
   printf stack machinery and overflows the CE's small stack in the deep
   package-reader call chain. Silence it here; main.c reports boot status
   with the stack-free dbg_puts(). */
#undef dbg_printf
#define dbg_printf(...) ((void)0)

/* Header layout (v2): magic[4], version, kind, chunk, chunkCount, mapper,
   compressionMethod, storedTotalSize (bytes actually kept in AppVars),
   originalTotalSize (decompressed size), payloadSize, checksum. */
#define CE_PACKAGE_HEADER_SIZE 22
#define CE_PACKAGE_PAYLOAD_SIZE 0x8000L
#define CE_PACKAGE_VERSION 2
#define CE_PACKAGE_DIRECT_CHUNKS 8
#define CE_PACKAGE_COMPRESSION_NONE 0
#define CE_PACKAGE_COMPRESSION_LZSS 1

static const uint8_t packageMagic[4] = {'E', 'M', '6', '4'};
static const char *const packageIds[] = {
    CE_PACKAGE_BASIC,
    CE_PACKAGE_KERNAL,
    CE_PACKAGE_CHARS
};
static uint8_t directHandles[3][CE_PACKAGE_DIRECT_CHUNKS];
static const uint8_t *directData[3][CE_PACKAGE_DIRECT_CHUNKS];
static void *directOwnedBuffer[3];

static int packageIndex(const char *packageId)
{
    int i;
    for (i = 0; i < 3; ++i)
        if (!strcmp(packageId, packageIds[i]))
            return i;
    return -1;
}

static unsigned int read16(const uint8_t *data)
{
    return (unsigned int)data[0] | ((unsigned int)data[1] << 8);
}

static uint32_t read32(const uint8_t *data)
{
    return (uint32_t)data[0]
         | ((uint32_t)data[1] << 8)
         | ((uint32_t)data[2] << 16)
         | ((uint32_t)data[3] << 24);
}

static int makeChunkName(char name[9], const char *packageId, uint8_t chunk)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t length = strlen(packageId);
    size_t index;

    if (!length || length > 4)
        return 0;

    name[0] = 'E';
    name[1] = 'C';
    for (index = 0; index < length; ++index)
    {
        char character = packageId[index];
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9')))
            return 0;
        name[index + 2] = character;
    }

    name[length + 2] = hex[chunk >> 4];
    name[length + 3] = hex[chunk & 0x0F];
    name[length + 4] = '\0';
    return 1;
}

static uint8_t openChunk(const char *packageId, uint8_t chunk, uint8_t header[CE_PACKAGE_HEADER_SIZE])
{
    char name[9];
    uint8_t handle;
    uint32_t totalSize;
    uint32_t payloadSize;

    if (!makeChunkName(name, packageId, chunk))
    {
        dbg_printf("[pkg] makeChunkName FAILED id=%s chunk=%d\n", packageId, (int)chunk);
        return 0;
    }

    dbg_printf("[pkg] open '%s'\n", name);
    handle = ti_Open(name, "r");
    if (!handle)
    {
        dbg_printf("[pkg] ti_Open('%s') -> 0 (not found)\n", name);
        return 0;
    }
    dbg_printf("[pkg] ti_Open('%s') -> handle %u, fileSize=%ld\n", name,
               (unsigned)handle, (long)ti_GetSize(handle));

    if (ti_GetSize(handle) < CE_PACKAGE_HEADER_SIZE ||
        ti_Read(header, CE_PACKAGE_HEADER_SIZE, 1, handle) != 1)
    {
        ti_Close(handle);
        return 0;
    }

    totalSize = read32(header + 10);
    if (totalSize <= (uint32_t)chunk * CE_PACKAGE_PAYLOAD_SIZE)
    {
        ti_Close(handle);
        return 0;
    }

    payloadSize = totalSize - (uint32_t)chunk * CE_PACKAGE_PAYLOAD_SIZE;
    if (payloadSize > CE_PACKAGE_PAYLOAD_SIZE)
        payloadSize = CE_PACKAGE_PAYLOAD_SIZE;

    if (memcmp(header, packageMagic, sizeof(packageMagic)) ||
        header[4] != CE_PACKAGE_VERSION ||
        header[5] != (uint8_t)packageIndex(packageId) ||
        header[6] != chunk ||
        !header[7] ||
        header[7] != (totalSize + CE_PACKAGE_PAYLOAD_SIZE - 1) / CE_PACKAGE_PAYLOAD_SIZE ||
        header[9] > CE_PACKAGE_COMPRESSION_LZSS ||
        read16(header + 18) != payloadSize ||
        ti_GetSize(handle) != CE_PACKAGE_HEADER_SIZE + payloadSize)
    {
        ti_Close(handle);
        return 0;
    }

    return handle;
}

typedef struct
{
    const char *packageId;
    uint8_t chunk;
    uint8_t chunkCount;
    uint8_t handle;
    const uint8_t *data;
    unsigned int size;
    unsigned int offset;
} LzssSource;

static void lzssSourceInit(LzssSource *source, const char *packageId)
{
    source->packageId = packageId;
    source->chunk = 0;
    source->chunkCount = 0;
    source->handle = 0;
    source->data = NULL;
    source->size = 0;
    source->offset = 0;
}

static void lzssSourceClose(LzssSource *source)
{
    if (source->handle)
        ti_Close(source->handle);
    source->handle = 0;
}

/* Pulls the next raw (stored) byte of a package, transparently crossing the
   32 KiB AppVar chunk boundaries used purely for on-calculator storage. */
static int lzssSourceByte(LzssSource *source)
{
    uint8_t header[CE_PACKAGE_HEADER_SIZE];

    while (source->offset >= source->size)
    {
        if (source->handle)
        {
            ti_Close(source->handle);
            source->handle = 0;
        }
        if (source->chunkCount && source->chunk >= source->chunkCount)
            return -1;

        source->handle = openChunk(source->packageId, source->chunk, header);
        if (!source->handle)
            return -1;
        if (!source->chunkCount)
            source->chunkCount = header[7];

        source->data = ti_GetDataPtr(source->handle);
        if (!source->data)
        {
            ti_Close(source->handle);
            source->handle = 0;
            return -1;
        }
        source->size = read16(header + 18);
        source->offset = 0;
        ++source->chunk;
    }

    return source->data[source->offset++];
}

/* Decodes the LZSS stream written by tools/rom_to_appvar.py. Matches are
   copied directly out of the destination buffer, which doubles as the
   sliding window, so no extra scratch memory is required. */
static int decompressLzss(const char *packageId, uint8_t *destination, uint32_t originalSize)
{
    LzssSource source;
    uint32_t position = 0;
    unsigned int controlByte = 0;
    unsigned int controlBits = 0;

    lzssSourceInit(&source, packageId);

    while (position < originalSize)
    {
        int value;

        if (!controlBits)
        {
            value = lzssSourceByte(&source);
            if (value < 0)
            {
                lzssSourceClose(&source);
                return 0;
            }
            controlByte = (unsigned int)value;
            controlBits = 8;
        }

        --controlBits;
        if (controlByte & (1u << controlBits))
        {
            value = lzssSourceByte(&source);
            if (value < 0)
            {
                lzssSourceClose(&source);
                return 0;
            }
            destination[position++] = (uint8_t)value;
        }
        else
        {
            int lowByte = lzssSourceByte(&source);
            int highByte = lzssSourceByte(&source);
            unsigned int distance;
            unsigned int length;

            if (lowByte < 0 || highByte < 0)
            {
                lzssSourceClose(&source);
                return 0;
            }

            distance = ((unsigned int)lowByte | (((unsigned int)highByte & 0x0F) << 8)) + 1;
            length = ((unsigned int)highByte >> 4) + 3;
            if (distance > position)
            {
                lzssSourceClose(&source);
                return 0;
            }
            if (length > originalSize - position)
                length = originalSize - position;

            while (length--)
            {
                destination[position] = destination[position - distance];
                ++position;
            }
        }
    }

    lzssSourceClose(&source);
    return 1;
}

static int packageCompressionMethod(const char *packageId)
{
    uint8_t header[CE_PACKAGE_HEADER_SIZE];
    uint8_t handle = openChunk(packageId, 0, header);
    int method;

    if (!handle)
        return -1;
    method = header[9];
    ti_Close(handle);
    return method;
}

uint32_t CEPackageSize(const char *packageId)
{
    uint8_t header[CE_PACKAGE_HEADER_SIZE];
    uint8_t handle = openChunk(packageId, 0, header);
    uint32_t size;

    if (!handle)
        return 0;

    size = read32(header + 14);
    ti_Close(handle);
    return size;
}

int CEPackageValidate(const char *packageId, uint32_t expectedSize)
{
    uint8_t header[CE_PACKAGE_HEADER_SIZE];
    /* static so the checksum scratch buffer does NOT live on the stack (the
       CE stack is small and overflows into BSS easily). */
    static uint8_t buffer[128];
    uint8_t chunk;
    uint8_t chunkCount = 0;
    uint8_t compressionMethod = 0;

    for (chunk = 0; !chunkCount || chunk < chunkCount; ++chunk)
    {
        uint8_t handle = openChunk(packageId, chunk, header);
        unsigned int remaining;
        unsigned int expectedChecksum;
        unsigned int checksum = 0;

        if (!handle)
        {
            dbg_printf("[pkg] validate '%s' chunk %d open failed\n", packageId, (int)chunk);
            return 0;
        }

        if (!chunkCount)
        {
            chunkCount = header[7];
            compressionMethod = header[9];
        }
        dbg_printf("[pkg] validate chunk=%d/%d method=%d stored=%lu orig=%lu\n",
                   (int)chunk, (int)chunkCount, (int)compressionMethod,
                   (unsigned long)read32(header + 10), (unsigned long)read32(header + 14));
        if (header[7] != chunkCount || header[9] != compressionMethod ||
            read32(header + 14) != expectedSize)
        {
            dbg_printf("[pkg] validate header mismatch (expectedSize=%lu)\n",
                       (unsigned long)expectedSize);
            ti_Close(handle);
            return 0;
        }

        remaining = read16(header + 18);
        expectedChecksum = read16(header + 20);
        dbg_printf("[pkg] validate payload=%u expectChecksum=%04x\n",
                   remaining, expectedChecksum);
        while (remaining)
        {
            unsigned int count = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            unsigned int index;
            unsigned int got = ti_Read(buffer, 1, count, handle);
            if (got != count)
            {
                dbg_printf("[pkg] validate ti_Read got=%u want=%u\n", got, count);
                ti_Close(handle);
                return 0;
            }
            for (index = 0; index < count; ++index)
                checksum += buffer[index];
            remaining -= count;
        }

        ti_Close(handle);
        dbg_printf("[pkg] validate checksum=%04x expect=%04x\n",
                   (unsigned)(checksum & 0xFFFF), expectedChecksum);
        if ((checksum & 0xFFFF) != expectedChecksum)
            return 0;
    }

    dbg_printf("[pkg] validate '%s' OK\n", packageId);
    return 1;
}

const uint8_t *CEPackageGetDirect(const char *packageId, uint32_t expectedSize)
{
    return CEPackageGetDirectChunk(packageId, 0, expectedSize);
}

const uint8_t *CEPackageGetDirectChunk(const char *packageId, uint8_t chunk,
                                       uint32_t expectedSize)
{
    uint8_t header[CE_PACKAGE_HEADER_SIZE];
    uint8_t handle;
    const uint8_t *data;
    int index = packageIndex(packageId);
    int method;

    if (index < 0 || chunk >= CE_PACKAGE_DIRECT_CHUNKS)
        return NULL;
    if (directData[index][chunk])
        return directData[index][chunk];

    dbg_printf("[pkg] GetDirectChunk idx=%d chunk=%d size=%lu\n",
               index, (int)chunk, (unsigned long)expectedSize);
    if (!CEPackageValidate(packageId, expectedSize))
        return NULL;
    dbg_printf("[pkg] validated\n");

    method = packageCompressionMethod(packageId);
    dbg_printf("[pkg] method=%d\n", method);
    if (method < 0)
        return NULL;

    if (method != CE_PACKAGE_COMPRESSION_NONE)
    {
        uint8_t *decoded = malloc(expectedSize);
        uint8_t fill;

        dbg_printf("[pkg] malloc(%lu)=%p method=%d\n",
                   (unsigned long)expectedSize, (void *)decoded, method);
        if (!decoded || !decompressLzss(packageId, decoded, expectedSize))
        {
            free(decoded);
            return NULL;
        }
        dbg_printf("[pkg] decompressed ok\n");

        directOwnedBuffer[index] = decoded;
        /* The decompressed buffer is contiguous original data; point every
           direct-chunk slot at its slice so banked (mapper) reads work. */
        for (fill = 0; fill < CE_PACKAGE_DIRECT_CHUNKS; ++fill)
            directData[index][fill] = decoded + (uint32_t)fill * CE_PACKAGE_PAYLOAD_SIZE;
        return directData[index][chunk];
    }

    handle = openChunk(packageId, chunk, header);
    if (!handle || read32(header + 14) != expectedSize)
    {
        if (handle)
            ti_Close(handle);
        return NULL;
    }

    data = ti_GetDataPtr(handle);
    /* Close the handle right away. The direct data pointer to an AppVar stays
       valid for the whole emulation because we never create, delete, resize,
       or move any variable while it is in use (that would trigger a garbage
       collection and invalidate it). Keeping the handle open is what caused
       ti_Close() to fault at shutdown. */
    ti_Close(handle);
    if (!data)
        return NULL;

    directData[index][chunk] = data;
    return directData[index][chunk];
}

const uint8_t *CEPackageChunkPtr(const char *packageId, uint32_t offset,
                                 uint32_t expectedSize)
{
    uint8_t chunk = (uint8_t)(offset / CE_PACKAGE_PAYLOAD_SIZE);

    if (offset >= expectedSize)
        return NULL;
    if (!CEPackageGetDirectChunk(packageId, chunk, expectedSize))
        return NULL;
    return directData[packageIndex(packageId)][chunk]
         + (offset % CE_PACKAGE_PAYLOAD_SIZE);
}

void CEPackageCloseDirect(void)
{
    int index;
    int chunk;

    for (index = 0; index < 3; ++index)
    {
        /* Handles are closed as soon as their data pointer is taken, so there
           is nothing to close here; just drop the cached pointers and free any
           malloc'd decompression buffer. */
        for (chunk = 0; chunk < CE_PACKAGE_DIRECT_CHUNKS; ++chunk)
        {
            directHandles[index][chunk] = 0;
            directData[index][chunk] = NULL;
        }
        if (directOwnedBuffer[index])
        {
            free(directOwnedBuffer[index]);
            directOwnedBuffer[index] = NULL;
        }
    }
}

int CEPackageRead(const char *packageId, uint8_t *destination, uint32_t expectedSize)
{
    uint8_t header[CE_PACKAGE_HEADER_SIZE];
    uint8_t chunk;
    uint8_t chunkCount = 0;
    uint32_t offset = 0;
    int method;

    dbg_printf("[pkg] Read id=%s dest=%p expected=%lu\n", packageId,
               (void *)destination, (unsigned long)expectedSize);
    method = packageCompressionMethod(packageId);
    dbg_printf("[pkg]   compression method=%d\n", method);
    if (method < 0)
        return 0;
    if (method != CE_PACKAGE_COMPRESSION_NONE)
    {
        int ok = CEPackageValidate(packageId, expectedSize);
        dbg_printf("[pkg]   compressed validate=%d\n", ok);
        return ok && decompressLzss(packageId, destination, expectedSize);
    }

    for (chunk = 0; !chunkCount || chunk < chunkCount; ++chunk)
    {
        uint8_t handle = openChunk(packageId, chunk, header);
        unsigned int payloadSize;
        unsigned int expectedChecksum;
        unsigned int checksum = 0;
        unsigned int index;

        if (!handle)
            return 0;

        if (!chunkCount)
            chunkCount = header[7];
        if (header[7] != chunkCount || read32(header + 14) != expectedSize)
        {
            ti_Close(handle);
            return 0;
        }

        payloadSize = read16(header + 18);
        expectedChecksum = read16(header + 20);
        if (offset + payloadSize > expectedSize ||
            ti_Read(destination + offset, 1, payloadSize, handle) != payloadSize)
        {
            ti_Close(handle);
            return 0;
        }

        ti_Close(handle);
        for (index = 0; index < payloadSize; ++index)
            checksum += destination[offset + index];
        checksum &= 0xFFFF;
        dbg_printf("[pkg]   chunk %d payload=%u checksum=%04x expected=%04x\n",
                   (int)chunk, payloadSize, (unsigned)checksum, expectedChecksum);
        if (checksum != expectedChecksum)
            return 0;

        offset += payloadSize;
    }

    dbg_printf("[pkg]   read done offset=%lu expected=%lu\n",
               (unsigned long)offset, (unsigned long)expectedSize);
    return offset == expectedSize;
}

int CEPackageReadAt(const char *packageId, uint32_t offset)
{
    uint8_t header[CE_PACKAGE_HEADER_SIZE];
    uint8_t chunk;
    uint8_t handle;
    unsigned int chunkOffset;
    int value;

    /* Compressed packages have no fixed mapping from an original-data offset
       to a stored byte position, so random access is not supported for them. */
    if (packageCompressionMethod(packageId) != CE_PACKAGE_COMPRESSION_NONE)
        return -1;

    chunk = (uint8_t)(offset / CE_PACKAGE_PAYLOAD_SIZE);
    chunkOffset = (unsigned int)(offset % CE_PACKAGE_PAYLOAD_SIZE);
    handle = openChunk(packageId, chunk, header);
    if (!handle || chunkOffset >= read16(header + 18) ||
        ti_Seek(CE_PACKAGE_HEADER_SIZE + chunkOffset, SEEK_SET, handle) == EOF)
    {
        if (handle)
            ti_Close(handle);
        return -1;
    }

    value = ti_GetC(handle);
    ti_Close(handle);
    return value;
}
