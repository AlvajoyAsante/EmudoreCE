#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <graphx.h>
#include <keypadc.h>
#include <sys/timers.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define KEYS(...) mock_keys((const kb_lkey_t[]){__VA_ARGS__}, \
    ARRAY_COUNT(((const kb_lkey_t[]){__VA_ARGS__})))
#define NO_KEYS ((KeySnapshot){{0}})
#define CHECK(condition) check((condition), __LINE__, "%s", #condition)
#define CHECK_MSG(condition, ...) check((condition), __LINE__, __VA_ARGS__)

enum {
    SCREEN_WIDTH = 320, SCREEN_HEIGHT = 240, FRAME_BYTES = 76800,
    VRAM_BYTES = 153600, GUARD_BYTES = 64, PALETTE_GUARD = 8,
    TRACE_CAPACITY = 4096, QUEUE_CAPACITY = 128, KEY_READ_BUDGET = 100000,
    POLL_TICKS = 546, MENU_LEFT = 48, MENU_TOP = 76,
    MENU_WIDTH = 224, MENU_HEIGHT = 116, MENU_SCAN_TICKS = 32768
};

typedef struct { uint8_t rows[8]; } KeySnapshot;
typedef struct {
    KeySnapshot keys;
    uint32_t ticks;
    void (*observe)(void);
} QueuedScan;
typedef enum {
    DRAW_SCALE, DRAW_FG, DRAW_BG, DRAW_TRANSPARENT, DRAW_WIDTH, DRAW_TEXT,
    DRAW_COLOR, DRAW_FILL_RECT, DRAW_RECT
} DrawKind;
typedef struct {
    DrawKind kind;
    int left, top;
    unsigned int width, height;
    uint8_t foreground, background, transparent, color, scale_x, scale_y;
    uint16_t timer_control;
    uint32_t timer_ticks;
    size_t scan_position;
    char text[80];
} DrawCall;
typedef struct {
    uint16_t control;
    uint32_t before[3], after[3];
    size_t draw_count;
} ScanCall;
typedef struct {
    kb_scan_mode_t mode;
    uint8_t config;
    uint16_t timer_control;
    uint32_t timer_ticks;
    size_t draw_count, scan_position;
} ModeCall;
typedef struct {
    unsigned int timer;
    bool enabled;
    uint16_t control;
} TimerCall;
typedef struct { void *pointer; size_t size; } Allocation;

static uint8_t guarded_vram[VRAM_BYTES + 2 * GUARD_BYTES];
static uint8_t saved_vram[sizeof(guarded_vram)];
static uint8_t vram_view_source[FRAME_BYTES];
static uint16_t guarded_palette[256 + 2 * PALETTE_GUARD];
static uint16_t saved_palette[ARRAY_COUNT(guarded_palette)];
uint8_t *const gfx_vram = guarded_vram + GUARD_BYTES;
uint16_t *const gfx_palette = guarded_palette + PALETTE_GUARD;
uint8_t *mem_ram = vram_view_source;
volatile uint8_t kb_Config;
uint8_t kb_Data[8];
volatile uint16_t timer_Control;
static uint32_t timer_ticks[3];
static struct {
    DrawCall draw[TRACE_CAPACITY];
    size_t draw_count, pixels, active_pixels, out_of_bounds;
    uint8_t foreground, background, transparent, color, scale_x, scale_y;
    QueuedScan queue[QUEUE_CAPACITY];
    size_t queue_count, queue_position, scans, key_reads, mode_calls;
    size_t scan_observations;
    ScanCall scan[QUEUE_CAPACITY];
    ModeCall mode[QUEUE_CAPACITY];
    TimerCall timer[QUEUE_CAPACITY];
        size_t timer_count, render_requests, turbo_calls, frame_skip_calls, swaps;
    size_t menu_render_baseline;
    Allocation allocations[16];
    size_t malloc_calls, free_calls, live_bytes, last_allocation_size;
    size_t allocation_draw_count, allocation_scan_position;
    uint16_t allocation_control;
    uint32_t allocation_ticks;
    bool fail_next_allocation;
    bool turbo_enabled;
    bool turbo_adaptive;
    uint8_t turbo_multiplier;
    bool frame_skip_enabled;
    bool render_suppressed;
} mock;
static jmp_buf test_escape;
static const char *current_test;
static size_t checks, failures;

static void check(bool passed, int line, const char *format, ...)
{
    ++checks;
    if (!passed) {
        va_list arguments;
        ++failures;
        fprintf(stderr, "FAIL %s io_test.c:%d: ", current_test, line);
        va_start(arguments, format);
        vfprintf(stderr, format, arguments);
        va_end(arguments);
        fputc('\n', stderr);
    }
}

static void require(bool passed, const char *message)
{
    if (!passed) {
        CHECK_MSG(false, "mock: %s", message);
        longjmp(test_escape, 1);
    }
}

static DrawCall *record_draw(DrawKind kind)
{
    DrawCall *call;
    require(mock.draw_count < TRACE_CAPACITY, "graphics trace exhausted");
    call = &mock.draw[mock.draw_count++];
    *call = (DrawCall){ .kind = kind, .foreground = mock.foreground,
        .background = mock.background, .transparent = mock.transparent,
        .color = mock.color, .scale_x = mock.scale_x, .scale_y = mock.scale_y,
        .timer_control = timer_Control, .timer_ticks = timer_ticks[0],
        .scan_position = mock.queue_position };
    return call;
}

static unsigned int text_width(const char *text)
{
    require(strlen(text) < sizeof(mock.draw[0].text), "text too long for trace");
    return (unsigned int)strlen(text) * 8u * mock.scale_x;
}

static void paint_pixel(int column, int row, uint8_t color)
{
    if (column < 0 || column >= SCREEN_WIDTH || row < 0 || row >= SCREEN_HEIGHT) {
        ++mock.out_of_bounds;
        return;
    }
    ++mock.pixels;
    if (row >= 20 && row < 220)
        ++mock.active_pixels;
    gfx_vram[row * SCREEN_WIDTH + column] = color;
}

void gfx_SetTextScale(uint8_t width_scale, uint8_t height_scale)
{
    require(width_scale > 0 && height_scale > 0, "zero text scale");
    mock.scale_x = width_scale;
    mock.scale_y = height_scale;
    record_draw(DRAW_SCALE);
}

uint8_t gfx_SetTextFGColor(uint8_t color)
{
    uint8_t previous = mock.foreground;
    mock.foreground = color;
    record_draw(DRAW_FG);
    return previous;
}

uint8_t gfx_SetTextBGColor(uint8_t color)
{
    uint8_t previous = mock.background;
    mock.background = color;
    record_draw(DRAW_BG);
    return previous;
}

uint8_t gfx_SetTextTransparentColor(uint8_t color)
{
    uint8_t previous = mock.transparent;
    mock.transparent = color;
    record_draw(DRAW_TRANSPARENT);
    return previous;
}

unsigned int gfx_GetStringWidth(const char *text)
{
    DrawCall *call = record_draw(DRAW_WIDTH);
    call->width = text_width(text);
    strcpy(call->text, text);
    return call->width;
}

void gfx_PrintStringXY(const char *text, int left, int top)
{
    DrawCall *call = record_draw(DRAW_TEXT);
    unsigned int row, column;
    call->left = left;
    call->top = top;
    call->width = text_width(text);
    call->height = 8u * mock.scale_y;
    strcpy(call->text, text);
    for (row = 0; row < call->height; ++row) {
        for (column = 0; column < call->width; ++column) {
            unsigned int glyph_column = (column / mock.scale_x) % 8;
            unsigned int glyph_row = row / mock.scale_y;
            bool ink = text[column / (8u * mock.scale_x)] != ' ' &&
                (glyph_column == 1 || glyph_column == 6 ||
                 glyph_row == 1 || glyph_row == 6);
            uint8_t color = ink ? mock.foreground : mock.background;
            if (color != mock.transparent)
                paint_pixel(left + (int)column, top + (int)row, color);
        }
    }
}

uint8_t gfx_SetColor(uint8_t color)
{
    uint8_t previous = mock.color;
    mock.color = color;
    record_draw(DRAW_COLOR);
    return previous;
}

static void draw_rectangle(DrawKind kind, unsigned int left, uint8_t top,
                           unsigned int width, uint8_t height)
{
    DrawCall *call = record_draw(kind);
    unsigned int row, column;
    call->left = (int)left;
    call->top = top;
    call->width = width;
    call->height = height;
    require(left <= SCREEN_WIDTH && width <= SCREEN_WIDTH - left &&
            top <= SCREEN_HEIGHT && height <= SCREEN_HEIGHT - top,
            "NoClip rectangle exceeds framebuffer");
    for (row = 0; row < height; ++row)
        for (column = 0; column < width; ++column)
            if (kind == DRAW_FILL_RECT || row == 0 || row + 1 == height ||
                column == 0 || column + 1 == width)
                paint_pixel((int)(left + column), top + (int)row, mock.color);
}

void gfx_FillRectangle_NoClip(unsigned int left, uint8_t top,
                              unsigned int width, uint8_t height)
{
    draw_rectangle(DRAW_FILL_RECT, left, top, width, height);
}

void gfx_Rectangle_NoClip(unsigned int left, uint8_t top,
                         unsigned int width, uint8_t height)
{
    draw_rectangle(DRAW_RECT, left, top, width, height);
}

KeySnapshot mock_keys(const kb_lkey_t *keys, size_t count)
{
    KeySnapshot snapshot = NO_KEYS;
    size_t index;
    for (index = 0; index < count; ++index) {
        unsigned int group = keys[index] >> 8;
        require(group > 0 && group < 8, "invalid keypad group");
        snapshot.rows[group] |= (uint8_t)keys[index];
    }
    return snapshot;
}

void mock_set_keys(KeySnapshot snapshot)
{
    memcpy(kb_Data, snapshot.rows, sizeof(kb_Data));
}

void mock_queue_scan(KeySnapshot snapshot, uint32_t ticks)
{
    require(mock.queue_count < QUEUE_CAPACITY, "keypad queue full");
    mock.queue[mock.queue_count++] = (QueuedScan){ .keys = snapshot, .ticks = ticks };
}

void mock_advance_ticks(uint32_t ticks)
{
    unsigned int timer;
    for (timer = 1; timer <= 3; ++timer)
        if (timer_Control & (1u << (3 * (timer - 1)))) {
            if (timer_Control & (1u << (timer + 8)))
                timer_ticks[timer - 1] += ticks;
            else
                timer_ticks[timer - 1] -= ticks;
        }
}

uint8_t kb_SetMode(kb_scan_mode_t mode)
{
    ModeCall *call;
    require(mock.mode_calls < QUEUE_CAPACITY, "keypad mode trace exhausted");
    call = &mock.mode[mock.mode_calls++];
    kb_Config = (uint8_t)((kb_Config & ~3u) | (unsigned int)mode);
    *call = (ModeCall){ .mode = mode, .config = kb_Config,
        .timer_control = timer_Control, .timer_ticks = timer_ticks[0],
        .draw_count = mock.draw_count, .scan_position = mock.queue_position };
    return kb_Config;
}

uint8_t kb_IsDown(kb_lkey_t key)
{
    require(++mock.key_reads <= KEY_READ_BUDGET, "keypad read budget exhausted");
    require((key >> 8) > 0 && (key >> 8) < 8, "invalid keypad key");
    return kb_Data[key >> 8] & (uint8_t)key;
}

uint8_t kb_AnyKey(void)
{
    uint8_t pressed = 0;
    size_t group;
    require(++mock.key_reads <= KEY_READ_BUDGET, "keypad read budget exhausted");
    for (group = 1; group < ARRAY_COUNT(kb_Data); ++group)
        pressed |= kb_Data[group];
    return pressed;
}

