#include "mock.h"
#include "CEPackage.h"
#include "browser_icons.h"
#include "font.h"
#include <fileioc.h>
#include <graphx.h>
#include <keypadc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

UiState ui;
jmp_buf ui_jump;
uint8_t kb_Data[8];

static struct {
    uint8_t before[32];
    uint8_t pixels[UI_PIXELS];
    uint8_t guest_ram[UI_PIXELS];
    uint8_t after[32];
} video;
static struct {
    uint16_t before[16], colors[256], after[16];
} palette;
static struct {
    unsigned int keys;
    bool quiet;
    void (*inspect)(void);
} input[4096];
uint8_t *const gfx_vram = video.pixels;
uint16_t *const gfx_palette = palette.colors;

_Static_assert(offsetof(gfx_sprite_t, data) == 2, "SDK sprite data offset");
_Static_assert(sizeof(BrowserIcon) == 66, "Browser icon byte layout");
_Static_assert(32 % _Alignof(max_align_t) == 0, "Allocation guard alignment");

static void call(UiCallKind kind, unsigned int value)
{
    UI_CHECK(ui.calls < UI_MAX_DRAWS);
    ui.call[ui.calls++] = (UiCall){kind, value, ui.scans};
}

unsigned int ui_call_position(UiCallKind kind)
{
    for (unsigned int index = 0; index < ui.calls; ++index)
        if (ui.call[index].kind == kind)
            return index;
    UI_CHECK(false);
    return 0;
}

_Noreturn void ui_fail(const char *file, int line, const char *condition)
{
    fprintf(stderr, "%s:%d: %s\n", file, line, condition);
    longjmp(ui_jump, 1);
}

void ui_reset(uint8_t count)
{
    free(ui.allocation_base);
    memset(&ui, 0, sizeof(ui));
    memset(&video, 0xa5, sizeof(video));
    memset(&palette, 0x5a, sizeof(palette));
    memset(input, 0, sizeof(input));
    memset(kb_Data, 0, sizeof(kb_Data));
    ui.file_count = count;
    ui.mapped = true;
    ui.direct_open = true;
    ui.graphics_active = true;
    ui.identified = ui.remap_ok = ui.remap_identified = true;
    ui.remap_count = count;
    ui.identity = (DriveIdentity){174848, 0x12345678};
    ui.remap_identity = ui.identity;
    ui.foreground = 1;
    ui.text_transparent = 255;
    ui.scale_x = ui.scale_y = 1;
    for (unsigned int index = 0; index < count; ++index) {
        snprintf(ui.files[index].info.name, sizeof(ui.files[index].info.name),
                 "PRG%03u", index);
        ui.files[index].info.size = 4098 + index;
        ui.files[index].info.load_address = (uint16_t)(0x0801 + index);
        ui.files[index].valid = ui.files[index].name_valid = true;
        ui.allowed_indices[index] = true;
    }
}

void ui_queue(unsigned int keys)
{
    UI_CHECK(ui.queued < sizeof(input) / sizeof(input[0]));
    input[ui.queued++].keys = keys;
}

void ui_tap(unsigned int keys)
{
    ui_queue(keys);
    ui_queue(UI_NONE);
}

void ui_quiet(unsigned int keys)
{
    ui_queue(keys);
    input[ui.queued - 1].quiet = true;
}

void ui_probe(void (*inspect)(void))
{
    ui_queue(UI_NONE);
    input[ui.queued - 1].inspect = inspect;
}

void ui_allow_indices(const uint8_t *indices, size_t count)
{
    memset(ui.allowed_indices, 0, sizeof(ui.allowed_indices));
    for (size_t index = 0; index < count; ++index) {
        UI_CHECK(indices[index] < ui.file_count);
        ui.allowed_indices[indices[index]] = true;
    }
}

