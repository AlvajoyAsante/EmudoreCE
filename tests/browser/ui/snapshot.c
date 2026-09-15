#include "snapshot.h"
#include "mock.h"
#include "crc32.h"
#include <graphx.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

bool ui_snapshots_enabled;

static void put16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned int index = 0; index < 4; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8));
}

static uint8_t channel(uint16_t color, unsigned int shift)
{
    return (uint8_t)(((color >> shift) & 31u) * 255 / 31);
}

void ui_snapshot(const char *name)
{
    uint8_t header[54] = {0};
    bool used[256] = {false};
    unsigned int colors = 0, differing = 0;
    char path[128];
    FILE *output;
    int directory;
    ui_verify_guards();
    UI_CHECK(ui.graphics_active && !ui.saving);
    for (size_t index = 0; index < UI_PIXELS; ++index) {
        used[gfx_vram[index]] = true;
        if (gfx_vram[index] != gfx_vram[0])
            ++differing;
    }
    for (unsigned int index = 0; index < 256; ++index)
        if (used[index])
            ++colors;
    UI_CHECK(colors >= 8 && differing > UI_PIXELS / 2);
    if (!ui_snapshots_enabled)
        return;
#ifdef _WIN32
    directory = _mkdir("snapshots");
#else
    directory = mkdir("snapshots", 0777);
#endif
    UI_CHECK(directory == 0 || errno == EEXIST);
    UI_CHECK(strchr(name, '/') == NULL && strchr(name, '\\') == NULL);
    UI_CHECK(snprintf(path, sizeof(path), "snapshots/%s.bmp", name) < (int)sizeof(path));
    header[0] = 'B';
    header[1] = 'M';
    put32(header + 2, 54 + 1024 + UI_PIXELS);
    put32(header + 10, 54 + 1024);
    put32(header + 14, 40);
    put32(header + 18, 320);
    put32(header + 22, 240);
    put16(header + 26, 1);
    put16(header + 28, 8);
    put32(header + 34, UI_PIXELS);
    put32(header + 46, 256);
    output = fopen(path, "wb");
    UI_CHECK(output != NULL);
    UI_CHECK(fwrite(header, 1, sizeof(header), output) == sizeof(header));
    for (unsigned int index = 0; index < 256; ++index) {
        uint16_t color = gfx_palette[index];
        uint8_t entry[4] = {channel(color, 0), channel(color, 5), channel(color, 10), 0};
        UI_CHECK(fwrite(entry, 1, sizeof(entry), output) == sizeof(entry));
    }
    for (int row = 239; row >= 0; --row)
        UI_CHECK(fwrite(gfx_vram + (unsigned int)row * 320, 1, 320, output) == 320);
    UI_CHECK(fclose(output) == 0);
    printf("SNAPSHOT %s: 320x240, 77878 bytes, %u colors, %u differing pixels, "
           "frame CRC32=%08" PRIx32 ", palette CRC32=%08" PRIx32 "\n",
           path, colors, differing, crc32_update(0, gfx_vram, UI_PIXELS),
           crc32_update(0, (const uint8_t *)gfx_palette, 512));
}