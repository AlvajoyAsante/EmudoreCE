/*
 * emudore-CE: MOS VIC-II video chip (C port of vic.cpp).
 *
 * Completed row compositions go to io.c's graphx framebuffer.
 * Stop reasons (illegal opcode / unsupported mode) are stored in globals and
 * reported by main() after the loop; no debug printing in the hot path.
 */
#include <string.h>
#include <sys/timers.h>
#include "vic.h"
#include "vblank.h"
#include "vic_char.h"
#include "perf.h"
#include "cpu.h"
#include "cia2.h"
#include "memory.h"
#include "io.h"
#include "util.h"

enum
{
    GM_CHAR,
    GM_MC_CHAR,
    GM_BITMAP,
    GM_MC_BITMAP,
    GM_EXT_BG,
    GM_ILLEGAL
};

/* sprites */
static uint8_t mx[8], my[8];
static uint8_t msbx;
static uint8_t sprite_enabled, sprite_priority, sprite_multicolor;
static uint8_t sprite_double_width, sprite_double_height;
static uint8_t sprite_shared_colors[2];
static uint8_t sprite_colors[8];
/* collision latches ($D01E/$D01F, cleared on read) */
static uint8_t sprite_coll_ss, sprite_coll_sb;
/* stateful sprite display counters (0xff = inactive; see sprite_tick) */
static uint8_t spr_row[8];
static uint8_t spr_phase[8];
/* colors */
static uint8_t border_color;
static uint8_t bgcolor[4];
/* cpu sync */
unsigned int vic_raster_due;
static unsigned int frame_count;
static unsigned int rendered_frame_count;
static VBlankPacer frame_pacer;
/* control registers */
static uint8_t cr1, cr2;
static uint8_t raster_c;
static int raster_irq;
static uint8_t irq_status, irq_enabled;
/* memory pointers */
static uint16_t screen_mem, char_mem, bitmap_mem;
static uint8_t mem_pointers;
/* current graphic mode */
static uint8_t graphic_mode;

/* Inputs to vic_hires_row.s / vic_mc_row.s. Globals avoid per-scanline C ABI
   stack setup. */
uint8_t *vic_row_dst;
const uint8_t *vic_row_bitmap;
const uint8_t *vic_row_screen;
const uint8_t *vic_row_colram;
uint8_t vic_row_bg;
uint8_t vic_row_cells;
extern void vic_expand_hires_row(void);
extern void vic_expand_mc_row(void);
static bool hires_asm_ready;
static bool mc_asm_ready;
static bool char_asm_ready;
static bool row_asm_enabled;

/* Returns a bitmask: bit0 = hires bitmap, bit1 = MC bitmap, bit2 = character
    rows. Each failing kernel independently falls back to C. */
#if defined(__clang__)
__attribute__((__minsize__))
#endif
uint8_t vic_row_asm_selftest(void)
{
    enum
    {
        GUARD_SIZE = 16,
        GUARD_VALUE = 0xa5
    };
    static uint8_t source[320];
    static uint8_t screen[40];
    static uint8_t colors[40];
    static uint8_t output[320 + 2 * GUARD_SIZE];
    unsigned int cell;
    unsigned int bit;
    unsigned int scanline;
    unsigned int guard;

    for (cell = 0; cell < 40; ++cell)
    {
        for (scanline = 0; scanline < 8; ++scanline)
            source[cell * 8 + scanline] = (uint8_t)((cell * 37u) ^
                                                    (scanline * 73u) ^ (0x80u >> scanline));
        screen[cell] = (uint8_t)((((cell + 3u) & 15u) << 4) | ((cell + 9u) & 15u));
        colors[cell] = (uint8_t)(0xa0u | ((cell + 5u) & 15u));
    }

    vic_row_dst = output + GUARD_SIZE;
    vic_row_screen = screen;
    vic_row_colram = colors;
    vic_row_bg = 6;
    vic_row_cells = 40;
    hires_asm_ready = true;
    mc_asm_ready = true;
    for (scanline = 0; scanline < 8; ++scanline)
    {
        vic_row_bitmap = source + scanline;
        memset(output, GUARD_VALUE, sizeof(output));
        vic_expand_hires_row();
        for (guard = 0; guard < GUARD_SIZE; ++guard)
            if (output[guard] != GUARD_VALUE ||
                output[GUARD_SIZE + 320 + guard] != GUARD_VALUE)
                hires_asm_ready = false;
        for (cell = 0; cell < 40 && hires_asm_ready; ++cell)
        {
            uint8_t fg = (uint8_t)((screen[cell] >> 4) | IO_TAG_FOREGROUND);
            uint8_t bg = screen[cell] & 0x0f;
            uint8_t data = source[cell * 8 + scanline];
            for (bit = 0; bit < 8; ++bit)
            {
                uint8_t expected = (data & (uint8_t)(0x80u >> bit)) ? fg : bg;
                if (output[GUARD_SIZE + cell * 8 + bit] != expected)
                {
                    hires_asm_ready = false;
                    break;
                }
            }
        }

        memset(output, GUARD_VALUE, sizeof(output));
        vic_expand_mc_row();
        for (guard = 0; guard < GUARD_SIZE; ++guard)
            if (output[guard] != GUARD_VALUE ||
                output[GUARD_SIZE + 320 + guard] != GUARD_VALUE)
                mc_asm_ready = false;
        for (cell = 0; cell < 40 && mc_asm_ready; ++cell)
        {
            uint8_t lut[4];
            uint8_t data = source[cell * 8 + scanline];
            lut[0] = 6;
            lut[1] = screen[cell] >> 4;
            lut[2] = (uint8_t)((screen[cell] & 0x0f) | IO_TAG_FOREGROUND);
            lut[3] = (uint8_t)((colors[cell] & 0x0f) | IO_TAG_FOREGROUND);
            for (bit = 0; bit < 4; ++bit)
            {
                uint8_t px = lut[(data >> (6 - bit * 2)) & 3];
                if (output[GUARD_SIZE + cell * 8 + bit * 2] != px ||
                    output[GUARD_SIZE + cell * 8 + bit * 2 + 1] != px)
                {
                    mc_asm_ready = false;
                    break;
                }
            }
        }
    }

    char_asm_ready = EMUDORE_CHAR_ASM && vic_char_selftest();
    return vic_asm_status();
}

uint8_t vic_asm_status(void)
{
    return (uint8_t)((hires_asm_ready ? 1 : 0) | (mc_asm_ready ? 2 : 0) |
                     (char_asm_ready ? 4 : 0));
}

/* Render only some frames: raster/IRQ timing still runs every line, but
   the pixel drawing (the bulk of the per-line cost) is skipped. */
static bool render_frame;
static bool render_requested; /* set by io on key change: render next frame */
static uint8_t cpu_turbo_multiplier;
static bool cpu_turbo_adaptive;
static unsigned int adaptive_frame_start_ticks;
static bool frame_skip_enabled;
static bool render_suppressed;

#ifndef TURBO_ADAPTIVE_TARGET_FPS
#define TURBO_ADAPTIVE_TARGET_FPS 6u
#endif

#ifndef NDEBUG
/* Render-path profile: 0=frame-wrap flush ticks, 1=draw_char_row ticks,
   2=rendered frames, 3=draw_char_row calls, 4=process_line calls. */
unsigned long vic_prof[5];
#endif

/* Active and starting sprite lines finish at their raster boundary because
    later RAM writes can change their pointers and data. Enabled but inactive
    sprites do not prevent batching: register changes and frame wrap flush
    pending_line onward, grouping whole glyph rows when possible. cur_line
    feeds the renderers; inlining a raster read crashes the backend below. */
static int pending_line;
static int cur_line;

