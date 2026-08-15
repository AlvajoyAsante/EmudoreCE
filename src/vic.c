/*
 * emudore-CE: MOS VIC-II video chip (C port of vic.cpp).
 *
 * Drawing calls go to io.c (graphx framebuffer) instead of an SDL texture.
 * Stop reasons (illegal opcode / unsupported mode) are stored in globals and
 * reported by main() after the loop; no debug printing in the hot path.
 */
#include <string.h>
#include <sys/timers.h>
#include "vic.h"
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
static const bool hires_asm_enabled = true;

/* Returns a bitmask: bit0 = hires row OK, bit1 = MC row OK. Each asm row is
   enabled independently; a failing row silently uses the C path. */
uint8_t vic_row_asm_selftest(void)
{
    static uint8_t source[40];
    static uint8_t screen[40];
    static uint8_t output[320];
    unsigned int cell;
    unsigned int bit;

    for (cell = 0; cell < 40; ++cell)
    {
        source[cell] = (uint8_t)(0x80u >> (cell & 7)) ^ (uint8_t)(cell * 37u);
        screen[cell] = (uint8_t)(((cell + 3u) << 4) | (15u - cell));
    }

    vic_row_dst = output;
    vic_row_bitmap = source;
    vic_row_screen = screen;
    vic_row_cells = 40;
    vic_expand_hires_row();

    hires_asm_ready = true;
    for (cell = 0; cell < 40 && hires_asm_ready; ++cell)
    {
        uint8_t fg = (uint8_t)((screen[cell] >> 4) | IO_TAG_FOREGROUND);
        uint8_t bg = screen[cell] & 0x0f;
        for (bit = 0; bit < 8; ++bit)
        {
            uint8_t expected = (source[cell] & (uint8_t)(0x80u >> bit)) ? fg : bg;
            if (output[(cell << 3) + bit] != expected)
            {
                hires_asm_ready = false;
                break;
            }
        }
    }

    /* multicolor row: same buffers; screen doubles as colour RAM source */
    vic_row_colram = screen;
    vic_row_bg = 6;
    vic_expand_mc_row();

    mc_asm_ready = true;
    for (cell = 0; cell < 40 && mc_asm_ready; ++cell)
    {
        uint8_t lut[4];
        lut[0] = 6;
        lut[1] = screen[cell] >> 4;
        lut[2] = (uint8_t)((screen[cell] & 0x0f) | IO_TAG_FOREGROUND);
        lut[3] = (uint8_t)((screen[cell] & 0x0f) | IO_TAG_FOREGROUND);
        for (bit = 0; bit < 4; ++bit)
        {
            uint8_t px = lut[(source[cell] >> (6 - bit * 2)) & 3];
            if (output[(cell << 3) + bit * 2] != px ||
                output[(cell << 3) + bit * 2 + 1] != px)
            {
                mc_asm_ready = false;
                break;
            }
        }
    }

    return (uint8_t)((hires_asm_ready ? 1 : 0) | (mc_asm_ready ? 2 : 0));
}

/* Render only some frames: raster/IRQ timing still runs every line, but
   the pixel drawing (the bulk of the per-line cost) is skipped. */
static bool render_frame;
static bool render_requested; /* set by io on key change: render next frame */

/* Lazy (TIBoyce-style) rendering: lines are NOT drawn as the raster
   advances. pending_line is the first undrawn line; a display-affecting
   register write flushes pending..current-1 with the pre-write state, and
   the frame wrap flushes the rest. Register-stable stripes covering a whole
   glyph row take the bulk 8-line path. cur_line feeds the renderers (an
   inlined raster-counter read crashes the ez80 backend, see below). */
static int pending_line;
static int cur_line;

void vic_request_render(void)
{
    render_requested = true;
}

/* set when vic_emulate stops on an unsupported mode; reported by main() */
uint8_t vic_bad_mode;

/* Rolling trace of sprite/control register writes with their raster line:
   diagnoses multiplexer schedules. Wraps; main() dumps the tail at exit. */