void kb_Scan(void)
{
    QueuedScan *scan;
    ScanCall *call;
    require(mock.scans < QUEUE_CAPACITY, "keypad scan trace exhausted");
    call = &mock.scan[mock.scans];
    ++mock.scans;
    require(mock.queue_position < mock.queue_count, "keypad scan queue exhausted");
    call->control = timer_Control;
    call->draw_count = mock.draw_count;
    memcpy(call->before, timer_ticks, sizeof(call->before));
    scan = &mock.queue[mock.queue_position++];
    mock_set_keys(scan->keys);
    mock_advance_ticks(scan->ticks);
    memcpy(call->after, timer_ticks, sizeof(call->after));
    if (scan->observe) {
        ++mock.scan_observations;
        scan->observe();
    }
}

uint32_t timer_Get(unsigned int timer)
{
    require(timer >= 1 && timer <= 3, "invalid timer number");
    return timer_ticks[timer - 1];
}

static void record_timer(unsigned int timer, bool enabled)
{
    require(mock.timer_count < QUEUE_CAPACITY, "timer trace exhausted");
    mock.timer[mock.timer_count++] = (TimerCall){timer, enabled, timer_Control};
}

uint16_t timer_Disable(unsigned int timer)
{
    require(timer >= 1 && timer <= 3, "invalid timer number");
    timer_Control &= (uint16_t)~(1u << (3 * (timer - 1)));
    record_timer(timer, false);
    if (timer == 1) {
        /* io.c's only timer_Disable(1) caller is show_settings_menu, right
           before it draws anything: mark "menu drawing starts here" so
           expect_menu_return can ignore any HUD indicator redraw the same
           io_emulate() call already did (with the timer still running)
           before reaching the menu. This replaces the old malloc-call
           marker now that the menu no longer allocates. */
        mock.allocation_draw_count = mock.draw_count;
        mock.allocation_scan_position = mock.queue_position;
        mock.allocation_control = timer_Control;
        mock.allocation_ticks = timer_ticks[0];
    }
    return timer_Control;
}

uint16_t timer_Enable(unsigned int timer, uint8_t rate,
                      uint8_t interrupt, uint8_t direction)
{
    unsigned int shift;
    require(timer >= 1 && timer <= 3, "invalid timer number");
    shift = 3 * (timer - 1);
    timer_Control = (uint16_t)((timer_Control &
        ~(7u << shift | 1u << (timer + 8))) |
        (1u | (unsigned int)rate << 1 | (unsigned int)interrupt << 2) << shift |
        (unsigned int)direction << (timer + 8));
    record_timer(timer, true);
    return timer_Control;
}

void *mock_malloc(size_t size)
{
    size_t index;
    void *pointer;
    ++mock.malloc_calls;
    mock.last_allocation_size = size;
    mock.allocation_draw_count = mock.draw_count;
    mock.allocation_scan_position = mock.queue_position;
    mock.allocation_control = timer_Control;
    mock.allocation_ticks = timer_ticks[0];
    if (mock.fail_next_allocation) {
        mock.fail_next_allocation = false;
        return NULL;
    }
    pointer = malloc(size);
    if (!pointer)
        return NULL;
    for (index = 0; index < ARRAY_COUNT(mock.allocations); ++index)
        if (!mock.allocations[index].pointer) {
            mock.allocations[index] = (Allocation){pointer, size};
            mock.live_bytes += size;
            return pointer;
        }
    free(pointer);
    require(false, "allocation trace exhausted");
    return NULL;
}

void mock_free(void *pointer)
{
    size_t index;
    ++mock.free_calls;
    if (!pointer)
        return;
    for (index = 0; index < ARRAY_COUNT(mock.allocations); ++index)
        if (mock.allocations[index].pointer == pointer) {
            mock.live_bytes -= mock.allocations[index].size;
            mock.allocations[index] = (Allocation){0};
            free(pointer);
            return;
        }
    require(false, "free of untracked allocation");
}

#define malloc mock_malloc
#define free mock_free
#include "../../src/io.c"
#undef free
#undef malloc

void vic_request_render(void)
{
    ++mock.render_requests;
}

void vic_set_cpu_turbo(uint8_t multiplier)
{
    mock.turbo_enabled = multiplier > 1;
    mock.turbo_adaptive = false;
    mock.turbo_multiplier = multiplier;
    ++mock.turbo_calls;
}

void vic_set_cpu_turbo_adaptive(bool enabled)
{
    mock.turbo_adaptive = enabled;
    mock.turbo_enabled = enabled;
    if (enabled)
        mock.turbo_multiplier = 1;
}

void vic_set_frame_skip(bool enabled)
{
    mock.frame_skip_enabled = enabled;
    ++mock.frame_skip_calls;
}

void vic_set_render_suppressed(bool enabled)
{
    mock.render_suppressed = enabled;
}

void gfx_SetDraw(gfx_location_t location)
{
    (void)location;
}

/* The step debugger's calls into the CPU core (cpu.h/memory.h, transitively
   included by io.c above) are stubbed out here: this harness only exercises
   io.c's menu/keypad/HUD logic, not real 6502 stepping. */
CpuState cpu;
MemPage mem_rd_pg[256];

uint8_t mem_read_io(uint16_t addr)
{
    (void)addr;
    return 0;
}

void cpu_asm_disable(void)
{
}

bool cpu_step_slow(void)
{
    ++cpu.pc;
    return true;
}

bool cpu_run(unsigned int until_cycles)
{
    cpu.cycles = until_cycles;
    return cpu_step_slow();
}

static void save_framebuffer(void)
{
    memcpy(saved_vram, guarded_vram, sizeof(saved_vram));
    memcpy(saved_palette, guarded_palette, sizeof(saved_palette));
}

static void reset_fixture(void)
{
    size_t offset;
    memset(&mock, 0, sizeof(mock));
    mock.foreground = 1;
    mock.transparent = 255;
    mock.scale_x = mock.scale_y = 1;
    memset(guarded_vram, 0xa5, GUARD_BYTES);
    memset(gfx_vram + VRAM_BYTES, 0x5a, GUARD_BYTES);
    for (offset = 0; offset < VRAM_BYTES; ++offset)
        gfx_vram[offset] = (uint8_t)(0x80u | ((offset * 37 + offset / 256) & 0x7fu));
    for (offset = 0; offset < FRAME_BYTES; ++offset)
        vram_view_source[offset] = (uint8_t)(offset * 19u + offset / 17u);
    for (offset = 0; offset < ARRAY_COUNT(guarded_palette); ++offset)
        guarded_palette[offset] = 0xa55a;
    mock_set_keys(NO_KEYS);
    timer_Control = 0;
    timer_Enable(1, TIMER_32K, TIMER_NOINT, TIMER_UP);
    save_framebuffer();
    io_init();
}

static bool poll_keys(KeySnapshot keys)
{
    require(!(keys.rows[1] & (uint8_t)kb_KeyGraph) ||
            vram_view_active || debug_view_active ||
            mock.queue_position < mock.queue_count,
            "GRAPH polling requires a bounded keypad queue");
    mock.key_reads = 0;
    mock_set_keys(keys);
    mock_advance_ticks(POLL_TICKS);
    return io_emulate();
}

static void clear_draw_trace(void)
{
    mock.draw_count = 0;
    mock.pixels = 0;
    mock.active_pixels = 0;
    mock.out_of_bounds = 0;
}

static size_t draw_count(DrawKind kind)
{
    size_t index, count = 0;
    for (index = 0; index < mock.draw_count; ++index)
        if (mock.draw[index].kind == kind)
            ++count;
    return count;
}

static void expect_text(const char *text, int left, int top, bool pressed)
{
    size_t index;
    for (index = 0; index < mock.draw_count; ++index) {
        const DrawCall *call = &mock.draw[index];
        if (call->kind != DRAW_TEXT || strcmp(call->text, text) ||
            call->left != left || call->top != top)
            continue;
        CHECK(call->foreground == (pressed ? 0 : 1));
        CHECK(call->background == (pressed ? 13 : 0));
        CHECK(call->transparent == 255);
        CHECK(call->scale_x == 1 && call->scale_y == 1);
        return;
    }
    CHECK_MSG(false, "missing text '%s' at (%d,%d), pressed=%d",
              text, left, top, pressed);
}

static void expect_cell(unsigned int column, int top, const char *key,
                        const char *action, bool pressed)
{
    int left = (int)column * 64;
    expect_text(key, left + (64 - (int)strlen(key) * 8) / 2, top + 1, pressed);
    expect_text(action, left + (64 - (int)strlen(action) * 8) / 2, top + 11, pressed);
    CHECK_MSG(gfx_vram[top * SCREEN_WIDTH + left] == (pressed ? 13 : 0),
              "cell %u at row %d has wrong highlight", column, top);
}

static void expect_shortcut_status(unsigned int column, bool enabled)
{
    CHECK(gfx_vram[2 * SCREEN_WIDTH + column * 64 + 57] == (enabled ? 13 : 0));
}

static void expect_protected_memory(void)
{
    size_t index;
    CHECK(memcmp(gfx_vram + 20 * SCREEN_WIDTH,
                 saved_vram + GUARD_BYTES + 20 * SCREEN_WIDTH,
                 200 * SCREEN_WIDTH) == 0);
    CHECK(memcmp(gfx_vram + FRAME_BYTES,
                 saved_vram + GUARD_BYTES + FRAME_BYTES,
                 VRAM_BYTES - FRAME_BYTES) == 0);
    CHECK(memcmp(guarded_vram, saved_vram, GUARD_BYTES) == 0);
    CHECK(memcmp(gfx_vram + VRAM_BYTES,
                 saved_vram + GUARD_BYTES + VRAM_BYTES, GUARD_BYTES) == 0);
    CHECK(mock.active_pixels == 0);
    CHECK(mock.out_of_bounds == 0);
    CHECK(memcmp(guarded_palette, saved_palette, sizeof(saved_palette)) == 0);
    for (index = 0; index < mock.draw_count; ++index) {
        const DrawCall *call = &mock.draw[index];
        if (call->kind == DRAW_TEXT) {
            CHECK(call->left >= 0 && call->left + (int)call->width <= SCREEN_WIDTH);
            CHECK((call->top >= 0 && call->top + (int)call->height <= 20) ||
                  (call->top >= 220 && call->top + (int)call->height <= SCREEN_HEIGHT));
        }
    }
}

static void expect_matrix(uint8_t first_row, uint8_t shift_row, uint8_t last_row)
{
    int row;
    for (row = 0; row < 8; ++row) {
        uint8_t expected = row == 0 ? first_row :
            (row == 1 ? shift_row : (row == 7 ? last_row : 0xff));
        CHECK_MSG(io_keyboard_matrix_row(row) == expected,
                  "matrix row %d: expected %02x, got %02x", row,
                  (unsigned int)expected, (unsigned int)io_keyboard_matrix_row(row));
    }
}

static void expect_only_cell_changed(unsigned int column, int top)
{
    int row;
    size_t left = column * 64;
    for (row = 0; row < SCREEN_HEIGHT; ++row) {
        size_t offset = (size_t)row * SCREEN_WIDTH;
        const uint8_t *saved = saved_vram + GUARD_BYTES + offset;
        if (row >= top && row < top + 20) {
            CHECK(memcmp(gfx_vram + offset, saved, left) == 0);
            CHECK(memcmp(gfx_vram + offset + left + 64, saved + left + 64,
                         SCREEN_WIDTH - left - 64) == 0);
        } else {
            CHECK(memcmp(gfx_vram + offset, saved, SCREEN_WIDTH) == 0);
        }
    }
    expect_protected_memory();
}

