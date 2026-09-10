#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/vic_char.h"
#if VIC_TEST_CHAR_INTEGRATION
static void host_char_expand_rows(void);
#define vic_char_expand_rows host_char_expand_rows
#endif
#include "../../src/vic.c"

#ifdef timer_Get
#undef timer_Get
#endif
#if VIC_TEST_CHAR_INTEGRATION
#undef vic_char_expand_rows
#endif

extern uint8_t vic_char_hires_lut[16][16][4];
extern uint8_t vic_char_mc_lut[8][16][4];

enum
{
    HOST_WIDTH = 320,
    HOST_HEIGHT = 240,
    HOST_GUARD_BYTES = 32,
    HOST_GUARD_VALUE = 0xa5,
    HOST_BACKGROUND = 6
};

typedef struct
{
    uint8_t before[HOST_GUARD_BYTES];
    uint8_t pixels[HOST_WIDTH];
    uint8_t after[HOST_GUARD_BYTES];
} GuardedRow;

typedef struct
{
    uint8_t *destination;
    const uint8_t *bitmap, *screen, *colors;
    uint8_t background, cells;
} BitmapState;

enum
{
    BITMAP_FAULT_PREFIX = 1,
    BITMAP_FAULT_SUFFIX = 2,
    BITMAP_FAULT_LAST_PIXEL = 4,
    BITMAP_FAULT_FOREGROUND = 8,
    BITMAP_FAULT_COLOR_RAM = 16
};

static GuardedRow host_framebuffer[HOST_HEIGHT];
static uint8_t host_ram[MEM_SIZE];
static uint8_t host_low_ram[MEM_LOW_SIZE];
static uint8_t host_char_rom[0x1000];
static uint16_t host_vic_bank;
static uint32_t host_timer_ticks;
static unsigned int host_irq_count;
static unsigned int host_refresh_count;
static unsigned int assertion_failures;
static uint8_t host_hires_stride;
static uint8_t host_mc_stride;
static unsigned int host_bitmap_calls[2];
static BitmapState host_bitmap_state[2];
static const uint8_t *host_bitmap_sources[2][8];
static uint8_t host_bitmap_faults[2];

#if VIC_TEST_CHAR_INTEGRATION
static uint8_t *host_char_pixels;
static unsigned int host_char_kernel_calls;
#endif

CpuState cpu;
uint8_t cpu_warp;
uint8_t *mem_ram = host_ram;
uint8_t *mem_low = host_low_ram;
uint8_t mem_color_ram[0x400];

uint32_t timer_Get(unsigned int timer)
{
    (void)timer;
    return host_timer_ticks;
}

void cpu_irq(void)
{
    ++host_irq_count;
}

uint16_t cia2_vic_base_address(void)
{
    return host_vic_bank;
}

uint8_t *mem_chars_rom_ptr(void)
{
    return host_char_rom;
}

uint8_t mem_vic_read_byte(uint16_t address)
{
    uint16_t banked_address = (uint16_t)(host_vic_bank + (address & 0x3fff));
    if ((banked_address & 0x7000) == 0x1000)
        return host_char_rom[banked_address & 0x0fff];
    return *mem_host_ptr(banked_address);
}

uint8_t *io_text_row(int frame_y)
{
    int lcd_y = frame_y - IO_CROP_Y;
    if (lcd_y < 0 || lcd_y >= HOST_HEIGHT)
        return NULL;
#if VIC_TEST_CHAR_INTEGRATION
    if (host_char_pixels && lcd_y >= 20 && lcd_y < 28)
        return host_char_pixels + (lcd_y - 20) * HOST_WIDTH;
#endif
    return host_framebuffer[lcd_y].pixels;
}

void io_screen_draw_border(int frame_y, uint8_t color)
{
    uint8_t *destination = io_text_row(frame_y);
    (void)color;
    if (destination)
        memset(destination, 0, HOST_WIDTH);
}

void io_screen_refresh(void)
{
    ++host_refresh_count;
}

static void reset_bitmap_adapters(void)
{
    host_hires_stride = 0;
    host_mc_stride = 0;
    memset(host_bitmap_calls, 0, sizeof(host_bitmap_calls));
    memset(host_bitmap_state, 0, sizeof(host_bitmap_state));
    memset(host_bitmap_sources, 0, sizeof(host_bitmap_sources));
    memset(host_bitmap_faults, 0, sizeof(host_bitmap_faults));
}

static void capture_bitmap_row(unsigned int kernel)
{
    host_bitmap_state[kernel] = (BitmapState){
        vic_row_dst, vic_row_bitmap, vic_row_screen, vic_row_colram,
        vic_row_bg, vic_row_cells
    };
    if (host_bitmap_calls[kernel] < 8)
        host_bitmap_sources[kernel][host_bitmap_calls[kernel]] = vic_row_bitmap;
    ++host_bitmap_calls[kernel];
}

static void corrupt_bitmap_output(unsigned int kernel)
{
    if (host_bitmap_faults[kernel] & BITMAP_FAULT_PREFIX)
        vic_row_dst[-1] = 0;
    if (host_bitmap_faults[kernel] & BITMAP_FAULT_SUFFIX)
        vic_row_dst[vic_row_cells * 8] = 0;
    if (host_bitmap_faults[kernel] & BITMAP_FAULT_LAST_PIXEL)
        vic_row_dst[vic_row_cells * 8 - 1] ^= 1;
    if (host_bitmap_faults[kernel] & BITMAP_FAULT_FOREGROUND)
        vic_row_dst[0] ^= IO_TAG_FOREGROUND;
}

void vic_expand_hires_row(void)
{
    capture_bitmap_row(0);
    if (!host_hires_stride)
    {
        memset(vic_row_dst, 0, (size_t)vic_row_cells * 8);
        return;
    }
    for (unsigned int cell = 0; cell < vic_row_cells; ++cell)
    {
        uint8_t data = vic_row_bitmap[cell * host_hires_stride];
        uint8_t foreground = (uint8_t)((vic_row_screen[cell] >> 4) | IO_TAG_FOREGROUND);
        uint8_t background = vic_row_screen[cell] & 15;
        for (unsigned int pixel = 0; pixel < 8; ++pixel)
            vic_row_dst[cell * 8 + pixel] = (data & (0x80u >> pixel)) ? foreground : background;
    }
    corrupt_bitmap_output(0);
}

void vic_expand_mc_row(void)
{
    capture_bitmap_row(1);
    if (!host_mc_stride)
    {
        memset(vic_row_dst, 0, (size_t)vic_row_cells * 8);
        return;
    }
    for (unsigned int cell = 0; cell < vic_row_cells; ++cell)
    {
        uint8_t data = vic_row_bitmap[cell * host_mc_stride];
        uint8_t palette[4] = {
            vic_row_bg,
            (uint8_t)(vic_row_screen[cell] >> 4),
            (uint8_t)((vic_row_screen[cell] & 15) | IO_TAG_FOREGROUND),
            (uint8_t)((vic_row_colram[cell] & 15) | IO_TAG_FOREGROUND)
        };
        if (host_bitmap_faults[1] & BITMAP_FAULT_COLOR_RAM)
            palette[3] = palette[2];
        for (unsigned int pair = 0; pair < 4; ++pair)
        {
            uint8_t color = palette[(data >> (6 - pair * 2)) & 3];
            vic_row_dst[cell * 8 + pair * 2] = color;
            vic_row_dst[cell * 8 + pair * 2 + 1] = color;
        }
    }
    corrupt_bitmap_output(1);
}

#define EXPECT_EQ(actual, expected) do { \
    unsigned long observed_value = (unsigned long)(actual); \
    unsigned long expected_value = (unsigned long)(expected); \
    if (observed_value != expected_value) { \
        fprintf(stderr, "  line %d: %s = 0x%lx, expected 0x%lx\n", \
                __LINE__, #actual, observed_value, expected_value); \
        ++assertion_failures; \
    } \
} while (0)

typedef struct
{
    uint8_t screen[HOST_GUARD_BYTES + 40 + HOST_GUARD_BYTES];
    uint8_t colors[HOST_GUARD_BYTES + 40 + HOST_GUARD_BYTES];
    uint8_t glyphs[HOST_GUARD_BYTES + 2048 + HOST_GUARD_BYTES];
} CharSource;

typedef struct
{
    uint8_t before[HOST_GUARD_BYTES];
    uint8_t pixels[HOST_WIDTH * 8];
    uint8_t after[HOST_GUARD_BYTES];
} CharOutput;

typedef struct
{
    uint8_t *destination;
    const uint8_t *screen, *colors, *glyphs;
    uint8_t cells, rows, multicolor;
} CharState;

static CharState char_state(void)
{
    CharState state = {
        vic_char_dst, vic_char_screen, vic_char_colram, vic_char_glyphs,
        vic_char_cells, vic_char_rows, vic_char_multicolor
    };
    return state;
}

#if VIC_TEST_CHAR_INTEGRATION
static CharState host_char_kernel_state;

static void host_char_expand_rows(void)
{
    ++host_char_kernel_calls;
    host_char_kernel_state = char_state();
    vic_char_expand_rows();
}
#endif

static void expect_char_state(CharState expected)
{
    EXPECT_EQ(vic_char_dst == expected.destination, true);
    EXPECT_EQ(vic_char_screen == expected.screen, true);
    EXPECT_EQ(vic_char_colram == expected.colors, true);
    EXPECT_EQ(vic_char_glyphs == expected.glyphs, true);
    EXPECT_EQ(vic_char_cells, expected.cells);
    EXPECT_EQ(vic_char_rows, expected.rows);
    EXPECT_EQ(vic_char_multicolor, expected.multicolor);
}

static void expect_bytes_equal(const void *actual, const void *expected,
                               size_t length, const char *label)
{
    const uint8_t *actual_bytes = actual;
    const uint8_t *expected_bytes = expected;
    for (size_t byte_index = 0; byte_index < length; ++byte_index)
    {
        if (actual_bytes[byte_index] != expected_bytes[byte_index])
        {
            fprintf(stderr, "  %s byte %zu = 0x%02x, expected 0x%02x\n",
                    label, byte_index, actual_bytes[byte_index], expected_bytes[byte_index]);
            ++assertion_failures;
            return;
        }
    }
}

