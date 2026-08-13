/*
 * emudore-CE: calculator screen + keypad replacing the SDL2 IO backend.
 *
 * SCREEN FIT
 *   The C64 text area is exactly 320x200, a perfect width match for the
 *   320x240 LCD. We crop the borders away 1:1 (no rescaling) and centre the
 *   200 text lines vertically, so the 20-line bands above/below show the real
 *   border colour. Every LCD row is repainted each frame - no stale pixels.
 *
 * KEYBOARD
 *   Letters follow the CE's printed green ALPHA labels (Math=A ... 2=Z).
 *   Digit keys type digits; hold ALPHA for their letters (O P Q T U V Y Z),
 *   hold 2nd for BASIC symbols (/ * - + = , : " and shifted digits).
 *   Y=/Window/Zoom/Trace = F1/F3/F5/F7, X,T,theta,n = SPACE, Mode = C=,
 *   Vars = RUN/STOP, Del = DEL (2nd+Del = CLR/HOME). CLEAR quits.
 */
#include <graphx.h>
#include <keypadc.h>
#include <string.h>
#include "io.h"
#include "vic.h"
#include "util.h"

#define LCD_W 320
#define LCD_H 240

/* Crop offsets inside the 403x284 C64 frame (frame coords -> LCD coords).
   CROP_X is 43, not VIC_G_FIRST_COL (42), because draw_char emits its first
   pixel at x+1; CROP_Y = 22 centres the text area (rows 42..241) on the LCD. */
#define CROP_X        43
#define CROP_Y        22

static uint8_t keyboard_matrix[8];
static bool quit_requested;

/* Per-row framebuffer pointers (single-buffer mode draws straight into the
   visible screen; the second VRAM half is the C64's RAM). Avoids a 24-bit
   multiply per pixel. */
static uint8_t *row_ptr[LCD_H];

/* C64 palette (original emudore RGB values) converted to graphx 1555. */
static const uint16_t c64_palette[16] = {
    gfx_RGBTo1555(0x00, 0x00, 0x00), /* 0  black        */
    gfx_RGBTo1555(0xff, 0xff, 0xff), /* 1  white        */
    gfx_RGBTo1555(0xab, 0x31, 0x26), /* 2  red          */
    gfx_RGBTo1555(0x66, 0xda, 0xff), /* 3  cyan         */
    gfx_RGBTo1555(0xbb, 0x3f, 0xb8), /* 4  purple       */
    gfx_RGBTo1555(0x55, 0xce, 0x58), /* 5  green        */
    gfx_RGBTo1555(0x1d, 0x0e, 0x97), /* 6  blue         */
    gfx_RGBTo1555(0xea, 0xf5, 0x7c), /* 7  yellow       */
    gfx_RGBTo1555(0xb9, 0x74, 0x18), /* 8  orange       */
    gfx_RGBTo1555(0x78, 0x53, 0x00), /* 9  brown        */
    gfx_RGBTo1555(0xdd, 0x93, 0x87), /* 10 light red    */
    gfx_RGBTo1555(0x5b, 0x5b, 0x5b), /* 11 dark grey    */
    gfx_RGBTo1555(0x8b, 0x8b, 0x8b), /* 12 grey         */
    gfx_RGBTo1555(0xb0, 0xf4, 0xac), /* 13 light green  */
    gfx_RGBTo1555(0xaa, 0x9d, 0xef), /* 14 light blue   */
    gfx_RGBTo1555(0xb8, 0xb8, 0xb8)  /* 15 light grey   */
};

void io_init(void)
{
    int i;
    uint8_t *row = gfx_vram;
    memset(keyboard_matrix, 0xff, sizeof(keyboard_matrix));
    quit_requested = false;
    for (i = 0; i < LCD_H; ++i)
    {
        row_ptr[i] = row;
        row += LCD_W;
    }
    /* C64 entries 0/1 are black/white, same as the boot palette, so the boot
       status text stays readable. */
    gfx_SetPalette(c64_palette, sizeof(c64_palette), 0);
}

void io_shutdown(void)
{
}

/* ---- keypad -> C64 matrix --------------------------------------------- */

/* mask = 1 << column ; the byte is the C64 row. Positions come straight from
   the original emudore keymap_ (row, col). shift=1 also presses C64 LSHIFT. */
typedef struct { kb_lkey_t key; uint8_t row; uint8_t col; uint8_t shift; } KeyMap;

/* Base layer: letters follow the CE's printed green ALPHA labels, plus
   controls. Digit keys live in separate layers (digits vs ALPHA letters). */