static void test_palette_init(void)
{
    static const uint16_t expected[] = {
        0x0000, 0x7fff, 0x54c4, 0x337f, 0x5cf7, 0x2b2b, 0x0c32, 0x77cf,
        0x5dc3, 0x3d40, 0x6e50, 0x2d6b, 0x4631, 0x5bd5, 0x567d, 0x5ef7
    };
    unsigned int index;
    reset_fixture();
    for (index = 0; index < 256; ++index) {
        CHECK(gfx_palette[index] == expected[index % 16]);
        CHECK(io_palette_probe((uint8_t)index) == expected[index % 16]);
    }
    CHECK(memcmp(guarded_palette, saved_palette, PALETTE_GUARD * sizeof(uint16_t)) == 0);
    CHECK(memcmp(gfx_palette + 256, saved_palette + PALETTE_GUARD + 256,
                 PALETTE_GUARD * sizeof(uint16_t)) == 0);
    CHECK(memcmp(guarded_vram, saved_vram, sizeof(saved_vram)) == 0);
    CHECK(mock.draw_count == 0);
}

static void test_initial_hud(void)
{
    static const char *const keys[] = {"Y=", "WINDOW", "ZOOM", "TRACE", "GRAPH"};
    static const char *const actions[] = {"F1", "F3", "F5", "F7", "MENU"};
    static const char *const bottom_keys[] = {"2nd", "ALPHA", "MODE", "VARS", "CLEAR"};
    static const char *const bottom_actions[] = {"FIRE", "ABC", "C=", "STOP", "EXIT"};
    unsigned int column;
    reset_fixture();
    save_framebuffer();
    CHECK(poll_keys(NO_KEYS));
    CHECK(draw_count(DRAW_TEXT) == 20);
    for (column = 0; column < 5; ++column) {
        expect_cell(column, 0, keys[column], actions[column], false);
        expect_cell(column, 220, bottom_keys[column], bottom_actions[column], false);
    }
    CHECK(mock.foreground == 1 && mock.background == 0);
    CHECK(memcmp(guarded_palette, saved_palette, sizeof(saved_palette)) == 0);
    expect_protected_memory();
}

static void test_first_poll_2nd(void)
{
    static const char *const labels[] = {"Y=", "WINDOW", "ZOOM", "TRACE"};
    unsigned int column;
    reset_fixture();
    save_framebuffer();
    CHECK(poll_keys(KEYS(kb_Key2nd)));
    expect_cell(0, 220, "2nd", "FIRE", true);
    CHECK_MSG(draw_count(DRAW_TEXT) == 20,
              "initial HUD: expected 20 text calls, got %zu", draw_count(DRAW_TEXT));
    for (column = 0; column < 4; ++column)
        expect_cell(column, 0, labels[column], "--", false);
    expect_matrix(0xff, 0xff, 0xff);
    CHECK(io_joystick2() == 0xef);
    expect_protected_memory();
}

static const kb_lkey_t function_keys[] = {
    kb_KeyYequ, kb_KeyWindow, kb_KeyZoom, kb_KeyTrace
};
static const uint8_t function_masks[] = {0x10, 0x20, 0x40, 0x08};
static const char *const function_labels[] = {"Y=", "WINDOW", "ZOOM", "TRACE"};
static const char *const odd_labels[] = {"F1", "F3", "F5", "F7"};

static void test_base_function_mapping(void)
{
    size_t index;
    for (index = 0; index < ARRAY_COUNT(function_keys); ++index) {
        reset_fixture();
        save_framebuffer();
        CHECK(poll_keys(KEYS(function_keys[index])));
        expect_matrix((uint8_t)~function_masks[index], 0xff, 0xff);
        CHECK(io_joystick2() == 0xff);
        CHECK(draw_count(DRAW_TEXT) == 20);
        expect_cell((unsigned int)index, 0, function_labels[index], odd_labels[index], true);
        expect_protected_memory();
        CHECK(poll_keys(NO_KEYS));
        expect_matrix(0xff, 0xff, 0xff);
    }
    reset_fixture();
    CHECK(poll_keys(KEYS(kb_KeyYequ, kb_KeyWindow, kb_KeyZoom, kb_KeyTrace)));
    expect_matrix(0x87, 0xff, 0xff);
}

static void test_graph_not_guest_input(void)
{
    size_t index;
    static const struct { const KeyMap *map; int count; } maps[] = {
        {base_map, MAP_COUNT(base_map)}, {digit_map, MAP_COUNT(digit_map)},
        {alpha_map, MAP_COUNT(alpha_map)}, {snd_map, MAP_COUNT(snd_map)}
    };
    reset_fixture();
    CHECK(poll_keys(NO_KEYS));
    clear_draw_trace();
    save_framebuffer();
    mock_set_keys(KEYS(kb_KeyGraph));
    for (index = 0; index < ARRAY_COUNT(maps); ++index) {
        memset(keyboard_matrix, 0xff, sizeof(keyboard_matrix));
        apply_map(maps[index].map, maps[index].count);
        expect_matrix(0xff, 0xff, 0xff);
    }
    update_key_indicators();
    CHECK(draw_count(DRAW_TEXT) == 2);
    expect_cell(4, 0, "GRAPH", "MENU", true);
    expect_only_cell_changed(4, 0);
    clear_draw_trace();
    save_framebuffer();
    mock_set_keys(NO_KEYS);
    update_key_indicators();
    expect_cell(4, 0, "GRAPH", "MENU", false);
    expect_only_cell_changed(4, 0);
    mock_set_keys(KEYS(kb_KeyGraphVar));
    apply_map(base_map, MAP_COUNT(base_map));
    expect_matrix(0xff, 0xff, 0xef);
    CHECK(mock.scans == 0);
}

static void test_unchanged_hud(void)
{
    static const KeySnapshot empty = {{0}};
    KeySnapshot held[] = {empty, KEYS(kb_KeyYequ), KEYS(kb_Key2nd),
        KEYS(kb_KeyAlpha), KEYS(kb_KeyLeft, kb_KeyTrace)};
    size_t state;
    for (state = 0; state < ARRAY_COUNT(held); ++state) {
        unsigned int repeat;
        reset_fixture();
        CHECK(poll_keys(NO_KEYS));
        CHECK(poll_keys(held[state]));
        clear_draw_trace();
        save_framebuffer();
        for (repeat = 0; repeat < 4; ++repeat) {
            CHECK(poll_keys(held[state]));
            CHECK(mock.draw_count == 0);
            CHECK(mock.pixels == 0);
            CHECK(memcmp(guarded_vram, saved_vram, sizeof(saved_vram)) == 0);
            CHECK(memcmp(guarded_palette, saved_palette, sizeof(saved_palette)) == 0);
        }
    }
    reset_fixture();
    CHECK(poll_keys(NO_KEYS));
    clear_draw_trace();
    save_framebuffer();
    CHECK(poll_keys(KEYS(kb_KeyMath)));
    CHECK(io_keyboard_matrix_row(1) == 0xfb);
    CHECK(mock.draw_count == 0);
    CHECK(memcmp(guarded_vram, saved_vram, sizeof(saved_vram)) == 0);
}

static void test_function_highlights(void)
{
    size_t index;
    reset_fixture();
    CHECK(poll_keys(NO_KEYS));
    for (index = 0; index < ARRAY_COUNT(function_keys); ++index) {
        clear_draw_trace();
        save_framebuffer();
        CHECK(poll_keys(KEYS(function_keys[index])));
        CHECK(draw_count(DRAW_TEXT) == 2);
        expect_cell((unsigned int)index, 0, function_labels[index], odd_labels[index], true);
        expect_only_cell_changed((unsigned int)index, 0);
        CHECK(mock.foreground == 1 && mock.background == 0);
        clear_draw_trace();
        save_framebuffer();
        CHECK(poll_keys(NO_KEYS));
        CHECK(draw_count(DRAW_TEXT) == 2);
        expect_cell((unsigned int)index, 0, function_labels[index], odd_labels[index], false);
        expect_only_cell_changed((unsigned int)index, 0);
    }
}

static void test_bottom_highlights(void)
{
    static const kb_lkey_t keys[] = {
        kb_Key2nd, kb_KeyAlpha, kb_KeyMode, kb_KeyVars, kb_KeyClear
    };
    static const char *const labels[] = {"2nd", "ALPHA", "MODE", "VARS", "CLEAR"};
    static const char *const actions[] = {"FIRE", "ABC", "C=", "STOP", "EXIT"};
    size_t index;
    for (index = 0; index < ARRAY_COUNT(keys); ++index) {
        bool running = keys[index] != kb_KeyClear;
        reset_fixture();
        CHECK(poll_keys(NO_KEYS));
        clear_draw_trace();
        save_framebuffer();
        CHECK(poll_keys(KEYS(keys[index])) == running);
        CHECK(draw_count(DRAW_TEXT) == (index == 0 ? 12 : 2));
        expect_cell((unsigned int)index, 220, labels[index], actions[index], true);
        expect_matrix(0xff, 0xff,
                      keys[index] == kb_KeyMode ? 0xdf :
                      (keys[index] == kb_KeyVars ? 0x7f : 0xff));
        if (index != 0)
            expect_only_cell_changed((unsigned int)index, 220);
        else
            expect_protected_memory();
        clear_draw_trace();
        save_framebuffer();
        CHECK(poll_keys(NO_KEYS) == running);
        CHECK(draw_count(DRAW_TEXT) == (index == 0 ? 12 : 2));
        expect_cell((unsigned int)index, 220, labels[index], actions[index], false);
        expect_matrix(0xff, 0xff, 0xff);
        CHECK(io_quit_requested() == !running);
        if (index != 0)
            expect_only_cell_changed((unsigned int)index, 220);
        else
            expect_protected_memory();
    }
}

static void test_second_layer_functions(void)
{
    static const char *const second_labels[] = {"ALPHA", "TURBO", "SKIP", "VRAM"};
    unsigned int column;
    reset_fixture();
    CHECK(poll_keys(NO_KEYS));
    clear_draw_trace();
    save_framebuffer();
    CHECK(poll_keys(KEYS(kb_Key2nd, kb_KeyYequ, kb_KeyWindow, kb_KeyZoom, kb_KeyTrace)));
    expect_matrix(0xff, 0xff, 0xff);
    CHECK(io_joystick2() == 0xff);
    CHECK(draw_count(DRAW_TEXT) == 14);
    for (column = 0; column < 4; ++column)
        expect_cell(column, 0, function_labels[column], second_labels[column], false);
    expect_shortcut_status(0, true);
    expect_shortcut_status(1, true);
    expect_shortcut_status(2, true);
    expect_shortcut_status(3, true);
    expect_cell(0, 220, "2nd", "FIRE", true);
    expect_cell(4, 0, "GRAPH", "MENU", false);
    expect_protected_memory();
    clear_draw_trace();
    save_framebuffer();
    CHECK(poll_keys(KEYS(kb_Key2nd)));
    CHECK(mock.draw_count == 0);
    CHECK(memcmp(guarded_vram, saved_vram, sizeof(saved_vram)) == 0);
    CHECK(poll_keys(KEYS(kb_KeyYequ, kb_KeyWindow, kb_KeyZoom, kb_KeyTrace)));
    expect_matrix(0x87, 0xff, 0xff);
    for (column = 0; column < 4; ++column)
        expect_cell(column, 0, function_labels[column], odd_labels[column], true);
    expect_cell(0, 220, "2nd", "FIRE", false);
    CHECK(io_joystick2() == 0xff);
    expect_protected_memory();
}

