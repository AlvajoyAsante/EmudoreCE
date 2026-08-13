/*
 * emudore-CE: MOS VIC-II video chip (C port of vic.cpp).
 *
 * Drawing calls go to io.c (graphx framebuffer) instead of an SDL texture.
 * The D() debug print on an unsupported graphic mode is kept (goes to the
 * emulator console in debug builds).
 */
#include "vic.h"
#include "cpu.h"
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
/* colors */
static uint8_t border_color;
static uint8_t bgcolor[4];
/* cpu sync */
static unsigned int next_raster_at;
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

void vic_reset(void)
{
    int i;
    raster_irq = raster_c = 0;
    irq_enabled = irq_status = 0;
    next_raster_at = VIC_LINE_CYCLES;
    for (i = 0; i < 8; ++i)
        mx[i] = my[i] = sprite_colors[i] = 0;
    msbx = sprite_double_height = sprite_double_width = 0;
    sprite_enabled = sprite_priority = sprite_multicolor = 0;
    sprite_shared_colors[0] = sprite_shared_colors[1] = 0;
    border_color = 0;
    bgcolor[0] = bgcolor[1] = bgcolor[2] = bgcolor[3] = 0;
    cr1 = cr2 = 0;
    frame_count = 0;
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

static uint8_t get_screen_char(int column, int row)
{
    return mem_vic_read_byte(screen_mem + (row * VIC_G_COLS) + column);
}

static uint8_t get_char_color(int column, int row)
{
    return mem_read_byte_no_io(ADDR_COLOR_RAM + (row * VIC_G_COLS) + column) & 0x0f;
}

static uint8_t get_char_data(int chr, int line)
{
    return mem_vic_read_byte(char_mem + (chr * 8) + line);
}

static uint8_t get_bitmap_data(int column, int row, int line)
{
    return mem_vic_read_byte(bitmap_mem + (row * VIC_G_COLS + column) * 8 + line);
}

static uint16_t get_sprite_ptr(int n)
{
    return VIC_SPRITE_SIZE * mem_vic_read_byte(screen_mem + VIC_SPRITE_PTRS_OFFSET + n);
}

/* ---- drawing ----------------------------------------------------------- */

static void draw_char(int x, int y, uint8_t data, uint8_t color)
{
    int i;
    for (i = 0; i < 8; ++i)
    {
        int xoffs = x + 8 - i + horizontal_scroll();
        if (xoffs > VIC_G_FIRST_COL + VIC_G_RES_X)
            continue;
        if (ISSET_BIT(data, i))
            io_screen_update_pixel(xoffs, y, color);
    }
}

static void draw_mcchar(int x, int y, uint8_t data, uint8_t color)
{
    int i;
    for (i = 0; i < 4; ++i)
    {
        uint8_t c;
        uint8_t cs = (data >> (i * 2)) & 0x3;
        switch (cs)
        {
        case 0: c = bgcolor[0]; break;
        case 1: c = bgcolor[1]; break;
        case 2: c = bgcolor[2]; break;
        default: c = color; break;
        }
        {
            int xoffs = x + 8 - i * 2 + horizontal_scroll();
            io_screen_update_pixel(xoffs, y, c);
            io_screen_update_pixel(xoffs + 1, y, c);
        }
    }
}

static void draw_bitmap(int x, int y, uint8_t data, uint8_t color)
{
    uint8_t forec = (color >> 4) & 0xf;
    uint8_t bgc = color & 0xf;
    int i;
    for (i = 0; i < 8; ++i)
    {
        int xoffs = x + 8 - i + horizontal_scroll();
        if (xoffs > VIC_G_FIRST_COL + VIC_G_RES_X)
            continue;
        if (ISSET_BIT(data, i))
            io_screen_update_pixel(xoffs, y, forec);
        else
            io_screen_update_pixel(xoffs, y, bgc);
    }
}

static void draw_mcbitmap(int x, int y, uint8_t data, uint8_t scolor, uint8_t rcolor)
{
    int i;
    for (i = 0; i < 4; ++i)
    {
        uint8_t c;
        uint8_t cs = (data >> (i * 2)) & 0x3;
        switch (cs)
        {
        case 0: c = bgcolor[0]; break;
        case 1: c = (scolor >> 4) & 0xf; break;
        case 2: c = scolor & 0xf; break;
        default: c = rcolor; break;
        }
        {
            int xoffs = x + 8 - i * 2 + horizontal_scroll();
            io_screen_update_pixel(xoffs, y, c);
            io_screen_update_pixel(xoffs + 1, y, c);
        }
    }
}

static void draw_raster_char_mode(void)
{
    int rstr = get_raster_counter();
    int y = rstr - VIC_FIRST_VISIBLE_LINE;
    int column;

    if (rstr >= VIC_G_FIRST_LINE && rstr < VIC_G_LAST_LINE && !is_screen_off())
    {
        io_screen_draw_rect(VIC_G_FIRST_COL, y, VIC_G_RES_X, bgcolor[0]);
        for (column = 0; column < VIC_G_COLS; ++column)
        {
            int x, line, row, char_row;
            uint8_t c, data, color;

            if (!ISSET_BIT(cr2, 3))
            {
                if (column == 0 || column == VIC_G_COLS - 1)
                    continue;
            }
            x = VIC_G_FIRST_COL + column * 8;
            line = rstr - VIC_G_FIRST_LINE;
            row = line / 8;
            char_row = line % 8;
            c = get_screen_char(column, row);
            data = get_char_data(c, char_row);
            color = get_char_color(column, row);

            if (graphic_mode == GM_MC_CHAR && ISSET_BIT(color, 3))
                draw_mcchar(x, y, data, (color & 0x7));
            else
                draw_char(x, y, data, color);
        }
    }
}

static void draw_raster_bitmap_mode(void)
{
    int rstr = get_raster_counter();
    int y = rstr - VIC_FIRST_VISIBLE_LINE;
    int column;

    if (rstr >= VIC_G_FIRST_LINE && rstr < VIC_G_LAST_LINE && !is_screen_off())
    {
        io_screen_draw_rect(VIC_G_FIRST_COL, y, VIC_G_RES_X, bgcolor[0]);
        for (column = 0; column < VIC_G_COLS; ++column)
        {
            int x = VIC_G_FIRST_COL + column * 8;
            int line = rstr - VIC_G_FIRST_LINE;
            int row = line / 8;
            int bitmap_row = line % 8;
            uint8_t data = get_bitmap_data(column, row, bitmap_row);
            uint8_t scolor = get_screen_char(column, row);
            uint8_t rcolor = get_char_color(column, row);

            if (graphic_mode == GM_BITMAP)
                draw_bitmap(x, y, data, scolor);
            else
                draw_mcbitmap(x, y, data, scolor, rcolor);
        }
    }
}

static void draw_mcsprite(int x, int y, int sprite, int row)
{
    uint16_t addr = get_sprite_ptr(sprite);
    int i, j;
    for (i = 0; i < 3; ++i)
    {
        uint8_t data = mem_vic_read_byte(addr + row * 3 + i);
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
                io_screen_update_pixel(x + i * 8 + 8 - j * 2, y, c);
                io_screen_update_pixel(x + i * 8 + 8 - j * 2 + 1, y, c);
            }
        }
    }
}