static const KeyMap base_map[] = {
    /* --- letters (CE key -> the letter printed on it) --- */
    { kb_KeyMath,   1, 2, 0 }, /* A */
    { kb_KeyApps,   3, 4, 0 }, /* B */
    { kb_KeyPrgm,   2, 4, 0 }, /* C */
    { kb_KeyRecip,  2, 2, 0 }, /* D */
    { kb_KeySin,    1, 6, 0 }, /* E */
    { kb_KeyCos,    2, 5, 0 }, /* F */
    { kb_KeyTan,    3, 2, 0 }, /* G */
    { kb_KeyPower,  3, 5, 0 }, /* H */
    { kb_KeySquare, 4, 1, 0 }, /* I */
    { kb_KeyComma,  4, 2, 0 }, /* J */
    { kb_KeyLParen, 4, 5, 0 }, /* K */
    { kb_KeyRParen, 5, 2, 0 }, /* L */
    { kb_KeyDiv,    4, 4, 0 }, /* M */
    { kb_KeyLog,    4, 7, 0 }, /* N */
    { kb_KeyMul,    2, 1, 0 }, /* R */
    { kb_KeyLn,     1, 5, 0 }, /* S */
    { kb_KeySub,    1, 1, 0 }, /* W */
    { kb_KeySto,    2, 7, 0 }, /* X */

    /* --- always-available symbols --- */
    { kb_KeyAdd,      5, 0, 0 }, /* +     */
    { kb_KeyDecPnt,   5, 4, 0 }, /* .     */
    { kb_KeyChs,      6, 2, 0 }, /* ;     */
    { kb_KeyStat,     5, 6, 0 }, /* @     */
    { kb_KeyGraphVar, 7, 4, 0 }, /* SPACE (X,T,theta,n key) */

    /* --- function keys (top row) --- */
    { kb_KeyYequ,   0, 4, 0 }, /* F1 */
    { kb_KeyWindow, 0, 5, 0 }, /* F3 */
    { kb_KeyZoom,   0, 6, 0 }, /* F5 */
    { kb_KeyTrace,  0, 3, 0 }, /* F7 */

    /* --- control / editing --- */
    { kb_KeyEnter, 0, 1, 0 }, /* RETURN          */
    { kb_KeyDel,   0, 0, 0 }, /* DEL (backspace) */
    { kb_KeyMode,  7, 5, 0 }, /* Commodore key   */
    { kb_KeyVars,  7, 7, 0 }, /* RUN/STOP        */

    /* --- cursors (shift selects the left/up direction) --- */
    { kb_KeyRight,  0, 2, 0 },
    { kb_KeyLeft,   0, 2, 1 },
    { kb_KeyDown,   0, 7, 0 },
    { kb_KeyUp,     0, 7, 1 },
};

/* Digit keys, default layer: face value. */
static const KeyMap digit_map[] = {
    { kb_Key1, 7, 0, 0 }, { kb_Key2, 7, 3, 0 }, { kb_Key3, 1, 0, 0 },
    { kb_Key4, 1, 3, 0 }, { kb_Key5, 2, 0, 0 }, { kb_Key6, 2, 3, 0 },
    { kb_Key7, 3, 0, 0 }, { kb_Key8, 3, 3, 0 }, { kb_Key9, 4, 0, 0 },
    { kb_Key0, 4, 3, 0 },
};

/* Digit keys while ALPHA is held: their printed green letters. */
static const KeyMap alpha_map[] = {
    { kb_Key7,      4, 6, 0 }, /* O */
    { kb_Key8,      5, 1, 0 }, /* P */
    { kb_Key9,      7, 6, 0 }, /* Q */
    { kb_Key4,      2, 6, 0 }, /* T */
    { kb_Key5,      3, 6, 0 }, /* U */
    { kb_Key6,      3, 7, 0 }, /* V */
    { kb_Key1,      3, 1, 0 }, /* Y */
    { kb_Key2,      1, 4, 0 }, /* Z */
    { kb_Key0,      7, 4, 0 }, /* space */
    { kb_KeyDecPnt, 5, 5, 0 }, /* :     */
    { kb_KeyChs,    6, 7, 1 }, /* ?     */
};

/* 2nd layer: BASIC operators/symbols; shifted digits give C64 punctuation
   (2nd+2 = ", 2nd+8 = ( ...). Replaces the other layers while 2nd is held. */