static uint8_t composed_pixels[8][320];
static uint8_t *const composed_rows[8] = {
    composed_pixels[0], composed_pixels[1], composed_pixels[2], composed_pixels[3],
    composed_pixels[4], composed_pixels[5], composed_pixels[6], composed_pixels[7]};
static uint8_t *composed_destinations[8];
static int composed_first_y;
static uint8_t composed_count;

static const uint16_t row_offset_40[VIC_G_RES_Y / 8] = {
    0, 40, 80, 120, 160, 200, 240, 280, 320, 360,
    400, 440, 480, 520, 560, 600, 640, 680, 720, 760,
    800, 840, 880, 920, 960};

static const uint16_t row_offset_320[VIC_G_RES_Y / 8] = {
    0, 320, 640, 960, 1280, 1600, 1920, 2240, 2560, 2880,
    3200, 3520, 3840, 4160, 4480, 4800, 5120, 5440, 5760, 6080,
    6400, 6720, 7040, 7360, 7680};

void vic_request_render(void)
{
    render_requested = true;
}

void vic_set_cpu_turbo(uint8_t multiplier)
{
    cpu_turbo_adaptive = false;
    cpu_turbo_multiplier = multiplier > 200 ? 200 : multiplier;
    if (cpu_turbo_multiplier == 0)
        cpu_turbo_multiplier = 1;
}

void vic_set_cpu_turbo_adaptive(bool enabled)
{
    cpu_turbo_adaptive = enabled;
    adaptive_frame_start_ticks = 0;
    if (enabled)
        cpu_turbo_multiplier = 1;
}

void vic_set_render_suppressed(bool enabled)
{
    render_suppressed = enabled;
    if (enabled)
        render_frame = false;
}

void vic_set_frame_skip(bool enabled)
{
    frame_skip_enabled = enabled;
    vblank_reset(&frame_pacer, (uint32_t)timer_Get(1));
    if (!enabled)
    {
        render_frame = true;
        render_requested = true;
    }
}

bool vic_frame_skip_enabled(void)
{
    return frame_skip_enabled;
}

/* Hidden boot: suppress all rendering while the KERNAL boots and the disk
   autoload types LOAD/RUN. drive.c lifts it once the program is running;
   a frame-cap safety in vic_emulate prevents a permanently black screen. */
static bool boot_hidden;

void vic_set_boot_hidden(bool hidden)
{
    boot_hidden = hidden;
    if (hidden)
        render_frame = false;
}

/* set when vic_emulate stops on an unsupported mode; reported by main() */
uint8_t vic_bad_mode;

/* Rolling trace of sprite/control register writes with their raster line:
   diagnoses multiplexer schedules. Wraps; main() dumps the tail at exit. */
VicWriteTrace vic_wtrace[VIC_WTRACE_MAX];
uint8_t vic_wtrace_pos;

/* Cold diagnostics for main(): copy of the display configuration. */
#if defined(__clang__)
__attribute__((__minsize__))
#endif
void vic_debug_snapshot(VicDebug *d)
{
    int i;
    d->cr1 = cr1;
    d->cr2 = cr2;
    d->mem_ptrs = mem_pointers;
    d->border = border_color;
    d->bg0 = bgcolor[0];
    d->mode = graphic_mode;
    d->sprite_en = sprite_enabled;
    d->spr_dh = sprite_double_height;
    d->spr_dw = sprite_double_width;
    d->spr_prio = sprite_priority;
    d->spr_mc = sprite_multicolor;
    d->spr_msbx = msbx;
    for (i = 0; i < 8; ++i)
    {
        d->spr_x[i] = mx[i];
        d->spr_y[i] = my[i];
        d->spr_ptr[i] = mem_vic_read_byte(screen_mem + VIC_SPRITE_PTRS_OFFSET + i);
        d->spr_color[i] = sprite_colors[i];
    }
    d->spr_shared[0] = sprite_shared_colors[0];
    d->spr_shared[1] = sprite_shared_colors[1];
    d->screen_mem = screen_mem;
    d->char_mem = char_mem;
    d->bitmap_mem = bitmap_mem;
    d->raster_irq = raster_irq;
    d->irq_en = irq_enabled;
    d->vic_base = cia2_vic_base_address();
}

#if defined(__clang__)
__attribute__((__minsize__))
#endif
void vic_reset(void)
{
    int i;
    raster_irq = raster_c = 0;
    irq_enabled = irq_status = 0;
    pending_line = cur_line = 0;
    composed_count = 0;
    vic_raster_due = VIC_LINE_CYCLES;
    for (i = 0; i < 8; ++i)
        mx[i] = my[i] = sprite_colors[i] = 0;
    msbx = sprite_double_height = sprite_double_width = 0;
    sprite_enabled = sprite_priority = sprite_multicolor = 0;
    sprite_coll_ss = sprite_coll_sb = 0;
    sprite_shared_colors[0] = sprite_shared_colors[1] = 0;
    memset(spr_row, 0xff, sizeof(spr_row));
    memset(spr_phase, 0, sizeof(spr_phase));
    border_color = 0;
    bgcolor[0] = bgcolor[1] = bgcolor[2] = bgcolor[3] = 0;
    cr1 = cr2 = 0;
    frame_count = 0;
    rendered_frame_count = 0;
    vblank_reset(&frame_pacer, (uint32_t)timer_Get(1));
    render_frame = true;
    render_requested = false;
    cpu_turbo_multiplier = 1;
    cpu_turbo_adaptive = false;
    adaptive_frame_start_ticks = 0;
    frame_skip_enabled = true;
    render_suppressed = false;
    row_asm_enabled = true;
    boot_hidden = false;
    screen_mem = ADDR_SCREEN;
    char_mem = ADDR_CHARS;
    bitmap_mem = ADDR_BITMAP;
    mem_pointers = (1 << 0);
    graphic_mode = GM_CHAR;
}

unsigned int vic_frames(void)
{
    return frame_count;
}

/* ---- helpers ---------------------------------------------------------- */

static void set_raster_counter(int v)
{
    raster_c = (uint8_t)(v & 0xff);
    cr1 &= 0x7f;
    cr1 |= ((v >> 1) & 0x80);
}

/* noinline: an inlined read lets the optimizer range-narrow raster math to
   14-bit adds, which the ez80 backend cannot legalize (compiler crash). */
__attribute__((noinline)) static int get_raster_counter(void)
{
    return raster_c | ((cr1 & 0x80) << 1);
}

static bool is_screen_off(void)
{
    return (cr1 & (1 << 4)) == 0;
}

static bool is_bad_line(void)
{
    int rstr = get_raster_counter();
    return (rstr >= 0x30 && rstr <= 0xf7 &&
            (rstr & 0x7) == ((cr1 & 0x7) & 0x7));
}

static bool raster_irq_enabled_fn(void)
{
    return ISSET_BIT(irq_enabled, 0);
}

static uint8_t horizontal_scroll(void) { return cr2 & 0x7; }
static uint8_t vertical_scroll(void) { return cr1 & 0x7; }

static bool is_sprite_enabled(int n) { return ISSET_BIT(sprite_enabled, n); }
static bool is_double_width_sprite(int n) { return ISSET_BIT(sprite_double_width, n); }
static bool is_double_height_sprite(int n) { return ISSET_BIT(sprite_double_height, n); }
static bool is_multicolor_sprite(int n) { return ISSET_BIT(sprite_multicolor, n); }

static int sprite_x(int n)
{
    int x = mx[n];
    if (ISSET_BIT(msbx, n))
        x |= 1 << 8;
    return x;
}

static void begin_row_composition(int first_y, uint8_t count)
{
    uint8_t row_index;
    composed_count = 0;
    if (count == 0 || count > 8)
        return;
    for (row_index = 0; row_index < count; ++row_index)
    {
        uint8_t *destination = io_text_row(first_y + row_index);
        if (!destination)
            return;
        composed_destinations[row_index] = destination;
    }
    composed_first_y = first_y;
    composed_count = count;
}

