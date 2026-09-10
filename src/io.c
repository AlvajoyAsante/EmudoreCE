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
 *   GRAPH pauses for settings; the top-row function bank can be odd or even.
 */
#include <graphx.h>
#include <keypadc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timers.h>
#include "io.h"
#include "vic.h"
#include "util.h"

#define LCD_W 320
#define LCD_H 240

/* Crop offsets live in io.h (IO_CROP_X/IO_CROP_Y): CROP_X is 43, not
   VIC_G_FIRST_COL (42), because draw_char emits its first pixel at x+1;
   CROP_Y = 22 centres the text area (rows 42..241) on the LCD. */
#define CROP_X IO_CROP_X
#define CROP_Y IO_CROP_Y

static uint8_t keyboard_matrix[8];
static uint8_t joystick2 = 0xff;
static bool quit_requested;
static bool indicators_enabled = true;
static bool even_function_keys;
static uint8_t indicator_top = 0xff;
static uint8_t indicator_bottom = 0xff;
static uint8_t indicator_layer = 0xff;

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

void io_reload_palette(void)
{
    int i;
    /* ALL 256 entries repeat the 16 C64 colours: any framebuffer byte
       (including tag banks 16-63) displays a legal colour. */
    for (i = 0; i < 256; ++i)
        gfx_palette[i] = c64_palette[i & 15];
}

void io_init(void)
{
    int i;
    uint8_t *row = gfx_vram;
    memset(keyboard_matrix, 0xff, sizeof(keyboard_matrix));
    quit_requested = false;
    indicators_enabled = true;
    even_function_keys = false;
    indicator_top = 0xff;
    indicator_bottom = 0xff;
    indicator_layer = 0xff;
    for (i = 0; i < LCD_H; ++i)
    {
        row_ptr[i] = row;
        row += LCD_W;
    }
    /* C64 entries 0/1 are black/white, same as the boot palette, so the boot
       status text stays readable. Entries 16-63 mirror 0-15 (framebuffer tag
       bits display identically). */
    io_reload_palette();
}

uint16_t io_palette_probe(uint8_t index)
{
    return gfx_palette[index];
}

