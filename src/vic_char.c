#include "vic_char.h"
#include "io.h"

#include <stdlib.h>
#include <string.h>

uint8_t *vic_char_dst;
const uint8_t *vic_char_screen, *vic_char_colram, *vic_char_glyphs;
uint8_t vic_char_cells, vic_char_rows, vic_char_multicolor;

extern uint8_t vic_char_hires_lut[16][16][4];
extern uint8_t vic_char_mc_lut[8][16][4];

static uint8_t cached_bg0, cached_bg1, cached_bg2;
static bool colors_initialized;

_Static_assert(IO_TAG_FOREGROUND == 0x20, "Character assembly foreground tag");

void vic_char_set_colors(uint8_t bg0, uint8_t bg1, uint8_t bg2)
{
    uint8_t color, pattern;
    bool hires_changed, mc_changed;

    bg0 &= 0x0f;
    bg1 &= 0x0f;
    bg2 &= 0x0f;
    hires_changed = !colors_initialized || bg0 != cached_bg0;
    mc_changed = hires_changed || bg1 != cached_bg1 || bg2 != cached_bg2;

    if (!mc_changed)
        return;

    if (hires_changed)
    {
        for (color = 0; color < 16; ++color)
        {
            uint8_t foreground = (uint8_t)(color | IO_TAG_FOREGROUND);
            for (pattern = 0; pattern < 16; ++pattern)
            {
                uint8_t *pixels = vic_char_hires_lut[color][pattern];
                pixels[0] = (pattern & 8) ? foreground : bg0;
                pixels[1] = (pattern & 4) ? foreground : bg0;
                pixels[2] = (pattern & 2) ? foreground : bg0;
                pixels[3] = (pattern & 1) ? foreground : bg0;
            }
        }
    }

    if (mc_changed)
    {
        for (color = 0; color < 8; ++color)
        {
            const uint8_t palette[4] = {
                bg0, bg1, (uint8_t)(bg2 | IO_TAG_FOREGROUND),
                (uint8_t)(color | IO_TAG_FOREGROUND)
            };
            for (pattern = 0; pattern < 16; ++pattern)
            {
                uint8_t *pixels = vic_char_mc_lut[color][pattern];
                pixels[0] = pixels[1] = palette[pattern >> 2];
                pixels[2] = pixels[3] = palette[pattern & 3];
            }
        }
    }

    cached_bg0 = bg0;
    cached_bg1 = bg1;
    cached_bg2 = bg2;
    colors_initialized = true;
}

enum
{
    CHAR_TEST_GUARD = 16,
    CHAR_TEST_SENTINEL = 0xa5,
    CHAR_TEST_CELLS = 40,
    CHAR_TEST_STRIDE = 320,
    CHAR_TEST_ROWS = 8,
    CHAR_TEST_GLYPHS = 2048
};

typedef struct
{
    uint8_t pixels[CHAR_TEST_GUARD * 2 + CHAR_TEST_STRIDE * CHAR_TEST_ROWS];
    uint8_t glyphs[CHAR_TEST_GUARD * 2 + CHAR_TEST_GLYPHS];
    uint8_t screen[CHAR_TEST_GUARD * 2 + CHAR_TEST_CELLS];
    uint8_t colram[CHAR_TEST_GUARD * 2 + CHAR_TEST_CELLS];
} VicCharTestScratch;

_Static_assert(sizeof(VicCharTestScratch) == 4816, "Character test scratch size");

#if defined(__clang__)
__attribute__((__minsize__, noinline))
#endif
static bool char_test_guard(const uint8_t *bytes, size_t length)
{
    while (length != 0)
    {
        if (*bytes++ != CHAR_TEST_SENTINEL)
            return false;
        --length;
    }
    return true;
}

#if defined(__clang__)
__attribute__((__minsize__, noinline))
#endif
static void char_test_reference(uint8_t *pixels, uint8_t data, uint8_t color,
                                uint8_t multicolor, uint8_t bg0,
                                uint8_t bg1, uint8_t bg2)
{
    uint8_t pixel;
    color &= 15;
    for (pixel = 0; pixel < 8; ++pixel)
    {
        if (multicolor != 0 && (color & 8) != 0)
        {
            switch ((data >> (6 - (pixel & 6))) & 3)
            {
                case 0: pixels[pixel] = bg0; break;
                case 1: pixels[pixel] = bg1; break;
                case 2: pixels[pixel] = (uint8_t)(bg2 | 0x20); break;
                default: pixels[pixel] = (uint8_t)((color & 7) | 0x20); break;
            }
        }
        else
        {
            pixels[pixel] = (data & (0x80u >> pixel)) != 0
                          ? (uint8_t)(color | 0x20) : bg0;
        }
    }
}

