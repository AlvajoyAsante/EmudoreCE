#ifndef UI_MOCK_H
#define UI_MOCK_H

#include "browser.h"
#include "browser_storage.h"
#include <setjmp.h>
#include <stddef.h>

#define UI_PIXELS (320u * 240u)
#define UI_MAX_DRAWS 40000
#define UI_MAX_ACTIVE 128
#define UI_CHECK(condition) \
    ((condition) ? (void)0 : ui_fail(__FILE__, __LINE__, #condition))

enum {
    UI_NONE = 0, UI_UP = 1, UI_DOWN = 2, UI_LEFT = 4, UI_RIGHT = 8,
    UI_VIEW = 16, UI_MARK = 32, UI_HIDE = 64, UI_ENTER = 128,
    UI_CLEAR = 256, UI_MODE = 512
};

typedef enum { UI_FILL, UI_RECTANGLE, UI_TEXT, UI_SPRITE } UiDrawKind;

typedef struct {
    UiDrawKind kind;
    int left, top;
    unsigned int width, height, scan;
    uint8_t foreground, background, transparent, sprite_kind;
    char text[128];
} UiDraw;

typedef struct {
    DriveFileInfo info;
    uint8_t lock;
    bool valid, name_valid;
} UiFile;

typedef enum {
    UI_COUNT, UI_IDENTITY, UI_NAME, UI_INFO, UI_INVALIDATE, UI_CLOSE,
    UI_GC_INSTALL, UI_SAVE_START, UI_GFX_END, UI_GFX_BEGIN, UI_PALETTE,
    UI_SAVE_END, UI_GC_CLEAR, UI_REMAP, UI_SELECT, UI_DISABLE,
    UI_ALLOCATE, UI_FREE, UI_LOAD_PREFERENCES, UI_LOAD_INFO
} UiCallKind;

typedef struct {
    UiCallKind kind;
    unsigned int value, scan;
} UiCall;

typedef struct {
    uint8_t file_count;
    unsigned int scans, queued, draws, saves, selects, disables, closes;
    unsigned int allocations, frees;
    unsigned int names, infos, preference_loads, info_loads, active_count;
    unsigned int last_scan_draws;
    unsigned int calls, invalidations, remaps, gc_installs, gc_clears;
    unsigned int gfx_begins, gfx_ends, palette_sets, gc_cycles, simulate_gc;
    unsigned int allocation_attempts;
    size_t allocation_bytes;
    size_t peak_bytes;
    void *allocation, *allocation_base;
    bool allocation_failure, mapped;
    bool identified, remap_ok, remap_identified, saving, save_complete;
    bool direct_open, graphics_active, checked_identity, checked_count;
    bool needs_palette;
    uint8_t remap_count;
    DriveIdentity identity, remap_identity, saved_identity;
    BrowserSaveResult save_result;
    uint8_t selected, color, foreground, background, text_transparent;
    uint8_t transparent, scale_x, scale_y;
    uint8_t saved_count;
    BrowserPreferences preferences, saved_preferences;
    void (*gc_before)(void);
    void (*gc_after)(void);
    UiFile files[255];
    bool allowed_indices[255];
    uint8_t name_indices[UI_MAX_DRAWS], info_indices[UI_MAX_DRAWS];
    UiDraw draw[UI_MAX_DRAWS];
    UiCall call[UI_MAX_DRAWS];
    unsigned int active[UI_MAX_ACTIVE];
    const uint8_t *sprite_sources[5];
} UiState;

extern UiState ui;
extern jmp_buf ui_jump;

_Noreturn void ui_fail(const char *file, int line, const char *condition);
void ui_reset(uint8_t count);
void ui_queue(unsigned int keys);
void ui_tap(unsigned int keys);
void ui_quiet(unsigned int keys);
void ui_probe(void (*inspect)(void));
void ui_allow_indices(const uint8_t *indices, size_t count);
const UiDraw *ui_text_at(int left, int top);
bool ui_has_text(const char *text);
bool ui_has_sprite(uint8_t kind, int left, int top);
unsigned int ui_call_position(UiCallKind kind);
void ui_verify_guards(void);
void ui_verify_handoff(void);
void ui_verify_return(void);
void *ui_calloc(size_t count, size_t size);
void ui_free(void *pointer);

#endif