static void test_second_layer_host_shortcuts(void)
{
    reset_fixture();
    CHECK(poll_keys(NO_KEYS));

    CHECK(poll_keys(KEYS(kb_Key2nd, kb_KeyYequ)));
    expect_matrix(0xff, 0xff, 0xff);
    CHECK(io_joystick2() == 0xff);
    CHECK(alpha_lock);
    CHECK(poll_keys(NO_KEYS));
    CHECK(poll_keys(KEYS(kb_Key7)));
    CHECK(io_keyboard_matrix_row(4) == 0xbf);

    CHECK(poll_keys(NO_KEYS));
    CHECK(poll_keys(KEYS(kb_Key2nd, kb_KeyWindow)));
    CHECK(cpu_turbo_index == 3 && mock.turbo_enabled && mock.turbo_multiplier == 200);
    CHECK(frame_skip && mock.frame_skip_enabled);
    CHECK(poll_keys(NO_KEYS));
    CHECK(poll_keys(KEYS(kb_Key2nd, kb_KeyWindow)));
    CHECK(cpu_turbo_index == 0 && !mock.turbo_enabled);

    CHECK(poll_keys(NO_KEYS));
    CHECK(poll_keys(KEYS(kb_Key2nd, kb_KeyZoom)));
    CHECK(!frame_skip && !mock.frame_skip_enabled && mock.frame_skip_calls == 2);

    CHECK(poll_keys(NO_KEYS));
    CHECK(poll_keys(KEYS(kb_Key2nd, kb_KeyTrace)));
    CHECK(vram_view_active);
    CHECK(mock.render_suppressed);
    CHECK(memcmp(gfx_vram, vram_view_source, FRAME_BYTES) == 0);
    CHECK(poll_keys(NO_KEYS));
    CHECK(poll_keys(KEYS(kb_Key2nd, kb_KeyTrace)));
    CHECK(!vram_view_active);
    CHECK(!mock.render_suppressed);
}

static void test_shifted_functions(void)
{
    static const kb_lkey_t cursors[] = {kb_KeyLeft, kb_KeyUp};
    static const char *const even_labels[] = {"F2", "F4", "F6", "F8"};
    size_t cursor;
    unsigned int function, column;
    for (cursor = 0; cursor < ARRAY_COUNT(cursors); ++cursor) {
        for (function = 0; function < ARRAY_COUNT(function_keys); ++function) {
            uint8_t cursor_mask = cursors[cursor] == kb_KeyLeft ? 0x04 : 0x80;
            reset_fixture();
            CHECK(poll_keys(NO_KEYS));
            clear_draw_trace();
            save_framebuffer();
            CHECK(poll_keys(KEYS(cursors[cursor], function_keys[function])));
            expect_matrix((uint8_t)~(cursor_mask | function_masks[function]), 0x7f, 0xff);
            CHECK(io_joystick2() == (cursors[cursor] == kb_KeyLeft ? 0xfb : 0xfe));
            CHECK(draw_count(DRAW_TEXT) == 10);
            for (column = 0; column < 4; ++column)
                expect_cell(column, 0, function_labels[column], even_labels[column],
                            column == function);
            expect_protected_memory();
            clear_draw_trace();
            save_framebuffer();
            CHECK(poll_keys(KEYS(function_keys[function])));
            expect_matrix((uint8_t)~function_masks[function], 0xff, 0xff);
            for (column = 0; column < 4; ++column)
                expect_cell(column, 0, function_labels[column], odd_labels[column],
                            column == function);
            expect_protected_memory();
            clear_draw_trace();
            save_framebuffer();
            CHECK(poll_keys(NO_KEYS));
            expect_cell(function, 0, function_labels[function], odd_labels[function], false);
            expect_only_cell_changed(function, 0);
        }
    }
}

static KeySnapshot hud_combination(unsigned int mask, kb_lkey_t cursor)
{
    static const kb_lkey_t candidates[] = {
        kb_KeyYequ, kb_KeyWindow, kb_KeyZoom, kb_KeyTrace,
        kb_Key2nd, kb_KeyAlpha, kb_KeyMode, kb_KeyVars
    };
    kb_lkey_t held[ARRAY_COUNT(candidates) + 1];
    size_t index, count = 0;
    for (index = 0; index < ARRAY_COUNT(candidates); ++index)
        if (mask & (1u << index))
            held[count++] = candidates[index];
    if (cursor)
        held[count++] = cursor;
    return mock_keys(held, count);
}

static void test_hud_band_bounds(void)
{
    static const kb_lkey_t cursors[] = {0, kb_KeyLeft, kb_KeyUp};
    size_t cursor, combinations = 0;
    unsigned int mask;
    reset_fixture();
    CHECK(poll_keys(NO_KEYS));
    save_framebuffer();
    for (cursor = 0; cursor < ARRAY_COUNT(cursors); ++cursor) {
        for (mask = 0; mask < 256; ++mask) {
            clear_draw_trace();
            CHECK(poll_keys(hud_combination(mask, cursors[cursor])));
            expect_protected_memory();
            ++combinations;
        }
    }
    CHECK(combinations == 768);
}

static void expect_no_frame_writes(void)
{
    CHECK(mock.draw_count == 0);
    CHECK(mock.pixels == 0);
    CHECK(memcmp(guarded_vram, saved_vram, sizeof(saved_vram)) == 0);
    CHECK(memcmp(guarded_palette, saved_palette, sizeof(saved_palette)) == 0);
}

static void test_indicators_disabled(void)
{
    static const kb_lkey_t cursors[] = {0, kb_KeyLeft, kb_KeyUp};
    size_t cursor, combinations = 0;
    unsigned int warm, mask;
    for (warm = 0; warm < 2; ++warm) {
        reset_fixture();
        if (warm) {
            CHECK(poll_keys(NO_KEYS));
            CHECK(poll_keys(KEYS(kb_Key2nd)));
        }
        indicators_enabled = false;
        clear_draw_trace();
        save_framebuffer();
        for (cursor = 0; cursor < ARRAY_COUNT(cursors); ++cursor) {
            for (mask = 0; mask < 256; ++mask) {
                CHECK(poll_keys(hud_combination(mask, cursors[cursor])));
                expect_no_frame_writes();
                ++combinations;
            }
        }
        CHECK(poll_keys(KEYS(kb_KeyYequ)));
        expect_matrix(0xef, 0xff, 0xff);
        expect_no_frame_writes();
        CHECK(!poll_keys(KEYS(kb_KeyClear)));
        CHECK(io_quit_requested());
        expect_no_frame_writes();
    }
    CHECK(combinations == 1536);
}

static void test_poll_timing(void)
{
    uint32_t before;
    reset_fixture();
    CHECK(poll_keys(NO_KEYS));
    clear_draw_trace();
    save_framebuffer();
    before = timer_Get(1);
    mock_set_keys(KEYS(kb_KeyYequ));
    mock_advance_ticks(POLL_TICKS - 1);
    CHECK(timer_Get(1) == before + POLL_TICKS - 1);
    CHECK(io_emulate());
    expect_matrix(0xff, 0xff, 0xff);
    expect_no_frame_writes();
    mock_advance_ticks(1);
    CHECK(io_emulate());
    expect_matrix(0xef, 0xff, 0xff);
    CHECK(draw_count(DRAW_TEXT) == 2);
    expect_only_cell_changed(0, 0);
    clear_draw_trace();
    save_framebuffer();
    CHECK(io_emulate());
    expect_no_frame_writes();
    CHECK(timer_Get(1) == before + POLL_TICKS);
}

static void test_mock_keypad_timer(void)
{
    uint32_t before;
    uint16_t enabled;
    uint8_t previous_mode;
    reset_fixture();
    save_framebuffer();
    CHECK(sizeof(timer_Control) == sizeof(uint16_t));
    CHECK(timer_Control == 0x0203);
    timer_Enable(2, TIMER_CPU, TIMER_0INT, TIMER_DOWN);
    enabled = timer_Control;
    CHECK(enabled == 0x022b);
    before = timer_Get(1);
    mock_set_keys(KEYS(kb_KeyGraph, kb_Key2nd));
    mock_queue_scan(NO_KEYS, 10);
    mock_queue_scan(KEYS(kb_KeyEnter, kb_KeyUp), 20);
    mock_queue_scan(NO_KEYS, 30);
    CHECK(kb_IsDown(kb_KeyGraph) == 1);
    CHECK(kb_IsDown(kb_KeyGraphVar) == 0);
    CHECK(kb_IsDown(kb_Key2nd) == 0x20);
    CHECK(kb_AnyKey() != 0);
    CHECK(mock.queue_position == 0);
    CHECK(timer_Disable(1) == (enabled & (uint16_t)~TIMER1_ENABLE));
    kb_Scan();
    CHECK(!kb_AnyKey());
    CHECK(timer_Get(1) == before);
    CHECK(!(timer_Control & TIMER1_ENABLE));
    CHECK(timer_Enable(1, TIMER_32K, TIMER_NOINT, TIMER_UP) == enabled);
    kb_Scan();
    CHECK(kb_IsDown(kb_KeyEnter) == 1);
    CHECK(kb_IsDown(kb_KeyUp) == 8);
    CHECK(timer_Get(1) == before + 20);
    CHECK(mock.queue_position == 2);
    CHECK(kb_AnyKey() != 0);
    CHECK(mock.queue_position == 2);
    kb_Scan();
    CHECK(!kb_AnyKey());
    CHECK(timer_Get(1) == before + 50);
    CHECK(mock.scans == 3 && mock.queue_position == mock.queue_count);
    CHECK(mock.timer_count == 4);
    CHECK(mock.timer[2].timer == 1 && !mock.timer[2].enabled);
    CHECK(mock.timer[2].control == (enabled & (uint16_t)~TIMER1_ENABLE));
    CHECK(mock.timer[3].enabled && mock.timer[3].control == enabled);
    previous_mode = kb_Config;
    kb_Config = 0xa0;
    CHECK(kb_SetMode(MODE_3_CONTINUOUS) == 0xa3);
    CHECK(kb_SetMode(MODE_1_INDISCRIMINATE) == 0xa1);
    kb_Config = previous_mode;
    expect_no_frame_writes();
}

static void test_mock_graphics(void)
{
    const DrawCall *text;
    reset_fixture();
    save_framebuffer();
    CHECK(gfx_SetColor(7) == 0);
    gfx_FillRectangle_NoClip(318, 0, 2, 2);
    CHECK(gfx_vram[318] == 7 && gfx_vram[SCREEN_WIDTH + 319] == 7);
    CHECK(mock.draw[1].kind == DRAW_FILL_RECT && mock.draw[1].color == 7);
    CHECK(gfx_SetColor(13) == 7);
    gfx_Rectangle_NoClip(316, 236, 4, 4);
    CHECK(gfx_vram[236 * SCREEN_WIDTH + 316] == 13);
    CHECK(gfx_vram[239 * SCREEN_WIDTH + 319] == 13);
    CHECK(gfx_vram[237 * SCREEN_WIDTH + 317] ==
          saved_vram[GUARD_BYTES + 237 * SCREEN_WIDTH + 317]);
    CHECK(mock.draw[3].kind == DRAW_RECT && mock.draw[3].color == 13);
    CHECK(gfx_GetStringWidth("WINDOW") == 48);
    gfx_SetTextScale(2, 2);
    CHECK(gfx_GetStringWidth("AB") == 32);
    CHECK(gfx_SetTextFGColor(5) == 1);
    CHECK(gfx_SetTextBGColor(2) == 0);
    CHECK(gfx_SetTextTransparentColor(255) == 255);
    gfx_PrintStringXY("A", 8, 1);
    text = &mock.draw[mock.draw_count - 1];
    CHECK(text->kind == DRAW_TEXT && strcmp(text->text, "A") == 0);
    CHECK(text->left == 8 && text->top == 1 && text->width == 16 && text->height == 16);
    CHECK(text->foreground == 5 && text->background == 2 && text->transparent == 255);
    CHECK(text->timer_control == timer_Control);
    CHECK(gfx_vram[SCREEN_WIDTH + 8] == 2);
    CHECK(gfx_vram[3 * SCREEN_WIDTH + 10] == 5);
    expect_protected_memory();
    gfx_SetTextScale(1, 1);
    clear_draw_trace();
    gfx_PrintStringXY("A", -7, 0);
    gfx_PrintStringXY("A", 319, 239);
    CHECK(mock.draw_count == 2);
    CHECK(mock.draw[0].left == -7 && mock.draw[1].top == 239);
    CHECK(mock.out_of_bounds == 119);
    CHECK(mock.pixels == 9 && mock.active_pixels == 0);
    CHECK(memcmp(gfx_vram + FRAME_BYTES, saved_vram + GUARD_BYTES + FRAME_BYTES,
                 VRAM_BYTES - FRAME_BYTES + GUARD_BYTES) == 0);
    CHECK(memcmp(guarded_vram, saved_vram, GUARD_BYTES) == 0);
}

