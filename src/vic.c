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
    vic_row_bg = IO_SLOT_BG0;
    vic_expand_mc_row();

    mc_asm_ready = true;
    for (cell = 0; cell < 40 && mc_asm_ready; ++cell)
    {
        uint8_t lut[4];
        lut[0] = IO_SLOT_BG0;
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

void vic_request_render(void)
{
    render_requested = true;
}

/* set when vic_emulate stops on an unsupported mode; reported by main() */
uint8_t vic_bad_mode;

/* Cold diagnostics for main(): copy of the display configuration. */
void vic_debug_snapshot(VicDebug *d)
{
    d->cr1 = cr1;
    d->cr2 = cr2;
    d->mem_ptrs = mem_pointers;
    d->border = border_color;
    d->bg0 = bgcolor[0];
    d->mode = graphic_mode;
    d->sprite_en = sprite_enabled;
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
    vic_raster_due = VIC_LINE_CYCLES;
    for (i = 0; i < 8; ++i)
        mx[i] = my[i] = sprite_colors[i] = 0;
    msbx = sprite_double_height = sprite_double_width = 0;
    sprite_enabled = sprite_priority = sprite_multicolor = 0;
    sprite_coll_ss = sprite_coll_sb = 0;
    sprite_shared_colors[0] = sprite_shared_colors[1] = 0;
    border_color = 0;
    bgcolor[0] = bgcolor[1] = bgcolor[2] = bgcolor[3] = 0;
    io_set_slot_color(IO_SLOT_BG0, 0);
    io_set_slot_color(IO_SLOT_BG1, 0);
    io_set_slot_color(IO_SLOT_BG2, 0);
    io_set_slot_color(IO_SLOT_BG3, 0);
    io_set_slot_color(IO_SLOT_BORDER, 0);
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
    int rstr = get_raster_counter();
    int y = rstr - VIC_FIRST_VISIBLE_LINE;
    int line = rstr - VIC_G_FIRST_LINE;
    int row = line >> 3;
    int char_row = line & 7;
    int xs = horizontal_scroll();
    int c, c0 = 0, c1 = VIC_G_COLS;
    unsigned int addr;
    const uint8_t *scr, *colram, *chardata;
    uint8_t *dst0 = io_text_row(y);

    if (!dst0)
        return;

    /* All source pointers are resolved ONCE per line: screen row and colour
       RAM are contiguous 40-byte runs, and the 2 KiB char generator block is
       either entirely inside or outside the char-ROM window. The VIC's 14
       address bits wrap inside the 16 KiB bank. */
    addr = (uint16_t)(cia2_vic_base_address() +
                      ((screen_mem + row * VIC_G_COLS) & 0x3fff));
    scr = mem_host_ptr((uint16_t)addr);
    colram = mem_host_ptr((uint16_t)(ADDR_COLOR_RAM + row * VIC_G_COLS));
    addr = (uint16_t)(cia2_vic_base_address() + (char_mem & 0x3fff));
    if ((addr >= 0x1000 && addr < 0x2000) || (addr >= 0x9000 && addr < 0xa000))
        chardata = mem_chars_rom_ptr() + (addr & 0x0fff);
    else
        chardata = mem_host_ptr((uint16_t)addr);

    memset(dst0, IO_SLOT_BG0, 320);

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
                /* top 2 char-code bits select the cell background slot */
                uint8_t cbg = (uint8_t)(IO_SLOT_BG0 + (sc >> 6));
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
                px = (data >> 6) & 3; v = (px == 3) ? mc3 : (px == 2) ? bg2 : (px == 1) ? IO_SLOT_BG1 : IO_SLOT_BG0; d[0] = d[1] = v;
                px = (data >> 4) & 3; v = (px == 3) ? mc3 : (px == 2) ? bg2 : (px == 1) ? IO_SLOT_BG1 : IO_SLOT_BG0; d[2] = d[3] = v;
                px = (data >> 2) & 3; v = (px == 3) ? mc3 : (px == 2) ? bg2 : (px == 1) ? IO_SLOT_BG1 : IO_SLOT_BG0; d[4] = d[5] = v;
                px = data & 3;        v = (px == 3) ? mc3 : (px == 2) ? bg2 : (px == 1) ? IO_SLOT_BG1 : IO_SLOT_BG0; d[6] = d[7] = v;
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
    int rstr = get_raster_counter();
    int y = rstr - VIC_FIRST_VISIBLE_LINE;
    int line = rstr - VIC_G_FIRST_LINE;
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

    /* bank base (bits 14-15) and 14-bit offset never overlap: compose with
       OR, all uint16_t (the compiler backend crashes on masked int adds) */
    addr = (uint16_t)(screen_mem + row * VIC_G_COLS);
    addr = (uint16_t)(base | (addr & 0x3fff));
    scr = mem_host_ptr(addr);
    colram = mem_host_ptr((uint16_t)(ADDR_COLOR_RAM + row * VIC_G_COLS));

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

    memset(dst0, IO_SLOT_BG0, 320);

    /* 38-column mode: render all 40 columns (border overlay is cropped) */

    lut[0] = IO_SLOT_BG0;

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
            vic_row_bg = IO_SLOT_BG0;
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

static void draw_mcsprite(int x, int y, int sprite, int row)
{
    uint16_t addr = get_sprite_ptr(sprite);
    bool behind = ISSET_BIT(sprite_priority, sprite);
    uint8_t *dst = io_text_row(y);
    int i, j;
    if (!dst)
        return;
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
                int px = x + i * 8 + 8 - j * 2;
                sprite_pixel_at(sprite, behind, dst, px, c);
                sprite_pixel_at(sprite, behind, dst, px + 1, c);
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

    /* side borders are cropped off the LCD; only the 24-row top/bottom
       border still needs the overlay */
    if (!ISSET_BIT(cr1, 3))
    {
        top_border_offset = 2;
        btm_border_offset = 4;
    }
    y_in_border = (y < VIC_G_FIRST_COL + top_border_offset ||
                   y >= VIC_G_RES_Y + VIC_G_FIRST_COL - btm_border_offset);

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
                    int new_x = (x + w + (i * 8 * swid) + (8 * swid) - (j * swid));

                    if (y_in_border)
                    {
                        unsigned int idx = (unsigned int)(new_x - IO_CROP_X);
                        if (idx < 320)
                            dst[idx] = IO_SLOT_BORDER;
                    }
                    else
                        sprite_pixel_at(sprite, behind, dst, new_x,
                                        sprite_colors[sprite]);
                }
            }
        }
    }
}