static uint8_t char_expected_pixel(uint8_t data, uint8_t color,
                                   uint8_t multicolor, unsigned int pixel,
                                   const uint8_t palette[3])
{
    color &= 15;
    if (multicolor != 0 && (color & 8) != 0)
    {
        switch ((data >> (6 - 2 * (pixel / 2))) & 3)
        {
            case 0: return palette[0] & 15;
            case 1: return palette[1] & 15;
            case 2: return (palette[2] & 15) | 0x20;
            default: return (color & 7) | 0x20;
        }
    }
    return (data & (0x80u >> pixel)) ? color | 0x20 : palette[0] & 15;
}

static void expect_char_tables(const uint8_t palette[3])
{
    for (unsigned int color = 0; color < 16; ++color)
        for (unsigned int pattern = 0; pattern < 16; ++pattern)
            for (unsigned int pixel = 0; pixel < 4; ++pixel)
            {
                uint8_t data = (uint8_t)(pattern << 4);
                EXPECT_EQ(vic_char_hires_lut[color][pattern][pixel],
                          char_expected_pixel(data, (uint8_t)color, 0, pixel, palette));
                if (color < 8)
                    EXPECT_EQ(vic_char_mc_lut[color][pattern][pixel],
                              char_expected_pixel(data, (uint8_t)(color | 8), 1,
                                                  pixel, palette));
            }
}

static void test_char_tables(void)
{
    EXPECT_EQ((uintptr_t)vic_char_hires_lut & 255u, 0);
    EXPECT_EQ((uintptr_t)vic_char_mc_lut & 255u, 0);
    for (unsigned int background = 0; background < 16; ++background)
    {
        const uint8_t palette[3] = {
            (uint8_t)(0xf0 | background),
            (uint8_t)(0xa0 | ((background + 5) & 15)),
            (uint8_t)(0xb0 | ((background + 11) & 15))
        };
        vic_char_set_colors(palette[0], palette[1], palette[2]);
        expect_char_tables(palette);
    }
}

static void test_char_color_invalidation(void)
{
    static const uint8_t palettes[][3] = {
        {6, 1, 12}, {6, 1, 12}, {6, 9, 12}, {6, 9, 4},
        {2, 9, 4}, {2, 9, 4}, {0xf2, 0xa9, 0xb4}, {6, 1, 12}
    };
    for (size_t phase = 0; phase < sizeof(palettes) / sizeof(palettes[0]); ++phase)
    {
        const uint8_t *palette = palettes[phase];
        vic_char_set_colors(palette[0], palette[1], palette[2]);
        expect_char_tables(palette);
    }
}

static void prepare_char_source(CharSource *source)
{
    memset(source, HOST_GUARD_VALUE, sizeof(*source));
    for (unsigned int cell = 0; cell < 40; ++cell)
    {
        source->screen[HOST_GUARD_BYTES + cell] = (uint8_t)(229 + cell * 17);
        source->colors[HOST_GUARD_BYTES + cell] = (uint8_t)(0xa0 | (cell & 15));
    }
    for (unsigned int code = 0; code < 256; ++code)
        for (unsigned int row = 0; row < 8; ++row)
            source->glyphs[HOST_GUARD_BYTES + code * 8 + row] =
                (uint8_t)(code * 29 + row * 71);
}

static void check_char_expansion(CharSource *source, uint8_t cells, uint8_t rows,
                                 uint8_t start_row, unsigned int left,
                                 uint8_t multicolor)
{
    const uint8_t palette[3] = {6, 1, 12};
    CharSource saved_source = *source;
    CharOutput output, expected;
    CharState saved_state;
    uint8_t saved_hires[sizeof(vic_char_hires_lut)];
    uint8_t saved_mc[sizeof(vic_char_mc_lut)];
    bool valid_counts = cells > 0 && cells <= 40 && rows > 0 && rows <= 8;

    memset(&output, HOST_GUARD_VALUE, sizeof(output));
    memset(&expected, HOST_GUARD_VALUE, sizeof(expected));
    vic_char_set_colors(palette[0], palette[1], palette[2]);
    memcpy(saved_hires, vic_char_hires_lut, sizeof(saved_hires));
    memcpy(saved_mc, vic_char_mc_lut, sizeof(saved_mc));
    vic_char_dst = output.pixels + left;
    vic_char_screen = source->screen + HOST_GUARD_BYTES;
    vic_char_colram = source->colors + HOST_GUARD_BYTES;
    vic_char_glyphs = source->glyphs + HOST_GUARD_BYTES + start_row;
    vic_char_cells = cells;
    vic_char_rows = rows;
    vic_char_multicolor = multicolor;
    saved_state = char_state();

    if (valid_counts)
    {
        EXPECT_EQ(start_row + rows <= 8, true);
        EXPECT_EQ(left + cells * 8 <= HOST_WIDTH, true);
        for (unsigned int row = 0; row < rows; ++row)
            for (unsigned int cell = 0; cell < cells; ++cell)
            {
                uint8_t code = saved_state.screen[cell];
                uint8_t data = saved_state.glyphs[(unsigned int)code * 8 + row];
                for (unsigned int pixel = 0; pixel < 8; ++pixel)
                    expected.pixels[row * HOST_WIDTH + left + cell * 8 + pixel] =
                        char_expected_pixel(data, saved_state.colors[cell], multicolor,
                                            pixel, palette);
            }
    }

    vic_char_expand_rows();
    expect_char_state(saved_state);
    expect_bytes_equal(&output, &expected, sizeof(output), "character output/guards");
    expect_bytes_equal(source, &saved_source, sizeof(*source), "character inputs/guards");
    expect_bytes_equal(vic_char_hires_lut, saved_hires, sizeof(saved_hires), "hires table");
    expect_bytes_equal(vic_char_mc_lut, saved_mc, sizeof(saved_mc), "multicolor table");
}

static void test_char_mixed(void)
{
    CharSource source;
    prepare_char_source(&source);
    check_char_expansion(&source, 40, 8, 0, 0, 0);
    check_char_expansion(&source, 40, 8, 0, 0, 1);
    check_char_expansion(&source, 40, 8, 0, 0, 0xff);
}

static void test_char_row_span(void)
{
    CharSource source;
    prepare_char_source(&source);
    source.screen[HOST_GUARD_BYTES] = 255;
    source.colors[HOST_GUARD_BYTES] = 0xff;
    for (uint8_t rows = 1; rows <= 8; ++rows)
        for (unsigned int mode = 0; mode < 2; ++mode)
            check_char_expansion(&source, 1, rows, (uint8_t)(8 - rows), 312,
                                 mode ? 0xff : 0);
}

static void test_char_cell_span(void)
{
    CharSource source;
    prepare_char_source(&source);
    for (uint8_t cells = 1; cells <= 40; ++cells)
        for (unsigned int mode = 0; mode < 2; ++mode)
            check_char_expansion(&source, cells, 3, 2, (40 - cells) * 8,
                                 mode ? 0xff : 0);
}

static void test_char_live_glyphs(void)
{
    CharSource source;
    prepare_char_source(&source);
    source.screen[HOST_GUARD_BYTES] = 255;
    source.colors[HOST_GUARD_BYTES] = 0xaf;
    check_char_expansion(&source, 2, 3, 5, 11, 1);
    source.glyphs[HOST_GUARD_BYTES + 255 * 8 + 5] = 0xff;
    source.glyphs[HOST_GUARD_BYTES + 255 * 8 + 6] = 0x00;
    source.glyphs[HOST_GUARD_BYTES + 255 * 8 + 7] = 0x1b;
    check_char_expansion(&source, 2, 3, 5, 11, 1);
    source.colors[HOST_GUARD_BYTES] = 0xa3;
    check_char_expansion(&source, 2, 3, 5, 11, 1);
    source.screen[HOST_GUARD_BYTES] = 0;
    check_char_expansion(&source, 2, 3, 5, 11, 1);
}

static void test_char_invalid_counts(void)
{
    static const uint8_t counts[][2] = {
        {0, 1}, {41, 1}, {255, 1}, {1, 0}, {1, 9}, {1, 255}, {0, 0}, {255, 255}
    };
    CharSource source;
    prepare_char_source(&source);
    for (size_t case_index = 0; case_index < sizeof(counts) / sizeof(counts[0]); ++case_index)
    {
        CharState saved_state;
        check_char_expansion(&source, counts[case_index][0], counts[case_index][1], 0, 0, 1);
        vic_char_dst = NULL;
        vic_char_screen = NULL;
        vic_char_colram = NULL;
        vic_char_glyphs = NULL;
        saved_state = char_state();
        vic_char_expand_rows();
        expect_char_state(saved_state);
    }
}

static void test_char_selftest(void)
{
    uint8_t saved_hires[sizeof(vic_char_hires_lut)];
    uint8_t saved_mc[sizeof(vic_char_mc_lut)];
    CharState saved_state;
    uint8_t input = 0xa5;
    vic_char_dst = &input;
    vic_char_screen = &input;
    vic_char_colram = &input;
    vic_char_glyphs = &input;
    vic_char_cells = 23;
    vic_char_rows = 5;
    vic_char_multicolor = 0x81;
    saved_state = char_state();
    vic_char_set_colors(14, 5, 9);
    memcpy(saved_hires, vic_char_hires_lut, sizeof(saved_hires));
    memcpy(saved_mc, vic_char_mc_lut, sizeof(saved_mc));
    EXPECT_EQ(vic_char_selftest(), true);
    expect_char_state(saved_state);
    EXPECT_EQ(input, 0xa5);
    expect_bytes_equal(vic_char_hires_lut, saved_hires, sizeof(saved_hires), "restored hires table");
    expect_bytes_equal(vic_char_mc_lut, saved_mc, sizeof(saved_mc), "restored multicolor table");
}

static void test_bitmap_selftest_reject_packed(void)
{
    host_hires_stride = 1;
    host_mc_stride = 1;
    EXPECT_EQ(vic_row_asm_selftest() & 3u, 0);
    EXPECT_EQ(hires_asm_ready, false);
    EXPECT_EQ(mc_asm_ready, false);
}