static void test_mock_allocations(void)
{
    void *first, *second;
    reset_fixture();
    save_framebuffer();
    first = mock_malloc(73);
    require(first != NULL, "host allocation failed");
    CHECK(mock.last_allocation_size == 73 && mock.live_bytes == 73);
    memset(first, 0x5a, 73);
    mock.fail_next_allocation = true;
    CHECK(mock_malloc(19) == NULL);
    CHECK(!mock.fail_next_allocation && mock.live_bytes == 73);
    second = mock_malloc(19);
    require(second != NULL, "host allocation failed");
    CHECK(mock.last_allocation_size == 19 && mock.live_bytes == 92);
    mock_free(first);
    CHECK(mock.live_bytes == 19);
    mock_free(second);
    mock_free(NULL);
    CHECK(mock.malloc_calls == 3 && mock.free_calls == 3);
    CHECK(mock.live_bytes == 0);
    expect_no_frame_writes();
}

typedef struct {
    size_t renders, modes, timer_calls, scans, allocations, frees;
    uint32_t ticks;
    uint16_t control;
    uint8_t keypad_config;
} MenuStart;

static void menu_fixture(void)
{
    reset_fixture();
    CHECK(poll_keys(NO_KEYS));
    expect_matrix(0xff, 0xff, 0xff);
    CHECK(io_joystick2() == 0xff);
}

static MenuStart menu_start(void)
{
    clear_draw_trace();
    save_framebuffer();
    mock.menu_render_baseline = mock.render_requests;
    return (MenuStart){ .renders = mock.render_requests, .modes = mock.mode_calls,
        .timer_calls = mock.timer_count, .scans = mock.scans,
        .allocations = mock.malloc_calls, .frees = mock.free_calls,
        .ticks = timer_Get(1), .control = timer_Control,
        .keypad_config = kb_Config };
}

static void queue_key(KeySnapshot keys)
{
    mock_queue_scan(keys, MENU_SCAN_TICKS);
}

static void queue_observed(KeySnapshot keys, void (*observe)(void))
{
    queue_key(keys);
    mock.queue[mock.queue_count - 1].observe = observe;
}

static void queue_tap(kb_lkey_t key)
{
    queue_key(KEYS(key));
    queue_key(NO_KEYS);
}

static void queue_resume(kb_lkey_t key)
{
    queue_key(KEYS(key));
    queue_key(KEYS(key, kb_KeyMath, kb_Key2nd));
    queue_key(KEYS(kb_KeyMath, kb_Key2nd));
    queue_key(KEYS(kb_KeyMath));
    queue_key(NO_KEYS);
}

static void expect_menu_memory(void)
{
    /* io.c no longer restores the exact pre-menu pixels itself - that is
       now the real VIC's job on the next rendered frame (vic_request_render),
       which this host-only harness does not run. Only confirm the modal
       never wrote outside its own screen/palette bounds. */
    CHECK_MSG(memcmp(gfx_vram + FRAME_BYTES,
                    saved_vram + GUARD_BYTES + FRAME_BYTES,
                    VRAM_BYTES - FRAME_BYTES + GUARD_BYTES) == 0,
              "guest VRAM or trailing guard changed");
    CHECK(memcmp(guarded_vram, saved_vram, GUARD_BYTES) == 0);
    CHECK(memcmp(guarded_palette, saved_palette, sizeof(saved_palette)) == 0);
    CHECK(mock.out_of_bounds == 0);
}

static void expect_menu_selection(const uint8_t *selected, size_t count)
{
    size_t index, found = 0;
    CHECK_MSG(draw_count(DRAW_RECT) == count,
              "menu redraws: expected %zu, got %zu", count, draw_count(DRAW_RECT));
    for (index = 0; index < mock.draw_count; ++index) {
        const DrawCall *call = &mock.draw[index];
        if (call->kind != DRAW_FILL_RECT || call->color != 13)
            continue;
        CHECK_MSG(found < count, "unexpected selection highlight %zu", found);
        if (found < count) {
            CHECK_MSG(call->top == 98 + 14 * selected[found],
                      "redraw %zu: selected row top %d, expected %d (entry %u)",
                      found, call->top, 98 + 14 * selected[found],
                      (unsigned int)selected[found]);
            CHECK(call->left == 56 && call->width == 208 && call->height == 12);
        }
        ++found;
    }
    CHECK(found == count);
}

static void expect_menu_return(MenuStart start, bool running)
{
    const ModeCall *restored;
    size_t index;
    uint16_t stopped = start.control & (uint16_t)~TIMER1_ENABLE;
    uint32_t frozen = start.ticks + ((start.control & TIMER1_ENABLE) ? POLL_TICKS : 0);
    CHECK(io_quit_requested() == !running);
    CHECK(mock.queue_position == mock.queue_count);
    CHECK(!kb_AnyKey());
    expect_matrix(0xff, 0xff, 0xff);
    CHECK(io_joystick2() == 0xff);
    CHECK(mock.render_requests == start.renders + 1);
    CHECK(mock.malloc_calls == start.allocations && mock.free_calls == start.frees);
    CHECK(mock.timer_count == start.timer_calls + 1);
    if (mock.timer_count > start.timer_calls) {
        const TimerCall *call = &mock.timer[start.timer_calls];
        CHECK(call->timer == 1 && !call->enabled);
        CHECK(call->control == stopped);
    }
    CHECK(timer_Control == start.control);
    CHECK_MSG(timer_Get(1) == frozen, "timer1 jumped: expected %lu, got %lu",
              (unsigned long)frozen, (unsigned long)timer_Get(1));
    CHECK(mock.allocation_control == stopped);
    CHECK(mock.allocation_ticks == frozen);
    CHECK(mock.mode_calls == start.modes + 1);
    CHECK((kb_Config & 3u) == MODE_3_CONTINUOUS);
    CHECK((kb_Config & ~3u) == (start.keypad_config & ~3u));
    for (index = start.scans; index < mock.scans; ++index) {
        const ScanCall *call = &mock.scan[index];
        CHECK_MSG(call->control == stopped,
                  "scan %zu: timer control expected %04x, got %04x", index,
                  (unsigned int)stopped, (unsigned int)call->control);
        CHECK_MSG(call->before[0] == frozen && call->after[0] == frozen,
                  "scan %zu: timer1 advanced while modal (%lu -> %lu, expected %lu)",
                  index, (unsigned long)call->before[0],
                  (unsigned long)call->after[0], (unsigned long)frozen);
    }
    if (mock.mode_calls <= start.modes)
        return;
    restored = &mock.mode[start.modes];
    CHECK(restored->mode == MODE_3_CONTINUOUS);
    CHECK(restored->scan_position == mock.queue_count);
    CHECK(restored->timer_control == stopped && restored->timer_ticks == frozen);
    for (index = mock.allocation_draw_count; index < restored->draw_count; ++index) {
        const DrawCall *call = &mock.draw[index];
        CHECK_MSG(call->timer_control == stopped && call->timer_ticks == frozen,
                  "modal graphics call %zu kind %d: control=%04x ticks=%lu, expected %04x/%lu",
                  index, call->kind, (unsigned int)call->timer_control,
                  (unsigned long)call->timer_ticks, (unsigned int)stopped,
                  (unsigned long)frozen);
    }
    CHECK(mock.foreground == 1 && mock.background == 0);
    CHECK(mock.scale_x == 1 && mock.scale_y == 1 && mock.transparent == 255);
    expect_menu_memory();
}

static void expect_menu_idle(size_t renders, bool running)
{
    unsigned int repeat;
    size_t scans = mock.scans, modes = mock.mode_calls;
    clear_draw_trace();
    save_framebuffer();
    for (repeat = 0; repeat < 4; ++repeat) {
        CHECK(poll_keys(NO_KEYS) == running);
        expect_no_frame_writes();
        expect_matrix(0xff, 0xff, 0xff);
        CHECK(io_joystick2() == 0xff);
        CHECK(mock.render_requests == renders);
        CHECK(mock.scans == scans && mock.mode_calls == modes);
    }
}

static void expect_menu_values(int top, const char *const *values, size_t count)
{
    size_t index, found = 0;
    for (index = 0; index < mock.draw_count; ++index) {
        const DrawCall *call = &mock.draw[index];
        if (call->kind != DRAW_TEXT || call->top != top || call->left < 180)
            continue;
        CHECK_MSG(found < count, "unexpected menu value '%s' at row %d", call->text, top);
        if (found < count)
            CHECK_MSG(strcmp(call->text, values[found]) == 0,
                      "menu value %zu at row %d: expected '%s', got '%s'",
                      found, top, values[found], call->text);
        ++found;
    }
    CHECK_MSG(found == count, "menu values at row %d: expected %zu, got %zu",
              top, count, found);
}

static const char *const even_bank_labels[] = {"F2", "F4", "F6", "F8"};

static void choose_function_bank(bool even)
{
    static const uint8_t selected[] = {0, 1, 2, 2};
    const DrawCall *final_value = NULL;
    size_t index;
    unsigned int column;
    MenuStart start = menu_start();
    queue_key(NO_KEYS);
    queue_tap(kb_KeyDown);
    queue_tap(kb_KeyDown);
    queue_tap(even ? kb_KeyRight : kb_KeyLeft);
    queue_resume(kb_KeyGraph);
    CHECK(poll_keys(KEYS(kb_KeyGraph)));
    expect_menu_selection(selected, ARRAY_COUNT(selected));
    expect_menu_return(start, true);
    for (index = 0; index < mock.draw_count; ++index) {
        const DrawCall *call = &mock.draw[index];
        if (call->kind == DRAW_TEXT && call->left == 196 && call->top == 128)
            final_value = call;
    }
    CHECK(final_value != NULL);
    if (final_value) {
        CHECK(strcmp(final_value->text, even ? "F2/4/6/8" : "F1/3/5/7") == 0);
        CHECK(final_value->foreground == 0 && final_value->background == 13);
        CHECK(final_value->transparent == 255);
        CHECK(final_value->scale_x == 1 && final_value->scale_y == 1);
    }
    for (column = 0; column < 4; ++column)
        expect_cell(column, 0, function_labels[column],
                    even ? even_bank_labels[column] : odd_labels[column], false);
}