VicWriteTrace vic_wtrace[VIC_WTRACE_MAX];
uint8_t vic_wtrace_pos;

/* Cold diagnostics for main(): copy of the display configuration. */
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
    }
    d->screen_mem = screen_mem;
    d->char_mem = char_mem;
    d->bitmap_mem = bitmap_mem;
    d->raster_irq = raster_irq;
    d->irq_en = irq_enabled;
    d->vic_base = cia2_vic_base_address();
}

void vic_reset(void)
{
    int i;
    raster_irq = raster_c = 0;
    irq_enabled = irq_status = 0;
    pending_line = cur_line = 0;
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
    render_frame = true;
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
__attribute__((noinline))
static int get_raster_counter(void)
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
static uint8_t vertical_scroll(void)   { return cr1 & 0x7; }

static bool is_sprite_enabled(int n)       { return ISSET_BIT(sprite_enabled, n); }
static bool is_double_width_sprite(int n)  { return ISSET_BIT(sprite_double_width, n); }
static bool is_double_height_sprite(int n) { return ISSET_BIT(sprite_double_height, n); }
static bool is_multicolor_sprite(int n)    { return ISSET_BIT(sprite_multicolor, n); }

static int sprite_x(int n)
{
    int x = mx[n];
    if (ISSET_BIT(msbx, n))
        x |= 1 << 8;
    return x;
}

/* ---- graphics data fetch ---------------------------------------------- */

static uint16_t get_sprite_ptr(int n)
{
    return VIC_SPRITE_SIZE * mem_vic_read_byte(screen_mem + VIC_SPRITE_PTRS_OFFSET + n);
}

/* Per-line sprite coverage mask: pixels cannot store all 8 sprite numbers,
   so $D01E needs this side channel for both-bits collisions. */
static uint8_t sprite_line_mask[VIC_SPRITES_FIRST_COL + 512 + 48];

/* Sprite pixel straight into the framebuffer row: no per-pixel io calls.
   fx is a C64 frame x coordinate; row comes from io_text_row(). */
__attribute__((__always_inline__))
static inline void sprite_pixel_at(int n, bool behind, uint8_t *row, int fx,
                                   uint8_t color)
{
    unsigned int idx = (unsigned int)(fx - IO_CROP_X);
    uint8_t under;
    uint8_t bit = (uint8_t)(1 << n);

    if (idx >= 320)
        return;
    if ((unsigned int)fx < sizeof(sprite_line_mask))
    {
        uint8_t prev = sprite_line_mask[fx];
        if (prev)
            sprite_coll_ss |= prev | bit;
        sprite_line_mask[fx] = (uint8_t)(prev | bit);
    }
    under = row[idx];
    if (under & IO_TAG_FOREGROUND)
    {
        sprite_coll_sb |= bit;
        if (behind)
            return; /* behind-priority sprite hides under foreground */
    }
    row[idx] = (uint8_t)(color | IO_TAG_SPRITE);
}

/* ---- drawing ----------------------------------------------------------- */

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
    uint8_t *dst0 = io_text_row(y);

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
                      ((screen_mem + row * VIC_G_COLS) & 0x3fff));
    scr = mem_host_ptr((uint16_t)addr);
    colram = mem_color_ram + row * VIC_G_COLS;
    addr = (uint16_t)(cia2_vic_base_address() + (char_mem & 0x3fff));
    if ((addr >= 0x1000 && addr < 0x2000) || (addr >= 0x9000 && addr < 0xa000))
        chardata = mem_chars_rom_ptr() + (addr & 0x0fff);
    else
        chardata = mem_host_ptr((uint16_t)addr);

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
                break; /* scrolled last column would overrun the row */

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
                px = (data >> 6) & 3; v = (px == 3) ? mc3 : (px == 2) ? bg2 : (px == 1) ? bgcolor[1] : bgcolor[0]; d[0] = d[1] = v;
                px = (data >> 4) & 3; v = (px == 3) ? mc3 : (px == 2) ? bg2 : (px == 1) ? bgcolor[1] : bgcolor[0]; d[2] = d[3] = v;
                px = (data >> 2) & 3; v = (px == 3) ? mc3 : (px == 2) ? bg2 : (px == 1) ? bgcolor[1] : bgcolor[0]; d[4] = d[5] = v;
                px = data & 3;        v = (px == 3) ? mc3 : (px == 2) ? bg2 : (px == 1) ? bgcolor[1] : bgcolor[0]; d[6] = d[7] = v;
            }
            else
            {
                uint8_t fg = (uint8_t)(color | IO_TAG_FOREGROUND);
                data = chline[(uint16_t)((uint16_t)sc << 3)];
                if (data & 0x80) d[0] = fg;
                if (data & 0x40) d[1] = fg;
                if (data & 0x20) d[2] = fg;
                if (data & 0x10) d[3] = fg;
                if (data & 0x08) d[4] = fg;
                if (data & 0x04) d[5] = fg;
                if (data & 0x02) d[6] = fg;
                if (data & 0x01) d[7] = fg;
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
    uint8_t *dst0 = io_text_row(y);
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
    addr = (uint16_t)(screen_mem + row * VIC_G_COLS);
    addr = (uint16_t)(base | (addr & 0x3fff));
    scr = mem_host_ptr(addr);
    colram = mem_color_ram + row * VIC_G_COLS;

    /* Bitmap line = 40 bytes with stride 8 (313-byte span). Use a direct
       pointer unless the span touches a char-ROM shadow window ($1xxx/$9xxx);
       then fall back to per-byte VIC reads (rare: bitmaps live at $2000+). */
    bmp_addr = (uint16_t)(bitmap_mem + row * (VIC_G_COLS * 8) + (line & 7));
    bmp_addr = (uint16_t)(base | (bmp_addr & 0x3fff));
    span_end = (uint16_t)(bmp_addr + 312);
    win0 = (uint8_t)(bmp_addr >> 12);
    win1 = (uint8_t)(span_end >> 12);
    if (win0 != 1 && win0 != 9 && win1 != 1 && win1 != 9)
        bmp = mem_host_ptr(bmp_addr);

    memset(dst0, bgcolor[0], 320);

    /* 38-column mode: render all 40 columns (border overlay is cropped) */

    lut[0] = bgcolor[0];

    if (!xs && bmp && hires_asm_enabled)
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
            px = lut[(data >> 6) & 3]; d[0] = px; d[1] = px;
            px = lut[(data >> 4) & 3]; d[2] = px; d[3] = px;
            px = lut[(data >> 2) & 3]; d[4] = px; d[5] = px;
            px = lut[data & 3];        d[6] = px; d[7] = px;
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
__attribute__((noinline))
static void draw_char_row(int rstr)
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

    for (k = 0; k < 8; ++k)
    {
        rows[k] = io_text_row(y + k);
        if (!rows[k])
            return;
        memset(rows[k], bgcolor[0], 320);
    }

    /* same pointer resolution as the per-line renderer, once per row */
    addr = (uint16_t)(cia2_vic_base_address() +
                      ((screen_mem + row * VIC_G_COLS) & 0x3fff));
    scr = mem_host_ptr((uint16_t)addr);
    colram = mem_color_ram + row * VIC_G_COLS;
    addr = (uint16_t)(cia2_vic_base_address() + (char_mem & 0x3fff));
    if ((addr >= 0x1000 && addr < 0x2000) || (addr >= 0x9000 && addr < 0xa000))
        chardata = mem_chars_rom_ptr() + (addr & 0x0fff);
    else
        chardata = mem_host_ptr((uint16_t)addr);

    for (c = 0; c < VIC_G_COLS; ++c)
    {
        uint8_t sc = scr[c];
        uint8_t color = colram[c] & 0x0f;
        const uint8_t *chline;
        unsigned int xoff = ((unsigned int)c << 3) + (unsigned int)xs;

        if (c == VIC_G_COLS - 1 && xs)
            break; /* scrolled last column would overrun the row */

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
                px = (data >> 6) & 3; v = (px == 3) ? mc3 : (px == 2) ? bg2 : (px == 1) ? bgcolor[1] : bgcolor[0]; d[0] = d[1] = v;
                px = (data >> 4) & 3; v = (px == 3) ? mc3 : (px == 2) ? bg2 : (px == 1) ? bgcolor[1] : bgcolor[0]; d[2] = d[3] = v;
                px = (data >> 2) & 3; v = (px == 3) ? mc3 : (px == 2) ? bg2 : (px == 1) ? bgcolor[1] : bgcolor[0]; d[4] = d[5] = v;
                px = data & 3;        v = (px == 3) ? mc3 : (px == 2) ? bg2 : (px == 1) ? bgcolor[1] : bgcolor[0]; d[6] = d[7] = v;
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
                if (data & 0x80) d[0] = fg;
                if (data & 0x40) d[1] = fg;
                if (data & 0x20) d[2] = fg;
                if (data & 0x10) d[3] = fg;
                if (data & 0x08) d[4] = fg;
                if (data & 0x04) d[5] = fg;
                if (data & 0x02) d[6] = fg;
                if (data & 0x01) d[7] = fg;
            }
        }
    }
}

