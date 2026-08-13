/*
 * emudore-CE: calculator screen + keypad replacing the SDL2 IO backend.
 *
 * SCREEN FIT
 *   The C64 visible area is 403x284 but the CE LCD is only 320x240, so we
 *   crop the side/top/bottom borders down to the 344x244 region that contains
 *   the 320x200 text area plus a minimal border, then squeeze 344 -> 320
 *   horizontally by taking 15 of every 16 source columns. Characters stay
 *   readable (no per-pixel rescale) and the full 40-column screen is shown.
 *
 * KEYBOARD
 *   The CE keypad is scanned into the C64 keyboard matrix each frame. Letter
 *   keys follow the TI-84+CE ALPHA labels, digits map directly, and the top
 *   row becomes the C64 function keys. CLEAR quits. 2nd = left SHIFT,
 *   ALPHA = right SHIFT, MODE = Commodore key, GRAPHVAR = Control.
 */
#include <graphx.h>
#include <keypadc.h>
#include <string.h>
#include "io.h"
#include "util.h"

#define LCD_W 320
#define LCD_H 240

/* Crop window inside the 403x284 C64 frame, and the squeeze mapping. */
#define CROP_X        40            /* VIC_G_FIRST_COL, skips the left border */
#define CROP_Y        38            /* skips the top border                   */
#define CROP_W        344           /* 320 text + 12 border each side         */
#define CROP_H        244           /* 200 text + 22 top/bottom border        */

static uint8_t keyboard_matrix[8];
static bool quit_requested;

/* Draw directly into the graphx current buffer (the back buffer once main()
   has called gfx_SetDrawBuffer()). */
#define draw_buf ((uint8_t *)gfx_vbuffer)

/* Column remapping table for the 344 -> 320 horizontal squeeze. */
static uint8_t src_column[CROP_W];

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
    int dropped;
    memset(keyboard_matrix, 0xff, sizeof(keyboard_matrix));
    quit_requested = false;
    /* 344 -> 320 : take 15 of every 16 source columns, WITHOUT the '/' operator
       (eZ80 integer division calls a 24-bit helper that clobbers IX/IY). */
    dropped = 0;
    for (i = 0; i < CROP_W; ++i)
    {
        if (i == (dropped + 1) * 15)
            ++dropped;
        src_column[i] = (uint8_t)(i + dropped);
    }
}

void io_shutdown(void)
{
}

/* ---- keypad -> C64 matrix --------------------------------------------- */

/* mask = 1 << column ; the byte is the C64 row. Positions come straight from
   the original emudore keymap_ (row, col). */
typedef struct { kb_lkey_t key; uint8_t row; uint8_t col; } KeyMap;