static void test_menu_held_graph_resume(void)
{
    size_t index, renders;
    uint32_t before;
    uint16_t control;
    MenuStart start;
    reset_fixture();
    CHECK(poll_keys(NO_KEYS));
    start = menu_start();
    renders = mock.render_requests;
    before = timer_Get(1);
    control = timer_Control;
    mock_queue_scan(KEYS(kb_KeyGraph), 1000);
    mock_queue_scan(KEYS(kb_KeyGraph, kb_KeyMath), 1000);
    mock_queue_scan(KEYS(kb_KeyMath), 1000);
    mock_queue_scan(NO_KEYS, 1000);
    mock_queue_scan(NO_KEYS, 1000);
    mock_queue_scan(KEYS(kb_KeyGraph), 1000);
    mock_queue_scan(KEYS(kb_KeyGraph, kb_KeyEnter), 1000);
    mock_queue_scan(KEYS(kb_KeyEnter), 1000);
    mock_queue_scan(NO_KEYS, 1000);
    CHECK(poll_keys(KEYS(kb_KeyGraph)));
    CHECK(!io_quit_requested());
    CHECK(mock.scans == 9 && mock.queue_position == mock.queue_count);
    CHECK(!kb_AnyKey());
    CHECK((kb_Config & 3u) == MODE_3_CONTINUOUS);
    CHECK(draw_count(DRAW_RECT) == 1);
    for (index = 0; index < mock.draw_count; ++index) {
        const DrawCall *call = &mock.draw[index];
        if (call->kind == DRAW_RECT) {
            CHECK_MSG(call->scan_position == 4,
                      "menu drawn after scan %zu, expected 4 (all entry keys released)",
                      call->scan_position);
            CHECK(call->left == 48 && call->top == 76);
            CHECK(call->width == 224 && call->height == 116);
        }
    }
    expect_text("Resume", 60, 100, true);
    expect_matrix(0xff, 0xff, 0xff);
    CHECK(io_joystick2() == 0xff);
    CHECK(mock.render_requests == renders + 1);
    CHECK(timer_Control == control);
    CHECK(timer_Get(1) == before + POLL_TICKS);
    CHECK(mock.malloc_calls == 0 && mock.free_calls == 0);
    CHECK(mock.active_pixels > 0 && mock.out_of_bounds == 0);
    CHECK(memcmp(gfx_vram + FRAME_BYTES,
                 saved_vram + GUARD_BYTES + FRAME_BYTES,
                 VRAM_BYTES - FRAME_BYTES + GUARD_BYTES) == 0);
    CHECK(memcmp(guarded_vram, saved_vram, GUARD_BYTES) == 0);
    CHECK(memcmp(guarded_palette, saved_palette, sizeof(saved_palette)) == 0);
    expect_menu_return(start, true);
    expect_menu_idle(start.renders + 1, true);
}

static void test_menu_enter_resume(void)
{
    static const uint8_t selected[] = {0};
    MenuStart start;
    menu_fixture();
    start = menu_start();
    queue_key(KEYS(kb_KeyGraph, kb_KeyEnter));
    queue_key(KEYS(kb_KeyEnter));
    queue_key(NO_KEYS);
    queue_tap(kb_KeyLeft);
    queue_tap(kb_KeyRight);
    queue_key(NO_KEYS);
    queue_resume(kb_KeyEnter);
    CHECK(poll_keys(KEYS(kb_KeyGraph, kb_KeyEnter)));
    expect_menu_selection(selected, ARRAY_COUNT(selected));
    expect_menu_return(start, true);
    CHECK(mock.scans == 13);
    expect_menu_idle(start.renders + 1, true);
}

static void test_menu_navigation_edges(void)
{
    static const uint8_t selected[] = {0, 1, 2, 3, 4, 5, 4, 3, 2, 1, 0};
    unsigned int repeat;
    MenuStart start;
    menu_fixture();
    start = menu_start();
    queue_key(NO_KEYS);
    queue_key(KEYS(kb_KeyUp));
    queue_tap(kb_KeyUp);
    for (repeat = 0; repeat < 5; ++repeat) {
        queue_key(KEYS(kb_KeyDown));
        queue_key(KEYS(kb_KeyDown));
        queue_key(KEYS(kb_KeyUp));
        queue_key(NO_KEYS);
    }
    for (repeat = 0; repeat < 5; ++repeat) {
        queue_key(KEYS(kb_KeyUp));
        queue_key(KEYS(kb_KeyUp));
        queue_key(KEYS(kb_KeyDown));
        queue_key(NO_KEYS);
    }
    queue_key(KEYS(kb_KeyMath));
    queue_key(KEYS(kb_KeyDown));
    queue_key(NO_KEYS);
    queue_resume(kb_KeyGraph);
    CHECK(poll_keys(KEYS(kb_KeyGraph)));
    expect_menu_selection(selected, ARRAY_COUNT(selected));
    expect_menu_return(start, true);
    CHECK(mock.scans == 52);
    expect_menu_idle(start.renders + 1, true);
}

static void test_menu_indicator_toggle(void)
{
    static const uint8_t selected[] = {0, 1, 1, 1, 1, 1, 1, 1, 1};
    static const char *const values[] = {
        "[x]", "[x]", "[ ]", "[x]", "[ ]", "[ ]", "[x]", "[x]", "[ ]"
    };
    static const uint8_t enabled_selected[] = {0, 1, 1};
    static const char *const enabled_values[] = {"[ ]", "[ ]", "[x]"};
    static const uint8_t blank_band[20 * SCREEN_WIDTH] = {0};
    MenuStart start;
    unsigned int column;
    menu_fixture();
    start = menu_start();
    queue_key(NO_KEYS);
    queue_tap(kb_KeyDown);
    queue_key(KEYS(kb_KeyEnter));
    queue_key(KEYS(kb_KeyEnter));
    queue_key(KEYS(kb_KeyRight));
    queue_key(NO_KEYS);
    queue_key(KEYS(kb_KeyEnter));
    queue_tap(kb_KeyEnter);
    queue_key(KEYS(kb_KeyLeft));
    queue_tap(kb_KeyLeft);
    queue_tap(kb_KeyLeft);
    queue_key(KEYS(kb_KeyRight));
    queue_tap(kb_KeyRight);
    queue_tap(kb_KeyRight);
    queue_tap(kb_KeyEnter);
    queue_resume(kb_KeyGraph);
    CHECK(poll_keys(KEYS(kb_KeyGraph)));
    expect_menu_selection(selected, ARRAY_COUNT(selected));
    expect_menu_values(114, values, ARRAY_COUNT(values));
    expect_menu_return(start, true);
    CHECK(memcmp(gfx_vram, blank_band, sizeof(blank_band)) == 0);
    CHECK(memcmp(gfx_vram + 220 * SCREEN_WIDTH, blank_band, sizeof(blank_band)) == 0);
    expect_menu_idle(start.renders + 1, true);
    CHECK(poll_keys(KEYS(kb_KeyYequ, kb_KeyWindow, kb_KeyZoom, kb_KeyTrace)));
    expect_matrix(0x87, 0xff, 0xff);
    expect_no_frame_writes();
    CHECK(poll_keys(NO_KEYS));
    expect_no_frame_writes();
    start = menu_start();
    queue_key(NO_KEYS);
    queue_tap(kb_KeyDown);
    queue_key(KEYS(kb_KeyRight));
    queue_tap(kb_KeyRight);
    queue_resume(kb_KeyGraph);
    CHECK(poll_keys(KEYS(kb_KeyGraph)));
    expect_menu_selection(enabled_selected, ARRAY_COUNT(enabled_selected));
    expect_menu_values(114, enabled_values, ARRAY_COUNT(enabled_values));
    expect_menu_return(start, true);
    for (column = 0; column < 4; ++column)
        expect_cell(column, 0, function_labels[column], odd_labels[column], false);
    expect_cell(4, 0, "GRAPH", "MENU", false);
    expect_cell(0, 220, "2nd", "FIRE", false);
    expect_cell(1, 220, "ALPHA", "ABC", false);
    expect_cell(2, 220, "MODE", "C=", false);
    expect_cell(3, 220, "VARS", "STOP", false);
    expect_cell(4, 220, "CLEAR", "EXIT", false);
    expect_menu_idle(start.renders + 1, true);
}

static void test_optimization_turbo_toggle(void)
{
    static const uint8_t multipliers[] = {1, 50, 100, 200, 1};
    menu_fixture();
    CHECK(cpu_turbo_index == 0 && !mock.turbo_enabled);
    for (unsigned int selection = 1; selection <= 5; ++selection)
    {
        unsigned int mode = selection % 5;
        queue_key(NO_KEYS);
        queue_tap(kb_KeyEnter);
        queue_tap(kb_KeyGraph);
        show_optimization_settings();
        CHECK(cpu_turbo_index == mode);
        CHECK(mock.turbo_enabled == (mode != 0));
        CHECK(mock.turbo_multiplier == multipliers[mode]);
        CHECK(mock.turbo_adaptive == (mode == 4));
        CHECK(frame_skip && mock.frame_skip_enabled);
    }
    queue_key(NO_KEYS);
    queue_tap(kb_KeyRight);
    queue_tap(kb_KeyRight);
    queue_tap(kb_KeyRight);
    queue_tap(kb_KeyRight);
    queue_tap(kb_KeyRight);
    queue_tap(kb_KeyGraph);
    show_optimization_settings();
    CHECK(cpu_turbo_index == 4 && mock.turbo_enabled && mock.turbo_adaptive);
    CHECK(frame_skip && mock.frame_skip_enabled);

    frame_skip = false;
    vic_set_frame_skip(false);
    queue_key(NO_KEYS);
    queue_tap(kb_KeyEnter);
    queue_tap(kb_KeyGraph);
    show_optimization_settings();
    CHECK(cpu_turbo_index == 0 && !mock.turbo_enabled);
    CHECK(!frame_skip && !mock.frame_skip_enabled);
}

static void test_optimization_frame_skip(void)
{
    menu_fixture();
    CHECK(frame_skip && mock.frame_skip_enabled);
    queue_key(NO_KEYS);
    queue_tap(kb_KeyDown);
    queue_tap(kb_KeyLeft);
    queue_tap(kb_KeyGraph);
    show_optimization_settings();
    CHECK(!frame_skip && !mock.frame_skip_enabled);

    queue_key(NO_KEYS);
    queue_tap(kb_KeyDown);
    queue_tap(kb_KeyRight);
    queue_tap(kb_KeyGraph);
    show_optimization_settings();
    CHECK(frame_skip && mock.frame_skip_enabled);
    CHECK(memcmp(guarded_vram, saved_vram, GUARD_BYTES) == 0);
    CHECK(memcmp(gfx_vram + FRAME_BYTES, saved_vram + GUARD_BYTES + FRAME_BYTES,
                 VRAM_BYTES - FRAME_BYTES + GUARD_BYTES) == 0);
}