static void test_bitmap_selftest_layout(void)
{
    static const uint8_t strides[] = {0, 1, 8};
    for (unsigned int hires_case = 0; hires_case < 3; ++hires_case)
        for (unsigned int mc_case = 0; mc_case < 3; ++mc_case)
        {
            uint8_t expected = (strides[hires_case] == 8 ? 1 : 0) |
                               (strides[mc_case] == 8 ? 2 : 0);
            reset_bitmap_adapters();
            host_hires_stride = strides[hires_case];
            host_mc_stride = strides[mc_case];
            EXPECT_EQ(vic_row_asm_selftest() & 3u, expected);
            EXPECT_EQ(hires_asm_ready, strides[hires_case] == 8);
            EXPECT_EQ(mc_asm_ready, strides[mc_case] == 8);
            EXPECT_EQ(char_asm_ready, EMUDORE_CHAR_ASM != 0);
            for (unsigned int kernel = 0; kernel < 2; ++kernel)
            {
                EXPECT_EQ(host_bitmap_calls[kernel], 8);
                EXPECT_EQ(host_bitmap_state[kernel].cells, 40);
                EXPECT_EQ(host_bitmap_state[kernel].screen != host_bitmap_state[kernel].colors, true);
                for (unsigned int scanline = 0; scanline < 8; ++scanline)
                    EXPECT_EQ(host_bitmap_sources[kernel][scanline] ==
                              host_bitmap_sources[kernel][0] + scanline, true);
            }
        }
}

static void test_bitmap_selftest_faults(void)
{
    static const uint8_t faults[] = {
        BITMAP_FAULT_PREFIX, BITMAP_FAULT_SUFFIX, BITMAP_FAULT_LAST_PIXEL,
        BITMAP_FAULT_FOREGROUND, BITMAP_FAULT_COLOR_RAM
    };
    for (unsigned int kernel = 0; kernel < 2; ++kernel)
        for (unsigned int fault = 0; fault < sizeof(faults); ++fault)
        {
            if (kernel == 0 && faults[fault] == BITMAP_FAULT_COLOR_RAM)
                continue;
            reset_bitmap_adapters();
            host_hires_stride = host_mc_stride = 8;
            host_bitmap_faults[kernel] = faults[fault];
            EXPECT_EQ(vic_row_asm_selftest() & 3u, kernel == 0 ? 2 : 1);
        }
}

static void reset_fixture(void)
{
    memset(host_ram, 0, sizeof(host_ram));
    memset(host_low_ram, 0, sizeof(host_low_ram));
    memset(host_char_rom, 0, sizeof(host_char_rom));
    memset(mem_color_ram, 0, sizeof(mem_color_ram));
    memset(&cpu, 0, sizeof(cpu));
    memset(host_framebuffer, HOST_GUARD_VALUE, sizeof(host_framebuffer));
    for (unsigned int row_index = 0; row_index < HOST_HEIGHT; ++row_index)
        memset(host_framebuffer[row_index].pixels, HOST_BACKGROUND, HOST_WIDTH);
    host_vic_bank = 0;
    host_timer_ticks = 0;
    host_irq_count = 0;
    host_refresh_count = 0;
    reset_bitmap_adapters();
    cpu_warp = 0;
    vic_char_dst = NULL;
    vic_char_screen = NULL;
    vic_char_colram = NULL;
    vic_char_glyphs = NULL;
    vic_char_cells = 0;
    vic_char_rows = 0;
    vic_char_multicolor = 0;
    vic_reset();
#if VIC_TEST_CHAR_INTEGRATION
    char_asm_ready = false;
    host_char_pixels = NULL;
    host_char_kernel_calls = 0;
#endif
    render_requested = false;
    hires_asm_ready = false;
    mc_asm_ready = false;
    vic_wtrace_pos = 0;
    memset(sprite_line_mask, 0, sizeof(sprite_line_mask));
}

static void check_framebuffer_guards(void)
{
    for (unsigned int row_index = 0; row_index < HOST_HEIGHT; ++row_index)
    {
        for (unsigned int guard_index = 0; guard_index < HOST_GUARD_BYTES; ++guard_index)
        {
            if (host_framebuffer[row_index].before[guard_index] != HOST_GUARD_VALUE ||
                host_framebuffer[row_index].after[guard_index] != HOST_GUARD_VALUE)
            {
                fprintf(stderr, "  framebuffer guard changed: row %u, guard %u\n",
                        row_index, guard_index);
                ++assertion_failures;
                return;
            }
        }
    }
}

static const uint8_t position_x[8] = {0x11, 0x32, 0x53, 0x74, 0x95, 0xb6, 0xd7, 0xf8};
static const uint8_t position_y[8] = {0x21, 0x42, 0x63, 0x84, 0xa5, 0xc6, 0xe7, 0x08};

static void write_distinct_positions(void)
{
    for (unsigned int sprite_index = 0; sprite_index < 8; ++sprite_index)
    {
        vic_write_register((uint8_t)(sprite_index * 2), position_x[sprite_index]);
        vic_write_register((uint8_t)(sprite_index * 2 + 1), position_y[sprite_index]);
    }
}

static void test_register_roundtrip(void)
{
    const uint8_t high_masks[] = {0xa5, 0x5a, 0xff, 0x00};
    write_distinct_positions();
    for (unsigned int mask_index = 0; mask_index < sizeof(high_masks); ++mask_index)
    {
        vic_write_register(0x10, high_masks[mask_index]);
        EXPECT_EQ(vic_read_register(0x10), high_masks[mask_index]);
        for (unsigned int sprite_index = 0; sprite_index < 8; ++sprite_index)
        {
            unsigned int expected_x = position_x[sprite_index];
            if (high_masks[mask_index] & (1u << sprite_index))
                expected_x += 256;
            EXPECT_EQ(vic_read_register((uint8_t)(sprite_index * 2)), position_x[sprite_index]);
            EXPECT_EQ(vic_read_register((uint8_t)(sprite_index * 2 + 1)), position_y[sprite_index]);
            EXPECT_EQ(sprite_x((int)sprite_index), expected_x);
        }
    }
}

static void test_snapshot(void)
{
    VicDebug snapshot;
    memset(&snapshot, 0xcc, sizeof(snapshot));
    write_distinct_positions();
    host_vic_bank = 0x8000;
    vic_write_register(0x10, 0x5a);
    vic_write_register(0x11, 0xbb);
    vic_write_register(0x12, 0x45);
    vic_write_register(0x15, 0xa5);
    vic_write_register(0x16, 0x18);
    vic_write_register(0x17, 0x3c);
    vic_write_register(0x18, 0xae);
    vic_write_register(0x1a, 0x01);
    vic_write_register(0x1b, 0x96);
    vic_write_register(0x1c, 0x69);
    vic_write_register(0x1d, 0xc3);
    vic_write_register(0x20, 0xf9);
    vic_write_register(0x21, 0xf4);
    vic_debug_snapshot(&snapshot);
    EXPECT_EQ(snapshot.cr1, 0x3b);
    EXPECT_EQ(snapshot.cr2, 0x18);
    EXPECT_EQ(snapshot.mem_ptrs, 0xaf);
    EXPECT_EQ(snapshot.border, 9);
    EXPECT_EQ(snapshot.bg0, 4);
    EXPECT_EQ(snapshot.mode, 3);
    EXPECT_EQ(snapshot.sprite_en, 0xa5);
    EXPECT_EQ(snapshot.irq_en, 1);
    EXPECT_EQ(snapshot.spr_dh, 0x3c);
    EXPECT_EQ(snapshot.spr_dw, 0xc3);
    EXPECT_EQ(snapshot.spr_prio, 0x96);
    EXPECT_EQ(snapshot.spr_mc, 0x69);
    EXPECT_EQ(snapshot.spr_msbx, 0x5a);
    for (unsigned int sprite_index = 0; sprite_index < 8; ++sprite_index)
    {
        EXPECT_EQ(snapshot.spr_x[sprite_index], position_x[sprite_index]);
        EXPECT_EQ(snapshot.spr_y[sprite_index], position_y[sprite_index]);
    }
    EXPECT_EQ(snapshot.screen_mem, 0x2800);
    EXPECT_EQ(snapshot.char_mem, 0x3800);
    EXPECT_EQ(snapshot.bitmap_mem, 0x2000);
    EXPECT_EQ(snapshot.vic_base, 0x8000);
    EXPECT_EQ(snapshot.raster_irq, 0x145);
}

static void prepare_display(void)
{
    vic_write_register(0x11, 0x1b);
    vic_write_register(0x16, 0x08);
    vic_write_register(0x18, 0x14);
    vic_write_register(0x21, HOST_BACKGROUND);
}

static uint8_t *sprite_data(unsigned int sprite_index)
{
    return mem_host_ptr((uint16_t)(host_vic_bank + 0x2000 + sprite_index * 64));
}

static void prepare_sprite(unsigned int sprite_index, unsigned int position,
                           uint8_t start_y, uint8_t color)
{
    uint8_t sprite_bit = (uint8_t)(1u << sprite_index);
    uint8_t high_bits = vic_read_register(0x10);
    vic_write_register((uint8_t)(sprite_index * 2), (uint8_t)position);
    vic_write_register((uint8_t)(sprite_index * 2 + 1), start_y);
    if (position >= 256)
        high_bits |= sprite_bit;
    else
        high_bits &= (uint8_t)~sprite_bit;
    vic_write_register(0x10, high_bits);
    vic_write_register(0x15, (uint8_t)(vic_read_register(0x15) | sprite_bit));
    vic_write_register((uint8_t)(0x27 + sprite_index), color);
    mem_write_byte_no_io((uint16_t)(host_vic_bank + 0x07f8 + sprite_index),
                         (uint8_t)(0x80 + sprite_index));
}

