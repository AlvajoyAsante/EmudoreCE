#include "crc32.h"

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    static const uint32_t table[16] = {
        0x00000000UL, 0x1db71064UL, 0x3b6e20c8UL, 0x26d930acUL,
        0x76dc4190UL, 0x6b6b51f4UL, 0x4db26158UL, 0x5005713cUL,
        0xedb88320UL, 0xf00f9344UL, 0xd6d6a3e8UL, 0xcb61b38cUL,
        0x9b64c2b0UL, 0x86d3d2d4UL, 0xa00ae278UL, 0xbdbdf21cUL
    };
    crc = ~crc;
    while (size--)
    {
        crc ^= *data++;
        crc = (crc >> 4) ^ table[crc & 15];
        crc = (crc >> 4) ^ table[crc & 15];
    }
    return ~crc;
}