void kb_Scan(void)
{
    static const kb_lkey_t keys[] = {
        kb_KeyUp, kb_KeyDown, kb_KeyLeft, kb_KeyRight, kb_KeyYequ,
        kb_KeyAlpha, kb_KeyTrace, kb_KeyEnter, kb_KeyClear, kb_KeyMode
    };
    unsigned int pressed;
    if (ui.scans && input[ui.scans - 1].quiet)
        UI_CHECK(ui.draws == ui.last_scan_draws);
    UI_CHECK(ui.scans < ui.queued);
    if (input[ui.scans].inspect)
        input[ui.scans].inspect();
    pressed = input[ui.scans++].keys;
    ui.last_scan_draws = ui.draws;
    memset(kb_Data, 0, sizeof(kb_Data));
    for (unsigned int index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index)
        if (pressed & (1u << index))
            kb_Data[keys[index] >> 8] |= (uint8_t)keys[index];
}

void ui_verify_guards(void)
{
    for (size_t index = 0; index < sizeof(video.before); ++index) {
        UI_CHECK(video.before[index] == 0xa5);
        UI_CHECK(video.after[index] == 0xa5);
    }
    for (size_t index = 0; index < UI_PIXELS; ++index)
        UI_CHECK(video.guest_ram[index] == 0xa5);
    for (size_t index = 0; index < 16; ++index) {
        UI_CHECK(palette.before[index] == 0x5a5a);
        UI_CHECK(palette.after[index] == 0x5a5a);
    }
    for (size_t index = 16; index < 256; ++index)
        UI_CHECK(palette.colors[index] == 0x5a5a);
    for (size_t index = 0; index < 5; ++index)
        if (ui.sprite_sources[index])
            UI_CHECK(memcmp(ui.sprite_sources[index], &browser_icons[index],
                            sizeof(BrowserIcon)) == 0);
}

void ui_verify_handoff(void)
{
    ui_verify_guards();
    UI_CHECK(ui.scans == ui.queued);
    UI_CHECK(ui.allocation == NULL);
    UI_CHECK(ui.allocation_base == NULL);
    UI_CHECK(ui.allocations == ui.frees);
    UI_CHECK(ui.graphics_active);
    UI_CHECK(ui.gc_before == NULL && ui.gc_after == NULL);
    UI_CHECK(ui.gfx_begins == ui.gfx_ends);
    UI_CHECK(ui.scale_x == 1 && ui.scale_y == 1);
    UI_CHECK(ui.foreground == 1 && ui.background == 0);
    UI_CHECK(ui.text_transparent == 255);
    UI_CHECK(palette.colors[0] == gfx_RGBTo1555(0, 0, 0));
    UI_CHECK(palette.colors[1] == gfx_RGBTo1555(255, 255, 255));
    for (size_t index = 0; index < UI_PIXELS; ++index)
        UI_CHECK(video.pixels[index] == 0);
}

void ui_verify_return(void)
{
    ui_verify_handoff();
    if (ui.transparent != 0)
        fprintf(stderr, "sprite transparency: expected 0, observed %u\n", ui.transparent);
    UI_CHECK(ui.transparent == 0);
}

void *ui_calloc(size_t count, size_t size)
{
    UI_CHECK(ui.allocation == NULL);
    UI_CHECK(count == 1 && size < 2048);
    ++ui.allocation_attempts;
    ui.allocation_bytes = count * size;
    call(UI_ALLOCATE, (unsigned int)ui.allocation_bytes);
    if (ui.allocation_failure)
        return NULL;
    ui.allocation_base = calloc(1, ui.allocation_bytes + 64);
    UI_CHECK(ui.allocation_base != NULL);
    ui.allocation = (uint8_t *)ui.allocation_base + 32;
    memset(ui.allocation_base, 0xa6, 32);
    memset((uint8_t *)ui.allocation + ui.allocation_bytes, 0xa6, 32);
    ui.peak_bytes = ui.allocation_bytes;
    ++ui.allocations;
    return ui.allocation;
}

void ui_free(void *pointer)
{
    UI_CHECK(pointer == ui.allocation && pointer != NULL);
    for (size_t index = 0; index < 32; ++index) {
        UI_CHECK(((uint8_t *)ui.allocation_base)[index] == 0xa6);
        UI_CHECK(((uint8_t *)ui.allocation)[ui.allocation_bytes + index] == 0xa6);
    }
    call(UI_FREE, 0);
    free(ui.allocation_base);
    ui.allocation = NULL;
    ui.allocation_base = NULL;
    ++ui.frees;
}