static const KeyMap snd_map[] = {
    { kb_KeyDiv,    6, 7, 0 }, /* /       */
    { kb_KeyMul,    6, 1, 0 }, /* *       */
    { kb_KeySub,    5, 3, 0 }, /* -       */
    { kb_KeyAdd,    5, 0, 0 }, /* +       */
    { kb_KeySto,    6, 5, 0 }, /* =       */
    { kb_KeyComma,  5, 7, 0 }, /* ,       */
    { kb_KeyDecPnt, 5, 5, 0 }, /* :       */
    { kb_KeyPower,  6, 6, 0 }, /* up-arrow (exponent) */
    { kb_KeyDel,    6, 3, 0 }, /* CLR/HOME */
    { kb_KeyEnter,  0, 1, 1 }, /* shift+RETURN */
    { kb_Key1, 7, 0, 1 }, { kb_Key2, 7, 3, 1 }, { kb_Key3, 1, 0, 1 },
    { kb_Key4, 1, 3, 1 }, { kb_Key5, 2, 0, 1 }, { kb_Key6, 2, 3, 1 },
    { kb_Key7, 3, 0, 1 }, { kb_Key8, 3, 3, 1 }, { kb_Key9, 4, 0, 1 },
    { kb_Key0, 4, 3, 1 },
    { kb_KeyRight,  0, 2, 0 },
    { kb_KeyLeft,   0, 2, 1 },
    { kb_KeyDown,   0, 7, 0 },
    { kb_KeyUp,     0, 7, 1 },
};

static void apply_map(const KeyMap *map, int count)
{
    int i;
    for (i = 0; i < count; ++i)
    {
        if (kb_IsDown(map[i].key))
        {
            keyboard_matrix[map[i].row] &= (uint8_t)~(1 << map[i].col);
            if (map[i].shift)
                keyboard_matrix[1] &= (uint8_t)~(1 << 7); /* LSHIFT */
        }
    }
}

#define MAP_COUNT(m) ((int)(sizeof(m) / sizeof((m)[0])))

bool io_emulate(void)
{
    /* io_emulate runs once per 8 raster lines, but kb_Scan is a slow
       hardware scan: only rescan once per emulated video frame. */
    static unsigned int scanned_frame = (unsigned int)-1;
    unsigned int frame = vic_frames();

    if (frame == scanned_frame)
        return !quit_requested;
    scanned_frame = frame;

    memset(keyboard_matrix, 0xff, sizeof(keyboard_matrix));
    kb_Scan();

    if (kb_IsDown(kb_KeyClear))
        quit_requested = true;

    if (kb_IsDown(kb_Key2nd))
        apply_map(snd_map, MAP_COUNT(snd_map));
    else
    {
        apply_map(base_map, MAP_COUNT(base_map));
        if (kb_IsDown(kb_KeyAlpha))
            apply_map(alpha_map, MAP_COUNT(alpha_map));
        else
            apply_map(digit_map, MAP_COUNT(digit_map));
    }

    return !quit_requested;
}

uint8_t io_keyboard_matrix_row(int col)
{
    if (col < 0 || col > 7)
        return 0xff;
    return keyboard_matrix[col];
}

bool io_quit_requested(void)
{
    return quit_requested;
}

/* ---- drawing ------------------------------------------------------------ */

void io_screen_update_pixel(int x, int y, uint8_t color)
{
    unsigned int dx = (unsigned int)(x - CROP_X);
    unsigned int dy = (unsigned int)(y - CROP_Y);
    if (dx >= LCD_W || dy >= LCD_H)
        return;
    row_ptr[dy][dx] = color & 0x0f;
}

void io_screen_draw_rect(int x, int y, int w, uint8_t color)
{
    /* +1: glyphs emit at x+1 (see CROP_X), keep the bg fill aligned to them */
    int dx = x - CROP_X + 1;
    unsigned int dy = (unsigned int)(y - CROP_Y);
    if (dy >= LCD_H)
        return;
    if (dx < 0)
    {
        w += dx;
        dx = 0;
    }
    if (dx + w > LCD_W)
        w = LCD_W - dx;
    if (w <= 0)
        return;
    memset(row_ptr[dy] + dx, color & 0x0f, (size_t)w);
}

void io_screen_draw_border(int y, uint8_t color)
{
    unsigned int dy = (unsigned int)(y - CROP_Y);
    (void)color;
    if (dy >= LCD_H)
        return;
    /* Black instead of the C64 border colour: the letterbox bands blend into
       the calculator bezel so the text area reads as a centred screen. */
    memset(row_ptr[dy], 0, LCD_W);
}

uint8_t *io_text_row(int y)
{
    unsigned int dy = (unsigned int)(y - CROP_Y);
    return (dy < LCD_H) ? row_ptr[dy] : NULL;
}

void io_screen_refresh(void)
{
    /* Single-buffered, palette loaded once in io_init: nothing to do. */
}