static uint8_t *composed_row(int y)
{
    unsigned int row_index = (unsigned int)(y - composed_first_y);
    return row_index < composed_count ? composed_rows[row_index] : io_text_row(y);
}

static void commit_row_composition(void)
{
    uint8_t row_index;
    for (row_index = 0; row_index < composed_count; ++row_index)
        memcpy(composed_destinations[row_index], composed_rows[row_index], 320);
    composed_count = 0;
}

/* ---- graphics data fetch ---------------------------------------------- */

static uint16_t get_sprite_ptr(int n)
{
    return VIC_SPRITE_SIZE * mem_vic_read_byte(screen_mem + VIC_SPRITE_PTRS_OFFSET + n);
}

/* Per-line sprite coverage mask: pixels cannot store all 8 sprite numbers,
   so $D01E needs this side channel for both-bits collisions. */
static uint8_t sprite_line_mask[VIC_SPRITES_FIRST_COL + 512 + 48];
static const uint8_t sprite_pixel_masks[8] = {
    0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};

__attribute__((__always_inline__)) static inline void sprite_pixel_at(uint8_t bit, bool behind, uint8_t *row,
                                                                      int fx, uint8_t color)
{
    unsigned int idx = (unsigned int)(fx - IO_CROP_X);
    uint8_t previous;
    uint8_t under;
    if (idx >= 320)
        return;
    previous = sprite_line_mask[fx];
    sprite_line_mask[fx] = (uint8_t)(previous | bit);
    if (previous)
        sprite_coll_ss |= (uint8_t)(previous | bit);
    under = row[idx];
    if (under & IO_TAG_FOREGROUND)
    {
        sprite_coll_sb |= bit;
        if (behind)
            return;
    }
    if (!previous)
        row[idx] = (uint8_t)(color | IO_TAG_SPRITE | (under & IO_TAG_FOREGROUND));
}

__attribute__((__always_inline__)) static inline void sprite_pixel_run(uint8_t bit, bool behind, uint8_t *row,
                                                                       int fx, uint8_t color, uint8_t width)
{
    unsigned int idx;
    uint8_t *destination;
    uint8_t *coverage;

    if (fx < IO_CROP_X)
    {
        unsigned int clipped = (unsigned int)(IO_CROP_X - fx);
        if (clipped >= width)
            return;
        width -= clipped;
        fx = IO_CROP_X;
    }
    idx = (unsigned int)(fx - IO_CROP_X);
    if (!width || idx >= 320)
        return;
    if (width > 320 - idx)
        width = (uint8_t)(320 - idx);
    destination = row + idx;
    coverage = sprite_line_mask + fx;
    color |= IO_TAG_SPRITE;
    do
    {
        uint8_t previous = *coverage;
        uint8_t under = *destination & IO_TAG_FOREGROUND;
        *coverage++ = (uint8_t)(previous | bit);
        if (previous)
            sprite_coll_ss |= (uint8_t)(previous | bit);
        if (under)
            sprite_coll_sb |= bit;
        if (!previous && !(behind && under))
            *destination = (uint8_t)(color | under);
        ++destination;
    } while (--width);
}

/* ---- drawing ----------------------------------------------------------- */

static void draw_char_edge(uint8_t *destination, const uint8_t *glyph_line,
                           uint8_t screen_code, uint8_t color, uint8_t width)
{
    bool extended = graphic_mode == GM_EXT_BG;
    uint8_t glyph_code = extended ? (screen_code & 0x3f) : screen_code;
    uint8_t data = glyph_line[(uint16_t)glyph_code * 8u];
    uint8_t background = extended ? bgcolor[screen_code >> 6] : bgcolor[0];
    uint8_t foreground = (uint8_t)(color | IO_TAG_FOREGROUND);
    uint8_t pixel;

    if (graphic_mode == GM_MC_CHAR && (color & 8))
    {
        uint8_t palette[4] = {
            bgcolor[0], bgcolor[1],
            (uint8_t)(bgcolor[2] | IO_TAG_FOREGROUND),
            (uint8_t)((color & 7) | IO_TAG_FOREGROUND)};
        for (pixel = 0; pixel < width; ++pixel)
            destination[pixel] = palette[(data >> (6 - (pixel & 6))) & 3];
    }
    else
    {
        for (pixel = 0; pixel < width; ++pixel)
            destination[pixel] = (data & (0x80u >> pixel))
                                     ? foreground
                                     : background;
    }
}

static void draw_raster_char_mode(void)
{
    int rstr = cur_line;
    int y = rstr - VIC_FIRST_VISIBLE_LINE;
    /* yscroll shifts the text window: default 3 keeps line = rstr - 56 */
    int line = rstr - (VIC_G_FIRST_LINE - 3) - vertical_scroll();
    int row = line >> 3;
    int char_row = line & 7;
    int xs = horizontal_scroll();
    int c, c0 = 0, c1 = VIC_G_COLS;
    unsigned int addr;
    const uint8_t *scr, *colram, *chardata;
    uint8_t *dst0 = composed_row(y);

    if (!dst0)
        return;
    if (line < 0 || line >= VIC_G_RES_Y)
    {
        memset(dst0, bgcolor[0], 320);
        return;
    }

    /* All source pointers are resolved ONCE per line: screen row and colour
       RAM are contiguous 40-byte runs, and the 2 KiB char generator block is
       either entirely inside or outside the char-ROM window. The VIC's 14
       address bits wrap inside the 16 KiB bank. */
    addr = (uint16_t)(cia2_vic_base_address() +
                      ((screen_mem + row_offset_40[row]) & 0x3fff));
    scr = mem_host_ptr((uint16_t)addr);
    colram = mem_color_ram + row_offset_40[row];
    addr = (uint16_t)(cia2_vic_base_address() + (char_mem & 0x3fff));
    if ((addr >= 0x1000 && addr < 0x2000) || (addr >= 0x9000 && addr < 0xa000))
        chardata = mem_chars_rom_ptr() + (addr & 0x0fff);
    else
        chardata = mem_host_ptr((uint16_t)addr);

    if (row_asm_enabled && char_asm_ready && graphic_mode != GM_EXT_BG)
    {
        vic_char_set_colors(bgcolor[0], bgcolor[1], bgcolor[2]);
        vic_char_dst = dst0 + xs;
        vic_char_screen = scr;
        vic_char_colram = colram;
        vic_char_glyphs = chardata + char_row;
        vic_char_cells = (uint8_t)(xs ? VIC_G_COLS - 1 : VIC_G_COLS);
        vic_char_rows = 1;
        vic_char_multicolor = graphic_mode == GM_MC_CHAR;
        vic_char_expand_rows();
        perf_note_char_kernel();
        if (xs)
        {
            memset(dst0, bgcolor[0], (size_t)xs);
            draw_char_edge(dst0 + 312 + xs, chardata + char_row,
                           scr[39], colram[39] & 15, (uint8_t)(8 - xs));
        }
        return;
    }

    memset(dst0, bgcolor[0], 320);

    /* 38-column mode is NOT narrowed: real VIC renders all 40 columns and
       overlays the border on ~7 edge px, which our LCD crop hides. */

    /* running pointers: eZ80 lowers per-column shifts/multiplies to runtime
       helper CALLS, so the loop carries incremented pointers instead */
    {
        bool ecm = (graphic_mode == GM_EXT_BG);
        const uint8_t *chline = chardata + char_row;
        uint8_t *d = dst0 + (c0 << 3) + xs;

        for (c = c0; c < c1; ++c, d += 8)
        {
            uint8_t sc = scr[c];
            uint8_t data;
            uint8_t color = colram[c] & 0x0f;

            if (c == VIC_G_COLS - 1 && xs)
            {
                draw_char_edge(d, chline, sc, color, (uint8_t)(8 - xs));
                break;
            }

            if (ecm)
            {
                /* top 2 char-code bits select the cell background colour */
                uint8_t cbg = bgcolor[sc >> 6];
                uint8_t fg = (uint8_t)(color | IO_TAG_FOREGROUND);
                data = chline[(uint16_t)((uint16_t)(sc & 0x3f) << 3)];
                d[0] = (data & 0x80) ? fg : cbg;
                d[1] = (data & 0x40) ? fg : cbg;
                d[2] = (data & 0x20) ? fg : cbg;
                d[3] = (data & 0x10) ? fg : cbg;
                d[4] = (data & 0x08) ? fg : cbg;
                d[5] = (data & 0x04) ? fg : cbg;
                d[6] = (data & 0x02) ? fg : cbg;
                d[7] = (data & 0x01) ? fg : cbg;
            }
            else if (graphic_mode == GM_MC_CHAR && (color & 0x08))
            {
                /* MC pairs 10/11 are foreground for priority/collision */
                uint8_t mc3 = (uint8_t)((color & 0x07) | IO_TAG_FOREGROUND);
                uint8_t bg2 = (uint8_t)(bgcolor[2] | IO_TAG_FOREGROUND);
                uint8_t px, v;
                data = chline[(uint16_t)((uint16_t)sc << 3)];
                px = (data >> 6) & 3;
                v = (px == 3) ? mc3 : (px == 2) ? bg2
                                  : (px == 1)   ? bgcolor[1]
                                                : bgcolor[0];
                d[0] = d[1] = v;
                px = (data >> 4) & 3;
                v = (px == 3) ? mc3 : (px == 2) ? bg2
                                  : (px == 1)   ? bgcolor[1]
                                                : bgcolor[0];
                d[2] = d[3] = v;
                px = (data >> 2) & 3;
                v = (px == 3) ? mc3 : (px == 2) ? bg2
                                  : (px == 1)   ? bgcolor[1]
                                                : bgcolor[0];
                d[4] = d[5] = v;
                px = data & 3;
                v = (px == 3) ? mc3 : (px == 2) ? bg2
                                  : (px == 1)   ? bgcolor[1]
                                                : bgcolor[0];
                d[6] = d[7] = v;
            }
            else
            {
                uint8_t fg = (uint8_t)(color | IO_TAG_FOREGROUND);
                data = chline[(uint16_t)((uint16_t)sc << 3)];
                if (data & 0x80)
                    d[0] = fg;
                if (data & 0x40)
                    d[1] = fg;
                if (data & 0x20)
                    d[2] = fg;
                if (data & 0x10)
                    d[3] = fg;
                if (data & 0x08)
                    d[4] = fg;
                if (data & 0x04)
                    d[5] = fg;
                if (data & 0x02)
                    d[6] = fg;
                if (data & 0x01)
                    d[7] = fg;
            }
        }
    }
}