static void test_menu_function_banks(void)
{
    unsigned int bank, column;
    menu_fixture();
    for (bank = 0; bank < 3; ++bank) {
        bool even = bank == 1;
        if (bank)
            choose_function_bank(even);
        for (column = 0; column < 4; ++column) {
            size_t renders = mock.render_requests;
            clear_draw_trace();
            save_framebuffer();
            CHECK(poll_keys(KEYS(function_keys[column])));
            expect_matrix((uint8_t)~function_masks[column], even ? 0x7f : 0xff, 0xff);
            CHECK(io_joystick2() == 0xff);
            CHECK(mock.render_requests == renders + 1);
            CHECK(draw_count(DRAW_TEXT) == 2);
            expect_cell(column, 0, function_labels[column],
                        even ? even_bank_labels[column] : odd_labels[column], true);
            expect_only_cell_changed(column, 0);
            clear_draw_trace();
            save_framebuffer();
            CHECK(poll_keys(NO_KEYS));
            expect_matrix(0xff, 0xff, 0xff);
            CHECK(mock.render_requests == renders + 2);
            CHECK(draw_count(DRAW_TEXT) == 2);
            expect_cell(column, 0, function_labels[column],
                        even ? even_bank_labels[column] : odd_labels[column], false);
            expect_only_cell_changed(column, 0);
        }
        clear_draw_trace();
        CHECK(poll_keys(KEYS(kb_KeyYequ, kb_KeyWindow, kb_KeyZoom, kb_KeyTrace)));
        expect_matrix(0x87, even ? 0x7f : 0xff, 0xff);
        CHECK(io_joystick2() == 0xff);
        for (column = 0; column < 4; ++column)
            expect_cell(column, 0, function_labels[column],
                        even ? even_bank_labels[column] : odd_labels[column], true);
        CHECK(poll_keys(NO_KEYS));
        expect_matrix(0xff, 0xff, 0xff);
    }
}

static void test_menu_second_suppression(void)
{
    unsigned int bank, combination, column;
    for (bank = 0; bank < 2; ++bank) {
        menu_fixture();
        choose_function_bank(bank != 0);
        for (combination = 0; combination < 5; ++combination) {
            KeySnapshot functions = combination < 4 ? KEYS(function_keys[combination]) :
                KEYS(kb_KeyYequ, kb_KeyWindow, kb_KeyZoom, kb_KeyTrace);
            KeySnapshot suppressed = functions;
            uint8_t mask = combination < 4 ? function_masks[combination] : 0x78;
            size_t renders;
            suppressed.rows[1] |= (uint8_t)kb_Key2nd;
            clear_draw_trace();
            save_framebuffer();
            CHECK(poll_keys(suppressed));
            expect_matrix(0xff, 0xff, 0xff);
            CHECK(io_joystick2() == 0xef);
            CHECK(draw_count(DRAW_TEXT) == 12);
            for (column = 0; column < 4; ++column)
                expect_cell(column, 0, function_labels[column], "--", false);
            expect_cell(0, 220, "2nd", "FIRE", true);
            expect_protected_memory();
            renders = mock.render_requests;
            clear_draw_trace();
            save_framebuffer();
            CHECK(poll_keys(suppressed));
            expect_no_frame_writes();
            CHECK(mock.render_requests == renders);
            CHECK(poll_keys(functions));
            expect_matrix((uint8_t)~mask, bank ? 0x7f : 0xff, 0xff);
            CHECK(io_joystick2() == 0xff);
            for (column = 0; column < 4; ++column)
                expect_cell(column, 0, function_labels[column],
                            bank ? even_bank_labels[column] : odd_labels[column],
                            combination == 4 || column == combination);
            expect_protected_memory();
            CHECK(poll_keys(NO_KEYS));
            expect_matrix(0xff, 0xff, 0xff);
        }
    }
}

static void test_menu_even_no_spurious_shift(void)
{
    const struct {
        KeySnapshot keys;
        uint8_t first_row, shift_row, last_row, joystick;
    } cases[] = {
        {NO_KEYS, 0xff, 0xff, 0xff, 0xff},
        {KEYS(kb_KeyMath), 0xff, 0xfb, 0xff, 0xff},
        {KEYS(kb_KeyAlpha), 0xff, 0xff, 0xff, 0xff},
        {KEYS(kb_KeyMode), 0xff, 0xff, 0xdf, 0xff},
        {KEYS(kb_KeyVars), 0xff, 0xff, 0x7f, 0xff},
        {KEYS(kb_KeyGraphVar), 0xff, 0xff, 0xef, 0xff},
        {KEYS(kb_KeyDown), 0x7f, 0xff, 0xff, 0xfd},
        {KEYS(kb_KeyRight), 0xfb, 0xff, 0xff, 0xf7},
        {KEYS(kb_KeyEnter), 0xfd, 0xff, 0xff, 0xff},
        {KEYS(kb_KeyDel), 0xfe, 0xff, 0xff, 0xff},
        {KEYS(kb_Key2nd), 0xff, 0xff, 0xff, 0xef}
    };
    size_t index;
    menu_fixture();
    choose_function_bank(true);
    expect_menu_idle(mock.render_requests, true);
    for (index = 0; index < ARRAY_COUNT(cases); ++index) {
        CHECK(poll_keys(cases[index].keys));
        expect_matrix(cases[index].first_row, cases[index].shift_row, cases[index].last_row);
        CHECK(io_joystick2() == cases[index].joystick);
        CHECK_MSG((io_keyboard_matrix_row(1) & 0x80) != 0,
                  "even bank injected LSHIFT without a function key in case %zu", index);
        CHECK((io_keyboard_matrix_row(0) & 0x78) == 0x78);
        CHECK(poll_keys(NO_KEYS));
        expect_matrix(0xff, 0xff, 0xff);
    }
}

static void test_menu_held_functions_idle(void)
{
    unsigned int bank, combination, repeat;
    for (bank = 0; bank < 2; ++bank) {
        menu_fixture();
        choose_function_bank(bank != 0);
        expect_menu_idle(mock.render_requests, true);
        for (combination = 0; combination < 5; ++combination) {
            KeySnapshot held = combination < 4 ? KEYS(function_keys[combination]) :
                KEYS(kb_KeyYequ, kb_KeyWindow, kb_KeyZoom, kb_KeyTrace);
            uint8_t mask = combination < 4 ? function_masks[combination] : 0x78;
            size_t renders, scans = mock.scans;
            CHECK(poll_keys(held));
            renders = mock.render_requests;
            clear_draw_trace();
            save_framebuffer();
            for (repeat = 0; repeat < 4; ++repeat) {
                CHECK(poll_keys(held));
                expect_matrix((uint8_t)~mask, bank ? 0x7f : 0xff, 0xff);
                CHECK(io_joystick2() == 0xff);
                expect_no_frame_writes();
                CHECK(mock.render_requests == renders && mock.scans == scans);
            }
            CHECK(poll_keys(NO_KEYS));
            expect_menu_idle(renders + 1, true);
        }
    }
}

static void expect_menu_layout(void)
{
    static const char *const labels[] = {
        "Settings", "PAUSED", "Resume", "Key indicators", "Function keys",
        "Turbo CPU", "Exit game", "Step debugger", "[x]", "[ ]", "1x",
        "F1/3/5/7", "F2/4/6/8"
    };
    bool seen[ARRAY_COUNT(labels)] = {false};
    size_t index, frame_start = 0, menu_texts = 0;
    for (index = 0; index < mock.draw_count; ++index) {
        const DrawCall *call = &mock.draw[index];
        if (call->kind == DRAW_RECT) {
            CHECK(call->left == MENU_LEFT && call->top == MENU_TOP);
            CHECK(call->width == MENU_WIDTH && call->height == MENU_HEIGHT);
            frame_start = index;
        }
        if (call->kind == DRAW_RECT || call->kind == DRAW_FILL_RECT) {
            CHECK(call->left >= MENU_LEFT && call->top >= MENU_TOP);
            CHECK(call->left + (int)call->width <= MENU_LEFT + MENU_WIDTH);
            CHECK(call->top + (int)call->height <= MENU_TOP + MENU_HEIGHT);
            CHECK(call->left >= 0 && call->left + (int)call->width <= 320);
            CHECK(call->top >= 20 && call->top + (int)call->height <= 220);
        }
        if (call->kind == DRAW_TEXT) {
            size_t other;
            CHECK(call->width == strlen(call->text) * 8u && call->height == 8);
            CHECK(call->scale_x == 1 && call->scale_y == 1);
            CHECK(call->left >= 0 && call->left + (int)call->width <= SCREEN_WIDTH);
            if (call->top < 20 || call->top >= 220) {
                CHECK((call->top >= 0 && call->top + (int)call->height <= 20) ||
                      (call->top >= 220 && call->top + (int)call->height <= SCREEN_HEIGHT));
                continue;
            }
            ++menu_texts;
            CHECK_MSG(call->left >= MENU_LEFT && call->top >= MENU_TOP &&
                      call->left + (int)call->width <= MENU_LEFT + MENU_WIDTH &&
                      call->top + (int)call->height <= MENU_TOP + MENU_HEIGHT,
                      "menu text '%s' at (%d,%d) size %ux%u exceeds fixed 224x116 bounds",
                      call->text, call->left, call->top, call->width, call->height);
            for (other = frame_start; other < index; ++other) {
                const DrawCall *previous = &mock.draw[other];
                if (previous->kind != DRAW_TEXT)
                    continue;
                CHECK_MSG(call->left + (int)call->width <= previous->left ||
                          previous->left + (int)previous->width <= call->left ||
                          call->top + (int)call->height <= previous->top ||
                          previous->top + (int)previous->height <= call->top,
                          "menu text '%s' overlaps '%s' at 8px character width",
                          call->text, previous->text);
            }
            for (other = 0; other < ARRAY_COUNT(labels); ++other)
                if (strcmp(call->text, labels[other]) == 0)
                    seen[other] = true;
        }
    }
    for (index = 0; index < ARRAY_COUNT(labels); ++index)
        CHECK_MSG(seen[index], "missing real menu label/value '%s'", labels[index]);
    CHECK(menu_texts == draw_count(DRAW_RECT) * 9);
    CHECK(mock.active_pixels > 0 && mock.out_of_bounds == 0);
}

static void test_menu_layout_bounds(void)
{
    static const uint8_t selected[] = {0, 1, 1, 2, 2, 2, 1, 1};
    static const char *const banks[] = {
        "F1/3/5/7", "F1/3/5/7", "F1/3/5/7", "F1/3/5/7",
        "F2/4/6/8", "F1/3/5/7", "F1/3/5/7", "F1/3/5/7"
    };
    MenuStart start;
    menu_fixture();
    start = menu_start();
    queue_key(NO_KEYS);
    queue_tap(kb_KeyDown);
    queue_tap(kb_KeyEnter);
    queue_tap(kb_KeyDown);
    queue_key(KEYS(kb_KeyEnter));
    queue_key(KEYS(kb_KeyEnter));
    queue_key(KEYS(kb_KeyLeft));
    queue_key(NO_KEYS);
    queue_tap(kb_KeyEnter);
    queue_tap(kb_KeyUp);
    queue_tap(kb_KeyEnter);
    queue_resume(kb_KeyGraph);
    CHECK(poll_keys(KEYS(kb_KeyGraph)));
    expect_menu_selection(selected, ARRAY_COUNT(selected));
    expect_menu_values(128, banks, ARRAY_COUNT(banks));
    expect_menu_layout();
    expect_menu_return(start, true);
    expect_menu_idle(start.renders + 1, true);
}