static void bounds(int left, int top, unsigned int width, unsigned int height)
{
    UI_CHECK(left >= 0 && top >= 0);
    UI_CHECK(width <= 320 && height <= 240);
    UI_CHECK((unsigned int)left + width <= 320);
    UI_CHECK((unsigned int)top + height <= 240);
}

static bool overlaps(const UiDraw *first, const UiDraw *second)
{
    return first->left < second->left + (int)second->width &&
           second->left < first->left + (int)first->width &&
           first->top < second->top + (int)second->height &&
           second->top < first->top + (int)first->height;
}

static UiDraw *record(UiDrawKind kind, int left, int top,
                      unsigned int width, unsigned int height)
{
    UiDraw *draw;
    UI_CHECK(ui.graphics_active);
    UI_CHECK(!ui.needs_palette);
    bounds(left, top, width, height);
    UI_CHECK(ui.draws < UI_MAX_DRAWS);
    draw = &ui.draw[ui.draws++];
    draw->kind = kind;
    draw->left = left;
    draw->top = top;
    draw->width = width;
    draw->height = height;
    draw->scan = ui.scans;
    draw->foreground = ui.foreground;
    draw->background = ui.background;
    draw->transparent = kind == UI_TEXT ? ui.text_transparent : ui.transparent;
    for (unsigned int index = 0; index < ui.active_count;) {
        const UiDraw *active = &ui.draw[ui.active[index]];
        if (!overlaps(active, draw)) {
            ++index;
            continue;
        }
        if (kind == UI_RECTANGLE || kind == UI_FILL) {
            UI_CHECK(left <= active->left && top <= active->top);
            UI_CHECK(left + (int)width >= active->left + (int)active->width);
            UI_CHECK(top + (int)height >= active->top + (int)active->height);
            ui.active[index] = ui.active[--ui.active_count];
        } else {
            fprintf(stderr, "overlap: (%d,%d %ux%u) with (%d,%d %ux%u) '%s'\n",
                    left, top, width, height, active->left, active->top,
                    active->width, active->height, active->text);
            UI_CHECK(false);
        }
    }
    if (kind == UI_TEXT || kind == UI_SPRITE) {
        UI_CHECK(ui.active_count < UI_MAX_ACTIVE);
        ui.active[ui.active_count++] = ui.draws - 1;
    }
    return draw;
}

const UiDraw *ui_text_at(int left, int top)
{
    for (unsigned int index = 0; index < ui.active_count; ++index) {
        const UiDraw *draw = &ui.draw[ui.active[index]];
        if (draw->kind == UI_TEXT && draw->left == left && draw->top == top)
            return draw;
    }
    UI_CHECK(false);
    return NULL;
}

bool ui_has_text(const char *text)
{
    for (unsigned int index = 0; index < ui.active_count; ++index) {
        const UiDraw *draw = &ui.draw[ui.active[index]];
        if (draw->kind == UI_TEXT && strcmp(draw->text, text) == 0)
            return true;
    }
    return false;
}

bool ui_has_sprite(uint8_t kind, int left, int top)
{
    for (unsigned int index = 0; index < ui.active_count; ++index) {
        const UiDraw *draw = &ui.draw[ui.active[index]];
        if (draw->kind == UI_SPRITE && draw->sprite_kind == kind &&
            draw->left == left && draw->top == top)
            return true;
    }
    return false;
}

void gfx_Begin(void)
{
    UI_CHECK(!ui.graphics_active);
    call(UI_GFX_BEGIN, 0);
    ui.graphics_active = true;
    ui.needs_palette = true;
    ++ui.gfx_begins;
    ui.transparent = 0;
    ui.foreground = 1;
    ui.background = ui.text_transparent = 255;
    ui.scale_x = ui.scale_y = 1;
}