/* Letters follow the TI-84+CE ALPHA layer, digits map directly. */
static const KeyMap key_map[] = {
    /* --- ALPHA letters (CE key -> the C64 letter printed on it) --- */
    { kb_KeyMath,   1, 2 }, /* A */
    { kb_KeyApps,   3, 4 }, /* B */
    { kb_KeyPrgm,   2, 4 }, /* C */
    { kb_KeyRecip,  2, 2 }, /* D */
    { kb_KeySin,    1, 6 }, /* E */
    { kb_KeyCos,    2, 5 }, /* F */
    { kb_KeyTan,    3, 2 }, /* G */
    { kb_KeyPower,  3, 5 }, /* H */
    { kb_KeySquare, 4, 1 }, /* I */
    { kb_KeyComma,  4, 2 }, /* J */
    { kb_KeyLParen, 4, 5 }, /* K */
    { kb_KeyRParen, 5, 2 }, /* L */
    { kb_KeyDiv,    4, 4 }, /* M */
    { kb_KeyMul,    4, 7 }, /* N */
    { kb_KeyAdd,    4, 6 }, /* O */
    { kb_KeySto,    5, 1 }, /* P */
    { kb_KeyLn,     7, 6 }, /* Q */
    { kb_KeyLog,    2, 1 }, /* R */
    { kb_Key7,      1, 5 }, /* S */
    { kb_Key8,      2, 6 }, /* T */
    { kb_Key9,      3, 6 }, /* U */
    { kb_Key4,      3, 7 }, /* V */
    { kb_Key5,      1, 1 }, /* W */
    { kb_Key6,      2, 7 }, /* X */
    { kb_Key1,      3, 1 }, /* Y */
    { kb_Key2,      1, 4 }, /* Z */

    /* --- digits (the number keys keep their face value) --- */
    { kb_Key0,      4, 3 }, /* 0 */
    { kb_Key3,      1, 0 }, /* 3 */
    /* 1/2/4..9 double as ALPHA letters above; the C64 numbers they produce
       are reached with 2nd (shift) below. */

    /* --- function keys (top row) --- */
    { kb_KeyYequ,   0, 4 }, /* F1 */
    { kb_KeyWindow, 0, 4 }, /* F3 */
    { kb_KeyZoom,   0, 4 }, /* F5 */
    { kb_KeyTrace,  0, 4 }, /* F7 */

    /* --- control / editing --- */
    { kb_KeyEnter,    0, 1 }, /* RETURN         */
    { kb_KeyDel,      0, 0 }, /* DEL (backspace)*/
    { kb_Key2nd,      1, 7 }, /* left SHIFT     */
    { kb_KeyAlpha,    6, 4 }, /* right SHIFT    */
    { kb_KeyMode,     7, 5 }, /* Commodore key  */
    { kb_KeyGraphVar, 7, 2 }, /* Control        */

    /* --- cursors --- */
    { kb_KeyLeft,   0, 2 }, /* CRSR left/right */
    { kb_KeyRight,  0, 2 },
    { kb_KeyUp,     0, 7 }, /* CRSR up/down    */
    { kb_KeyDown,   0, 7 },
};

/* Symbols reached by holding 2nd (C64 SHIFT) plus a CE key. */
static const KeyMap shift_map[] = {
    { kb_KeyEnter,  7, 4 }, /* SPACE */
    { kb_KeyComma,  5, 7 }, /* ,     */
    { kb_KeyDecPnt, 5, 4 }, /* .     */
    { kb_KeyDiv,    6, 7 }, /* /     */
    { kb_KeySub,    5, 3 }, /* -     */
    { kb_KeyAdd,    5, 0 }, /* +     */
    { kb_KeyMul,    6, 1 }, /* *     */
    { kb_KeyStat,   5, 6 }, /* @     */
    { kb_KeyChs,    6, 2 }, /* ;     */
    { kb_KeyVars,   6, 5 }, /* =     */
};

static void apply_map(const KeyMap *map, int count)
{
    int i;
    for (i = 0; i < count; ++i)
    {
        if (kb_IsDown(map[i].key))
            keyboard_matrix[map[i].row] &= (uint8_t)~(1 << map[i].col);
    }
}

bool io_emulate(void)
{
    memset(keyboard_matrix, 0xff, sizeof(keyboard_matrix));
    kb_Scan();

    if (kb_IsDown(kb_KeyClear))
        quit_requested = true;

    apply_map(key_map, (int)(sizeof(key_map) / sizeof(key_map[0])));
    if (kb_IsDown(kb_Key2nd))
        apply_map(shift_map, (int)(sizeof(shift_map) / sizeof(shift_map[0])));

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
    int cx = x - CROP_X;
    int cy = y - CROP_Y;
    int dx;
    if ((unsigned)cx >= CROP_W || (unsigned)cy >= CROP_H || cy >= LCD_H)
        return;
    dx = (int)src_column[cx];
    if (dx < LCD_W)
        draw_buf[cy * LCD_W + dx] = (uint8_t)(color & 0x0f);
}

void io_screen_draw_rect(int x, int y, int w, uint8_t color)
{
    int cy = y - CROP_Y;
    int i;
    if ((unsigned)cy >= CROP_H || cy >= LCD_H)
        return;
    for (i = 0; i < w; ++i)
        io_screen_update_pixel(x + i, y, color);
}

void io_screen_draw_border(int y, uint8_t color)
{
    int cy = y - CROP_Y;
    if ((unsigned)cy >= LCD_H)
        return;
    memset(&draw_buf[cy * LCD_W], color & 0x0f, LCD_W);
}

void io_screen_refresh(void)
{
    gfx_SetPalette(c64_palette, sizeof(c64_palette), 0);
    gfx_BlitBuffer();
}