static void draw_raster_sprites(void)
{
    if (sprite_enabled != 0)
    {
        int rstr = get_raster_counter();
        int y = rstr - VIC_FIRST_VISIBLE_LINE;
        int sp_y = rstr - VIC_SPRITES_FIRST_LINE;
        int n;
        memset(sprite_line_mask, 0, sizeof(sprite_line_mask));
        for (n = 7; n >= 0; --n)
        {
            int height = is_double_height_sprite(n) ? VIC_SPRITE_HEIGHT * 2 : VIC_SPRITE_HEIGHT;
            if (is_sprite_enabled(n) && sp_y >= my[n] && sp_y < my[n] + height)
            {
                int row = sp_y - my[n];
                int x = VIC_SPRITES_FIRST_COL + sprite_x(n);
                if (is_double_height_sprite(n))
                    row = (sp_y - my[n]) / 2;
                if (is_multicolor_sprite(n))
                    draw_mcsprite(x, y, n, row);
                else
                    draw_sprite(x, y, n, row);
            }
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

        if (raster_irq_enabled_fn() && rstr == raster_irq)
        {
            irq_status |= (1 << 0);
            cpu_irq();
        }

        if (render_frame &&
            rstr >= VIC_FIRST_VISIBLE_LINE && rstr < VIC_LAST_VISIBLE_LINE)
        {
            int screen_y = rstr - VIC_FIRST_VISIBLE_LINE;
            bool text_row = (rstr >= VIC_G_FIRST_LINE &&
                             rstr < VIC_G_LAST_LINE && !is_screen_off());

            /* Text rows repaint the full 320px themselves; only border rows
               need the explicit fill. */
            if (!text_row)
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
            draw_raster_sprites();
        }

        vic_raster_due += is_bad_line() ? VIC_BAD_LINE_CYCLES : VIC_LINE_CYCLES;

        set_raster_counter(++rstr);
        if (rstr >= VIC_SCREEN_LINES)
        {
            io_screen_refresh();
            frame_count++;
            /* Wall-clock frame pacing: render at a steady real-time rate
               (~12 fps of the shared 32 kHz timer) regardless of how fast
               the emulation runs; key activity still forces a render. */
            if (frame_count < 300)
                render_frame = render_requested || ((frame_count & 15) == 0);
            else
            {
                static unsigned int last_render_ticks;
                unsigned int now = (unsigned int)timer_Get(1);
                render_frame = render_requested ||
                               (now - last_render_ticks >= 32768u / 12u);
                if (render_frame)
                    last_render_ticks = now;
            }
            render_requested = false;
            set_raster_counter(0);
        }
    }
    return true;
}

/* ---- register access ----------------------------------------------------- */

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
    case 0x11: retval = cr1; break;
    case 0x12: retval = raster_c; break;
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
    /* collision latches clear on read (hardware behaviour) */
    case 0x1e: retval = sprite_coll_ss; sprite_coll_ss = 0; break;
    case 0x1f: retval = sprite_coll_sb; sprite_coll_sb = 0; break;
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
        io_set_slot_color(IO_SLOT_BORDER, border_color);
        break;
    case 0x21: case 0x22: case 0x23: case 0x24:
        bgcolor[r - 0x21] = v & 0x0f;
        io_set_slot_color((uint8_t)(IO_SLOT_BG0 + (r - 0x21)), v);
        break;
    case 0x25: case 0x26:
        sprite_shared_colors[r - 0x25] = v & 0x0f; break;
    case 0x27: case 0x28: case 0x29: case 0x2a:
    case 0x2b: case 0x2c: case 0x2d: case 0x2e:
        sprite_colors[r - 0x27] = v & 0x0f; break;
    default: break;
    }
}