void gfx_End(void)
{
    UI_CHECK(ui.graphics_active);
    call(UI_GFX_END, 0);
    ui.graphics_active = false;
    ++ui.gfx_ends;
}

void gfx_SetPalette(const void *colors, unsigned int size, uint8_t offset)
{
    UI_CHECK(ui.graphics_active);
    call(UI_PALETTE, size);
    ++ui.palette_sets;
    ui.needs_palette = false;
    UI_CHECK(size % 2 == 0 && (unsigned int)offset * 2 + size <= sizeof(palette.colors));
    memcpy(palette.colors + offset, colors, size);
}

uint8_t gfx_SetColor(uint8_t color)
{ uint8_t previous = ui.color; ui.color = color; return previous; }
uint8_t gfx_SetTransparentColor(uint8_t color)
{ uint8_t previous = ui.transparent; ui.transparent = color; return previous; }
uint8_t gfx_SetTextFGColor(uint8_t color)
{ uint8_t previous = ui.foreground; ui.foreground = color; return previous; }
uint8_t gfx_SetTextBGColor(uint8_t color)
{ uint8_t previous = ui.background; ui.background = color; return previous; }
uint8_t gfx_SetTextTransparentColor(uint8_t color)
{ uint8_t previous = ui.text_transparent; ui.text_transparent = color; return previous; }
void gfx_SetTextScale(uint8_t width, uint8_t height)
{ ui.scale_x = width; ui.scale_y = height; }

unsigned int gfx_GetStringWidth(const char *text)
{ return (unsigned int)strlen(text) * 8 * ui.scale_x; }

void gfx_FillRectangle_NoClip(unsigned int left, uint8_t top,
                              unsigned int width, uint8_t height)
{
    record(UI_RECTANGLE, (int)left, top, width, height)->background = ui.color;
    for (unsigned int row = top; row < (unsigned int)top + height; ++row)
        memset(video.pixels + row * 320 + left, ui.color, width);
}

void gfx_FillScreen(uint8_t color)
{
    record(UI_FILL, 0, 0, 320, 240)->background = color;
    memset(video.pixels, color, sizeof(video.pixels));
}

void gfx_PrintStringXY(const char *text, int left, int top)
{
    UiDraw *draw = record(UI_TEXT, left, top, gfx_GetStringWidth(text),
                          8 * ui.scale_y);
    UI_CHECK(strlen(text) < sizeof(draw->text));
    strcpy(draw->text, text);
    if (left == 27 && top >= 43 && top < 218) {
        UI_CHECK(draw->width <= 128 && left + (int)draw->width <= 155);
        UI_CHECK((top - 43) % 18 == 0);
    }
    if (left >= 198 && top >= 88 && top <= 206)
        UI_CHECK(left + (int)draw->width <= 316);
    for (unsigned int character = 0; text[character]; ++character) {
        const uint8_t fallback[] = {14,17,1,2,4,0,4};
        const uint8_t *rows = fallback;
        UI_CHECK((unsigned char)text[character] >= 32 &&
                 (unsigned char)text[character] < 127);
        for (size_t glyph = 0; glyph < sizeof(ui_font) / sizeof(ui_font[0]); ++glyph)
            if (ui_font[glyph].character == text[character]) {
                rows = ui_font[glyph].rows;
                break;
            }
        for (unsigned int row = 0; row < 8u * ui.scale_y; ++row)
            for (unsigned int column = 0; column < 8u * ui.scale_x; ++column) {
                unsigned int glyph_row = row / ui.scale_y;
                unsigned int glyph_column = column / ui.scale_x;
                bool ink = glyph_row < 7 && glyph_column >= 1 && glyph_column <= 5 &&
                           (rows[glyph_row] & (1u << (5 - glyph_column)));
                uint8_t color = ink ? ui.foreground : ui.background;
                if (color != ui.text_transparent)
                    video.pixels[((unsigned int)top + row) * 320 + left +
                                 character * 8 * ui.scale_x + column] = color;
            }
    }
}