void io_shutdown(void)
{
    kb_SetMode(MODE_1_INDISCRIMINATE); /* OS default after continuous mode */
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

static void draw_key_indicator(uint8_t column, uint8_t top,
                               const char *key, const char *action,
                               bool pressed)
{
    int left = column * 64;
    uint8_t background = pressed ? 13 : 0;
    uint8_t row;

    for (row = top; row < top + 20; ++row)
        memset(row_ptr[row] + left, background, 64);
    gfx_SetTextFGColor(pressed ? 0 : 1);
    gfx_SetTextBGColor(background);
    gfx_SetTextTransparentColor(255);
    gfx_PrintStringXY(key, left + (64 - gfx_GetStringWidth(key)) / 2, top + 1);
    gfx_PrintStringXY(action, left + (64 - gfx_GetStringWidth(action)) / 2,
                      top + 11);
}

static void update_key_indicators(void)
{
    static const char *const top_keys[] = {
        "Y=", "WINDOW", "ZOOM", "TRACE", "GRAPH"
    };
    static const char *const odd_functions[] = { "F1", "F3", "F5", "F7" };
    static const char *const even_functions[] = { "F2", "F4", "F6", "F8" };
    static const char *const bottom_keys[] = {
        "2nd", "ALPHA", "MODE", "VARS", "CLEAR"
    };
    static const char *const bottom_actions[] = {
        "FIRE", "ABC", "C=", "STOP", "EXIT"
    };
    uint8_t top = 0;
    uint8_t bottom = 0;
    uint8_t layer = 0;
    uint8_t column;
    uint8_t mask = 1;

    if (!indicators_enabled)
        return;

    if (!(keyboard_matrix[0] & 0x10)) top |= 0x01;
    if (!(keyboard_matrix[0] & 0x20)) top |= 0x02;
    if (!(keyboard_matrix[0] & 0x40)) top |= 0x04;
    if (!(keyboard_matrix[0] & 0x08)) top |= 0x08;
    if (kb_IsDown(kb_KeyGraph)) top |= 0x10;
    if (kb_IsDown(kb_Key2nd)) bottom |= 0x01;
    if (kb_IsDown(kb_KeyAlpha)) bottom |= 0x02;
    if (!(keyboard_matrix[7] & 0x20)) bottom |= 0x04;
    if (!(keyboard_matrix[7] & 0x80)) bottom |= 0x08;
    if (kb_IsDown(kb_KeyClear)) bottom |= 0x10;
    if (bottom & 0x01)
        layer = 1;
    else if (even_function_keys || !(keyboard_matrix[1] & 0x80))
        layer = 2;

    if (top == indicator_top && bottom == indicator_bottom &&
        layer == indicator_layer)
        return;

    gfx_SetTextScale(1, 1);
    for (column = 0; column < 5; ++column)
    {
        if (((top ^ indicator_top) & mask) || layer != indicator_layer)
        {
            const char *action = "MENU";
            if (column < 4)
                action = layer == 1 ? "--" :
                         (layer == 2 ? even_functions[column] : odd_functions[column]);
            draw_key_indicator(column, 0, top_keys[column], action,
                               (top & mask) != 0);
        }
        if (indicator_bottom == 0xff || ((bottom ^ indicator_bottom) & mask))
            draw_key_indicator(column, 220, bottom_keys[column],
                               bottom_actions[column], (bottom & mask) != 0);
        mask += mask;
    }
    gfx_SetTextFGColor(1);
    gfx_SetTextBGColor(0);
    indicator_top = top;
    indicator_bottom = bottom;
    indicator_layer = layer;
}

#define SETTINGS_LEFT 48
#define SETTINGS_TOP 76
#define SETTINGS_WIDTH 224
#define SETTINGS_HEIGHT 88

static void draw_settings_menu(uint8_t selected)
{
    static const char *const labels[] = {
        "Resume", "Key indicators", "Function keys", "Quit emulator"
    };
    uint8_t entry;

    gfx_SetTextScale(1, 1);
    gfx_SetTextTransparentColor(255);
    gfx_SetColor(0);
    gfx_FillRectangle_NoClip(SETTINGS_LEFT, SETTINGS_TOP,
                            SETTINGS_WIDTH, SETTINGS_HEIGHT);
    gfx_SetColor(12);
    gfx_Rectangle_NoClip(SETTINGS_LEFT, SETTINGS_TOP,
                        SETTINGS_WIDTH, SETTINGS_HEIGHT);
    gfx_SetTextFGColor(1);
    gfx_SetTextBGColor(0);
    gfx_PrintStringXY("Settings", SETTINGS_LEFT + 12, SETTINGS_TOP + 7);
    gfx_SetTextFGColor(15);
    gfx_PrintStringXY("PAUSED",
                      SETTINGS_LEFT + SETTINGS_WIDTH - 12 - gfx_GetStringWidth("PAUSED"),
                      SETTINGS_TOP + 7);

    for (entry = 0; entry < 4; ++entry)
    {
        uint8_t top = SETTINGS_TOP + 24 + entry * 14;
        uint8_t background = entry == selected ? 13 : 0;
        const char *value = NULL;

        gfx_SetColor(background);
        gfx_FillRectangle_NoClip(SETTINGS_LEFT + 8, top - 2,
                                SETTINGS_WIDTH - 16, 12);
        gfx_SetTextBGColor(background);
        gfx_SetTextFGColor(entry == selected ? 0 : 1);
        gfx_PrintStringXY(labels[entry], SETTINGS_LEFT + 12, top);
        if (entry == 1)
            value = indicators_enabled ? "[x]" : "[ ]";
        else if (entry == 2)
            value = even_function_keys ? "F2/4/6/8" : "F1/3/5/7";
        if (value)
            gfx_PrintStringXY(value,
                              SETTINGS_LEFT + SETTINGS_WIDTH - 12 - gfx_GetStringWidth(value),
                              top);
    }
}

static void wait_for_key_release(void)
{
    do
    {
        kb_Scan();
    }
    while (kb_AnyKey());
}

static void __attribute__((noinline)) show_settings_menu(void)
{
    bool timer_was_running = (timer_Control & 1u) != 0;
    uint8_t *saved_pixels;
    uint8_t *saved_row;
    uint8_t row;
    uint8_t selected = 0;
    bool key_held = false;
    bool redraw = true;
    bool finished = false;

    timer_Disable(1);
    saved_pixels = malloc(SETTINGS_WIDTH * SETTINGS_HEIGHT);
    wait_for_key_release();

    if (saved_pixels)
    {
        saved_row = saved_pixels;
        for (row = SETTINGS_TOP; row < SETTINGS_TOP + SETTINGS_HEIGHT; ++row)
        {
            memcpy(saved_row, row_ptr[row] + SETTINGS_LEFT, SETTINGS_WIDTH);
            saved_row += SETTINGS_WIDTH;
        }

        while (!finished)
        {
            if (redraw)
            {
                draw_settings_menu(selected);
                redraw = false;
            }
            kb_Scan();
            if (!kb_AnyKey())
            {
                key_held = false;
                continue;
            }
            if (key_held)
                continue;
            key_held = true;

            if (kb_IsDown(kb_KeyGraph))
                finished = true;
            else if (kb_IsDown(kb_KeyClear))
            {
                quit_requested = true;
                finished = true;
            }
            else if (kb_IsDown(kb_KeyUp) && selected > 0)
            {
                --selected;
                redraw = true;
            }
            else if (kb_IsDown(kb_KeyDown) && selected < 3)
            {
                ++selected;
                redraw = true;
            }
            else if (kb_IsDown(kb_KeyEnter) || kb_IsDown(kb_KeyLeft) ||
                     kb_IsDown(kb_KeyRight))
            {
                if (selected == 0 && kb_IsDown(kb_KeyEnter))
                    finished = true;
                else if (selected == 1)
                {
                    if (kb_IsDown(kb_KeyLeft))
                        indicators_enabled = false;
                    else if (kb_IsDown(kb_KeyRight))
                        indicators_enabled = true;
                    else
                        indicators_enabled = !indicators_enabled;
                    redraw = true;
                }
                else if (selected == 2)
                {
                    if (kb_IsDown(kb_KeyLeft))
                        even_function_keys = false;
                    else if (kb_IsDown(kb_KeyRight))
                        even_function_keys = true;
                    else
                        even_function_keys = !even_function_keys;
                    redraw = true;
                }
                else if (selected == 3 && kb_IsDown(kb_KeyEnter))
                {
                    quit_requested = true;
                    finished = true;
                }
            }
        }

        wait_for_key_release();
        saved_row = saved_pixels;
        for (row = SETTINGS_TOP; row < SETTINGS_TOP + SETTINGS_HEIGHT; ++row)
        {
            memcpy(row_ptr[row] + SETTINGS_LEFT, saved_row, SETTINGS_WIDTH);
            saved_row += SETTINGS_WIDTH;
        }
        free(saved_pixels);
    }
    else
    {
        memset(row_ptr[220], 0, LCD_W * 20);
        gfx_SetTextScale(1, 1);
        gfx_SetTextFGColor(1);
        gfx_SetTextBGColor(0);
        gfx_SetTextTransparentColor(255);
        gfx_PrintStringXY("Menu unavailable: not enough memory", 4, 226);
        do
        {
            kb_Scan();
        }
        while (!kb_AnyKey());
        if (kb_IsDown(kb_KeyClear))
            quit_requested = true;
        wait_for_key_release();
    }

    memset(row_ptr[0], 0, LCD_W * 20);
    memset(row_ptr[220], 0, LCD_W * 20);
    indicator_top = 0xff;
    indicator_bottom = 0xff;
    indicator_layer = 0xff;
    gfx_SetTextFGColor(1);
    gfx_SetTextBGColor(0);
    kb_SetMode(MODE_3_CONTINUOUS);
    if (timer_was_running)
        timer_Control |= 1u;
}

bool io_emulate(void)
{
    /* Rescan the keypad at a real-time 60 Hz (546 ticks of the 32 kHz timer
       cia1.c runs): matches the KERNAL's real-time IRQ scan rate, so short
       taps register no matter how slow the emulated frame rate is. */
    static unsigned int last_scan_ticks;
    static bool continuous;
    bool keys_changed = false;
    unsigned int now = (unsigned int)timer_Get(1);

    if (now - last_scan_ticks < 546)
        return !quit_requested;
    last_scan_ticks = now;

    /* kb_Scan() busy-waits a full hardware scan (~1.3 ms measured): put the
       controller in continuous mode ONCE instead and just read kb_Data.
       Set lazily because boot/menu code calls kb_Scan after io_init. */
    if (!continuous)
    {
        kb_SetMode(MODE_3_CONTINUOUS);
        continuous = true;
    }

    memset(keyboard_matrix, 0xff, sizeof(keyboard_matrix));
    joystick2 = 0xff;

    if (kb_IsDown(kb_KeyClear))
        quit_requested = true;

    if (!quit_requested && kb_IsDown(kb_KeyGraph))
    {
        update_key_indicators();
        show_settings_menu();
        update_key_indicators();
        return !quit_requested;
    }

    if (kb_IsDown(kb_Key2nd))
        apply_map(snd_map, MAP_COUNT(snd_map));
    else
    {
        apply_map(base_map, MAP_COUNT(base_map));
        if (even_function_keys && (keyboard_matrix[0] & 0x78) != 0x78)
            keyboard_matrix[1] &= (uint8_t)~0x80;
        if (kb_IsDown(kb_KeyAlpha))
            apply_map(alpha_map, MAP_COUNT(alpha_map));
        else
            apply_map(digit_map, MAP_COUNT(digit_map));
    }

    /* Joystick port 2: arrows + [2nd] fire (active low). Games poll this
       through CIA1 $DC00; the keyboard cursor mapping stays active too. */
    joystick2 = 0xff;
    if (kb_IsDown(kb_KeyUp))    joystick2 &= (uint8_t)~0x01;
    if (kb_IsDown(kb_KeyDown))  joystick2 &= (uint8_t)~0x02;
    if (kb_IsDown(kb_KeyLeft))  joystick2 &= (uint8_t)~0x04;
    if (kb_IsDown(kb_KeyRight)) joystick2 &= (uint8_t)~0x08;
    if (kb_IsDown(kb_Key2nd))   joystick2 &= (uint8_t)~0x10;

    /* Key state changed -> render the next frame so typing feels instant
       even with aggressive frame skipping. */
    {
        static uint8_t prev_matrix[8];
        static uint8_t prev_joy;
        if (memcmp(prev_matrix, keyboard_matrix, sizeof(prev_matrix)) ||
            prev_joy != joystick2)
        {
            memcpy(prev_matrix, keyboard_matrix, sizeof(prev_matrix));
            prev_joy = joystick2;
            vic_request_render();
            keys_changed = true;
        }
    }

    if (indicators_enabled &&
        (keys_changed || indicator_layer == 0xff ||
         (kb_IsDown(kb_KeyAlpha) != 0) != ((indicator_bottom & 0x02) != 0) ||
         (kb_IsDown(kb_KeyClear) != 0) != ((indicator_bottom & 0x10) != 0)))
        update_key_indicators();
    return !quit_requested;
}

uint8_t io_keyboard_matrix_row(int col)
{
    if (col < 0 || col > 7)
        return 0xff;
    return keyboard_matrix[col];
}

uint8_t io_joystick2(void)
{
    return joystick2;
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
    row_ptr[dy][dx] = color & 0x7f;
}

uint8_t io_screen_peek(int x, int y)
{
    unsigned int dx = (unsigned int)(x - CROP_X);
    unsigned int dy = (unsigned int)(y - CROP_Y);
    if (dx >= LCD_W || dy >= LCD_H)
        return 0;
    return row_ptr[dy][dx];
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

#if IO_SHOW_FPS_OVERLAY
/* Emulated-frames-per-second overlay (50 = full PAL speed). Counts every
    VIC frame wrap; the digits reformat once per real second (32 kHz timer 1)
    and are redrawn after every rendered frame, because the frame repaint
    erases them. No sprintf (IX-clobber lore). */
void io_fps_overlay(bool can_draw)
{
    static unsigned int last_ticks;
    static unsigned int frames;
    static char text[5] = "--";
    unsigned int now;

    ++frames;
    now = (unsigned int)timer_Get(1);
    if (now - last_ticks >= 32768u)
    {
        unsigned int fps = frames < 999 ? frames : 999;
        char *p = text;
        if (fps >= 100)
            *p++ = (char)('0' + fps / 100);
        if (fps >= 10)
            *p++ = (char)('0' + (fps / 10) % 10);
        *p++ = (char)('0' + fps % 10);
        *p = '\0';
        frames = 0;
        last_ticks = now;
    }
    if (!can_draw || indicators_enabled)
        return;
    gfx_SetTextFGColor(1);        /* white on opaque black, in the top    */
    gfx_SetTextBGColor(0);        /* letterbox band so it barely overlaps */
    gfx_SetTextTransparentColor(255);
    gfx_PrintStringXY(text, LCD_W - gfx_GetStringWidth(text) - 2, 2);
}
#endif