static void draw_mcsprite(int x, int y, int sprite, int row)
{
    /* each 2-bit pair covers 2 pixels, doubled again when X-expanded */
    int swid = is_double_width_sprite(sprite) ? 2 : 1;
    uint16_t addr = get_sprite_ptr(sprite);
    bool behind = ISSET_BIT(sprite_priority, sprite);
    uint8_t *dst = io_text_row(y);
    int i, j, k;
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

    for (i = 0; i < 3; ++i)
    {
        uint8_t data = mem_vic_read_byte(addr + row * 3 + i);
        if (!data)
            continue; /* transparent byte: skip 8 pixels at once */
        for (j = 0; j < 4; ++j)
        {
            uint8_t c = 0;
            uint8_t cs = (data >> (j * 2)) & 0x3;
            switch (cs)
            {
            case 0: break;
            case 1: c = sprite_shared_colors[0]; break;
            case 2: c = sprite_colors[sprite]; break;
            default: c = sprite_shared_colors[1]; break;
            }
            if (cs != 0)
            {
                /* pair j=3 is the MSB pair = leftmost 2 pixels */
                int px = x + (i * 8 + (3 - j) * 2) * swid;
                for (k = 0; k < 2 * swid; ++k)
                    sprite_pixel_at(sprite, behind, dst, px + k, c);
            }
        }
    }
}