void gfx_ScaledTransparentSprite_NoClip(const gfx_sprite_t *sprite,
                                       unsigned int left, uint8_t top,
                                       uint8_t width, uint8_t height)
{
    const uint8_t *bytes = (const uint8_t *)sprite;
    UiDraw *draw;
    unsigned int kind;
    UI_CHECK(bytes[0] == 8 && bytes[1] == 8 && width && height);
    for (kind = 0; kind < 5; ++kind)
        if (memcmp(bytes, &browser_icons[kind], sizeof(BrowserIcon)) == 0)
            break;
    UI_CHECK(kind < 5);
    ui.sprite_sources[kind] = bytes;
    draw = record(UI_SPRITE, (int)left, top, bytes[0] * width, bytes[1] * height);
    draw->sprite_kind = (uint8_t)kind;
    for (unsigned int row = 0; row < bytes[1] * height; ++row)
        for (unsigned int column = 0; column < bytes[0] * width; ++column) {
            uint8_t color = bytes[2 + row / height * bytes[0] + column / width];
            if (color != ui.transparent)
                video.pixels[(top + row) * 320 + left + column] = color;
        }
}

uint8_t drive_file_count(void)
{
    UI_CHECK(ui.mapped && !ui.saving);
    call(UI_COUNT, ui.remaps ? ui.remap_count : ui.file_count);
    if (ui.remaps)
        ui.checked_count = true;
    return ui.remaps ? ui.remap_count : ui.file_count;
}

bool drive_init(const char *package_id)
{
    UI_CHECK(!strcmp(package_id, CE_PACKAGE_DISK));
    UI_CHECK(!ui.mapped && ui.save_complete && !ui.saving);
    UI_CHECK(ui.gc_before == NULL && ui.gc_after == NULL);
    UI_CHECK(ui.gc_clears == ui.saves);
    call(UI_REMAP, 0);
    ++ui.remaps;
    ui.mapped = ui.direct_open = ui.remap_ok;
    return ui.remap_ok;
}

void drive_invalidate(void)
{
    UI_CHECK(ui.mapped && !ui.saving && ui.saves == 0);
    call(UI_INVALIDATE, 0);
    ui.mapped = false;
    ++ui.invalidations;
}

void drive_disable(void)
{
    UI_CHECK(!ui.saving);
    call(UI_DISABLE, 0);
    ui.mapped = false;
    ++ui.disables;
}

void CEPackageCloseDirect(void)
{
    UI_CHECK(!ui.mapped && !ui.saving);
    call(UI_CLOSE, 0);
    ui.direct_open = false;
    ++ui.closes;
}

