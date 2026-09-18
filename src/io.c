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
#include "cpu.h"
#include "memory.h"

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
static bool alpha_lock;
static uint8_t cpu_turbo_index;
static bool frame_skip;
static bool vram_view_active;
static bool debug_view_active;
static uint16_t debug_view_address;
static unsigned int last_debug_view_ticks;
typedef enum { DEBUG_VIEW_MEMORY, DEBUG_VIEW_DISASSEMBLY } DebugViewMode;
static DebugViewMode debug_view_mode;
static const uint8_t turbo_multipliers[] = {1, 50, 100, 200};
static const char *const turbo_labels[] = {
    "1x", "50x", "100x", "200x", "Adaptive"
};
#define TURBO_OPTION_COUNT (sizeof(turbo_labels) / sizeof(turbo_labels[0]))

static void apply_turbo_setting(void)
{
    if (cpu_turbo_index < (uint8_t)sizeof(turbo_multipliers))
        vic_set_cpu_turbo(turbo_multipliers[cpu_turbo_index]);
    else
        vic_set_cpu_turbo_adaptive(true);
}
/* Key Settings (GRAPH menu and browser share this): where a finished
   session goes, and which key doubles as the joystick fire button. Reset
   every io_init() like the other settings above - not persisted. */
typedef enum { CLEAR_TO_BROWSER = 0, CLEAR_TO_OS = 1 } ClearAction;
static ClearAction clear_action = CLEAR_TO_BROWSER;
static bool fire_uses_mode;
static uint8_t indicator_top = 0xff;
static uint8_t indicator_bottom = 0xff;
static uint8_t indicator_layer = 0xff;
static uint8_t indicator_status = 0xff;
static uint8_t host_chords;
/* gates the lazy kb_SetMode(MODE_3_CONTINUOUS) switch in io_emulate(); reset
   in io_shutdown() so a later session re-applies it instead of assuming the
   keypad is still in continuous mode. */
static bool continuous;

/* Per-row framebuffer pointers (single-buffer mode draws straight into the
   visible screen; the second VRAM half is the C64's RAM). Avoids a 24-bit
   multiply per pixel. */
static uint8_t *row_ptr[LCD_H];

static void set_vram_view(bool enabled);

static void set_debug_view(bool enabled)
{
    if (enabled == debug_view_active)
        return;
    debug_view_active = enabled;
    last_debug_view_ticks = 0;
    vic_set_render_suppressed(enabled);
}

/* C64 palette (original emudore RGB values) converted to graphx 1555.
   Exported (not static) so the file browser can share this exact table
   instead of keeping its own colour scheme - see io.h. */