static void test_sprite_debug_snapshot(void)
{
    for (unsigned int bank = 0; bank < 4; ++bank)
        for (unsigned int screen = 0; screen < 16; ++screen)
        {
            VicDebug snapshot;
            bool character_rom = (bank & 1u) == 0 && screen >= 4 && screen < 8;
            reset_fixture();
            host_vic_bank = (uint16_t)(bank * 0x4000u);
            vic_write_register(0x18, (uint8_t)(screen << 4));
            vic_write_register(0x25, 0xf3);
            vic_write_register(0x26, 0xe7);
            for (unsigned int sprite = 0; sprite < 8; ++sprite)
            {
                uint16_t address = (uint16_t)(host_vic_bank + screen * 0x400u +
                                               0x3f8u + sprite);
                mem_write_byte_no_io(address, (uint8_t)(0x40u + sprite));
                host_char_rom[address & 0x0fffu] = (uint8_t)(0xa0u + sprite);
                vic_write_register((uint8_t)(0x27u + sprite), (uint8_t)(0xf0u | sprite));
            }
            vic_debug_snapshot(&snapshot);
            EXPECT_EQ(snapshot.screen_mem, screen * 0x400u);
            EXPECT_EQ(snapshot.spr_shared[0], 3);
            EXPECT_EQ(snapshot.spr_shared[1], 7);
            for (unsigned int sprite = 0; sprite < 8; ++sprite)
            {
                EXPECT_EQ(snapshot.spr_ptr[sprite], (character_rom ? 0xa0u : 0x40u) + sprite);
                EXPECT_EQ(snapshot.spr_color[sprite], sprite);
            }
        }
}

static void expect_row(int lcd_y, const uint8_t *expected)
{
    const uint8_t *actual_row;
    if (lcd_y < 0 || lcd_y >= HOST_HEIGHT)
    {
        fprintf(stderr, "  invalid expected LCD row %d\n", lcd_y);
        ++assertion_failures;
        return;
    }
    actual_row = io_text_row(lcd_y + IO_CROP_Y);
    for (unsigned int column_index = 0; column_index < HOST_WIDTH; ++column_index)
    {
        uint8_t actual = actual_row[column_index];
        if (actual != expected[column_index])
        {
            fprintf(stderr, "  LCD (%u, %d) = 0x%02x, expected 0x%02x\n",
                    column_index, lcd_y, actual, expected[column_index]);
            ++assertion_failures;
            return;
        }
    }
}

static void expect_untouched_outside(int first_row, int last_row)
{
    uint8_t expected[HOST_WIDTH];
    memset(expected, HOST_BACKGROUND, sizeof(expected));
    for (int row_index = 0; row_index < HOST_HEIGHT; ++row_index)
        if (row_index < first_row || row_index > last_row)
            expect_row(row_index, expected);
}

static uint8_t bitmap_pattern(unsigned int row, unsigned int cell, unsigned int scanline)
{
    return (uint8_t)((0x81u + row * 29u + cell * 37u) ^ (scanline * 73u));
}

static void prepare_bitmap_row(unsigned int row, uint16_t bank,
                               bool multicolor, bool accelerated)
{
    uint8_t *bitmap = mem_host_ptr((uint16_t)(bank + 0x2000 + row * 320));
    uint8_t *screen = mem_host_ptr((uint16_t)(bank + 0x0400 + row * 40));
    host_vic_bank = bank;
    vic_write_register(0x11, 0x3b);
    vic_write_register(0x16, multicolor ? 0x18 : 0x08);
    vic_write_register(0x18, 0x18);
    vic_write_register(0x21, HOST_BACKGROUND);
    for (unsigned int cell = 0; cell < 40; ++cell)
    {
        screen[cell] = (uint8_t)((((row + cell + 3) & 15) << 4) |
                                ((row + cell + 9) & 15));
        mem_color_ram[row * 40 + cell] = (uint8_t)(0xb0 | ((row + cell + 5) & 15));
        for (unsigned int scanline = 0; scanline < 8; ++scanline)
            bitmap[cell * 8 + scanline] = bitmap_pattern(row, cell, scanline);
    }
    host_hires_stride = host_mc_stride = 8;
    hires_asm_ready = mc_asm_ready = accelerated;
}

static uint8_t bitmap_expected_pixel(uint8_t data, uint8_t screen, uint8_t color,
                                     bool multicolor, unsigned int pixel)
{
    if (!multicolor)
        return (data & (0x80u >> pixel)) ? (screen >> 4) | 0x20 : screen & 15;
    switch ((data >> (6 - (pixel / 2) * 2)) & 3)
    {
        case 0: return HOST_BACKGROUND;
        case 1: return screen >> 4;
        case 2: return (screen & 15) | 0x20;
        default: return (color & 15) | 0x20;
    }
}

static void expected_bitmap_row(uint8_t *expected, unsigned int row,
                                unsigned int scanline, bool multicolor)
{
    for (unsigned int cell = 0; cell < 40; ++cell)
    {
        uint8_t screen = (uint8_t)((((row + cell + 3) & 15) << 4) |
                                    ((row + cell + 9) & 15));
        uint8_t color = (uint8_t)((row + cell + 5) & 15);
        uint8_t data = bitmap_pattern(row, cell, scanline);
        for (unsigned int pixel = 0; pixel < 8; ++pixel)
            expected[cell * 8 + pixel] = bitmap_expected_pixel(data, screen, color,
                                                              multicolor, pixel);
    }
}

static void test_bitmap_raster_addressing(void)
{
    static const uint16_t banks[] = {0, 0x4000, 0x8000, 0xc000};
    static const unsigned int rows[] = {0, 12, 24};
    uint8_t expected[HOST_WIDTH];
    for (unsigned int mode = 0; mode < 2; ++mode)
        for (unsigned int accelerated = 0; accelerated < 2; ++accelerated)
            for (unsigned int bank_index = 0; bank_index < 4; ++bank_index)
                for (unsigned int row_index = 0; row_index < 3; ++row_index)
                    for (unsigned int scanline = 0; scanline < 8; ++scanline)
                    {
                        unsigned int row = rows[row_index];
                        uint16_t bank = banks[bank_index];
                        reset_fixture();
                        prepare_bitmap_row(row, bank, mode != 0, accelerated != 0);
                        expected_bitmap_row(expected, row, scanline, mode != 0);
                        cur_line = 51 + (int)(row * 8 + scanline);
                        memset(composed_pixels, HOST_GUARD_VALUE, sizeof(composed_pixels));
                        process_line(cur_line);
                        expect_row(20 + (int)(row * 8 + scanline), expected);
                        expect_untouched_outside(20 + (int)(row * 8 + scanline),
                                                 20 + (int)(row * 8 + scanline));
                        EXPECT_EQ(composed_count, 0);
                        EXPECT_EQ(host_bitmap_calls[mode], accelerated);
                        EXPECT_EQ(host_bitmap_calls[1 - mode], 0);
                        if (accelerated)
                        {
                            const BitmapState *state = &host_bitmap_state[mode];
                            EXPECT_EQ(state->destination == composed_rows[0], true);
                            EXPECT_EQ(state->bitmap == mem_host_ptr((uint16_t)
                                      (bank + 0x2000 + row * 320 + scanline)), true);
                            EXPECT_EQ(state->screen == mem_host_ptr((uint16_t)
                                      (bank + 0x0400 + row * 40)), true);
                            EXPECT_EQ(state->cells, 40);
                            if (mode != 0)
                            {
                                EXPECT_EQ(state->colors == mem_color_ram + row * 40, true);
                                EXPECT_EQ(state->background, HOST_BACKGROUND);
                            }
                        }
                        check_framebuffer_guards();
                    }
}

static void test_bitmap_sprite_composition(void)
{
    uint8_t expected[HOST_WIDTH];
    uint8_t previous[HOST_WIDTH];
    memset(previous, HOST_BACKGROUND, sizeof(previous));
    for (unsigned int mode = 0; mode < 2; ++mode)
        for (unsigned int accelerated = 0; accelerated < 2; ++accelerated)
            for (unsigned int behind = 0; behind < 2; ++behind)
                for (unsigned int scanline = 0; scanline < 8; ++scanline)
                {
                    bool background_collision = false;
                    int frame_y = 42 + (int)scanline;
                    reset_fixture();
                    prepare_bitmap_row(0, 0x4000, mode != 0, accelerated != 0);
                    prepare_sprite(5, 24, (uint8_t)(50 + scanline), 10);
                    vic_write_register(0x1b, behind ? 0x20 : 0);
                    memset(sprite_data(5), 0xff, 3);
                    expected_bitmap_row(expected, 0, scanline, mode != 0);
                    memset(composed_pixels, HOST_GUARD_VALUE, sizeof(composed_pixels));
                    begin_row_composition(frame_y, 1);
                    cur_line = 51 + (int)scanline;
                    draw_raster_bitmap_mode();
                    expect_row(20 + (int)scanline, previous);
                    expect_bytes_equal(composed_row(frame_y), expected, HOST_WIDTH,
                                       "bitmap before sprite composition");
                    EXPECT_EQ(host_bitmap_calls[mode], accelerated);
                    EXPECT_EQ(host_bitmap_calls[1 - mode], 0);

                    sprite_tick(cur_line, true);
                    for (unsigned int pixel = 0; pixel < 24; ++pixel)
                    {
                        uint8_t foreground = expected[pixel] & 0x20;
                        background_collision |= foreground != 0;
                        if (!behind || !foreground)
                            expected[pixel] = (uint8_t)(10 | 0x10 | foreground);
                    }
                    expect_row(20 + (int)scanline, previous);
                    expect_bytes_equal(composed_row(frame_y), expected, HOST_WIDTH,
                                       "bitmap with sprite composition");
                    EXPECT_EQ(sprite_coll_ss, 0);
                    EXPECT_EQ(sprite_coll_sb, background_collision ? 0x20 : 0);
                    commit_row_composition();
                    EXPECT_EQ(composed_count, 0);
                    expect_row(20 + (int)scanline, expected);
                    expect_untouched_outside(20 + (int)scanline, 20 + (int)scanline);
                    check_framebuffer_guards();
                }
}

static void check_hires_position(unsigned int position, unsigned int scale)
{
    const unsigned int opaque_offsets[] = {0, 7, 9, 14, 23};
    uint8_t expected[HOST_WIDTH];
    uint8_t *source = sprite_data(5);
    prepare_display();
    prepare_sprite(5, position, 50, 9);
    vic_write_register(0x1d, scale == 2 ? 0x20 : 0);
    source[0] = 0x81;
    source[1] = 0x42;
    source[2] = 0x01;
    sprite_tick(50, true);
    EXPECT_EQ(spr_row[5], 0xff);
    expect_untouched_outside(1, 0);
    sprite_tick(51, true);
    memset(expected, HOST_BACKGROUND, sizeof(expected));
    for (size_t offset_index = 0;
         offset_index < sizeof(opaque_offsets) / sizeof(opaque_offsets[0]); ++offset_index)
        for (unsigned int repeat = 0; repeat < scale; ++repeat)
            expected[position - 24 + opaque_offsets[offset_index] * scale + repeat] =
                9 | IO_TAG_SPRITE;
    expect_row(20, expected);
    expect_untouched_outside(20, 20);
    EXPECT_EQ(spr_row[5], 1);
}