#if defined(__clang__)
__attribute__((__minsize__, noinline))
#endif
static bool char_test_case(VicCharTestScratch *scratch, uint8_t first_code,
                           uint8_t color, uint8_t cells, uint8_t rows,
                           uint8_t start_row, uint16_t left, uint8_t multicolor,
                           bool varying_colors, uint8_t bg0,
                           uint8_t bg1, uint8_t bg2)
{
    uint8_t *screen = scratch->screen + CHAR_TEST_GUARD;
    uint8_t *colram = scratch->colram + CHAR_TEST_GUARD;
    const uint8_t *glyphs = scratch->glyphs + CHAR_TEST_GUARD + start_row;
    uint8_t *destination = scratch->pixels + CHAR_TEST_GUARD + left;
    const uint8_t *actual = destination;
    uint8_t expected[8];
    uint8_t cell, row;

    memset(scratch->pixels, CHAR_TEST_SENTINEL, sizeof(scratch->pixels));
    for (cell = 0; cell < CHAR_TEST_CELLS; ++cell)
    {
        uint8_t cell_color = color;
        if (varying_colors)
            cell_color += cell;
        screen[cell] = (uint8_t)(first_code + cell);
        colram[cell] = (uint8_t)(0xa0 | (cell_color & 15));
    }

    vic_char_set_colors(bg0, bg1, bg2);
    vic_char_dst = destination;
    vic_char_screen = screen;
    vic_char_colram = colram;
    vic_char_glyphs = glyphs;
    vic_char_cells = cells;
    vic_char_rows = rows;
    vic_char_multicolor = multicolor;
    vic_char_expand_rows();

    if (vic_char_dst != destination || vic_char_screen != screen ||
        vic_char_colram != colram || vic_char_glyphs != glyphs ||
        vic_char_cells != cells || vic_char_rows != rows ||
        vic_char_multicolor != multicolor)
        return false;

    if (!char_test_guard(scratch->screen, CHAR_TEST_GUARD) ||
        !char_test_guard(screen + CHAR_TEST_CELLS, CHAR_TEST_GUARD) ||
        !char_test_guard(scratch->colram, CHAR_TEST_GUARD) ||
        !char_test_guard(colram + CHAR_TEST_CELLS, CHAR_TEST_GUARD) ||
        !char_test_guard(scratch->glyphs, CHAR_TEST_GUARD) ||
        !char_test_guard(scratch->glyphs + CHAR_TEST_GUARD + CHAR_TEST_GLYPHS,
                         CHAR_TEST_GUARD))
        return false;

    for (cell = 0; cell < CHAR_TEST_CELLS; ++cell)
    {
        uint8_t cell_color = color;
        if (varying_colors)
            cell_color += cell;
        if (screen[cell] != (uint8_t)(first_code + cell) ||
            colram[cell] != (uint8_t)(0xa0 | (cell_color & 15)))
            return false;
    }

    if (cells == 0 || cells > CHAR_TEST_CELLS ||
        rows == 0 || rows > CHAR_TEST_ROWS)
        return char_test_guard(scratch->pixels, sizeof(scratch->pixels));

    if (!char_test_guard(scratch->pixels, CHAR_TEST_GUARD + left))
        return false;
    for (row = 0; row < rows; ++row)
    {
        for (cell = 0; cell < cells; ++cell)
        {
            uint8_t data = glyphs[(unsigned int)screen[cell] * 8 + row];
            char_test_reference(expected, data, colram[cell], multicolor,
                                bg0, bg1, bg2);
            if (memcmp(actual, expected, sizeof(expected)) != 0)
                return false;
            actual += sizeof(expected);
        }
        if (row + 1 < rows)
        {
            size_t gap = CHAR_TEST_STRIDE - (unsigned int)cells * 8;
            if (!char_test_guard(actual, gap))
                return false;
            actual += gap;
        }
    }
    return char_test_guard(actual,
                           (size_t)(scratch->pixels + sizeof(scratch->pixels) - actual));
}