/* Bitmap modes, same fast shape as the char renderer: all source pointers
   resolved once per line, pixels written straight into the framebuffer row
   (the old path made a bounds-checked io_screen_update_pixel CALL per pixel,
   64000+ calls per frame). */
static void draw_raster_bitmap_mode(void)
{
    int rstr = cur_line;
    int y = rstr - VIC_FIRST_VISIBLE_LINE;
    /* yscroll shifts the display window like in char mode */
    int line = rstr - (VIC_G_FIRST_LINE - 3) - vertical_scroll();
    int row = line >> 3;
    int xs = horizontal_scroll();
    int c, c0 = 0, c1 = VIC_G_COLS;
    uint16_t base = cia2_vic_base_address();
    uint16_t addr, bmp_addr, span_end;
    uint8_t win0, win1;
    const uint8_t *scr, *colram, *bmp = NULL;
    uint8_t *dst0 = composed_row(y);
    bool mc = (graphic_mode == GM_MC_BITMAP);
    uint8_t lut[4];

    if (!dst0)
        return;
    if (line < 0 || line >= VIC_G_RES_Y)
    {
        memset(dst0, bgcolor[0], 320);
        return;
    }

    /* bank base (bits 14-15) and 14-bit offset never overlap: compose with
       OR, all uint16_t (the compiler backend crashes on masked int adds) */
    addr = (uint16_t)(screen_mem + row_offset_40[row]);
    addr = (uint16_t)(base | (addr & 0x3fff));
    scr = mem_host_ptr(addr);
    colram = mem_color_ram + row_offset_40[row];

    /* Bitmap line = 40 bytes with stride 8 (313-byte span). Use a direct
       pointer unless the span touches a char-ROM shadow window ($1xxx/$9xxx);
       then fall back to per-byte VIC reads (rare: bitmaps live at $2000+). */
    bmp_addr = (uint16_t)(bitmap_mem + row_offset_320[row] + (line & 7));
    bmp_addr = (uint16_t)(base | (bmp_addr & 0x3fff));
    span_end = (uint16_t)(bmp_addr + 312);
    win0 = (uint8_t)(bmp_addr >> 12);
    win1 = (uint8_t)(span_end >> 12);
    if (win0 != 1 && win0 != 9 && win1 != 1 && win1 != 9)
        bmp = mem_host_ptr(bmp_addr);

    memset(dst0, bgcolor[0], 320);

    /* 38-column mode: render all 40 columns (border overlay is cropped) */

    lut[0] = bgcolor[0];

    if (!xs && bmp && row_asm_enabled)
    {
        if (!mc && hires_asm_ready)
        {
            vic_row_dst = dst0;
            vic_row_bitmap = bmp;
            vic_row_screen = scr;
            vic_row_cells = VIC_G_COLS;
            vic_expand_hires_row();
            return;
        }
        if (mc && mc_asm_ready)
        {
            vic_row_dst = dst0;
            vic_row_bitmap = bmp;
            vic_row_screen = scr;
            vic_row_colram = colram;
            vic_row_bg = bgcolor[0];
            vic_row_cells = VIC_G_COLS;
            vic_expand_mc_row();
            return;
        }
    }

    {
        const uint8_t *bp = bmp ? bmp + (c0 << 3) : NULL;
        uint16_t slow_addr = (uint16_t)(bmp_addr + (c0 << 3));
        uint8_t *d = dst0 + (c0 << 3) + xs;

        for (c = c0; c < c1; ++c, d += 8, slow_addr += 8)
        {
            uint8_t data;
            uint8_t sc = scr[c];

            if (bp)
            {
                data = *bp;
                bp += 8;
            }
            else
                data = mem_vic_read_byte(slow_addr);

            if (c == VIC_G_COLS - 1 && xs)
                break; /* scrolled last column would overrun the row */

            if (mc)
            {
                uint8_t px;
                /* MC bitmap: 01 = background priority, 10/11 = foreground */
                lut[1] = (sc >> 4) & 0x0f;
                lut[2] = (uint8_t)((sc & 0x0f) | IO_TAG_FOREGROUND);
                lut[3] = (uint8_t)((colram[c] & 0x0f) | IO_TAG_FOREGROUND);
                px = lut[(data >> 6) & 3];
                d[0] = px;
                d[1] = px;
                px = lut[(data >> 4) & 3];
                d[2] = px;
                d[3] = px;
                px = lut[(data >> 2) & 3];
                d[4] = px;
                d[5] = px;
                px = lut[data & 3];
                d[6] = px;
                d[7] = px;
            }
            else
            {
                uint8_t fg = (uint8_t)(((sc >> 4) & 0x0f) | IO_TAG_FOREGROUND);
                uint8_t bg = sc & 0x0f;
                d[0] = (data & 0x80) ? fg : bg;
                d[1] = (data & 0x40) ? fg : bg;
                d[2] = (data & 0x20) ? fg : bg;
                d[3] = (data & 0x10) ? fg : bg;
                d[4] = (data & 0x08) ? fg : bg;
                d[5] = (data & 0x04) ? fg : bg;
                d[6] = (data & 0x02) ? fg : bg;
                d[7] = (data & 0x01) ? fg : bg;
            }
        }
    }
}