static void test_hires_position(void)
{
    EXPECT_EQ(IO_CROP_X, 43);
    EXPECT_EQ(IO_CROP_Y, 22);
    EXPECT_EQ(VIC_FIRST_VISIBLE_LINE, 9);
    check_hires_position(24, 1);
}

static void test_hires_high_x(void)
{
    check_hires_position(300, 1);
    EXPECT_EQ(vic_read_register(0x0a), 44);
    EXPECT_EQ(vic_read_register(0x10), 0x20);
}

static void test_hires_x_expand(void)
{
    check_hires_position(24, 2);
}

static void check_multicolor_position(unsigned int position, unsigned int scale)
{
    const uint8_t pair_pixels[12] = {
        3 | IO_TAG_SPRITE, 9 | IO_TAG_SPRITE, 12 | IO_TAG_SPRITE, HOST_BACKGROUND,
        9 | IO_TAG_SPRITE, 3 | IO_TAG_SPRITE, HOST_BACKGROUND, HOST_BACKGROUND,
        HOST_BACKGROUND, HOST_BACKGROUND, HOST_BACKGROUND, 12 | IO_TAG_SPRITE
    };
    uint8_t expected[HOST_WIDTH];
    uint8_t *source = sprite_data(5);
    prepare_display();
    prepare_sprite(5, position, 50, 9);
    vic_write_register(0x1c, 0x20);
    vic_write_register(0x1d, scale == 2 ? 0x20 : 0);
    vic_write_register(0x25, 3);
    vic_write_register(0x26, 12);
    source[0] = 0x6c;
    source[1] = 0x90;
    source[2] = 0x03;
    sprite_tick(51, true);
    memset(expected, HOST_BACKGROUND, sizeof(expected));
    for (unsigned int pair_index = 0; pair_index < 12; ++pair_index)
        for (unsigned int repeat = 0; repeat < 2 * scale; ++repeat)
            expected[position - 24 + pair_index * 2 * scale + repeat] = pair_pixels[pair_index];
    expect_row(20, expected);
    expect_untouched_outside(20, 20);
    EXPECT_EQ(spr_row[5], 1);
}

static void test_multicolor_position(void)
{
    check_multicolor_position(24, 1);
}

static void test_multicolor_high_x(void)
{
    check_multicolor_position(300, 1);
}

static void test_multicolor_x_expand(void)
{
    check_multicolor_position(24, 2);
}

static void test_guarded_clipping(void)
{
    const unsigned int positions[] = {0, 23, 24, 342, 343, 511};
    uint8_t expected[HOST_WIDTH];
    for (unsigned int multicolor = 0; multicolor < 2; ++multicolor)
    {
        for (unsigned int scale = 1; scale <= 2; ++scale)
        {
            for (size_t position_index = 0;
                 position_index < sizeof(positions) / sizeof(positions[0]); ++position_index)
            {
                int left_edge = (int)positions[position_index] - 24;
                reset_fixture();
                prepare_display();
                prepare_sprite(5, positions[position_index], 50, 9);
                vic_write_register(0x1c, multicolor ? 0x20 : 0);
                vic_write_register(0x1d, scale == 2 ? 0x20 : 0);
                vic_write_register(0x26, 9);
                memset(sprite_data(5), 0xff, 3);
                sprite_tick(51, true);
                memset(expected, HOST_BACKGROUND, sizeof(expected));
                for (int column_index = 0; column_index < HOST_WIDTH; ++column_index)
                    if (column_index >= left_edge && column_index < left_edge + (int)(24 * scale))
                        expected[column_index] = 9 | IO_TAG_SPRITE;
                expect_row(20, expected);
                expect_untouched_outside(20, 20);
                check_framebuffer_guards();
            }
        }
    }
}

static void test_sprite_border_rows(void)
{
    const struct
    {
        int raster;
        bool visible_25_rows;
        bool visible_24_rows;
    } cases[] = {
        {30, false, false}, {50, false, false}, {51, true, false},
        {52, true, false}, {53, true, true}, {246, true, true},
        {247, true, false}, {250, true, false}, {251, false, false},
        {271, false, false}
    };
    uint8_t expected[HOST_WIDTH];
    for (unsigned int multicolor = 0; multicolor < 2; ++multicolor)
    {
        for (unsigned int row_mode = 0; row_mode < 2; ++row_mode)
        {
            for (size_t case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index)
            {
                bool visible = row_mode ? cases[case_index].visible_24_rows
                                        : cases[case_index].visible_25_rows;
                int lcd_y = cases[case_index].raster - 31;
                reset_fixture();
                prepare_display();
                vic_write_register(0x11, row_mode ? 0x13 : 0x1b);
                prepare_sprite(5, 24, (uint8_t)(cases[case_index].raster - 1), 9);
                vic_write_register(0x1c, multicolor ? 0x20 : 0);
                vic_write_register(0x26, 9);
                memset(sprite_data(5), 0xff, 3);
                sprite_tick(cases[case_index].raster, true);
                if (visible)
                {
                    memset(expected, HOST_BACKGROUND, sizeof(expected));
                    memset(expected, 9 | IO_TAG_SPRITE, 24);
                    expect_row(lcd_y, expected);
                    expect_untouched_outside(lcd_y, lcd_y);
                }
                else
                    expect_untouched_outside(1, 0);
                check_framebuffer_guards();
            }
        }
    }
}

static void prepare_row_pattern(unsigned int sprite_index)
{
    uint8_t *source = sprite_data(sprite_index);
    for (unsigned int row_index = 0; row_index < 21; ++row_index)
        source[row_index * 3 + row_index / 8] = (uint8_t)(0x80u >> (row_index % 8));
}

static void expect_pattern_row(int raster, unsigned int source_row)
{
    uint8_t expected[HOST_WIDTH];
    memset(expected, HOST_BACKGROUND, sizeof(expected));
    expected[source_row] = 10 | IO_TAG_SPRITE;
    expect_row(raster - 31, expected);
}

static void test_sprite_21_rows(void)
{
    prepare_display();
    prepare_sprite(5, 24, 64, 10);
    prepare_row_pattern(5);
    sprite_tick(64, true);
    EXPECT_EQ(spr_row[5], 0xff);
    for (unsigned int row_index = 0; row_index < 21; ++row_index)
    {
        int raster = 65 + (int)row_index;
        sprite_tick(raster, true);
        expect_pattern_row(raster, row_index);
        EXPECT_EQ(spr_row[5], row_index == 20 ? 0xff : row_index + 1);
    }
    sprite_tick(86, true);
    EXPECT_EQ(spr_row[5], 0xff);
    expect_untouched_outside(34, 54);
}

static void advance_to_raster(int target_raster)
{
    for (unsigned int step_count = 0;
         step_count < VIC_SCREEN_LINES && get_raster_counter() < target_raster; ++step_count)
    {
        cpu.cycles = vic_raster_due;
        if (!vic_emulate())
        {
            fprintf(stderr, "  VIC stopped before raster %d\n", target_raster);
            ++assertion_failures;
            return;
        }
    }
    EXPECT_EQ(get_raster_counter(), target_raster);
}

static void check_sprite_ram_raster_order(bool change_pointer)
{
    uint8_t expected[HOST_WIDTH];
    uint8_t *source = sprite_data(5);
    uint8_t *replacement = change_pointer ? sprite_data(6) : source;

    prepare_display();
    prepare_sprite(5, 24, 64, 10);
    source[0] = 0x80;
    source[3] = 0x40;
    advance_to_raster(66);

    replacement[0] = 0x20;
    replacement[3] = 0x10;
    if (change_pointer)
        mem_write_byte_no_io(0x07fd, 0x86);
    advance_to_raster(67);
    vic_render_catchup();

    memset(expected, HOST_BACKGROUND, sizeof(expected));
    expected[0] = 10 | IO_TAG_SPRITE;
    expect_row(34, expected);
    expected[0] = HOST_BACKGROUND;
    expected[3] = 10 | IO_TAG_SPRITE;
    expect_row(35, expected);
}

static void test_sprite_pointer_raster_order(void)
{
    check_sprite_ram_raster_order(true);
}

static void test_sprite_data_raster_order(void)
{
    check_sprite_ram_raster_order(false);
}

#if VIC_TEST_CHAR_INTEGRATION
static void test_sprite_inactive_batching(void)
{
    prepare_display();
    prepare_sprite(5, 24, 70, 10);
    char_asm_ready = true;
    sprite_data(5)[0] = 0x80;

    advance_to_raster(59);
    vic_render_catchup();
    EXPECT_EQ(host_char_kernel_calls, 1);
    EXPECT_EQ(host_char_kernel_state.rows, 8);
    EXPECT_EQ(spr_row[5], 0xff);

    advance_to_raster(72);
    EXPECT_EQ(spr_row[5], 1);
    EXPECT_EQ(pending_line, 72);
    EXPECT_EQ(io_text_row(71 - VIC_FIRST_VISIBLE_LINE)[0], 10 | IO_TAG_SPRITE);

    advance_to_raster(99);
    vic_render_catchup();
    EXPECT_EQ(spr_row[5], 0xff);
    host_char_kernel_calls = 0;
    advance_to_raster(107);
    vic_render_catchup();
    EXPECT_EQ(host_char_kernel_calls, 1);
    EXPECT_EQ(host_char_kernel_state.rows, 8);
}
#endif

static void test_sprite_y_rearm(void)
{
    prepare_display();
    prepare_sprite(5, 24, 96, 10);
    prepare_row_pattern(5);
    advance_to_raster(110);
    vic_write_register(0x0b, 117);
    EXPECT_EQ(pending_line, 110);
    EXPECT_EQ(spr_row[5], 13);
    advance_to_raster(139);
    vic_render_catchup();
    for (unsigned int row_index = 0; row_index < 42; ++row_index)
        expect_pattern_row(97 + (int)row_index, row_index % 21);
    EXPECT_EQ(spr_row[5], 0xff);
    EXPECT_EQ(vic_read_register(0x0b), 117);
    expect_untouched_outside(66, 107);
}