#if defined(__clang__)
__attribute__((__minsize__))
#endif
bool vic_char_selftest(void)
{
    static const uint8_t backgrounds[][3] = {
        {0, 0, 0}, {0, 0, 0}, {0, 15, 0}, {0, 15, 8},
        {6, 15, 8}, {6, 15, 8}, {15, 3, 12}, {0, 0, 0}
    };
    static const uint8_t invalid_counts[][2] = {
        {0, 1}, {41, 1}, {255, 1}, {1, 0}, {1, 9}, {1, 255}
    };
    uint8_t *saved_dst = vic_char_dst;
    const uint8_t *saved_screen = vic_char_screen;
    const uint8_t *saved_colram = vic_char_colram;
    const uint8_t *saved_glyphs = vic_char_glyphs;
    uint8_t saved_cells = vic_char_cells;
    uint8_t saved_rows = vic_char_rows;
    uint8_t saved_multicolor = vic_char_multicolor;
    uint8_t saved_bg0 = cached_bg0;
    uint8_t saved_bg1 = cached_bg1;
    uint8_t saved_bg2 = cached_bg2;
    bool saved_initialized = colors_initialized;
    VicCharTestScratch *scratch = malloc(sizeof(*scratch));
    uint8_t *glyphs;
    unsigned int code;
    uint8_t row, mode, color, phase;
    bool passed = false;

    if (!scratch)
        return false;
    memset(scratch, CHAR_TEST_SENTINEL, sizeof(*scratch));
    glyphs = scratch->glyphs + CHAR_TEST_GUARD;
    for (code = 0; code < 256; ++code)
        for (row = 0; row < CHAR_TEST_ROWS; ++row)
            glyphs[code * 8 + row] = (uint8_t)(code ^ (row * 37u));

    if (((uintptr_t)vic_char_hires_lut & 255u) != 0 ||
        ((uintptr_t)vic_char_mc_lut & 255u) != 0)
        goto finish;

    for (phase = 0; phase < sizeof(backgrounds) / sizeof(backgrounds[0]); ++phase)
    {
        const uint8_t *palette = backgrounds[phase];
        for (mode = 0; mode < 2; ++mode)
        {
            uint8_t multicolor = mode ? 0xff : 0;
            if (!char_test_case(scratch, 216, 0, 40, 8, 0, 0, multicolor,
                                true, palette[0], palette[1], palette[2]) ||
                !char_test_case(scratch, 230, 7, 39, 5, 3, 7, multicolor,
                                true, palette[0], palette[1], palette[2]))
                goto finish;
        }
    }

    for (mode = 0; mode < 2; ++mode)
    {
        uint8_t multicolor = mode ? 0xff : 0;
        for (color = 0; color < 16; ++color)
        {
            for (code = 0; code < 256; code += 32)
            {
                if (!char_test_case(scratch, (uint8_t)code, color, 32, 1, 7,
                                    (uint8_t)(code / 32), multicolor, false,
                                    6, 15, 8))
                    goto finish;
            }
        }
        for (row = 1; row <= CHAR_TEST_ROWS; ++row)
        {
            if (!char_test_case(scratch, 255, 15, 1, row,
                                (uint8_t)(CHAR_TEST_ROWS - row), 312,
                                multicolor, false, 6, 15, 8))
                goto finish;
        }
    }

    for (phase = 0; phase < sizeof(invalid_counts) / sizeof(invalid_counts[0]); ++phase)
    {
        if (!char_test_case(scratch, 0, 0, invalid_counts[phase][0],
                            invalid_counts[phase][1], 0, 0, 1, false, 6, 15, 8))
            goto finish;
    }
    for (code = 0; code < 256; ++code)
    {
        for (row = 0; row < CHAR_TEST_ROWS; ++row)
        {
            if (glyphs[code * 8 + row] != (uint8_t)(code ^ (row * 37u)))
                goto finish;
        }
    }
    passed = true;

finish:
    if (!passed)
        colors_initialized = false;
    if (saved_initialized)
        vic_char_set_colors(saved_bg0, saved_bg1, saved_bg2);
    else
        colors_initialized = false;
    vic_char_dst = saved_dst;
    vic_char_screen = saved_screen;
    vic_char_colram = saved_colram;
    vic_char_glyphs = saved_glyphs;
    vic_char_cells = saved_cells;
    vic_char_rows = saved_rows;
    vic_char_multicolor = saved_multicolor;
    free(scratch);
    return passed;
}