/* Bulk glyph row: when no VIC register changed across a whole 8-line char
   row, each cell's screen code / colour / mode branch is fetched ONCE and
   all 8 scanlines are written together (the per-line path refetches them
   8x). Blank glyph rows (spaces) skip their 8 pixels entirely. */
__attribute__((noinline)) static void draw_char_row(int rstr)
{
    int y = rstr - VIC_FIRST_VISIBLE_LINE;
    int line = rstr - (VIC_G_FIRST_LINE - 3) - vertical_scroll();
    int row = line >> 3;
    int xs = horizontal_scroll();
    int c, k;
    unsigned int addr;
    const uint8_t *scr, *colram, *chardata;
    uint8_t *rows[8];
    bool ecm = (graphic_mode == GM_EXT_BG);
    bool mcm = (graphic_mode == GM_MC_CHAR);
    bool kernel_rows = row_asm_enabled && char_asm_ready && !ecm;

    for (k = 0; k < 8; ++k)
    {
        rows[k] = composed_row(y + k);
        if (!rows[k])
            return;
        if (k && rows[k] != rows[k - 1] + 320)
            kernel_rows = false;
    }

    /* same pointer resolution as the per-line renderer, once per row */
    addr = (uint16_t)(cia2_vic_base_address() +
                      ((screen_mem + row_offset_40[row]) & 0x3fff));
    scr = mem_host_ptr((uint16_t)addr);
    colram = mem_color_ram + row_offset_40[row];
    addr = (uint16_t)(cia2_vic_base_address() + (char_mem & 0x3fff));
    if ((addr >= 0x1000 && addr < 0x2000) || (addr >= 0x9000 && addr < 0xa000))
        chardata = mem_chars_rom_ptr() + (addr & 0x0fff);
    else
        chardata = mem_host_ptr((uint16_t)addr);

    if (kernel_rows)
    {
        vic_char_set_colors(bgcolor[0], bgcolor[1], bgcolor[2]);
        vic_char_dst = rows[0] + xs;
        vic_char_screen = scr;
        vic_char_colram = colram;
        vic_char_glyphs = chardata;
        vic_char_cells = (uint8_t)(xs ? VIC_G_COLS - 1 : VIC_G_COLS);
        vic_char_rows = 8;
        vic_char_multicolor = mcm;
        vic_char_expand_rows();
        perf_note_char_kernel();
        if (xs)
        {
            for (k = 0; k < 8; ++k)
            {
                memset(rows[k], bgcolor[0], (size_t)xs);
                draw_char_edge(rows[k] + 312 + xs, chardata + k,
                               scr[39], colram[39] & 15, (uint8_t)(8 - xs));
            }
        }
        return;
    }

    for (k = 0; k < 8; ++k)
        memset(rows[k], bgcolor[0], 320);

    {
        uint16_t xoff = (uint16_t)xs;
        for (c = 0; c < VIC_G_COLS; ++c, xoff += 8)
        {
            uint8_t sc = scr[c];
            uint8_t color = colram[c] & 0x0f;
            const uint8_t *chline;

            if (c == VIC_G_COLS - 1 && xs)
            {
                for (k = 0; k < 8; ++k)
                    draw_char_edge(rows[k] + xoff, chardata + k,
                                   sc, color, (uint8_t)(8 - xs));
                break;
            }

            if (ecm)
            {
                uint8_t cbg = bgcolor[sc >> 6];
                uint8_t fg = (uint8_t)(color | IO_TAG_FOREGROUND);
                chline = chardata + ((unsigned int)(sc & 0x3f) << 3);
                for (k = 0; k < 8; ++k)
                {
                    uint8_t data = chline[k];
                    uint8_t *d = rows[k] + xoff;
                    d[0] = (data & 0x80) ? fg : cbg;
                    d[1] = (data & 0x40) ? fg : cbg;
                    d[2] = (data & 0x20) ? fg : cbg;
                    d[3] = (data & 0x10) ? fg : cbg;
                    d[4] = (data & 0x08) ? fg : cbg;
                    d[5] = (data & 0x04) ? fg : cbg;
                    d[6] = (data & 0x02) ? fg : cbg;
                    d[7] = (data & 0x01) ? fg : cbg;
                }
            }
            else if (mcm && (color & 0x08))
            {
                uint8_t mc3 = (uint8_t)((color & 0x07) | IO_TAG_FOREGROUND);
                uint8_t bg2 = (uint8_t)(bgcolor[2] | IO_TAG_FOREGROUND);
                chline = chardata + ((unsigned int)sc << 3);
                for (k = 0; k < 8; ++k)
                {
                    uint8_t data = chline[k];
                    uint8_t *d;
                    uint8_t px, v;
                    if (!data)
                        continue; /* all-00 pairs equal the bg0 memset */
                    d = rows[k] + xoff;
                    px = (data >> 6) & 3;
                    v = (px == 3) ? mc3 : (px == 2) ? bg2
                                      : (px == 1)   ? bgcolor[1]
                                                    : bgcolor[0];
                    d[0] = d[1] = v;
                    px = (data >> 4) & 3;
                    v = (px == 3) ? mc3 : (px == 2) ? bg2
                                      : (px == 1)   ? bgcolor[1]
                                                    : bgcolor[0];
                    d[2] = d[3] = v;
                    px = (data >> 2) & 3;
                    v = (px == 3) ? mc3 : (px == 2) ? bg2
                                      : (px == 1)   ? bgcolor[1]
                                                    : bgcolor[0];
                    d[4] = d[5] = v;
                    px = data & 3;
                    v = (px == 3) ? mc3 : (px == 2) ? bg2
                                      : (px == 1)   ? bgcolor[1]
                                                    : bgcolor[0];
                    d[6] = d[7] = v;
                }
            }
            else
            {
                uint8_t fg = (uint8_t)(color | IO_TAG_FOREGROUND);
                chline = chardata + ((unsigned int)sc << 3);
                for (k = 0; k < 8; ++k)
                {
                    uint8_t data = chline[k];
                    uint8_t *d;
                    if (!data)
                        continue; /* blank glyph row: bg already memset */
                    d = rows[k] + xoff;
                    if (data & 0x80)
                        d[0] = fg;
                    if (data & 0x40)
                        d[1] = fg;
                    if (data & 0x20)
                        d[2] = fg;
                    if (data & 0x10)
                        d[3] = fg;
                    if (data & 0x08)
                        d[4] = fg;
                    if (data & 0x04)
                        d[5] = fg;
                    if (data & 0x02)
                        d[6] = fg;
                    if (data & 0x01)
                        d[7] = fg;
                }
            }
        }
    }
}