static void test_sprite_undrawn_state(void)
{
    prepare_display();
    prepare_sprite(5, 24, 96, 10);
    prepare_row_pattern(5);
    for (unsigned int row_index = 0; row_index < 42; ++row_index)
    {
        if (row_index == 13)
            vic_write_register(0x0b, 117);
        sprite_tick(97 + (int)row_index, false);
        EXPECT_EQ(spr_row[5], row_index % 21 == 20 ? 0xff : row_index % 21 + 1);
    }
    expect_untouched_outside(1, 0);
}

static void test_sprite_y_expand(void)
{
    prepare_display();
    prepare_sprite(5, 24, 64, 10);
    prepare_row_pattern(5);
    vic_write_register(0x17, 0x20);
    for (unsigned int row_index = 0; row_index < 42; ++row_index)
    {
        int raster = 65 + (int)row_index;
        sprite_tick(raster, true);
        expect_pattern_row(raster, row_index / 2);
        EXPECT_EQ(spr_row[5], row_index == 41 ? 0xff : (row_index + 1) / 2);
    }
    sprite_tick(107, true);
    EXPECT_EQ(spr_row[5], 0xff);
    expect_untouched_outside(34, 75);
}

static void prepare_glyph(uint8_t pixels, uint8_t color)
{
    mem_write_byte_no_io((uint16_t)(host_vic_bank + 0x0400), 1);
    memset(host_char_rom + 8, pixels, 8);
    mem_color_ram[0] = color;
}

static void draw_glyph_line(int raster)
{
    cur_line = raster;
    draw_raster_char_mode();
}

static void test_transparent_color_zero(void)
{
    uint8_t expected[HOST_WIDTH];
    for (unsigned int multicolor = 0; multicolor < 2; ++multicolor)
    {
        reset_fixture();
        prepare_display();
        prepare_sprite(5, 24, 50, 0);
        vic_write_register(0x1c, multicolor ? 0x20 : 0);
        sprite_data(5)[0] = multicolor ? 0x1b : 0x3f;
        prepare_glyph(0xc0, 0);
        draw_glyph_line(51);
        sprite_tick(51, true);
        memset(expected, HOST_BACKGROUND, sizeof(expected));
        memset(expected, IO_TAG_FOREGROUND, 2);
        memset(expected + 2, IO_TAG_SPRITE, 6);
        expect_row(20, expected);
        expect_untouched_outside(20, 20);
        EXPECT_EQ(vic_read_register(0x1e), 0);
        EXPECT_EQ(vic_read_register(0x1f), 0);
        check_framebuffer_guards();
    }
}

static void test_foreground_color_zero(void)
{
    uint8_t expected[HOST_WIDTH];
    prepare_display();
    prepare_sprite(5, 24, 50, 9);
    vic_write_register(0x1b, 0x20);
    sprite_data(5)[0] = 0xc0;
    prepare_glyph(0x80, 0);
    draw_glyph_line(51);
    sprite_tick(51, true);
    memset(expected, HOST_BACKGROUND, sizeof(expected));
    expected[0] = IO_TAG_FOREGROUND;
    expected[1] = 9 | IO_TAG_SPRITE;
    expect_row(20, expected);
    EXPECT_EQ(vic_read_register(0x1f), 0x20);
    EXPECT_EQ(vic_read_register(0x1e), 0);
}

static void test_foreground_multicolor_priority(void)
{
    uint8_t expected[HOST_WIDTH];
    prepare_display();
    vic_write_register(0x16, 0x18);
    vic_write_register(0x22, 5);
    vic_write_register(0x23, 0);
    prepare_sprite(5, 24, 50, 9);
    vic_write_register(0x1b, 0x20);
    sprite_data(5)[0] = 0xff;
    prepare_glyph(0x1b, 8);
    draw_glyph_line(51);
    sprite_tick(51, true);
    memset(expected, HOST_BACKGROUND, sizeof(expected));
    memset(expected, 9 | IO_TAG_SPRITE, 4);
    memset(expected + 4, IO_TAG_FOREGROUND, 4);
    expect_row(20, expected);
    EXPECT_EQ(vic_read_register(0x1f), 0x20);
}

static void test_sprite_pixel_runs(void)
{
    static const int positions[] = {
        0, IO_CROP_X - 4, IO_CROP_X - 2, IO_CROP_X - 1,
        IO_CROP_X, IO_CROP_X + 40, IO_CROP_X + 317,
        IO_CROP_X + 319, IO_CROP_X + 320, IO_CROP_X + 511
    };
    static const uint8_t widths[] = {0, 1, 2, 4};
    uint8_t actual[HOST_WIDTH];
    uint8_t expected[HOST_WIDTH];
    uint8_t expected_masks[sizeof(sprite_line_mask)];

    for (unsigned int sprite_index = 0; sprite_index < 8; ++sprite_index)
    for (unsigned int behind = 0; behind < 2; ++behind)
    for (unsigned int color = 0; color < 16; ++color)
    for (unsigned int position = 0; position < sizeof(positions) / sizeof(positions[0]); ++position)
    for (unsigned int width_index = 0; width_index < sizeof(widths); ++width_index)
    {
        uint8_t bit = (uint8_t)(1u << sprite_index);
        uint8_t expected_ss = 0x02;
        uint8_t expected_sb = 0x04;
        for (unsigned int pixel = 0; pixel < HOST_WIDTH; ++pixel)
            actual[pixel] = expected[pixel] = (uint8_t)((pixel * 7u) & 0x3f);
        for (unsigned int pixel = 0; pixel < sizeof(sprite_line_mask); ++pixel)
            sprite_line_mask[pixel] = expected_masks[pixel] =
                pixel % 3u ? 0 : (uint8_t)(1u << (pixel % 8u));
        for (unsigned int pixel = 0; pixel < widths[width_index]; ++pixel)
        {
            int frame_x = positions[position] + (int)pixel;
            int output_x = frame_x - IO_CROP_X;
            if (output_x >= 0 && output_x < HOST_WIDTH)
            {
                uint8_t previous = expected_masks[frame_x];
                uint8_t under = expected[output_x];
                if (previous)
                    expected_ss |= (uint8_t)(previous | bit);
                expected_masks[frame_x] = (uint8_t)(previous | bit);
                if (under & IO_TAG_FOREGROUND)
                    expected_sb |= bit;
                if (!previous && !(behind && (under & IO_TAG_FOREGROUND)))
                    expected[output_x] = (uint8_t)(color | IO_TAG_SPRITE | (under & IO_TAG_FOREGROUND));
            }
        }
        sprite_coll_ss = 0x02;
        sprite_coll_sb = 0x04;
        if (widths[width_index] == 1)
            sprite_pixel_at(bit, behind != 0, actual, positions[position], (uint8_t)color);
        else
            sprite_pixel_run(bit, behind != 0, actual, positions[position],
                             (uint8_t)color, widths[width_index]);
        expect_bytes_equal(actual, expected, sizeof(actual), "sprite pixel run");
        expect_bytes_equal(sprite_line_mask, expected_masks, sizeof(expected_masks), "sprite run coverage");
        EXPECT_EQ(sprite_coll_ss, expected_ss);
        EXPECT_EQ(sprite_coll_sb, expected_sb);
    }
}

static void test_priority_lowest_number(void)
{
    uint8_t expected[HOST_WIDTH];
    prepare_display();
    for (unsigned int sprite_index = 0; sprite_index < 8; ++sprite_index)
    {
        prepare_sprite(sprite_index, 24, 50, (uint8_t)(sprite_index + 1));
        sprite_data(sprite_index)[0] = 0x80;
    }
    sprite_tick(51, true);
    memset(expected, HOST_BACKGROUND, sizeof(expected));
    expected[0] = 1 | IO_TAG_SPRITE;
    expect_row(20, expected);
    EXPECT_EQ(vic_read_register(0x1e), 0xff);
    EXPECT_EQ(vic_read_register(0x1f), 0);
}

static void test_priority_transparent_winner(void)
{
    uint8_t expected[HOST_WIDTH];
    prepare_display();
    prepare_sprite(0, 24, 50, 2);
    prepare_sprite(1, 24, 50, 5);
    sprite_data(0)[0] = 0x40;
    sprite_data(1)[0] = 0xc0;
    sprite_tick(51, true);
    memset(expected, HOST_BACKGROUND, sizeof(expected));
    expected[0] = 5 | IO_TAG_SPRITE;
    expected[1] = 2 | IO_TAG_SPRITE;
    expect_row(20, expected);
    EXPECT_EQ(vic_read_register(0x1e), 3);
    EXPECT_EQ(vic_read_register(0x1f), 0);
}

static void render_foreground_overlap(uint8_t priority)
{
    prepare_display();
    prepare_sprite(0, 24, 50, 2);
    prepare_sprite(1, 24, 50, 5);
    vic_write_register(0x1b, priority);
    sprite_data(0)[0] = 0xc0;
    sprite_data(1)[0] = 0xc0;
    prepare_glyph(0x80, 0);
    draw_glyph_line(51);
    sprite_tick(51, true);
}

static void test_priority_behind_foreground(void)
{
    uint8_t expected[HOST_WIDTH];
    render_foreground_overlap(1);
    memset(expected, HOST_BACKGROUND, sizeof(expected));
    expected[0] = IO_TAG_FOREGROUND;
    expected[1] = 2 | IO_TAG_SPRITE;
    expect_row(20, expected);
    EXPECT_EQ(vic_read_register(0x1e), 3);
    EXPECT_EQ(vic_read_register(0x1f), 3);
}

static void test_composed_sprite_presentation(void)
{
    uint8_t expected[HOST_WIDTH];
    prepare_display();
    prepare_sprite(5, 24, 50, 10);
    sprite_data(5)[0] = 0x80;
    prepare_glyph(0x80, 0);
    memset(expected, HOST_BACKGROUND, sizeof(expected));

    memset(composed_pixels, HOST_GUARD_VALUE, sizeof(composed_pixels));
    begin_row_composition(42, 1);
    EXPECT_EQ(composed_row(42)[0], HOST_GUARD_VALUE);
    EXPECT_EQ(composed_row(42)[HOST_WIDTH - 1], HOST_GUARD_VALUE);
    draw_glyph_line(51);
    expect_row(20, expected);
    EXPECT_EQ(composed_row(42)[0], IO_TAG_FOREGROUND);
    sprite_tick(51, true);
    expect_row(20, expected);
    EXPECT_EQ(composed_row(42)[0], 10 | IO_TAG_SPRITE | IO_TAG_FOREGROUND);

    commit_row_composition();
    expected[0] = 10 | IO_TAG_SPRITE | IO_TAG_FOREGROUND;
    expect_row(20, expected);
    EXPECT_EQ(composed_count, 0);
    EXPECT_EQ(composed_row(42) == io_text_row(42), true);
}