static void test_menu_timer_freeze_resume(void)
{
    MenuStart start;
    uint32_t timer2_before, timer3_before, resumed;
    size_t index;
    menu_fixture();
    kb_Config = 0xa3;
    timer_Enable(1, TIMER_32K, TIMER_0INT, TIMER_UP);
    timer_Enable(2, TIMER_CPU, TIMER_0INT, TIMER_DOWN);
    timer_Enable(3, TIMER_32K, TIMER_NOINT, TIMER_UP);
    timer_Control |= 0x8000u;
    start = menu_start();
    timer2_before = timer_Get(2);
    timer3_before = timer_Get(3);
    queue_key(KEYS(kb_KeyGraph));
    queue_key(NO_KEYS);
    queue_tap(kb_KeyDown);
    queue_tap(kb_KeyDown);
    queue_tap(kb_KeyRight);
    queue_tap(kb_KeyLeft);
    queue_resume(kb_KeyGraph);
    CHECK(poll_keys(KEYS(kb_KeyGraph)));
    expect_menu_return(start, true);
    CHECK(mock.scans == 15);
    for (index = start.scans; index < mock.scans; ++index) {
        const ScanCall *call = &mock.scan[index];
        CHECK(call->after[1] == call->before[1] - MENU_SCAN_TICKS);
        CHECK(call->after[2] == call->before[2] + MENU_SCAN_TICKS);
    }
    CHECK(timer_Get(2) == timer2_before - POLL_TICKS - 15u * MENU_SCAN_TICKS);
    CHECK(timer_Get(3) == timer3_before + POLL_TICKS + 15u * MENU_SCAN_TICKS);
    resumed = timer_Get(1);
    clear_draw_trace();
    save_framebuffer();
    mock_set_keys(KEYS(kb_KeyYequ));
    mock_advance_ticks(POLL_TICKS - 1);
    CHECK(io_emulate());
    CHECK(timer_Get(1) == resumed + POLL_TICKS - 1);
    expect_matrix(0xff, 0xff, 0xff);
    expect_no_frame_writes();
    CHECK(mock.render_requests == start.renders + 1);
    mock_advance_ticks(1);
    CHECK(io_emulate());
    CHECK(timer_Get(1) == resumed + POLL_TICKS);
    expect_matrix(0xef, 0xff, 0xff);
    expect_cell(0, 0, "Y=", "F1", true);
    CHECK(mock.render_requests == start.renders + 2);
    CHECK(mock.scans == 15 && timer_Control == start.control);
}

static void test_menu_disabled_timer(void)
{
    MenuStart start;
    uint32_t timer2_before;
    menu_fixture();
    kb_Config = 0xb3;
    timer_Enable(1, TIMER_32K, TIMER_0INT, TIMER_UP);
    timer_Enable(2, TIMER_CPU, TIMER_0INT, TIMER_UP);
    timer_Control |= 0x8000u;
    mock_advance_ticks(POLL_TICKS);
    timer_Disable(1);
    start = menu_start();
    timer2_before = timer_Get(2);
    queue_key(KEYS(kb_KeyGraph));
    queue_key(NO_KEYS);
    queue_resume(kb_KeyEnter);
    CHECK(poll_keys(KEYS(kb_KeyGraph)));
    expect_menu_return(start, true);
    CHECK(mock.scans == 7);
    CHECK(timer_Get(2) == timer2_before + POLL_TICKS + 7u * MENU_SCAN_TICKS);
    CHECK(!(timer_Control & TIMER1_ENABLE));
    mock_advance_ticks(10u * MENU_SCAN_TICKS);
    CHECK(timer_Get(1) == start.ticks);
    CHECK(timer_Get(2) == timer2_before + POLL_TICKS + 17u * MENU_SCAN_TICKS);
    expect_menu_idle(start.renders + 1, true);
    CHECK(timer_Control == start.control && timer_Get(1) == start.ticks);
}

static void observe_menu_guest_idle(void)
{
    expect_matrix(0xff, 0xff, 0xff);
    CHECK(io_joystick2() == 0xff);
    CHECK_MSG(mock.render_requests == mock.menu_render_baseline,
              "menu-only scan %zu forced a game render", mock.queue_position);
    CHECK(!(timer_Control & TIMER1_ENABLE));
}

static void test_menu_input_isolation(void)
{
    static const uint8_t selected[] = {0, 1, 1, 2, 2, 1, 0};
    unsigned int held_guest;
    for (held_guest = 0; held_guest < 2; ++held_guest) {
        const KeySnapshot scans[] = {
            KEYS(kb_KeyGraph, kb_KeyYequ, kb_KeyRight, kb_Key2nd),
            KEYS(kb_KeyYequ, kb_KeyRight, kb_Key2nd), KEYS(kb_Key2nd), NO_KEYS,
            KEYS(kb_KeyDown), KEYS(kb_KeyDown, kb_KeyUp), NO_KEYS,
            KEYS(kb_KeyRight), NO_KEYS, KEYS(kb_KeyDown), NO_KEYS,
            KEYS(kb_KeyLeft), NO_KEYS, KEYS(kb_KeyUp), NO_KEYS,
            KEYS(kb_KeyUp), NO_KEYS,
            KEYS(kb_KeyYequ, kb_KeyWindow, kb_KeyZoom, kb_KeyTrace,
                 kb_Key2nd, kb_KeyAlpha, kb_KeyMath), NO_KEYS,
            KEYS(kb_KeyEnter),
            KEYS(kb_KeyEnter, kb_KeyGraph, kb_KeyUp, kb_KeyDown, kb_KeyLeft,
                 kb_KeyRight, kb_Key2nd, kb_KeyYequ, kb_KeyWindow, kb_KeyZoom,
                 kb_KeyTrace, kb_KeyAlpha, kb_KeyMath),
            KEYS(kb_Key2nd, kb_KeyMath), KEYS(kb_KeyMath), NO_KEYS
        };
        MenuStart start;
        size_t index;
        menu_fixture();
        if (held_guest) {
            CHECK(poll_keys(KEYS(kb_KeyYequ, kb_KeyRight)));
            expect_matrix(0xeb, 0xff, 0xff);
            CHECK(io_joystick2() == 0xf7);
        }
        start = menu_start();
        for (index = 0; index < ARRAY_COUNT(scans); ++index)
            queue_observed(scans[index], observe_menu_guest_idle);
        CHECK(poll_keys(KEYS(kb_KeyGraph, kb_KeyYequ, kb_KeyRight, kb_Key2nd)));
        expect_menu_selection(selected, ARRAY_COUNT(selected));
        expect_menu_return(start, true);
        CHECK(mock.scan_observations == ARRAY_COUNT(scans));
        CHECK(mock.scans == ARRAY_COUNT(scans));
        clear_draw_trace();
        save_framebuffer();
        CHECK(poll_keys(NO_KEYS));
        expect_no_frame_writes();
        CHECK_MSG(mock.render_requests == start.renders + 1 + held_guest,
                  "post-menu idle: expected %zu renders (including %u cached guest release), got %zu",
                  start.renders + 1 + held_guest, held_guest, mock.render_requests);
        expect_menu_idle(start.renders + 1 + held_guest, true);
    }
}

static void test_menu_explicit_quit(void)
{
    static const uint8_t selected[] = {0, 1, 2, 3, 4};
    MenuStart start;
    menu_fixture();
    start = menu_start();
    queue_key(NO_KEYS);
    queue_tap(kb_KeyDown);
    queue_tap(kb_KeyDown);
    queue_tap(kb_KeyDown);
    queue_tap(kb_KeyDown);
    queue_tap(kb_KeyLeft);
    queue_tap(kb_KeyRight);
    queue_resume(kb_KeyEnter);
    CHECK(!poll_keys(KEYS(kb_KeyGraph)));
    expect_menu_selection(selected, ARRAY_COUNT(selected));
    expect_menu_return(start, false);
    CHECK(mock.scans == 18);
    expect_menu_idle(start.renders + 1, false);
}

static void test_menu_clear_exit(void)
{
    static const uint8_t selected[] = {0, 1, 2, 3, 4, 5};
    unsigned int entry, down;
    for (entry = 0; entry < 6; ++entry) {
        MenuStart start;
        menu_fixture();
        start = menu_start();
        queue_key(NO_KEYS);
        for (down = 0; down < entry; ++down)
            queue_tap(kb_KeyDown);
        queue_resume(kb_KeyClear);
        CHECK(!poll_keys(KEYS(kb_KeyGraph)));
        expect_menu_selection(selected, entry + 1);
        expect_menu_return(start, false);
        CHECK(mock.scans == 6 + 2 * entry);
        expect_menu_idle(start.renders + 1, false);
    }
}

typedef struct { const char *name; void (*run)(void); } Test;
static void run_test(void (*body)(void))
{
    if (setjmp(test_escape) == 0)
        body();
}

static const Test tests[] = {
    {"palette-init", test_palette_init},
    {"initial-hud", test_initial_hud},
    {"first-poll-2nd", test_first_poll_2nd},
    {"base-function-mapping", test_base_function_mapping},
    {"graph-not-guest-input", test_graph_not_guest_input},
    {"unchanged-hud", test_unchanged_hud},
    {"function-highlights", test_function_highlights},
    {"bottom-highlights", test_bottom_highlights},
    {"second-layer-functions", test_second_layer_functions},
    {"second-layer-host-shortcuts", test_second_layer_host_shortcuts},
    {"shifted-functions", test_shifted_functions},
    {"hud-band-bounds", test_hud_band_bounds},
    {"indicators-disabled", test_indicators_disabled},
    {"poll-timing", test_poll_timing},
    {"mock-keypad-timer", test_mock_keypad_timer},
    {"mock-graphics", test_mock_graphics},
    {"mock-allocations", test_mock_allocations},
    {"menu-held-graph-resume", test_menu_held_graph_resume},
    {"menu-enter-resume", test_menu_enter_resume},
    {"menu-navigation-edges", test_menu_navigation_edges},
    {"menu-indicator-toggle", test_menu_indicator_toggle},
    {"optimization-turbo-toggle", test_optimization_turbo_toggle},
    {"optimization-frame-skip", test_optimization_frame_skip},
    {"menu-function-banks", test_menu_function_banks},
    {"menu-second-suppression", test_menu_second_suppression},
    {"menu-even-no-spurious-shift", test_menu_even_no_spurious_shift},
    {"menu-held-functions-idle", test_menu_held_functions_idle},
    {"menu-layout-bounds", test_menu_layout_bounds},
    {"menu-timer-freeze-resume", test_menu_timer_freeze_resume},
    {"menu-disabled-timer", test_menu_disabled_timer},
    {"menu-input-isolation", test_menu_input_isolation},
    {"menu-explicit-quit", test_menu_explicit_quit},
    {"menu-clear-exit", test_menu_clear_exit}
};

int main(int argc, char **argv)
{
    size_t index, selected = 0, passed = 0;
    int argument;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (index = 0; index < ARRAY_COUNT(tests); ++index)
            puts(tests[index].name);
        return EXIT_SUCCESS;
    }
    for (argument = 1; argument < argc; ++argument) {
        bool found = false;
        for (index = 0; index < ARRAY_COUNT(tests); ++index)
            if (strcmp(argv[argument], tests[index].name) == 0)
                found = true;
        if (!found) {
            fprintf(stderr, "Unknown test '%s'; use --list for valid names.\n", argv[argument]);
            return EXIT_FAILURE;
        }
    }
    for (index = 0; index < ARRAY_COUNT(tests); ++index) {
        bool enabled = argc == 1;
        size_t failures_before = failures;
        for (argument = 1; argument < argc; ++argument)
            if (strcmp(argv[argument], tests[index].name) == 0)
                enabled = true;
        if (!enabled)
            continue;
        ++selected;
        current_test = tests[index].name;
        run_test(tests[index].run);
        CHECK(mock.live_bytes == 0);
        if (failures == failures_before) {
            ++passed;
            printf("PASS %s\n", current_test);
        } else {
            printf("FAIL %s\n", current_test);
        }
    }
    printf("%zu/%zu tests passed; %zu checks; %zu failures\n",
           passed, selected, checks, failures);
    if (!selected)
        fprintf(stderr, "No tests selected; use --list for valid names.\n");
    return selected && passed == selected ? EXIT_SUCCESS : EXIT_FAILURE;
}

int mem_ready(void)
{
    return 1;
}

uint8_t mem_debug_peek_cpu(uint16_t addr)
{
    return (uint8_t)addr;
}

uint8_t mem_debug_peek_ram(uint16_t addr)
{
    return (uint8_t)addr;
}