static void draw_mcsprite(int x, int y, int sprite, int row)
{
    /* each 2-bit pair covers 2 pixels, doubled again when X-expanded */
    int swid = is_double_width_sprite(sprite) ? 2 : 1;
    uint16_t addr = get_sprite_ptr(sprite);
    uint8_t sprite_bit = (uint8_t)(1u << sprite);
    bool behind = ISSET_BIT(sprite_priority, sprite);
    uint8_t *dst = composed_row(y);
    uint8_t byte_index, pair_index;
    uint8_t pair_width = (uint8_t)(swid == 2 ? 4 : 2);
    uint8_t byte_width = (uint8_t)(swid == 2 ? 16 : 8);
    int pixel_x = x;
    int top_border_offset = 0, btm_border_offset = 0;
    bool y_in_border;
    if (!dst)
        return;

    if (!ISSET_BIT(cr1, 3))
    {
        top_border_offset = 2;
        btm_border_offset = 4;
    }
    y_in_border = (y < VIC_G_FIRST_COL + top_border_offset ||
                   y >= VIC_G_RES_Y + VIC_G_FIRST_COL - btm_border_offset);
    if (y_in_border)
        return; /* border covers sprites; bands stay their static black */

    for (byte_index = 0; byte_index < 3; ++byte_index)
    {
        uint8_t data = mem_vic_read_byte(addr + row * 3 + byte_index);
        const uint8_t *masks = sprite_pixel_masks;
        if (!data)
        {
            pixel_x += byte_width;
            continue;
        }
        for (pair_index = 0; pair_index < 4; ++pair_index)
        {
            uint8_t high_bit = data & *masks++;
            uint8_t low_bit = data & *masks++;
            if (high_bit || low_bit)
            {
                uint8_t color = !high_bit  ? sprite_shared_colors[0]
                                : !low_bit ? sprite_colors[sprite]
                                           : sprite_shared_colors[1];
                sprite_pixel_run(sprite_bit, behind, dst, pixel_x, color, pair_width);
            }
            pixel_x += pair_width;
        }
    }
}

static void draw_sprite(int x, int y, int sprite, int row)
{
    int swid = is_double_width_sprite(sprite) ? 2 : 1;
    uint16_t addr = get_sprite_ptr(sprite);
    uint8_t sprite_bit = (uint8_t)(1u << sprite);
    uint8_t color = sprite_colors[sprite];
    bool behind = ISSET_BIT(sprite_priority, sprite);
    uint8_t *dst = composed_row(y);
    uint8_t byte_index, pixel_index;
    uint8_t byte_width = (uint8_t)(swid == 2 ? 16 : 8);
    int pixel_x = x;
    int top_border_offset = 0, btm_border_offset = 0;
    bool y_in_border;
    if (!dst)
        return;

    /* side borders are cropped off the LCD; top/bottom border rows draw
       nothing at all (border covers sprites, bands stay static black) */
    if (!ISSET_BIT(cr1, 3))
    {
        top_border_offset = 2;
        btm_border_offset = 4;
    }
    y_in_border = (y < VIC_G_FIRST_COL + top_border_offset ||
                   y >= VIC_G_RES_Y + VIC_G_FIRST_COL - btm_border_offset);
    if (y_in_border)
        return;

    for (byte_index = 0; byte_index < 3; ++byte_index)
    {
        uint8_t data = mem_vic_read_byte(addr + row * 3 + byte_index);
        const uint8_t *masks = sprite_pixel_masks;
        if (!data)
        {
            pixel_x += byte_width;
            continue;
        }
        for (pixel_index = 0; pixel_index < 8; ++pixel_index)
        {
            if (data & *masks++)
            {
                if (swid == 1)
                    sprite_pixel_at(sprite_bit, behind, dst, pixel_x, color);
                else
                    sprite_pixel_run(sprite_bit, behind, dst, pixel_x, color, 2);
            }
            pixel_x += swid;
        }
    }
}

/* Stateful sprite display, like real VIC: a Y match ARMS the sprite, which
   then shows all 21 rows regardless of later Y rewrites (multiplexers
   rewrite Y/X for the NEXT strip while the current one still displays). */
static void sprite_tick(int rstr, bool draw)
{
    int y = rstr - VIC_FIRST_VISIBLE_LINE;
    uint8_t sp_y = (uint8_t)(rstr - VIC_SPRITES_FIRST_LINE);
    int n;
    bool masked = false;

    for (n = 0; n < 8; ++n)
    {
        if (!is_sprite_enabled(n))
        {
            spr_row[n] = 0xff;
            continue;
        }
        if (spr_row[n] == 0xff && sp_y == my[n])
        {
            spr_row[n] = 0;
            spr_phase[n] = 0;
        }
    }

    for (n = 0; n < 8; ++n)
    {
        if (spr_row[n] == 0xff)
            continue;
        if (draw)
        {
            perf_note_sprite_line();
            int x = VIC_SPRITES_FIRST_COL + sprite_x(n);
            if (!masked)
            {
                memset(sprite_line_mask, 0, sizeof(sprite_line_mask));
                masked = true;
            }
            if (is_multicolor_sprite(n))
                draw_mcsprite(x, y, n, spr_row[n]);
            else
                draw_sprite(x, y, n, spr_row[n]);
        }
    }

    for (n = 0; n < 8; ++n)
    {
        if (spr_row[n] == 0xff)
            continue;
        if (is_double_height_sprite(n) && !spr_phase[n])
            spr_phase[n] = 1;
        else
        {
            spr_phase[n] = 0;
            if (++spr_row[n] >= VIC_SPRITE_HEIGHT)
                spr_row[n] = 0xff;
        }
    }
}

static void set_graphic_mode(void)
{
    bool ecm = (cr1 & (1 << 6)) != 0;
    bool bmm = (cr1 & (1 << 5)) != 0;
    bool mcm = (cr2 & (1 << 4)) != 0;

    if (!ecm && !bmm && !mcm)
        graphic_mode = GM_CHAR;
    else if (!ecm && !bmm && mcm)
        graphic_mode = GM_MC_CHAR;
    else if (!ecm && bmm && !mcm)
        graphic_mode = GM_BITMAP;
    else if (!ecm && bmm && mcm)
        graphic_mode = GM_MC_BITMAP;
    else if (ecm && !bmm && !mcm)
        graphic_mode = GM_EXT_BG;
    else
        graphic_mode = GM_ILLEGAL;
}

/* ---- emulation ---------------------------------------------------------- */

static bool sprites_require_catchup(int raster)
{
    uint8_t match_y = (uint8_t)(raster - VIC_SPRITES_FIRST_LINE);
    uint8_t sprite_mask = 1;
    uint8_t sprite_index = 0;

    do
    {
        if ((sprite_enabled & sprite_mask) &&
            (spr_row[sprite_index] != 0xff || my[sprite_index] == match_y))
            return true;
        ++sprite_index;
        sprite_mask += sprite_mask;
    } while (sprite_mask);
    return false;
}

/* One raster line, exactly the old per-line vic_emulate body. */
static void process_line(int rstr)
{
    /* Pixel work only for the text window (rasters 51-250): the letterbox
       bands are static black (painted once before c64_run) and lines past
       the LCD edges are cropped, so every other line skips straight to
       sprite state upkeep. */
    bool in_window = (rstr >= VIC_G_FIRST_LINE && rstr < VIC_G_LAST_LINE);
    bool draw = in_window && (render_frame || sprites_require_catchup(rstr));

    if (draw)
    {
        int screen_y = rstr - VIC_FIRST_VISIBLE_LINE;
        begin_row_composition(screen_y, 1);

        if (is_screen_off())
        {
            uint8_t *destination = composed_row(screen_y);
            if (destination)
                memset(destination, 0, 320);
        }
        else
        {
            switch (graphic_mode)
            {
            case GM_CHAR:
            case GM_MC_CHAR:
            case GM_EXT_BG:
                draw_raster_char_mode();
                break;
            case GM_BITMAP:
            case GM_MC_BITMAP:
                draw_raster_bitmap_mode();
                break;
            default:
            {
                /* illegal ECM combos: real VIC outputs black, keep going */
                uint8_t *d = composed_row(screen_y);
                vic_bad_mode = graphic_mode;
                if (d)
                    memset(d, 0, 320);
                break;
            }
            }
        }
    }
    /* sprite state advances EVERY line (even unrendered frames) so
       multiplexer arming stays in sync; pixels only when rendering */
    sprite_tick(rstr, draw);
    if (draw && render_frame)
        commit_row_composition();
    else
        composed_count = 0;
}