static void test_composed_character_presentation(void)
{
    uint8_t expected[HOST_WIDTH];
    prepare_display();
    prepare_glyph(0x80, 4);
    memset(expected, HOST_BACKGROUND, sizeof(expected));
#if VIC_TEST_CHAR_INTEGRATION
    char_asm_ready = true;
#endif
    memset(composed_pixels, HOST_GUARD_VALUE, sizeof(composed_pixels));
    begin_row_composition(42, 8);
    EXPECT_EQ(composed_row(42)[0], HOST_GUARD_VALUE);
    EXPECT_EQ(composed_row(49)[HOST_WIDTH - 1], HOST_GUARD_VALUE);
    draw_char_row(51);
#if VIC_TEST_CHAR_INTEGRATION
    EXPECT_EQ(host_char_kernel_calls, 1);
#endif
    for (unsigned int row_index = 0; row_index < 8; ++row_index)
    {
        expect_row(20 + (int)row_index, expected);
        EXPECT_EQ(composed_row(42 + (int)row_index)[0], 4 | IO_TAG_FOREGROUND);
    }
    commit_row_composition();
    expected[0] = 4 | IO_TAG_FOREGROUND;
    for (unsigned int row_index = 0; row_index < 8; ++row_index)
        expect_row(20 + (int)row_index, expected);
}

static void test_foreground_tag_preserved(void)
{
    uint8_t expected[HOST_WIDTH];
    render_foreground_overlap(2);
    memset(expected, HOST_BACKGROUND, sizeof(expected));
    expected[0] = 2 | IO_TAG_SPRITE | IO_TAG_FOREGROUND;
    expected[1] = 2 | IO_TAG_SPRITE;
    expect_row(20, expected);
    EXPECT_EQ(vic_read_register(0x1e), 3);
    EXPECT_EQ(vic_read_register(0x1f), 3);
}

static void test_collision_all_foreground(void)
{
    const unsigned int sprites[] = {0, 3, 7};
    prepare_display();
    for (size_t sprite_index = 0; sprite_index < sizeof(sprites) / sizeof(sprites[0]); ++sprite_index)
    {
        prepare_sprite(sprites[sprite_index], 24, 50, 2);
        sprite_data(sprites[sprite_index])[0] = 0x80;
    }
    prepare_glyph(0x80, 0);
    draw_glyph_line(51);
    sprite_tick(51, true);
    EXPECT_EQ(vic_read_register(0x1e), 0x89);
    EXPECT_EQ(vic_read_register(0x1f), 0x89);
}

static void test_collision_clear_on_read(void)
{
    prepare_display();
    prepare_sprite(5, 24, 50, 2);
    prepare_sprite(6, 24, 50, 5);
    vic_write_register(0x1b, 0x60);
    sprite_data(5)[0] = sprite_data(5)[3] = 0xc0;
    sprite_data(6)[0] = sprite_data(6)[3] = 0xc0;
    prepare_glyph(0xc0, 0);
    draw_glyph_line(51);
    sprite_tick(51, true);
    vic_write_register(0x1e, 0xff);
    vic_write_register(0x1f, 0xff);
    EXPECT_EQ(vic_read_register(0x1e), 0x60);
    EXPECT_EQ(vic_read_register(0x1e), 0);
    EXPECT_EQ(vic_read_register(0x1f), 0x60);
    EXPECT_EQ(vic_read_register(0x1f), 0);
    draw_glyph_line(52);
    sprite_tick(52, true);
    EXPECT_EQ(vic_read_register(0x1f), 0x60);
    EXPECT_EQ(vic_read_register(0x1f), 0);
    EXPECT_EQ(vic_read_register(0x1e), 0x60);
    EXPECT_EQ(vic_read_register(0x1e), 0);
}

static void test_collision_read_catchup(void)
{
    prepare_display();
    prepare_sprite(0, 24, 50, 2);
    prepare_sprite(1, 24, 50, 5);
    vic_write_register(0x1b, 3);
    sprite_data(0)[0] = sprite_data(1)[0] = 0x80;
    prepare_glyph(0x80, 0);
    advance_to_raster(51);
    EXPECT_EQ(sprite_coll_ss, 0);
    EXPECT_EQ(sprite_coll_sb, 0);
    advance_to_raster(52);
    EXPECT_EQ(sprite_coll_ss, 3);
    EXPECT_EQ(sprite_coll_sb, 3);
    EXPECT_EQ(vic_read_register(0x1e), 3);
    EXPECT_EQ(pending_line, 52);
    EXPECT_EQ(vic_read_register(0x1e), 0);
    EXPECT_EQ(vic_read_register(0x1f), 3);
    EXPECT_EQ(vic_read_register(0x1f), 0);
}

static void test_pal_raster_timing(void)
{
    EXPECT_EQ(VIC_SCREEN_LINES, 312);
    EXPECT_EQ(VIC_LINE_CYCLES, 63);
    for (unsigned int raster = 0; raster < 312; ++raster)
    {
        unsigned int deadline = vic_raster_due;
        cpu.cycles = deadline - 1;
        EXPECT_EQ(vic_emulate(), true);
        EXPECT_EQ(get_raster_counter(), raster);
        EXPECT_EQ(vic_raster_due, deadline);
        EXPECT_EQ(vic_frames(), 0);
        cpu.cycles = deadline;
        EXPECT_EQ(vic_emulate(), true);
        EXPECT_EQ(get_raster_counter(), (raster + 1) % 312);
    }
    EXPECT_EQ(vic_frames(), 1);
    EXPECT_EQ(host_refresh_count, 1);
}

static void test_pal_every_frame(void)
{
    uint8_t expected[HOST_WIDTH];
    prepare_display();
    for (unsigned int frame_index = 0; frame_index < 3; ++frame_index)
    {
        uint8_t color = (uint8_t)(frame_index + 2);
        vic_write_register(0x21, color);
        for (unsigned int raster = 0; raster < 312; ++raster)
        {
            cpu.cycles = vic_raster_due;
            EXPECT_EQ(vic_emulate(), true);
        }
        EXPECT_EQ(vic_frames(), frame_index + 1);
        EXPECT_EQ(host_refresh_count, frame_index + 1);
        memset(expected, color, sizeof(expected));
        for (int lcd_y = 20; lcd_y < 220; ++lcd_y)
        {
            unsigned int prior_failures = assertion_failures;
            expect_row(lcd_y, expected);
            if (assertion_failures != prior_failures)
                break;
        }
        expect_untouched_outside(20, 219);
    }
}

static void prepare_char_display(unsigned int mode, unsigned int scroll)
{
    CharSource source;
    prepare_display();
    vic_write_register(0x11, mode == GM_EXT_BG ? 0x5b : 0x1b);
    vic_write_register(0x16, (uint8_t)(0x08 | scroll | (mode == GM_MC_CHAR ? 0x10 : 0)));
    vic_write_register(0x22, 1);
    vic_write_register(0x23, 12);
    vic_write_register(0x24, 9);
    prepare_char_source(&source);
    memcpy(mem_host_ptr(0x0400), source.screen + HOST_GUARD_BYTES, 40);
    memcpy(mem_color_ram, source.colors + HOST_GUARD_BYTES, 40);
    memcpy(host_char_rom, source.glyphs + HOST_GUARD_BYTES, 2048);
    mem_host_ptr(0x0400)[39] = 255;
    mem_color_ram[39] = 0xaf;
    for (unsigned int row = 0; row < 8; ++row)
    {
        uint8_t data = (uint8_t)(0xe4 ^ ((row * 19) & 0x3f));
        host_char_rom[255 * 8 + row] = data;
        host_char_rom[63 * 8 + row] = data;
    }
}

static void reference_char_display_row(uint8_t expected[HOST_WIDTH],
                                       unsigned int mode, unsigned int scroll,
                                       unsigned int glyph_row)
{
    const uint8_t backgrounds[4] = {6, 1, 12, 9};
    const uint8_t *screen = mem_host_ptr(0x0400);
    memset(expected, HOST_BACKGROUND, HOST_WIDTH);
    for (unsigned int cell = 0; cell < 40; ++cell)
    {
        uint8_t code = screen[cell];
        uint8_t palette[3] = {6, 1, 12};
        if (mode == GM_EXT_BG)
        {
            palette[0] = backgrounds[code >> 6];
            code &= 63;
        }
        for (unsigned int pixel = 0; pixel < 8; ++pixel)
        {
            unsigned int column = scroll + cell * 8 + pixel;
            if (column < HOST_WIDTH)
                expected[column] = char_expected_pixel(host_char_rom[code * 8 + glyph_row],
                                                       mem_color_ram[cell],
                                                       mode == GM_MC_CHAR, pixel, palette);
        }
    }
}

static void check_char_scroll_c(bool batch)
{
    const unsigned int modes[] = {GM_CHAR, GM_MC_CHAR, GM_EXT_BG};
    for (size_t mode_index = 0; mode_index < sizeof(modes) / sizeof(modes[0]); ++mode_index)
        for (unsigned int scroll = 0; scroll < 8; ++scroll)
        {
            uint8_t expected[HOST_WIDTH];
            unsigned int first_glyph_row = batch ? 0 : 3;
            unsigned int row_count = batch ? 8 : 1;
            unsigned int first_lcd_row = 20 + first_glyph_row;
            reset_fixture();
            prepare_char_display(modes[mode_index], scroll);
            if (batch)
                draw_char_row(VIC_G_FIRST_LINE);
            else
            {
                cur_line = VIC_G_FIRST_LINE + (int)first_glyph_row;
                draw_raster_char_mode();
            }
            for (unsigned int row = 0; row < row_count; ++row)
            {
                unsigned int prior_failures = assertion_failures;
                reference_char_display_row(expected, modes[mode_index], scroll,
                                           first_glyph_row + row);
                expect_row((int)(first_lcd_row + row), expected);
                if (assertion_failures != prior_failures)
                {
                    fprintf(stderr, "  character C %s: mode %u, scroll %u, glyph row %u\n",
                            batch ? "batch" : "raster", modes[mode_index], scroll,
                            first_glyph_row + row);
                    break;
                }
            }
            expect_untouched_outside((int)first_lcd_row, (int)(first_lcd_row + row_count - 1));
            check_framebuffer_guards();
        }
}