void drive_select_file(uint8_t index)
{
    UI_CHECK(ui.mapped && ui.direct_open && !ui.saving && index < ui.file_count);
    UI_CHECK(ui.allowed_indices[index]);
    UI_CHECK(ui.files[index].valid);
    if (ui.saves) {
        UI_CHECK(ui.remaps == 1 && ui.checked_identity && ui.checked_count);
        UI_CHECK(ui.remap_identity.size == ui.identity.size);
        UI_CHECK(ui.remap_identity.crc32 == ui.identity.crc32);
        UI_CHECK(ui.remap_count == ui.file_count);
    }
    call(UI_SELECT, index);
    ui.selected = index;
    ++ui.selects;
}
bool drive_identity(DriveIdentity *identity)
{
    UI_CHECK(ui.mapped && ui.direct_open && !ui.saving);
    call(UI_IDENTITY, 0);
    if (ui.remaps) {
        ui.checked_identity = ui.remap_identified;
        *identity = ui.remap_identity;
        return ui.remap_identified;
    }
    *identity = ui.identity;
    return ui.identified;
}
int drive_file_name(uint8_t index, char *buffer, uint8_t size)
{
    UI_CHECK(ui.mapped && ui.direct_open && !ui.saving);
    UI_CHECK(index < ui.file_count && size >= 17);
    UI_CHECK(ui.allowed_indices[index]);
    UI_CHECK(ui.names < UI_MAX_DRAWS);
    ui.name_indices[ui.names++] = index;
    call(UI_NAME, index);
    if (!ui.files[index].name_valid)
        return 0;
    memcpy(buffer, ui.files[index].info.name, 17);
    return 1;
}
bool drive_file_info(uint8_t index, DriveFileInfo *info)
{
    UI_CHECK(ui.mapped && ui.direct_open && !ui.saving && index < ui.file_count);
    UI_CHECK(ui.allowed_indices[index]);
    UI_CHECK(ui.infos < UI_MAX_DRAWS);
    ui.info_indices[ui.infos++] = index;
    call(UI_INFO, index);
    if (!ui.files[index].valid)
        return false;
    *info = ui.files[index].info;
    return true;
}
void browser_preferences_load(const DriveIdentity *identity, uint8_t count,
                              BrowserPreferences *preferences)
{
    UI_CHECK(identity->size == ui.identity.size && identity->crc32 == ui.identity.crc32);
    UI_CHECK(count == ui.file_count && ui.identified && ui.mapped);
    call(UI_LOAD_PREFERENCES, count);
    ++ui.preference_loads;
    *preferences = ui.preferences;
}
void browser_info_load(const DriveIdentity *identity, uint8_t count, uint8_t *locks)
{
    UI_CHECK(identity->size == ui.identity.size && identity->crc32 == ui.identity.crc32);
    UI_CHECK(count == ui.file_count && ui.identified && ui.mapped);
    call(UI_LOAD_INFO, count);
    ++ui.info_loads;
    for (unsigned int index = 0; index < count; ++index)
        locks[index] = ui.files[index].lock;
}
BrowserSaveResult browser_preferences_save(const DriveIdentity *identity,
                                          uint8_t count,
                                          const BrowserPreferences *preferences)
{
    UI_CHECK(!ui.mapped && !ui.direct_open && count == ui.file_count);
    UI_CHECK(ui.invalidations == 1 && ui.closes == 1 && ui.saves == 0);
    UI_CHECK(ui.gc_before == gfx_End && ui.gc_after != NULL);
    UI_CHECK(ui.gc_installs == 1 && ui.identified && !ui.saving);
    UI_CHECK(identity->size == ui.identity.size && identity->crc32 == ui.identity.crc32);
    call(UI_SAVE_START, count);
    ui.saved_identity = *identity;
    ui.saved_count = count;
    ui.saved_preferences = *preferences;
    ++ui.saves;
    ui.saving = true;
    for (unsigned int cycle = 0; cycle < ui.simulate_gc; ++cycle) {
        uint16_t expected_palette[16];
        memcpy(expected_palette, palette.colors, sizeof(expected_palette));
        ui.gc_before();
        UI_CHECK(!ui.graphics_active);
        memset(palette.colors, 0xcc, sizeof(expected_palette));
        memset(video.pixels, 0xcc, sizeof(video.pixels));
        ui.active_count = 0;
        ui.gc_after();
        UI_CHECK(ui.graphics_active && !ui.mapped && !ui.direct_open);
        UI_CHECK(ui.transparent == 15 && ui.scale_x == 1 && ui.scale_y == 1);
        UI_CHECK(memcmp(expected_palette, palette.colors, sizeof(expected_palette)) == 0);
        UI_CHECK(ui_has_text("Saving preferences"));
        ++ui.gc_cycles;
    }
    ui.saving = false;
    ui.save_complete = true;
    call(UI_SAVE_END, ui.save_result);
    return ui.save_result;
}
void ti_SetGCBehavior(void (*before)(void), void (*after)(void))
{
    UI_CHECK(!ui.saving);
    if (before || after) {
        UI_CHECK(before == gfx_End && after != NULL);
        UI_CHECK(!ui.mapped && !ui.direct_open && ui.invalidations == 1);
        UI_CHECK(ui.gc_before == NULL && ui.gc_after == NULL);
        call(UI_GC_INSTALL, 0);
        ++ui.gc_installs;
    } else {
        UI_CHECK(ui.save_complete);
        UI_CHECK(ui.gc_before != NULL && ui.gc_after != NULL);
        call(UI_GC_CLEAR, 0);
        ++ui.gc_clears;
    }
    ui.gc_before = before;
    ui.gc_after = after;
}