static void flush_lines(int upto)
{
    unsigned long sample_start;
    if (pending_line >= upto)
        return;
    if (render_suppressed)
    {
        pending_line = upto;
        return;
    }
    sample_start = perf_render_begin();
    /* Sprites idle = none enabled and none mid-display: sprite_tick is a
       no-op for every line, so line spans with no pixel work are skipped
       wholesale (unrendered frames entirely, border bands on rendered
       ones). Register writes flush first, so this cannot change mid-span. */
    bool sprites_idle = (sprite_enabled == 0);
    if (sprites_idle)
    {
        int n;
        for (n = 0; n < 8; ++n)
            if (spr_row[n] != 0xff)
            {
                sprites_idle = false;
                break;
            }
    }
    if (sprites_idle && !render_frame)
    {
        pending_line = upto;
        perf_render_end(sample_start);
        return;
    }

    while (pending_line < upto)
    {
        int rstr = pending_line;

        if (sprites_idle)
        {
            if (rstr < VIC_G_FIRST_LINE)
            {
                pending_line = upto < VIC_G_FIRST_LINE ? upto
                                                       : VIC_G_FIRST_LINE;
                continue;
            }
            if (rstr >= VIC_G_LAST_LINE)
            {
                pending_line = upto;
                break;
            }
        }

        if (render_frame && upto - rstr >= 8 && !is_screen_off() &&
            rstr >= VIC_G_FIRST_LINE && rstr + 7 < VIC_G_LAST_LINE &&
            (graphic_mode == GM_CHAR || graphic_mode == GM_MC_CHAR ||
             graphic_mode == GM_EXT_BG))
        {
            int line = rstr - (VIC_G_FIRST_LINE - 3) - vertical_scroll();
            if (line >= 0 && (line & 7) == 0 && line + 7 < VIC_G_RES_Y)
            {
                int k;
#ifndef NDEBUG
                unsigned int t0 = (unsigned int)timer_Get(1);
#endif
                begin_row_composition(rstr - VIC_FIRST_VISIBLE_LINE, 8);
                draw_char_row(rstr);
#ifndef NDEBUG
                vic_prof[1] += (unsigned int)(timer_Get(1) - t0);
                vic_prof[3]++;
#endif
                for (k = 0; k < 8; ++k)
                    sprite_tick(rstr + k, true);
                commit_row_composition();
                pending_line = rstr + 8;
                continue;
            }
        }
        cur_line = rstr;
#ifndef NDEBUG
        if (render_frame)
            vic_prof[4]++;
#endif
        process_line(rstr);
        pending_line = rstr + 1;
    }
    perf_render_end(sample_start);
}

/* Flush undrawn lines with the CURRENT register state; called before any
   display-affecting register change (and on VIC bank switches in cia2). */
void vic_render_catchup(void)
{
    flush_lines(get_raster_counter());
}

/* Separate noinline function: folding this into vic_emulate makes the ez80
   backend crash legalizing the renderer's masked 14-bit address adds. */
__attribute__((noinline)) static void raster_irq_check(void)
{
    uint16_t line_now = (uint16_t)get_raster_counter();
    if (raster_irq_enabled_fn() && line_now == (uint16_t)raster_irq)
    {
        irq_status |= (1 << 0);
        cpu_irq();
    }
}

#if defined(__clang__)
__attribute__((__minsize__, noinline))
#endif
static void vic_vblank(void)
{
    bool was_rendered = render_frame;
    uint32_t now;
#ifndef NDEBUG
    unsigned int started = (unsigned int)timer_Get(1);
#endif
    flush_lines(VIC_SCREEN_LINES);
    pending_line = 0;
    if (was_rendered)
        ++rendered_frame_count;
    io_screen_refresh();
    io_refresh_vram_view();
    perf_frame_complete(was_rendered, boot_hidden);
#ifndef NDEBUG
    vic_prof[0] += (unsigned int)(timer_Get(1) - started);
    if (was_rendered)
        vic_prof[2]++;
#endif
    ++frame_count;
    now = (uint32_t)timer_Get(1);
    if (cpu_turbo_adaptive)
    {
        unsigned int elapsed = (unsigned int)now - adaptive_frame_start_ticks;
        if (adaptive_frame_start_ticks != 0 && elapsed != 0)
        {
            unsigned int multiplier =
                cpu_turbo_multiplier * (32768u / TURBO_ADAPTIVE_TARGET_FPS) / elapsed;
            if (multiplier == 0)
                multiplier = 1;
            if (multiplier > 200)
                multiplier = 200;
            cpu_turbo_multiplier = (uint8_t)multiplier;
        }
        adaptive_frame_start_ticks = (unsigned int)now;
    }
    if (vblank_advance(&frame_pacer, now,
                       cpu_turbo_multiplier == 1 && !cpu_turbo_adaptive && !boot_hidden,
                       frame_skip_enabled, render_requested))
    {
        while ((int32_t)((uint32_t)timer_Get(1) - frame_pacer.deadline) < 0)
        {
        }
    }
    render_frame = frame_pacer.render;
    if (render_suppressed)
    {
        render_frame = false;
        io_refresh_debug_view();
    }
    if (boot_hidden)
    {
        render_frame = false;
        if (frame_count >= 900)
            boot_hidden = false;
    }
    render_requested = false;
    io_fps_overlay(was_rendered && !boot_hidden);
    set_raster_counter(0);
}

bool vic_emulate(void)
{
    /* re-raise interrupt if there are unacknowledged ones (inline check of
       irq_status instead of a vic_read_register call per raster line) */
    if (irq_status & 0x0f)
        cpu_irq();

    /* wrap-safe: 24-bit cycle counter overflows every ~17 emulated seconds */
    if ((int)(cpu.cycles - vic_raster_due) >= 0)
    {
        int rstr = get_raster_counter();

        if (sprite_enabled && sprites_require_catchup(rstr))
            flush_lines(rstr + 1);

        {
            unsigned int line_cycles = is_bad_line()
                                           ? VIC_BAD_LINE_CYCLES
                                           : VIC_LINE_CYCLES;
            vic_raster_due += line_cycles;
            vic_raster_due += line_cycles * (cpu_turbo_multiplier - 1u);
        }

        set_raster_counter(++rstr);
        if (rstr >= VIC_SCREEN_LINES)
            vic_vblank();

        /* Raster IRQ fires at the START of the target line: the handler's
           register writes (sprite multiplex, scroll splits) run during this
           line's CPU batch, BEFORE the line is drawn. The previous order
           delivered every raster IRQ one line late. */
        raster_irq_check();
    }
    return true;
}

/* ---- register access ----------------------------------------------------- */

/* TIBoyce-style waitloop skip: a SECOND read of the same IO register on the
   same raster line is a busy poll (loops read 5-10x per line); flag the CPU
   core to end the line batch instead of spinning through it. First read per
   line never warps, so one-shot reads keep their full line budget.
   Streams: 0 = $D011/$D012 raster, 1 = $DC00 joystick, 2 = $DC01 keyboard,
   3 = $DD00 IEC bus (no drive is attached, so polls only await timeouts).
   Separate keys so unrelated IO reads on one line (an IRQ handler touching
   CIA and VIC once each) never count as a poll. */
void vic_poll_check(uint8_t id)
{
    static int poll_key[4] = {-1, -1, -1, -1};
    int key = get_raster_counter() | (int)(frame_count << 9);
    if (key == poll_key[id])
        cpu_warp = 1;
    else
        poll_key[id] = key;
}

static void raster_poll_check(void)
{
    vic_poll_check(0);
}