static void draw_sprite(int x, int y, int sprite, int row)
{
    int swid = is_double_width_sprite(sprite) ? 2 : 1;
    uint16_t addr = get_sprite_ptr(sprite);
    bool behind = ISSET_BIT(sprite_priority, sprite);
    uint8_t *dst = io_text_row(y);
    int w, i, j;
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

    for (w = 0; w < swid; ++w)
    {
        for (i = 0; i < 3; ++i)
        {
            uint8_t data = mem_vic_read_byte(addr + row * 3 + i);
            if (!data)
                continue; /* transparent byte: skip 8 pixels at once */
            for (j = 0; j < 8; ++j)
            {
                if (ISSET_BIT(data, j))
                {
                    /* bit 7 is the leftmost pixel */
                    int new_x = x + w + (i * 8 + 7 - j) * swid;

                    sprite_pixel_at(sprite, behind, dst, new_x,
                                    sprite_colors[sprite]);
                }
            }
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

    for (n = 7; n >= 0; --n)
    {
        if (spr_row[n] == 0xff)
            continue;
        if (draw)
        {
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

    if (!ecm && !bmm && !mcm)       graphic_mode = GM_CHAR;
    else if (!ecm && !bmm && mcm)   graphic_mode = GM_MC_CHAR;
    else if (!ecm && bmm && !mcm)   graphic_mode = GM_BITMAP;
    else if (!ecm && bmm && mcm)    graphic_mode = GM_MC_BITMAP;
    else if (ecm && !bmm && !mcm)   graphic_mode = GM_EXT_BG;
    else                            graphic_mode = GM_ILLEGAL;
}

/* ---- emulation ---------------------------------------------------------- */

/* One raster line, exactly the old per-line vic_emulate body. */
static void process_line(int rstr)
{
    /* Pixel work only for the text window (rasters 51-250): the letterbox
       bands are static black (painted once before c64_run) and lines past
       the LCD edges are cropped, so every other line skips straight to
       sprite state upkeep. */
    bool in_window = (rstr >= VIC_G_FIRST_LINE && rstr < VIC_G_LAST_LINE);
    bool draw = render_frame && in_window;

    if (draw)
    {
        int screen_y = rstr - VIC_FIRST_VISIBLE_LINE;

        if (is_screen_off())
            io_screen_draw_border(screen_y, border_color);
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
                uint8_t *d = io_text_row(screen_y);
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
}

static void flush_lines(int upto)
{
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
                draw_char_row(rstr);
                for (k = 0; k < 8; ++k)
                    sprite_tick(rstr + k, true);
                pending_line = rstr + 8;
                continue;
            }
        }
        cur_line = rstr;
        process_line(rstr);
        pending_line = rstr + 1;
    }
}

/* Flush undrawn lines with the CURRENT register state; called before any
   display-affecting register change (and on VIC bank switches in cia2). */
void vic_render_catchup(void)
{
    flush_lines(get_raster_counter());
}

/* Separate noinline function: folding this into vic_emulate makes the ez80
   backend crash legalizing the renderer's masked 14-bit address adds. */
__attribute__((noinline))
static void raster_irq_check(void)
{
    uint16_t line_now = (uint16_t)get_raster_counter();
    if (raster_irq_enabled_fn() && line_now == (uint16_t)raster_irq)
    {
        irq_status |= (1 << 0);
        cpu_irq();
    }
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

        /* Drawing is DEFERRED (flush_lines): register-stable stripes render
           in bulk; register writes split the frame exactly where needed. */

        vic_raster_due += is_bad_line() ? VIC_BAD_LINE_CYCLES : VIC_LINE_CYCLES;

        set_raster_counter(++rstr);
        if (rstr >= VIC_SCREEN_LINES)
        {
            flush_lines(VIC_SCREEN_LINES);
            pending_line = 0;
            io_screen_refresh();
            frame_count++;
            /* Wall-clock frame pacing (~12 fps of the 32 kHz timer). The
               render is committed on ALTERNATING emulated frame parity:
               games blink sprites by toggling them every frame, and a
               fixed-phase sampler can lock onto the "off" phases, making
               blinking sprites permanently invisible on the LCD. */
            if (frame_count < 300)
                render_frame = render_requested || ((frame_count & 15) == 0);
            else
            {
                static unsigned int last_render_ticks;
                static bool render_pending;
                static uint8_t render_parity;
                unsigned int now = (unsigned int)timer_Get(1);
                if (render_requested ||
                    now - last_render_ticks >= 32768u / 12u)
                {
                    render_pending = true;
                    last_render_ticks = now;
                }
                render_frame = false;
                if (render_pending &&
                    (uint8_t)(frame_count & 1u) == render_parity)
                {
                    render_frame = true;
                    render_pending = false;
                    render_parity ^= 1;
                }
            }
            render_requested = false;
            set_raster_counter(0);
        }

        /* Raster IRQ fires at the START of the target line: the handler's
           register writes (sprite multiplex, scroll splits) run during this
           line's CPU batch, BEFORE the line is drawn. The previous order
           delivered every raster IRQ one line late. */
        raster_irq_check();
    }
    return true;
}

/* ---- register access ----------------------------------------------------- */

/* TIBoyce-style waitloop skip: a SECOND $D011/$D012 read on the same raster
   line is a busy poll (loops read 5-10x per line); flag the CPU core to end
   the line batch instead of spinning through it. First read per line never
   warps, so one-shot raster reads keep their full line budget. */
static void raster_poll_check(void)
{
    static int poll_key = -1;
    int key = get_raster_counter() | (int)(frame_count << 9);
    if (key == poll_key)
        cpu_warp = 1;
    else
        poll_key = key;
}

uint8_t vic_read_register(uint8_t r)
{
    uint8_t retval;

    switch (r)
    {
    case 0x0: case 0x2: case 0x4: case 0x6:
    case 0x8: case 0xc: case 0xe:
        retval = mx[r >> 1]; break;
    case 0x1: case 0x3: case 0x5: case 0x7:
    case 0x9: case 0xb: case 0xd: case 0xf:
        retval = my[r >> 1]; break;
    case 0x10: retval = msbx; break;
    case 0x11: retval = cr1; raster_poll_check(); break;
    case 0x12: retval = raster_c; raster_poll_check(); break;
    case 0x15: retval = sprite_enabled; break;
    case 0x16: retval = cr2; break;
    case 0x17: retval = sprite_double_height; break;
    case 0x18: retval = mem_pointers; break;
    case 0x19:
        retval = (0xf & irq_status);
        if (retval != 0) retval |= 0x80;
        retval |= 0x70;
        break;
    case 0x1a: retval = (0xf0 | irq_enabled); break;
    case 0x1b: retval = sprite_priority; break;
    case 0x1c: retval = sprite_multicolor; break;
    case 0x1d: retval = sprite_double_width; break;
    /* collision latches clear on read (hardware behaviour); flush pending
       lines first so collisions from already-executed lines are latched */
    case 0x1e:
        vic_render_catchup();
        retval = sprite_coll_ss; sprite_coll_ss = 0; break;
    case 0x1f:
        vic_render_catchup();
        retval = sprite_coll_sb; sprite_coll_sb = 0; break;
    case 0x20: retval = border_color; break;
    case 0x21: case 0x22: case 0x23: case 0x24:
        retval = bgcolor[r - 0x21]; break;
    case 0x25: case 0x26:
        retval = sprite_shared_colors[r - 0x25]; break;
    case 0x27: case 0x28: case 0x29: case 0x2a:
    case 0x2b: case 0x2c: case 0x2d: case 0x2e:
        retval = sprite_colors[r - 0x27]; break;
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
    case 0x12: case 0x13: case 0x14: case 0x19: case 0x1a:
    case 0x1e: case 0x1f: /* collision latches ignore writes */
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

    switch (r)
    {
    case 0x0: case 0x2: case 0x4: case 0x6:
    case 0x8: case 0xc: case 0xe:
        mx[r >> 1] = v; break;
    case 0x1: case 0x3: case 0x5: case 0x7:
    case 0x9: case 0xb: case 0xd: case 0xf:
        my[r >> 1] = v; break;
    case 0x10: msbx = v; break;
    case 0x11:
        cr1 = (v & 0x7f);
        raster_irq &= 0xff;
        raster_irq |= (v & 0x80) << 1;
        set_graphic_mode();
        break;
    case 0x12:
        raster_irq = v | (raster_irq & (1 << 8));
        break;
    case 0x15: sprite_enabled = v; break;
    case 0x16: cr2 = v; set_graphic_mode(); break;
    case 0x17: sprite_double_height = v; break;
    case 0x18:
        char_mem   = (v & 0xe) << 10;
        screen_mem = (v & 0xf0) << 6;
        bitmap_mem = (v & 0x8) << 10;
        mem_pointers = v | (1 << 0);
        break;
    case 0x19: irq_status &= ~(v & 0xf); break;
    case 0x1a: irq_enabled = v; break;
    case 0x1b: sprite_priority = v; break;
    case 0x1c: sprite_multicolor = v; break;
    case 0x1d: sprite_double_width = v; break;
    case 0x20:
        border_color = v & 0x0f;
        break;
    case 0x21: case 0x22: case 0x23: case 0x24:
        bgcolor[r - 0x21] = v & 0x0f;
        break;
    case 0x25: case 0x26:
        sprite_shared_colors[r - 0x25] = v & 0x0f; break;
    case 0x27: case 0x28: case 0x29: case 0x2a:
    case 0x2b: case 0x2c: case 0x2d: case 0x2e:
        sprite_colors[r - 0x27] = v & 0x0f; break;
    default: break;
    }
}