const uint16_t c64_palette[16] = {
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

#if defined(__clang__)
__attribute__((__minsize__))
#endif
void io_init(void)
{
    int i;
    uint8_t *row = gfx_vram;
    memset(keyboard_matrix, 0xff, sizeof(keyboard_matrix));
    quit_requested = false;
    indicators_enabled = true;
    even_function_keys = false;
    alpha_lock = false;
    cpu_turbo_index = 0;
    frame_skip = true;
    set_vram_view(false);
    debug_view_active = false;
    debug_view_address = 0;
    debug_view_mode = DEBUG_VIEW_MEMORY;
    apply_turbo_setting();
    vic_set_frame_skip(true);
    clear_action = CLEAR_TO_BROWSER;
    fire_uses_mode = false;
    indicator_top = 0xff;
    indicator_bottom = 0xff;
    indicator_layer = 0xff;
    indicator_status = 0xff;
    host_chords = 0;
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
    set_vram_view(false);
    set_debug_view(false);
    kb_SetMode(MODE_1_INDISCRIMINATE); /* OS default after continuous mode */
    continuous = false;
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

static void draw_shortcut_status(uint8_t column, bool enabled)
{
    uint8_t row;
    uint8_t color = enabled ? 13 : 0;
    int left = column * 64 + 57;

    for (row = 2; row < 7; ++row)
        memset(row_ptr[row] + left, color, 5);
}

static void update_key_indicators(void)
{
    static const char *const top_keys[] = {
        "Y=", "WINDOW", "ZOOM", "TRACE", "GRAPH"
    };
    static const char *const odd_functions[] = { "F1", "F3", "F5", "F7" };
    static const char *const even_functions[] = { "F2", "F4", "F6", "F8" };
    static const char *const second_functions[] = {
        "ALPHA", "TURBO", "SKIP", "VRAM"
    };
    static const char *const bottom_keys[] = {
        "2nd", "ALPHA", "MODE", "VARS", "CLEAR"
    };
    static const char *const bottom_actions[] = {
        "FIRE", "ABC", "C=", "STOP", "EXIT"
    };
    uint8_t top = 0;
    uint8_t bottom = 0;
    uint8_t layer = 0;
    uint8_t status = 0;
    uint8_t column;
    uint8_t mask = 1;

    if (!indicators_enabled)
        return;

    if (!(keyboard_matrix[0] & 0x10)) top |= 0x01;
    if (!(keyboard_matrix[0] & 0x20)) top |= 0x02;
    if (!(keyboard_matrix[0] & 0x40)) top |= 0x04;
    if (!(keyboard_matrix[0] & 0x08)) top |= 0x08;
    if (alpha_lock) status |= 0x01;
    if (cpu_turbo_index != 0) status |= 0x02;
    if (frame_skip) status |= 0x04;
    if (vram_view_active) status |= 0x08;
    if (kb_IsDown(kb_KeyGraph)) top |= 0x10;
    if (kb_IsDown(kb_Key2nd)) bottom |= 0x01;
    if (alpha_lock || kb_IsDown(kb_KeyAlpha)) bottom |= 0x02;
    if (!(keyboard_matrix[7] & 0x20)) bottom |= 0x04;
    if (!(keyboard_matrix[7] & 0x80)) bottom |= 0x08;
    if (kb_IsDown(kb_KeyClear)) bottom |= 0x10;
    if (bottom & 0x01)
        layer = 1;
    else if (even_function_keys || !(keyboard_matrix[1] & 0x80))
        layer = 2;

    if (top == indicator_top && bottom == indicator_bottom && status == indicator_status &&
        layer == indicator_layer)
        return;

    gfx_SetTextScale(1, 1);
    for (column = 0; column < 5; ++column)
    {
        if (((top ^ indicator_top) & mask) || layer != indicator_layer)
        {
            const char *action = "MENU";
            if (column < 4)
                action = layer == 1 ? second_functions[column] :
                         (layer == 2 ? even_functions[column] : odd_functions[column]);
            draw_key_indicator(column, 0, top_keys[column], action,
                               (top & mask) != 0);
        }
                    if (column < 4 && (((status ^ indicator_status) & mask) ||
                               ((top ^ indicator_top) & mask) ||
                               layer != indicator_layer))
                        draw_shortcut_status(column, (status & mask) != 0);
        if (indicator_bottom == 0xff || ((bottom ^ indicator_bottom) & mask))
        {
            /* Fire button setting can move FIRE from 2nd to MODE (Key
               Settings); swap the two labels so the bar stays accurate. */
            const char *action = bottom_actions[column];
            if (fire_uses_mode && column == 0) action = "SHIFT";
            else if (fire_uses_mode && column == 2) action = "FIRE";
            draw_key_indicator(column, 220, bottom_keys[column], action,
                               (bottom & mask) != 0);
        }
        mask += mask;
    }
    gfx_SetTextFGColor(1);
    gfx_SetTextBGColor(0);
    indicator_top = top;
    indicator_bottom = bottom;
    indicator_layer = layer;
    indicator_status = status;
}

#define SETTINGS_LEFT 48
#define SETTINGS_TOP 76
#define SETTINGS_WIDTH 224
#define SETTINGS_HEIGHT 116

static void wait_for_key_release(void);

static void set_vram_view(bool enabled)
{
    if (enabled == vram_view_active)
        return;
    vram_view_active = enabled;
    vic_set_render_suppressed(enabled);
    if (enabled)
        io_refresh_vram_view();
    else
    {
        /* The full-screen viewer overwrote both HUD bands. */
        indicator_top = indicator_bottom = indicator_layer = indicator_status = 0xff;
    }
}

static void draw_settings_menu(uint8_t selected)
{
    static const char *const labels[] = {
        "Resume", "Key indicators", "Key Settings", "Optimization", "Exit game",
        "Debugging"
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

    for (entry = 0; entry < 6; ++entry)
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
        if (value)
            gfx_PrintStringXY(value,
                              SETTINGS_LEFT + SETTINGS_WIDTH - 12 - gfx_GetStringWidth(value),
                              top);
    }
}

#define OPTIMIZATION_LEFT 32
#define OPTIMIZATION_TOP 62
#define OPTIMIZATION_WIDTH 256
#define OPTIMIZATION_HEIGHT 110

static void draw_optimization_settings(uint8_t selected)
{
    static const char *const labels[] = {
        "Turbo CPU/RAM", "Frame skip", "VRAM view", "Back"
    };
    uint8_t entry;

    gfx_SetTextScale(1, 1);
    gfx_SetTextTransparentColor(255);
    gfx_SetColor(0);
    gfx_FillRectangle_NoClip(OPTIMIZATION_LEFT, OPTIMIZATION_TOP,
                             OPTIMIZATION_WIDTH, OPTIMIZATION_HEIGHT);
    gfx_SetColor(6);
    gfx_Rectangle_NoClip(OPTIMIZATION_LEFT, OPTIMIZATION_TOP,
                         OPTIMIZATION_WIDTH, OPTIMIZATION_HEIGHT);
    gfx_SetTextFGColor(1);
    gfx_SetTextBGColor(0);
    gfx_PrintStringXY("Optimization", OPTIMIZATION_LEFT + 12, OPTIMIZATION_TOP + 7);

    for (entry = 0; entry < 4; ++entry)
    {
        uint8_t top = OPTIMIZATION_TOP + 24 + entry * 14;
        uint8_t background = entry == selected ? 13 : 0;
        const char *value = NULL;

        gfx_SetColor(background);
        gfx_FillRectangle_NoClip(OPTIMIZATION_LEFT + 8, top - 2,
                                 OPTIMIZATION_WIDTH - 16, 12);
        gfx_SetTextBGColor(background);
        gfx_SetTextFGColor(entry == selected ? 0 : 1);
        gfx_PrintStringXY(labels[entry], OPTIMIZATION_LEFT + 12, top);
        if (entry == 0)
            value = turbo_labels[cpu_turbo_index];
        else if (entry == 1)
            value = frame_skip ? "Adaptive" : "Off";
        if (value)
            gfx_PrintStringXY(value,
                              OPTIMIZATION_LEFT + OPTIMIZATION_WIDTH - 12 -
                              gfx_GetStringWidth(value), top);
    }
}

#if defined(__clang__)
__attribute__((__minsize__))
#endif
static bool __attribute__((noinline)) show_optimization_settings(void)
{
    uint8_t selected = 0;
    bool key_held = false;
    bool redraw = true;
    bool finished = false;
    bool enter_vram_view = false;

    wait_for_key_release();
    while (!finished)
    {
        if (redraw)
        {
            draw_optimization_settings(selected);
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

        if (kb_IsDown(kb_KeyGraph) || kb_IsDown(kb_KeyClear))
            finished = true;
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
            if (selected == 0)
            {
                if (kb_IsDown(kb_KeyLeft) && cpu_turbo_index > 0)
                    --cpu_turbo_index;
                else if (kb_IsDown(kb_KeyRight) &&
                         cpu_turbo_index + 1u < (uint8_t)TURBO_OPTION_COUNT)
                    ++cpu_turbo_index;
                else if (kb_IsDown(kb_KeyEnter))
                    cpu_turbo_index = (uint8_t)((cpu_turbo_index + 1) %
                                                TURBO_OPTION_COUNT);
                apply_turbo_setting();
                redraw = true;
            }
            else if (selected == 1)
            {
                frame_skip = kb_IsDown(kb_KeyLeft) ? false :
                             (kb_IsDown(kb_KeyRight) ? true : !frame_skip);
                vic_set_frame_skip(frame_skip);
                redraw = true;
            }
            else if (selected == 2 && kb_IsDown(kb_KeyEnter))
            {
                enter_vram_view = true;
                finished = true;
            }
            else if (selected == 3 && kb_IsDown(kb_KeyEnter))
                finished = true;
        }
    }
    wait_for_key_release();
    return enter_vram_view;
}

static void wait_for_key_release(void)
{
    do
    {
        kb_Scan();
    }
    while (kb_AnyKey());
}

/* Key Settings: the handful of key choices that actually matter while
   playing a game or file, kept short on purpose (a student mid-game should
   be able to scan it in a glance, not hunt through a full remapper). Shared
   by the GRAPH pause menu (mid-game) and the file browser (GRAPH there too)
   since both draw with the same real C64 16-colour palette. */
#define KEYSET_LEFT 32
#define KEYSET_TOP 62
#define KEYSET_WIDTH 256
#define KEYSET_HEIGHT 96

static void draw_key_settings(uint8_t selected)
{
    static const char *const labels[] = {
        "Clear action", "Function keys", "Fire button", "Back"
    };
    uint8_t entry;

    gfx_SetTextScale(1, 1);
    gfx_SetTextTransparentColor(255);
    gfx_SetColor(0);
    gfx_FillRectangle_NoClip(KEYSET_LEFT, KEYSET_TOP, KEYSET_WIDTH, KEYSET_HEIGHT);
    gfx_SetColor(6); /* C64 blue frame */
    gfx_Rectangle_NoClip(KEYSET_LEFT, KEYSET_TOP, KEYSET_WIDTH, KEYSET_HEIGHT);
    gfx_SetTextFGColor(1);
    gfx_SetTextBGColor(0);
    gfx_PrintStringXY("Key Settings", KEYSET_LEFT + 12, KEYSET_TOP + 7);

    for (entry = 0; entry < 4; ++entry)
    {
        uint8_t top = KEYSET_TOP + 24 + entry * 14;
        uint8_t background = entry == selected ? 13 : 0;
        const char *value = NULL;

        gfx_SetColor(background);
        gfx_FillRectangle_NoClip(KEYSET_LEFT + 8, top - 2,
                                KEYSET_WIDTH - 16, 12);
        gfx_SetTextBGColor(background);
        gfx_SetTextFGColor(entry == selected ? 0 : 1);
        gfx_PrintStringXY(labels[entry], KEYSET_LEFT + 12, top);
        if (entry == 0)
            value = clear_action == CLEAR_TO_OS ? "Exit to OS" : "Exit to Browser";
        else if (entry == 1)
            value = even_function_keys ? "F2/4/6/8" : "F1/3/5/7";
        else if (entry == 2)
            value = fire_uses_mode ? "MODE" : "2ND";
        if (value)
            gfx_PrintStringXY(value,
                              KEYSET_LEFT + KEYSET_WIDTH - 12 - gfx_GetStringWidth(value),
                              top);
    }
}

/**
 * Show the shared Key Settings modal (Clear action / Function keys / Fire
 * button). Safe to call before io_init() (e.g. from the file browser): it
 * only touches its own static state plus graphx/keypadc directly. GRAPH or
 * CLEAR both back out without requesting a quit; the caller is responsible
 * for repainting whatever was underneath afterward.
 */
#if defined(__clang__)
__attribute__((__minsize__))
#endif
void __attribute__((noinline)) io_show_key_settings(void)
{
    uint8_t selected = 0;
    bool key_held = false;
    bool redraw = true;
    bool finished = false;

    wait_for_key_release();
    while (!finished)
    {
        if (redraw)
        {
            draw_key_settings(selected);
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

        if (kb_IsDown(kb_KeyGraph) || kb_IsDown(kb_KeyClear))
            finished = true;
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
            if (selected == 0)
            {
                if (kb_IsDown(kb_KeyLeft))
                    clear_action = CLEAR_TO_BROWSER;
                else if (kb_IsDown(kb_KeyRight))
                    clear_action = CLEAR_TO_OS;
                else
                    clear_action = clear_action == CLEAR_TO_OS
                                 ? CLEAR_TO_BROWSER : CLEAR_TO_OS;
                redraw = true;
            }
            else if (selected == 1)
            {
                if (kb_IsDown(kb_KeyLeft))
                    even_function_keys = false;
                else if (kb_IsDown(kb_KeyRight))
                    even_function_keys = true;
                else
                    even_function_keys = !even_function_keys;
                redraw = true;
            }
            else if (selected == 2)
            {
                if (kb_IsDown(kb_KeyLeft))
                    fire_uses_mode = false;
                else if (kb_IsDown(kb_KeyRight))
                    fire_uses_mode = true;
                else
                    fire_uses_mode = !fire_uses_mode;
                redraw = true;
            }
            else if (selected == 3 && kb_IsDown(kb_KeyEnter))
                finished = true;
        }
    }
    wait_for_key_release();
}

bool io_clear_exits_to_os(void)
{
    return clear_action == CLEAR_TO_OS;
}

/* Single-instruction stepping needs a stopping point after every opcode;
   the asm core only supports running a whole batch to a cycle target. */
#define STEP_LEFT 32
#define STEP_TOP 50
#define STEP_WIDTH 256
#define STEP_HEIGHT 150

/* Step Over/Out run with no VIC/CIA emulation in between, so code that
   waits on a raster or CIA timer IRQ would otherwise hang forever; this caps
   how many instructions either action will execute before giving up. */
#define STEP_GUARD_LIMIT 200000UL

static void hex_digits(char *out, unsigned long value, uint8_t digits)
{
    static const char hex[] = "0123456789ABCDEF";
    uint8_t i;
    for (i = digits; i > 0; --i)
    {
        out[i - 1] = hex[value & 0xf];
        value >>= 4;
    }
    out[digits] = '\0';
}

static void append_hex(char **out, uint8_t value)
{
    hex_digits(*out, value, 2);
    *out += 2;
}

static void disassemble_6502(char *out, uint16_t address)
{
    static const char *const group_one[] = {
        "ORA", "AND", "EOR", "ADC", "STA", "LDA", "CMP", "SBC"
    };
    uint8_t opcode = mem_debug_peek_cpu(address);
    uint8_t low = mem_debug_peek_cpu((uint16_t)(address + 1));
    uint8_t high = mem_debug_peek_cpu((uint16_t)(address + 2));
    char *p = out;
    uint8_t group = opcode & 3;
    uint8_t mode = (opcode >> 2) & 7;

    append_hex(&p, opcode);
    *p++ = ' ';
    append_hex(&p, low);
    *p++ = ' ';
    append_hex(&p, high);
    *p++ = ' ';
    if (group == 1 && !(opcode == 0x89 || (mode == 2 && opcode == 0x81)))
    {
        strcpy(p, group_one[(opcode >> 5) & 7]);
        p += 3;
        *p++ = ' ';
        if (mode == 0) { *p++ = '('; *p++ = '$'; append_hex(&p, low); strcpy(p, ",X)"); }
        else if (mode == 1) { *p++ = '$'; append_hex(&p, low); }
        else if (mode == 2) { *p++ = '#'; *p++ = '$'; append_hex(&p, low); }
        else if (mode == 3) { *p++ = '$'; append_hex(&p, low); *p++ = '$'; append_hex(&p, high); }
        else if (mode == 4) { *p++ = '('; *p++ = '$'; append_hex(&p, low); strcpy(p, "),Y"); }
        else if (mode == 5) { *p++ = '$'; append_hex(&p, low); strcpy(p, ",X"); }
        else if (mode == 6) { *p++ = '$'; append_hex(&p, low); *p++ = '$'; append_hex(&p, high); strcpy(p, ",Y"); }
        else { *p++ = '$'; append_hex(&p, low); *p++ = '$'; append_hex(&p, high); strcpy(p, ",X"); }
        return;
    }
    switch (opcode)
    {
    case 0x00: strcpy(p, "BRK"); break;
    case 0x20: strcpy(p, "JSR $"); append_hex(&p, low); append_hex(&p, high); break;
    case 0x4c: strcpy(p, "JMP $"); append_hex(&p, low); append_hex(&p, high); break;
    case 0x6c: strcpy(p, "JMP ($"); append_hex(&p, low); append_hex(&p, high); strcat(p, ")"); break;
    case 0x60: strcpy(p, "RTS"); break;
    case 0x40: strcpy(p, "RTI"); break;
    case 0xea: strcpy(p, "NOP"); break;
    case 0x18: strcpy(p, "CLC"); break;
    case 0x38: strcpy(p, "SEC"); break;
    case 0x58: strcpy(p, "CLI"); break;
    case 0x78: strcpy(p, "SEI"); break;
    case 0xd8: strcpy(p, "CLD"); break;
    case 0xf8: strcpy(p, "SED"); break;
    case 0xaa: strcpy(p, "TAX"); break;
    case 0xa8: strcpy(p, "TAY"); break;
    case 0x8a: strcpy(p, "TXA"); break;
    case 0x98: strcpy(p, "TYA"); break;
    case 0xe8: strcpy(p, "INX"); break;
    case 0xc8: strcpy(p, "INY"); break;
    case 0xca: strcpy(p, "DEX"); break;
    case 0x88: strcpy(p, "DEY"); break;
    default: strcpy(p, "???"); break;
    }
}

/* @return false when the STEP_GUARD_LIMIT safety cap was hit first. */
static bool step_over_run(void)
{
    uint8_t entry_sp = cpu.sp;
    unsigned long guard = 0;

    if (mem_read_byte(cpu.pc) != 0x20) /* not JSR: same as Step Into */
    {
        cpu_run(cpu.cycles + 1);
        return true;
    }
    do
    {
        if (!cpu_run(cpu.cycles + 1))
            return true;
        if (++guard >= STEP_GUARD_LIMIT)
            return false;
    }
    while (cpu.sp < entry_sp);
    return true;
}

/* @return false when the STEP_GUARD_LIMIT safety cap was hit first. */
static bool step_out_run(void)
{
    uint8_t entry_sp = cpu.sp;
    unsigned long guard = 0;

    do
    {
        if (!cpu_run(cpu.cycles + 1))
            return true;
        if (++guard >= STEP_GUARD_LIMIT)
            return false;
    }
    while (cpu.sp <= entry_sp);
    return true;
}

static void draw_step_debugger(uint8_t selected, const char *status)
{
    static const char *const labels[] = {
        "Step Into", "Step Over", "Step Out", "Resume"
    };
    char text[24];
    uint8_t entry;

    gfx_SetTextScale(1, 1);
    gfx_SetTextTransparentColor(255);
    gfx_SetColor(0);
    gfx_FillRectangle_NoClip(STEP_LEFT, STEP_TOP, STEP_WIDTH, STEP_HEIGHT);
    gfx_SetColor(12);
    gfx_Rectangle_NoClip(STEP_LEFT, STEP_TOP, STEP_WIDTH, STEP_HEIGHT);
    gfx_SetTextFGColor(1);
    gfx_SetTextBGColor(0);
    gfx_PrintStringXY("Step Debugger", STEP_LEFT + 12, STEP_TOP + 7);

    strcpy(text, "PC=");
    hex_digits(text + 3, cpu.pc, 4);
    gfx_PrintStringXY(text, STEP_LEFT + 12, STEP_TOP + 24);

    strcpy(text, "A=");
    hex_digits(text + 2, cpu.a, 2);
    strcat(text, " X=");
    hex_digits(text + strlen(text), cpu.x, 2);
    strcat(text, " Y=");
    hex_digits(text + strlen(text), cpu.y, 2);
    gfx_PrintStringXY(text, STEP_LEFT + 12, STEP_TOP + 38);

    strcpy(text, "SP=01");
    hex_digits(text + 5, cpu.sp, 2);
    gfx_PrintStringXY(text, STEP_LEFT + 12, STEP_TOP + 52);

    text[0] = cpu.nf ? 'N' : '-';
    text[1] = cpu.of ? 'V' : '-';
    text[2] = cpu.bcf ? 'B' : '-';
    text[3] = cpu.dmf ? 'D' : '-';
    text[4] = cpu.idf ? 'I' : '-';
    text[5] = cpu.zf ? 'Z' : '-';
    text[6] = cpu.cf ? 'C' : '-';
    text[7] = '\0';
    gfx_PrintStringXY(text, STEP_LEFT + 96, STEP_TOP + 52);

    for (entry = 0; entry < 4; ++entry)
    {
        uint8_t top = STEP_TOP + 70 + entry * 16;
        uint8_t background = entry == selected ? 13 : 0;

        gfx_SetColor(background);
        gfx_FillRectangle_NoClip(STEP_LEFT + 8, top - 2, STEP_WIDTH - 16, 12);
        gfx_SetTextBGColor(background);
        gfx_SetTextFGColor(entry == selected ? 0 : 1);
        gfx_PrintStringXY(labels[entry], STEP_LEFT + 12, top);
    }

    gfx_SetTextFGColor(2);
    gfx_SetTextBGColor(0);
    gfx_PrintStringXY(status ? status : "", STEP_LEFT + 12,
                      STEP_TOP + STEP_HEIGHT - 14);
}

#if defined(__clang__)
__attribute__((__minsize__))
#endif
static void __attribute__((noinline)) show_step_debugger(void)
{
    uint8_t selected = 0;
    bool key_held = false;
    bool redraw = true;
    bool finished = false;
    const char *status = NULL;

    wait_for_key_release();

    while (!finished)
    {
        if (redraw)
        {
            draw_step_debugger(selected, status);
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
        status = NULL;

        /* CLEAR here only leaves the debugger, unlike the settings menu's
           CLEAR: it must not silently quit mid-debugging session. */
        if (kb_IsDown(kb_KeyGraph) || kb_IsDown(kb_KeyClear))
            finished = true;
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
        else if (kb_IsDown(kb_KeyEnter))
        {
            if (selected == 0)
                cpu_run(cpu.cycles + 1);
            else if (selected == 1)
                status = step_over_run() ? NULL : "step limit reached";
            else if (selected == 2)
                status = step_out_run() ? NULL : "step limit reached";
            else
                finished = true;
            redraw = true;
        }
    }

    wait_for_key_release();
}

#define DEBUG_LEFT 48
#define DEBUG_TOP 82
#define DEBUG_WIDTH 224
#define DEBUG_HEIGHT 88

static void draw_debugging_menu(uint8_t selected)
{
    static const char *const labels[] = {
        "Memory Viewer", "Disassembly Viewer", "Step debugger", "Back"
    };
    uint8_t entry;

    gfx_SetTextScale(1, 1);
    gfx_SetTextTransparentColor(255);
    gfx_SetColor(0);
    gfx_FillRectangle_NoClip(DEBUG_LEFT, DEBUG_TOP, DEBUG_WIDTH, DEBUG_HEIGHT);
    gfx_SetColor(6);
    gfx_Rectangle_NoClip(DEBUG_LEFT, DEBUG_TOP, DEBUG_WIDTH, DEBUG_HEIGHT);
    gfx_SetTextFGColor(1);
    gfx_SetTextBGColor(0);
    gfx_PrintStringXY("Debugging", DEBUG_LEFT + 12, DEBUG_TOP + 7);
    for (entry = 0; entry < 4; ++entry)
    {
        uint8_t top = DEBUG_TOP + 24 + entry * 14;
        uint8_t background = entry == selected ? 13 : 0;
        gfx_SetColor(background);
        gfx_FillRectangle_NoClip(DEBUG_LEFT + 8, top - 2, DEBUG_WIDTH - 16, 12);
        gfx_SetTextBGColor(background);
        gfx_SetTextFGColor(entry == selected ? 0 : 1);
        gfx_PrintStringXY(labels[entry], DEBUG_LEFT + 12, top);
    }
}

static uint8_t show_debugging_menu(void)
{
    uint8_t selected = 0;
    bool held = false;
    bool redraw = true;

    wait_for_key_release();
    for (;;)
    {
        if (redraw)
        {
            draw_debugging_menu(selected);
            redraw = false;
        }
        kb_Scan();
        if (!kb_AnyKey()) { held = false; continue; }
        if (held) continue;
        held = true;
        if (kb_IsDown(kb_KeyGraph) || kb_IsDown(kb_KeyClear)) break;
        if (kb_IsDown(kb_KeyUp) && selected) { --selected; redraw = true; }
        else if (kb_IsDown(kb_KeyDown) && selected < 3) { ++selected; redraw = true; }
        else if (kb_IsDown(kb_KeyEnter))
        {
            if (selected < 3) { wait_for_key_release(); return (uint8_t)(selected + 1); }
            break;
        }
    }
    wait_for_key_release();
    return 0;
}

#if defined(__clang__)
__attribute__((__minsize__))
#endif
static void __attribute__((noinline)) show_settings_menu(void)
{
    bool timer_was_running = (timer_Control & 1u) != 0;
    uint8_t selected = 0;
    bool key_held = false;
    bool redraw = true;
    bool finished = false;
    bool enter_step_debugger = false;
    bool enter_debug_view = false;
    uint8_t debug_action = 0;

    timer_Disable(1);
    wait_for_key_release();

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
        else if (kb_IsDown(kb_KeyDown) && selected < 5)
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
            else if (selected == 2 && kb_IsDown(kb_KeyEnter))
            {
                io_show_key_settings();
                redraw = true;
            }
            else if (selected == 3 && kb_IsDown(kb_KeyEnter))
            {
                if (show_optimization_settings())
                {
                    set_vram_view(true);
                    finished = true;
                }
                else
                    redraw = true;
            }
            else if (selected == 4 && kb_IsDown(kb_KeyEnter))
            {
                quit_requested = true;
                finished = true;
            }
            else if (selected == 5 && kb_IsDown(kb_KeyEnter))
            {
                debug_action = show_debugging_menu();
                if (debug_action == 1 || debug_action == 2)
                {
                    debug_view_mode = debug_action == 1 ? DEBUG_VIEW_MEMORY :
                                                        DEBUG_VIEW_DISASSEMBLY;
                    enter_debug_view = true;
                    finished = true;
                }
                else if (debug_action == 3)
                {
                    enter_step_debugger = true;
                    finished = true;
                }
                else
                    redraw = true;
            }
        }
    }

    wait_for_key_release();
    if (enter_step_debugger)
        show_step_debugger();
    if (enter_debug_view)
        set_debug_view(true);

    /* Screen-save/restore buffers previously here caused "not enough
       memory" on the very first GRAPH press (the heap had nowhere near
       enough headroom); a full VIC repaint needs no scratch memory at all
       and the guest state never changed while paused, so it reproduces the
       exact pre-menu picture. */
    vic_request_render();

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
    bool keys_changed = false;
    bool alpha_active;
    uint8_t new_chords = 0;
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

    if (!quit_requested && debug_view_active)
    {
        if (kb_IsDown(kb_KeyGraph))
        {
            set_debug_view(false);
            vic_request_render();
        }
        else if (kb_IsDown(kb_KeyUp)) debug_view_address -= 16;
        else if (kb_IsDown(kb_KeyDown)) debug_view_address += 16;
        else if (kb_IsDown(kb_KeyLeft)) debug_view_address -= 256;
        else if (kb_IsDown(kb_KeyRight)) debug_view_address += 256;
        return !quit_requested;
    }

    if (!quit_requested && vram_view_active)
    {
        bool chord_down = kb_IsDown(kb_Key2nd) && kb_IsDown(kb_KeyTrace);
        if (kb_IsDown(kb_KeyGraph) || (chord_down && !(host_chords & 0x08)))
        {
            set_vram_view(false);
            vic_request_render();
        }
        else
            io_refresh_vram_view();
        /* The entry chord must be released before it can close the viewer. */
        host_chords = chord_down ? 0x08 : 0;
        return !quit_requested;
    }

    if (!quit_requested && kb_IsDown(kb_KeyGraph))
    {
        if (vram_view_active)
        {
            set_vram_view(false);
            vic_request_render();
            return true;
        }
        update_key_indicators();
        show_settings_menu();
        update_key_indicators();
        return !quit_requested;
    }

    if (kb_IsDown(kb_Key2nd))
    {
        if (kb_IsDown(kb_KeyYequ))
            new_chords |= 0x01;
        if (kb_IsDown(kb_KeyWindow))
            new_chords |= 0x02;
        if (kb_IsDown(kb_KeyZoom))
            new_chords |= 0x04;
        if (kb_IsDown(kb_KeyTrace))
            new_chords |= 0x08;
    }
    /* Chord presses/releases can change the HUD without changing guest
       input (host shortcuts deliberately suppress joystick fire). */
    keys_changed = new_chords != host_chords;
    if ((new_chords & 0x01) && !(host_chords & 0x01))
        alpha_lock = !alpha_lock;
    if ((new_chords & 0x02) && !(host_chords & 0x02))
    {
        cpu_turbo_index = cpu_turbo_index == 3 ? 0 : 3;
        apply_turbo_setting();
    }
    if ((new_chords & 0x04) && !(host_chords & 0x04))
    {
        frame_skip = !frame_skip;
        vic_set_frame_skip(frame_skip);
    }
    if ((new_chords & 0x08) && !(host_chords & 0x08))
    {
        set_vram_view(!vram_view_active);
        vic_request_render();
    }
    host_chords = new_chords;
    /* Keep the viewer a pure RAM image and don't leak its entry chord to
       the guest or paint HUD text over the freshly copied frame. */
    if (vram_view_active)
        return !quit_requested;
    alpha_active = alpha_lock || kb_IsDown(kb_KeyAlpha);

    if (kb_IsDown(kb_Key2nd))
        apply_map(snd_map, MAP_COUNT(snd_map));
    else
    {
        apply_map(base_map, MAP_COUNT(base_map));
        if (even_function_keys && (keyboard_matrix[0] & 0x78) != 0x78)
            keyboard_matrix[1] &= (uint8_t)~0x80;
        if (alpha_active)
            apply_map(alpha_map, MAP_COUNT(alpha_map));
        else
            apply_map(digit_map, MAP_COUNT(digit_map));
    }

    /* Joystick port 2: arrows + fire (active low). Games poll this through
       CIA1 $DC00; the keyboard cursor mapping stays active too. Fire is
       2ND by default, or MODE when the Key Settings "Fire button" option
       picks it instead. */
    joystick2 = 0xff;
    if (kb_IsDown(kb_KeyUp))    joystick2 &= (uint8_t)~0x01;
    if (kb_IsDown(kb_KeyDown))  joystick2 &= (uint8_t)~0x02;
    if (kb_IsDown(kb_KeyLeft))  joystick2 &= (uint8_t)~0x04;
    if (kb_IsDown(kb_KeyRight)) joystick2 &= (uint8_t)~0x08;
    if (!new_chords && kb_IsDown(fire_uses_mode ? kb_KeyMode : kb_Key2nd))
        joystick2 &= (uint8_t)~0x10;

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
         alpha_active != ((indicator_bottom & 0x02) != 0) ||
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

void io_refresh_vram_view(void)
{
    if (!vram_view_active || !mem_ready())
        return;
    /* The second graphx page begins at mem_ram. Copy it into the active
       display page instead of swapping pages: that backing storage is C64
       RAM, so swapping would expose it while rendering continues elsewhere. */
    memcpy(gfx_vram, mem_ram, LCD_W * LCD_H);
}

void io_refresh_debug_view(void)
{
    unsigned int now;
    uint8_t row;
    char text[40];

    if (!debug_view_active || !mem_ready())
        return;
    now = (unsigned int)timer_Get(1);
    last_debug_view_ticks = now;
    memset(gfx_vram + LCD_W * 20, 0, LCD_W * 200);
    gfx_SetTextScale(1, 1);
    gfx_SetTextFGColor(13);
    gfx_SetTextBGColor(0);
    gfx_PrintStringXY(debug_view_mode == DEBUG_VIEW_MEMORY ?
                      "MEMORY VIEW  UP/DN ROW  L/R PAGE  GRAPH EXIT" :
                      "DISASSEMBLY  UP/DN ROW  L/R PAGE  GRAPH EXIT", 0, 22);
    for (row = 0; row < 16; ++row)
    {
        uint16_t address = (uint16_t)(debug_view_address + row *
            (debug_view_mode == DEBUG_VIEW_MEMORY ? 8u : 3u));
        uint8_t column;
        char *out = text;
        hex_digits(out, address, 4);
        out += 4;
        *out++ = ':';
        if (debug_view_mode == DEBUG_VIEW_DISASSEMBLY)
        {
            disassemble_6502(text, address);
        }
        else for (column = 0; column < 8; ++column)
        {
            *out++ = ' ';
            hex_digits(out, mem_debug_peek_cpu((uint16_t)(address + column)), 2);
            out += 2;
        }
        if (debug_view_mode == DEBUG_VIEW_MEMORY)
            *out = '\0';
        gfx_SetTextFGColor(row == 0 ? 13 : 1);
        gfx_PrintStringXY(text, 0, 34 + row * 11);
    }
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