static void draw_sprite(int x, int y, int sprite, int row)
{
    int swid = is_double_width_sprite(sprite) ? 2 : 1;
    uint16_t addr = get_sprite_ptr(sprite);
    int w, i, j;
    for (w = 0; w < swid; ++w)
    {
        for (i = 0; i < 3; ++i)
        {
            uint8_t data = mem_vic_read_byte(addr + row * 3 + i);
            for (j = 0; j < 8; ++j)
            {
                if (ISSET_BIT(data, j))
                {
                    int new_x = (x + w + (i * 8 * swid) + (8 * swid) - (j * swid));
                    int color = sprite_colors[sprite];
                    int side_border_offset = 0, top_border_offset = 0, btm_border_offset = 0;

                    if (!ISSET_BIT(cr2, 3))
                        side_border_offset = 8;
                    if (!ISSET_BIT(cr1, 3))
                    {
                        top_border_offset = 2;
                        btm_border_offset = 4;
                    }
                    if (new_x <= VIC_G_FIRST_COL + side_border_offset ||
                        y < VIC_G_FIRST_COL + top_border_offset ||
                        new_x > VIC_G_RES_X + VIC_G_FIRST_COL - side_border_offset ||
                        y >= VIC_G_RES_Y + VIC_G_FIRST_COL - btm_border_offset)
                        color = border_color;

                    io_screen_update_pixel(new_x, y, color);
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
    /* re-raise interrupt if there are unacknowledged ones */
    if ((vic_read_register(0x19) & 0x80) != 0)
        cpu_irq();

    if (cpu.cycles >= next_raster_at)
    {
        int rstr = get_raster_counter();

        if (raster_irq_enabled_fn() && rstr == raster_irq)
        {
            irq_status |= (1 << 0);
            cpu_irq();
        }

        if (rstr >= VIC_FIRST_VISIBLE_LINE && rstr < VIC_LAST_VISIBLE_LINE)
        {
            int screen_y = rstr - VIC_FIRST_VISIBLE_LINE;
            io_screen_draw_border(screen_y, border_color);

            switch (graphic_mode)
            {
            case GM_CHAR:
            case GM_MC_CHAR:
                draw_raster_char_mode();
                break;
            case GM_BITMAP:
            case GM_MC_BITMAP:
                draw_raster_bitmap_mode();
                break;
            default:
                D("unsupported graphic mode: %d\n", graphic_mode);
                return false;
            }
            draw_raster_sprites();
        }

        next_raster_at += is_bad_line() ? VIC_BAD_LINE_CYCLES : VIC_LINE_CYCLES;

        set_raster_counter(++rstr);
        if (rstr >= VIC_SCREEN_LINES)
        {
            io_screen_refresh();
            frame_count++;
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
    case 0x20: border_color = v; break;
    case 0x21: case 0x22: case 0x23: case 0x24:
        bgcolor[r - 0x21] = v; break;
    case 0x25: case 0x26:
        sprite_shared_colors[r - 0x25] = v; break;
    case 0x27: case 0x28: case 0x29: case 0x2a:
    case 0x2b: case 0x2c: case 0x2d: case 0x2e:
        sprite_colors[r - 0x27] = v; break;
    default: break;
    }
}