static void test_char_scroll_raster_c(void)
{
    check_char_scroll_c(false);
}

static void test_char_scroll_batch_c(void)
{
    check_char_scroll_c(true);
}

#if VIC_TEST_CHAR_INTEGRATION
static void check_char_kernel_render(bool batch)
{
    const unsigned int modes[] = {GM_CHAR, GM_MC_CHAR, GM_EXT_BG};
    for (size_t mode_index = 0; mode_index < sizeof(modes) / sizeof(modes[0]); ++mode_index)
        for (unsigned int scroll = 0; scroll < 8; ++scroll)
        {
            CharOutput output, expected;
            unsigned int first_glyph_row = batch ? 0 : 3;
            unsigned int row_count = batch ? 8 : 1;
            unsigned int mode = modes[mode_index];
            unsigned int prior_failures = assertion_failures;
            reset_fixture();
            prepare_char_display(mode, scroll);
            memset(&output, HOST_GUARD_VALUE, sizeof(output));
            memset(output.pixels, HOST_BACKGROUND, sizeof(output.pixels));
            expected = output;
            for (unsigned int row = first_glyph_row; row < first_glyph_row + row_count; ++row)
                reference_char_display_row(expected.pixels + row * HOST_WIDTH,
                                           mode, scroll, row);
            host_char_pixels = output.pixels;
            for (unsigned int row = 0; row < 8; ++row)
                EXPECT_EQ(io_text_row(VIC_G_FIRST_LINE - VIC_FIRST_VISIBLE_LINE + (int)row) ==
                          output.pixels + row * HOST_WIDTH, true);
            char_asm_ready = true;
            if (batch)
                draw_char_row(VIC_G_FIRST_LINE);
            else
            {
                cur_line = VIC_G_FIRST_LINE + (int)first_glyph_row;
                draw_raster_char_mode();
            }
            EXPECT_EQ(host_char_kernel_calls, mode == GM_EXT_BG ? 0 : 1);
            if (mode != GM_EXT_BG && host_char_kernel_calls != 0)
            {
                EXPECT_EQ(host_char_kernel_state.destination ==
                          output.pixels + first_glyph_row * HOST_WIDTH + scroll, true);
                EXPECT_EQ(host_char_kernel_state.screen == mem_host_ptr(0x0400), true);
                EXPECT_EQ(host_char_kernel_state.colors == mem_color_ram, true);
                EXPECT_EQ(host_char_kernel_state.glyphs == host_char_rom + first_glyph_row, true);
                EXPECT_EQ(host_char_kernel_state.cells, scroll ? 39 : 40);
                EXPECT_EQ(host_char_kernel_state.rows, row_count);
                EXPECT_EQ(host_char_kernel_state.multicolor != 0, mode == GM_MC_CHAR);
                expect_char_state(host_char_kernel_state);
            }
            expect_bytes_equal(&output, &expected, sizeof(output), "integrated character output/guards");
            expect_untouched_outside((int)(20 + first_glyph_row),
                                     (int)(20 + first_glyph_row + row_count - 1));
            check_framebuffer_guards();
            host_char_pixels = NULL;
            if (assertion_failures != prior_failures)
                fprintf(stderr, "  character kernel %s: mode %u, scroll %u\n",
                        batch ? "batch" : "raster", mode, scroll);
        }
}

static void test_char_kernel_raster(void)
{
    check_char_kernel_render(false);
}

static void test_char_kernel_batch(void)
{
    check_char_kernel_render(true);
}

static void test_char_kernel_row_fallback(void)
{
    const unsigned int modes[] = {GM_CHAR, GM_MC_CHAR};
    for (size_t mode_index = 0; mode_index < sizeof(modes) / sizeof(modes[0]); ++mode_index)
        for (unsigned int scroll = 0; scroll < 8; ++scroll)
        {
            uint8_t expected[HOST_WIDTH];
            reset_fixture();
            prepare_char_display(modes[mode_index], scroll);
            EXPECT_EQ((uintptr_t)io_text_row(43) - (uintptr_t)io_text_row(42),
                      sizeof(GuardedRow));
            EXPECT_EQ(sizeof(GuardedRow) != HOST_WIDTH, true);
            char_asm_ready = true;
            draw_char_row(VIC_G_FIRST_LINE);
            EXPECT_EQ(host_char_kernel_calls, 0);
            for (unsigned int row = 0; row < 8; ++row)
            {
                unsigned int prior_failures = assertion_failures;
                reference_char_display_row(expected, modes[mode_index], scroll, row);
                expect_row((int)(20 + row), expected);
                if (assertion_failures != prior_failures)
                    break;
            }
            expect_untouched_outside(20, 27);
            check_framebuffer_guards();
        }
}
#endif

typedef struct
{
    const char *name;
    void (*run)(void);
} VicTest;

static const VicTest test_cases[] = {
    {"register-roundtrip", test_register_roundtrip},
    {"snapshot", test_snapshot},
    {"sprite-debug-snapshot", test_sprite_debug_snapshot},
    {"bitmap-selftest-reject-packed", test_bitmap_selftest_reject_packed},
    {"bitmap-selftest-layout", test_bitmap_selftest_layout},
    {"bitmap-selftest-faults", test_bitmap_selftest_faults},
    {"bitmap-raster-addressing", test_bitmap_raster_addressing},
    {"bitmap-sprite-composition", test_bitmap_sprite_composition},
    {"hires-position", test_hires_position},
    {"hires-high-x", test_hires_high_x},
    {"hires-x-expand", test_hires_x_expand},
    {"mc-position", test_multicolor_position},
    {"mc-high-x", test_multicolor_high_x},
    {"mc-x-expand", test_multicolor_x_expand},
    {"guarded-clipping", test_guarded_clipping},
    {"sprite-border-rows", test_sprite_border_rows},
    {"sprite-21-rows", test_sprite_21_rows},
    {"sprite-pointer-raster-order", test_sprite_pointer_raster_order},
    {"sprite-data-raster-order", test_sprite_data_raster_order},
    {"sprite-y-rearm", test_sprite_y_rearm},
    {"sprite-undrawn-state", test_sprite_undrawn_state},
    {"sprite-y-expand", test_sprite_y_expand},
    {"transparent-color-zero", test_transparent_color_zero},
    {"foreground-color-zero", test_foreground_color_zero},
    {"foreground-mc-priority", test_foreground_multicolor_priority},
    {"sprite-pixel-runs", test_sprite_pixel_runs},
    {"priority-lowest-number", test_priority_lowest_number},
    {"priority-transparent-winner", test_priority_transparent_winner},
    {"priority-behind-foreground", test_priority_behind_foreground},
    {"composed-sprite-presentation", test_composed_sprite_presentation},
    {"composed-character-presentation", test_composed_character_presentation},
    {"foreground-tag-preserved", test_foreground_tag_preserved},
    {"collision-all-foreground", test_collision_all_foreground},
    {"collision-clear-on-read", test_collision_clear_on_read},
    {"collision-read-catchup", test_collision_read_catchup},
    {"pal-raster-timing", test_pal_raster_timing},
    {"pal-every-frame", test_pal_every_frame},
    {"char-selftest", test_char_selftest},
    {"char-tables", test_char_tables},
    {"char-color-invalidation", test_char_color_invalidation},
    {"char-mixed", test_char_mixed},
    {"char-row-span", test_char_row_span},
    {"char-cell-span", test_char_cell_span},
    {"char-live-glyphs", test_char_live_glyphs},
    {"char-invalid-counts", test_char_invalid_counts},
    {"char-scroll-raster-c", test_char_scroll_raster_c},
    {"char-scroll-batch-c", test_char_scroll_batch_c},
#if VIC_TEST_CHAR_INTEGRATION
    {"sprite-inactive-batching", test_sprite_inactive_batching},
    {"char-kernel-raster", test_char_kernel_raster},
    {"char-kernel-batch", test_char_kernel_batch},
    {"char-kernel-row-fallback", test_char_kernel_row_fallback},
#endif
};

int main(int argument_count, char **arguments)
{
    unsigned int selected_count = 0;
    unsigned int failed_count = 0;
    size_t test_count = sizeof(test_cases) / sizeof(test_cases[0]);
    if (argument_count == 2 && strcmp(arguments[1], "--list") == 0)
    {
        for (size_t test_index = 0; test_index < test_count; ++test_index)
            puts(test_cases[test_index].name);
        return EXIT_SUCCESS;
    }
    for (int argument_index = 1; argument_index < argument_count; ++argument_index)
    {
        bool known_name = false;
        for (size_t test_index = 0; test_index < test_count; ++test_index)
            known_name |= strcmp(arguments[argument_index], test_cases[test_index].name) == 0;
        if (!known_name)
        {
            fprintf(stderr, "Unknown test '%s'; use --list.\n", arguments[argument_index]);
            return 2;
        }
    }
    for (size_t test_index = 0; test_index < test_count; ++test_index)
    {
        bool selected = argument_count == 1;
        for (int argument_index = 1; argument_index < argument_count; ++argument_index)
            selected |= strcmp(arguments[argument_index], test_cases[test_index].name) == 0;
        if (!selected)
            continue;
        ++selected_count;
        assertion_failures = 0;
        reset_fixture();
        printf("RUN  %s\n", test_cases[test_index].name);
        fflush(stdout);
        test_cases[test_index].run();
        check_framebuffer_guards();
        if (assertion_failures)
            ++failed_count;
        printf("%s %s\n", assertion_failures ? "FAIL" : "PASS", test_cases[test_index].name);
    }
    printf("%u tests: %u passed, %u failed\n", selected_count,
           selected_count - failed_count, failed_count);
    return failed_count ? EXIT_FAILURE : EXIT_SUCCESS;
}