uint8_t vic_read_register(uint8_t r)
{
    uint8_t retval;

    switch (r)
    {
    case 0x0:
    case 0x2:
    case 0x4:
    case 0x6:
    case 0x8:
    case 0xa:
    case 0xc:
    case 0xe:
        retval = mx[r >> 1];
        break;
    case 0x1:
    case 0x3:
    case 0x5:
    case 0x7:
    case 0x9:
    case 0xb:
    case 0xd:
    case 0xf:
        retval = my[r >> 1];
        break;
    case 0x10:
        retval = msbx;
        break;
    case 0x11:
        retval = cr1;
        raster_poll_check();
        break;
    case 0x12:
        retval = raster_c;
        raster_poll_check();
        break;
    case 0x15:
        retval = sprite_enabled;
        break;
    case 0x16:
        retval = cr2;
        break;
    case 0x17:
        retval = sprite_double_height;
        break;
    case 0x18:
        retval = mem_pointers;
        break;
    case 0x19:
        retval = (0xf & irq_status);
        if (retval != 0)
            retval |= 0x80;
        retval |= 0x70;
        break;
    case 0x1a:
        retval = (0xf0 | irq_enabled);
        break;
    case 0x1b:
        retval = sprite_priority;
        break;
    case 0x1c:
        retval = sprite_multicolor;
        break;
    case 0x1d:
        retval = sprite_double_width;
        break;
    /* collision latches clear on read (hardware behaviour); flush pending
       lines first so collisions from already-executed lines are latched */
    case 0x1e:
        vic_render_catchup();
        retval = sprite_coll_ss;
        sprite_coll_ss = 0;
        break;
    case 0x1f:
        vic_render_catchup();
        retval = sprite_coll_sb;
        sprite_coll_sb = 0;
        break;
    case 0x20:
        retval = border_color;
        break;
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x24:
        retval = bgcolor[r - 0x21];
        break;
    case 0x25:
    case 0x26:
        retval = sprite_shared_colors[r - 0x25];
        break;
    case 0x27:
    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
    case 0x2c:
    case 0x2d:
    case 0x2e:
        retval = sprite_colors[r - 0x27];
        break;
    default:
        retval = 0xff;
        break;
    }
    return retval;
}

uint8_t vic_peek_register(uint8_t r)
{
    uint8_t retval;

    switch (r)
    {
    case 0x0:
    case 0x2:
    case 0x4:
    case 0x6:
    case 0x8:
    case 0xa:
    case 0xc:
    case 0xe:
        retval = mx[r >> 1];
        break;
    case 0x1:
    case 0x3:
    case 0x5:
    case 0x7:
    case 0x9:
    case 0xb:
    case 0xd:
    case 0xf:
        retval = my[r >> 1];
        break;
    case 0x10:
        retval = msbx;
        break;
    case 0x11:
        retval = cr1;
        break;
    case 0x12:
        retval = raster_c;
        break;
    case 0x15:
        retval = sprite_enabled;
        break;
    case 0x16:
        retval = cr2;
        break;
    case 0x17:
        retval = sprite_double_height;
        break;
    case 0x18:
        retval = mem_pointers;
        break;
    case 0x19:
        retval = (uint8_t)(0xf & irq_status);
        if (retval != 0)
            retval |= 0x80;
        retval |= 0x70;
        break;
    case 0x1a:
        retval = (uint8_t)(0xf0 | irq_enabled);
        break;
    case 0x1b:
        retval = sprite_priority;
        break;
    case 0x1c:
        retval = sprite_multicolor;
        break;
    case 0x1d:
        retval = sprite_double_width;
        break;
    case 0x1e:
        retval = sprite_coll_ss;
        break;
    case 0x1f:
        retval = sprite_coll_sb;
        break;
    case 0x20:
        retval = border_color;
        break;
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x24:
        retval = bgcolor[r - 0x21];
        break;
    case 0x25:
    case 0x26:
        retval = sprite_shared_colors[r - 0x25];
        break;
    case 0x27:
    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
    case 0x2c:
    case 0x2d:
    case 0x2e:
        retval = sprite_colors[r - 0x27];
        break;
    default:
        retval = 0xff;
        break;
    }
    return retval;
}

void vic_write_register(uint8_t r, uint8_t v)
{
    /* lazy rendering: draw already-executed lines with the pre-write state.
       Idempotent writes skip the flush (games hammer $D011 with the same
       value every other line) so register-stable stripes stay whole. */
    switch (r)
    {
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x19:
    case 0x1a:
    case 0x1e:
    case 0x1f: /* collision latches ignore writes */
        break; /* raster target / light pen / IRQ regs: no pixel effect */
    case 0x11:
        if (((cr1 ^ v) & 0x7f) != 0)
            vic_render_catchup();
        break;
    case 0x18:
        if (mem_pointers != (uint8_t)(v | 1))
            vic_render_catchup();
        break;
    default:
        if (r <= 0x2e &&
            vic_read_register(r) != ((r >= 0x20) ? (v & 0x0f) : v))
            vic_render_catchup();
        break;
    }

#if EMUDORE_VIC_TRACE
    /* trace sprite Y/X/enable/control/priority/colour writes; $D011 excluded
       (games hammer it every other line and flood the ring buffer) */
    if (r <= 0x10 || r == 0x12 || r == 0x15 || r == 0x17 || r == 0x18 ||
        (r >= 0x1b && r <= 0x1d) || (r >= 0x25 && r <= 0x2e))
    {
        VicWriteTrace *t = &vic_wtrace[vic_wtrace_pos];
        t->line = (uint16_t)get_raster_counter();
        t->reg = r;
        t->val = v;
        vic_wtrace_pos = (uint8_t)((vic_wtrace_pos + 1) & (VIC_WTRACE_MAX - 1));
    }
#endif

    switch (r)
    {
    case 0x0:
    case 0x2:
    case 0x4:
    case 0x6:
    case 0x8:
    case 0xa:
    case 0xc:
    case 0xe:
        mx[r >> 1] = v;
        break;
    case 0x1:
    case 0x3:
    case 0x5:
    case 0x7:
    case 0x9:
    case 0xb:
    case 0xd:
    case 0xf:
        my[r >> 1] = v;
        break;
    case 0x10:
        msbx = v;
        break;
    case 0x11:
        cr1 = (cr1 & 0x80) | (v & 0x7f);
        raster_irq &= 0xff;
        raster_irq |= (v & 0x80) << 1;
        set_graphic_mode();
        break;
    case 0x12:
        raster_irq = v | (raster_irq & (1 << 8));
        break;
    case 0x15:
        sprite_enabled = v;
        break;
    case 0x16:
        cr2 = v;
        set_graphic_mode();
        break;
    case 0x17:
        sprite_double_height = v;
        break;
    case 0x18:
        char_mem = (v & 0xe) << 10;
        screen_mem = (v & 0xf0) << 6;
        bitmap_mem = (v & 0x8) << 10;
        mem_pointers = v | (1 << 0);
        break;
    case 0x19:
        irq_status &= ~(v & 0xf);
        break;
    case 0x1a:
        irq_enabled = v;
        break;
    case 0x1b:
        sprite_priority = v;
        break;
    case 0x1c:
        sprite_multicolor = v;
        break;
    case 0x1d:
        sprite_double_width = v;
        break;
    case 0x20:
        border_color = v & 0x0f;
        break;
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x24:
        bgcolor[r - 0x21] = v & 0x0f;
        break;
    case 0x25:
    case 0x26:
        sprite_shared_colors[r - 0x25] = v & 0x0f;
        break;
    case 0x27:
    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
    case 0x2c:
    case 0x2d:
    case 0x2e:
        sprite_colors[r - 0x27] = v & 0x0f;
        break;
    default:
        break;
    }
}
