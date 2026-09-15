#include "browser.h"
#include "browser_model.h"
#include "browser_storage.h"
#include "browser_icons.h"
#include "CEPackage.h"
#include "io.h"
#include <fileioc.h>
#include <graphx.h>
#include <keypadc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define PANE_LEFT 192
#define LIST_TOP 38
#define ROW_HEIGHT 18
#define DISK_LIMIT 32
/* Real C64 palette indices (see io.c's c64_palette) so the browser and the
   in-game GRAPH menu/Key Settings modal always agree on what a colour means:
   0 black, 1 white, 2 red, 6 blue, 11 dark grey, 12 grey, 14 light blue,
   15 light grey. No custom UI colours - blue replaces the old red accent. */
#define COLOR_PAPER 15
#define COLOR_INK 0
#define COLOR_LINE 12
#define COLOR_ACCENT 6
#define COLOR_ERROR 2
#define COLOR_SELECTED 14
#define COLOR_MUTED 11

enum
{
    KEY_UP = 1, KEY_DOWN = 2, KEY_LEFT = 4, KEY_RIGHT = 8,
    KEY_BASIC = 16, KEY_MARK = 32, KEY_BACK = 64,
    KEY_OPEN = 128, KEY_QUIT = 256, KEY_SETTINGS = 512
};

typedef struct
{
    BrowserModel model;
    BrowserPreferences original;
    DriveIdentity identity;
    uint8_t locks[255];
    uint8_t rom_count;
    const char *package_id;
    bool identified;
} BrowserScreen;

static void fill(uint8_t color, unsigned int left, uint8_t top,
                 unsigned int width, uint8_t height)
{
    gfx_SetColor(color);
    gfx_FillRectangle_NoClip(left, top, width, height);
}

static void text(const char *value, int left, uint8_t top,
                 uint8_t foreground, uint8_t background)
{
    gfx_SetTextFGColor(foreground);
    gfx_SetTextBGColor(background);
    gfx_SetTextTransparentColor(255);
    gfx_PrintStringXY(value, left, top);
}

static void right_text(const char *value, int right, uint8_t top,
                       uint8_t foreground, uint8_t background)
{
    text(value, right - (int)gfx_GetStringWidth(value), top, foreground, background);
}

static char *number(char *buffer, uint32_t value)
{
    char digits[10];
    uint8_t length = 0;
    do
    {
        digits[length++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (length)
        *buffer++ = digits[--length];
    *buffer = '\0';
    return buffer;
}

static void icon(uint8_t kind, unsigned int left, uint8_t top, uint8_t scale)
{
    gfx_ScaledTransparentSprite_NoClip(
        (const gfx_sprite_t *)&browser_icons[kind], left, top, scale, scale);
}

static void palette_init(void)
{
    gfx_SetPalette(c64_palette, sizeof(c64_palette), 0);
    gfx_SetTransparentColor(15);
    gfx_SetTextScale(1, 1);
}

static uint16_t menu_keys(void)
{
    uint16_t keys = 0;
    if (kb_IsDown(kb_KeyUp)) keys |= KEY_UP;
    if (kb_IsDown(kb_KeyDown)) keys |= KEY_DOWN;
    if (kb_IsDown(kb_KeyLeft)) keys |= KEY_LEFT;
    if (kb_IsDown(kb_KeyRight)) keys |= KEY_RIGHT;
    if (kb_IsDown(kb_KeyYequ)) keys |= KEY_BASIC;
    if (kb_IsDown(kb_KeyAlpha)) keys |= KEY_MARK;
    if (kb_IsDown(kb_KeyTrace)) keys |= KEY_BACK;
    if (kb_IsDown(kb_KeyEnter)) keys |= KEY_OPEN;
    if (kb_IsDown(kb_KeyClear)) keys |= KEY_QUIT;
    if (kb_IsDown(kb_KeyGraph)) keys |= KEY_SETTINGS;
    return keys;
}

static void release_menu_keys(void)
{
    do { kb_Scan(); } while (menu_keys());
}

static bool notice(const char *message, const char *action)
{
    fill(COLOR_PAPER, 0, 0, 320, 240);
    fill(COLOR_ACCENT, 0, 0, 320, 20);
    text("emudore CE", 6, 6, 1, COLOR_ACCENT);
    text(message, (320 - (int)gfx_GetStringWidth(message)) / 2, 100,
         COLOR_INK, COLOR_PAPER);
    text(action, (320 - (int)gfx_GetStringWidth(action)) / 2, 124,
         COLOR_INK, COLOR_PAPER);
    release_menu_keys();
    for (;;)
    {
        kb_Scan();
        if (kb_IsDown(kb_KeyClear))
            return false;
        if (kb_IsDown(kb_KeyEnter))
            return true;
    }
}

static void draw_row(const BrowserScreen *screen, uint16_t row)
{
    char name[17];
    uint16_t entry;
    uint8_t top = LIST_TOP + (row - screen->model.top) * ROW_HEIGHT;
    uint8_t background = row == screen->model.selected ? COLOR_SELECTED : 1;
    fill(background, 0, top, 187, ROW_HEIGHT);
    if (row >= screen->model.count)
        return;
    entry = screen->model.entries[row];
    if (!entry)
    {
        icon(0, 5, top + 1, 2);
        text("BASIC", 27, top + 5, COLOR_INK, background);
        return;
    }
    if (!drive_file_name((uint8_t)(entry - 1), name, sizeof(name)))
        strcpy(name, "Invalid entry");
    icon(browser_flag(screen->model.preferences.launchers, (uint8_t)(entry - 1))
         ? 1 : 2, 5, top + 1, 2);
    text(name, 27, top + 5, COLOR_INK, background);
}

static void draw_list(const BrowserScreen *screen)
{
    uint16_t row;
    unsigned int thumb_height;
    unsigned int thumb_top;
    for (row = screen->model.top; row < screen->model.top + BROWSER_ROWS; ++row)
        draw_row(screen, row);
    fill(COLOR_PAPER, 187, LIST_TOP, 5, ROW_HEIGHT * BROWSER_ROWS);
    if (screen->model.count <= BROWSER_ROWS)
        return;
    thumb_height = ROW_HEIGHT * BROWSER_ROWS * BROWSER_ROWS / screen->model.count;
    if (thumb_height < 8)
        thumb_height = 8;
    thumb_top = (ROW_HEIGHT * BROWSER_ROWS - thumb_height) * screen->model.top
              / (screen->model.count - BROWSER_ROWS);
    fill(COLOR_MUTED, 188, LIST_TOP + thumb_top, 3, thumb_height);
}

static void name_lines(const char *name)
{
    char line[17];
    uint8_t used = 0;
    uint8_t length = (uint8_t)strlen(name);
    memcpy(line, name, length + 1);
    while (length && gfx_GetStringWidth(line) > 116)
        line[--length] = '\0';
    text(line, 198, 88, COLOR_INK, COLOR_PAPER);
    used = length;
    if (name[used])
        text(name + used, 198, 100, COLOR_INK, COLOR_PAPER);
}

static void field(const char *label, const char *value, uint8_t top)
{
    text(label, 198, top, COLOR_MUTED, COLOR_PAPER);
    right_text(value, 316, top, COLOR_INK, COLOR_PAPER);
}

static void footer_cell(unsigned int left, const char *key, const char *action);

static void draw_information(const BrowserScreen *screen)
{
    uint16_t entry = browser_model_entry(&screen->model);
    DriveFileInfo info;
    char value[20];
    uint8_t index;
    uint8_t digit;
    static const char hex[] = "0123456789ABCDEF";
    /* Redrawn every navigation (unlike the static "File Info" title), so
       clear the old width first - "20/20" is shorter than "255/255". */
    fill(COLOR_PAPER, 270, 24, 50, 10);
    {
        char position[12];
        char *end = number(position, screen->model.selected + 1);
        *end++ = '/';
        number(end, screen->model.count);
        right_text(position, 316, 25, COLOR_MUTED, COLOR_PAPER);
    }
    fill(COLOR_PAPER, PANE_LEFT + 1, LIST_TOP, 127, 182);
    if (!entry)
    {
        icon(0, 240, 47, 4);
        name_lines("BASIC");
        field("Type", "C64 BASIC", 116);
        text("Role", 198, 130, COLOR_MUTED, COLOR_PAPER);
        text("Interpreter", 198, 142, COLOR_INK, COLOR_PAPER);
        text("Built-in", 198, 158, COLOR_INK, COLOR_PAPER);
        field("Load", "-", 174);
        field("Locked", "-", 190);
        field("Disk", screen->package_id, 190);
        field("Size", "Built-in", 206);
        return;
    }
    index = (uint8_t)(entry - 1);
    if (!drive_file_info(index, &info))
    {
        text("Invalid PRG", 198, 100, COLOR_ERROR, COLOR_PAPER);
        return;
    }
    icon(browser_flag(screen->model.preferences.launchers, index) ? 1 : 2,
         240, 47, 4);
    name_lines(info.name);
    field("Type", "C64 PRG", 116);
    text("Role", 198, 130, COLOR_MUTED, COLOR_PAPER);
    text(browser_flag(screen->model.preferences.launchers, index)
         ? "Launcher" : "Unclassified", 198, 142, COLOR_INK, COLOR_PAPER);
    strcpy(number(value, info.size), " bytes");
    text(value, 198, 158, COLOR_INK, COLOR_PAPER);
    value[0] = '$';
    for (digit = 0; digit < 4; ++digit)
        value[digit + 1] = hex[(info.load_address >> (12 - digit * 4)) & 15];
    value[5] = '\0';
    field("Load", value, 174);
    field("Disk", screen->package_id, 190);
    field("Size", "AppVar", 206);
}

/* One 64px-wide cell showing the physical key on top and its action below -
   same idea as the in-game GRAPH-menu HUD, so the footer visually lines up
   with the key that triggers each action instead of being arbitrary text. */
static void footer_cell(unsigned int left, const char *key, const char *action)
{
    fill(COLOR_ACCENT, left, 220, 64, 20);
    text(key, left + (64 - (int)gfx_GetStringWidth(key)) / 2, 222, 1, COLOR_ACCENT);
    text(action, left + (64 - (int)gfx_GetStringWidth(action)) / 2, 231, 1, COLOR_ACCENT);
}

static void draw_footer(const BrowserScreen *screen)
{
    uint16_t entry = browser_model_entry(&screen->model);
    footer_cell(0, "Y=", "-");
    footer_cell(64, "ALPHA", entry &&
                browser_flag(screen->model.preferences.launchers, (uint8_t)(entry - 1))
                ? "Unmark" : entry ? "Mark" : "-");
    footer_cell(128, "TRACE", "Back");
    footer_cell(192, "GRAPH", "Settings");
    footer_cell(256, "ENTER", "Open");
}

static void draw_screen(const BrowserScreen *screen)
{
    char status[20];
    char *end;
    gfx_FillScreen(1);
    fill(COLOR_ACCENT, 0, 0, 320, 20);
    text("emudore CE", 6, 6, 1, COLOR_ACCENT);
    end = number(status, screen->model.file_count);
    strcpy(end, " PRGs   ROM ");
    end += strlen(end);
    number(end, screen->rom_count);
    right_text(status, 314, 6, 1, COLOR_ACCENT);
    fill(COLOR_PAPER, PANE_LEFT, 20, 128, 200);
    text("Disk Files", 202, 25, COLOR_INK, COLOR_PAPER);
    fill(COLOR_LINE, PANE_LEFT, 20, 1, 200);
    text(screen->package_id, 8, 25, COLOR_INK, 1);
    fill(COLOR_ACCENT, 0, 36, 96, 2);
    draw_list(screen);
    draw_information(screen);
    draw_footer(screen);
}

static void draw_disk_root(const CEPackageDisk *disks, uint8_t count,
                           uint8_t selected, uint8_t top)
{
    uint8_t index;
    char value[20];

    gfx_FillScreen(1);
    fill(COLOR_ACCENT, 0, 0, 320, 20);
    text("emudore CE", 6, 6, 1, COLOR_ACCENT);
    text("File Browser", 232, 6, 1, COLOR_ACCENT);
    fill(COLOR_PAPER, PANE_LEFT, 20, 128, 200);
    text("Disk Info", 202, 25, COLOR_INK, COLOR_PAPER);
    fill(COLOR_LINE, PANE_LEFT, 20, 1, 200);
    fill(1, 0, 20, 192, 18);
    text("All Disks", 8, 25, COLOR_INK, 1);
    fill(COLOR_ACCENT, 0, 36, 96, 2);
    for (index = 0; index < BROWSER_ROWS && top + index < count; ++index)
    {
        uint8_t row_top = LIST_TOP + index * ROW_HEIGHT;
        uint8_t background = top + index == selected ? COLOR_SELECTED : 1;

        fill(background, 0, row_top, 187, ROW_HEIGHT);
        icon(2, 5, row_top + 1, 2);
        text(disks[top + index].id, 27, row_top + 5, COLOR_INK, background);
    }
    fill(COLOR_PAPER, 192, LIST_TOP, 128, 182);
    icon(2, 240, 47, 4);
    text(disks[selected].id, 198, 88, COLOR_INK, COLOR_PAPER);
    field("Type", "C64 disk", 116);
    field("Files", "PRG set", 132);
    number(value, disks[selected].size);
    strcat(value, " bytes");
    text(value, 198, 158, COLOR_INK, COLOR_PAPER);
    field("Open", "ENTER", 190);
    fill(COLOR_ACCENT, 0, 220, 320, 20);
    footer_cell(0, "Y=", "BASIC");
    footer_cell(64, "UP/DOWN", "Select");
    footer_cell(256, "ENTER", "Open");
}

BrowserResult browser_select_disk(CEPackageDisk *disk, const char *selected_id)
{
    CEPackageDisk disks[DISK_LIMIT];
    uint8_t count;
    uint8_t selected = 0;
    uint8_t top = 0;
    bool held = false;
    BrowserResult result = {BROWSER_QUIT, 0};

    if (!disk)
        return result;
    count = CEPackageFindDisks(disks, DISK_LIMIT);
    if (!count)
        return result;
    if (selected_id)
    {
        for (selected = 0; selected < count; ++selected)
            if (!strcmp(disks[selected].id, selected_id))
                break;
        if (selected == count)
            selected = 0;
    }
    palette_init();
    draw_disk_root(disks, count, selected, top);
    release_menu_keys();
    for (;;)
    {
        uint16_t keys;

        kb_Scan();
        keys = menu_keys();
        if (!keys)
        {
            held = false;
            continue;
        }
        if (held)
            continue;
        held = true;
        if (keys & KEY_QUIT)
            return result;
        if (keys & KEY_BASIC)
        {
            result.action = BROWSER_BASIC;
            return result;
        }
        if (keys & KEY_OPEN)
        {
            *disk = disks[selected];
            result.action = BROWSER_FILE;
            return result;
        }
        if ((keys & KEY_UP) && selected)
            --selected;
        else if ((keys & KEY_DOWN) && selected + 1 < count)
            ++selected;
        else
            continue;
        if (selected < top)
            top = selected;
        else if (selected >= top + BROWSER_ROWS)
            top = selected - BROWSER_ROWS + 1;
        draw_disk_root(disks, count, selected, top);
    }
}

static void after_gc(void)
{
    gfx_Begin();
    palette_init();
    gfx_FillScreen(COLOR_PAPER);
    text("Saving preferences", 72, 110, COLOR_INK, COLOR_PAPER);
}

static BrowserResult finish(BrowserScreen *screen, BrowserResult result)
{
    bool changed = memcmp(&screen->model.preferences, &screen->original,
                          sizeof(screen->original)) != 0;
    if (changed)
    {
        BrowserSaveResult saved = BROWSER_SAVE_FAILED;
        if (screen->identified)
        {
            drive_invalidate();
            CEPackageCloseDirect();
            ti_SetGCBehavior(gfx_End, after_gc);
            saved = browser_preferences_save(&screen->identity,
                                             screen->model.file_count,
                                             &screen->model.preferences);
            ti_SetGCBehavior(NULL, NULL);
            if (result.action == BROWSER_FILE)
            {
                DriveIdentity remapped;
                if (!drive_init(screen->package_id) || !drive_identity(&remapped) ||
                    remapped.size != screen->identity.size ||
                    remapped.crc32 != screen->identity.crc32 ||
                    drive_file_count() != screen->model.file_count)
                {
                    result.action = notice("Disk unavailable", "BASIC")
                                  ? BROWSER_BASIC : BROWSER_QUIT;
                }
            }
        }
        if (saved != BROWSER_SAVE_OK &&
            !notice(saved == BROWSER_SAVE_RAM_ONLY ? "Preferences saved in RAM"
                                                  : "Preferences not saved",
                    result.action == BROWSER_QUIT ? "Quit" : "Continue"))
            result.action = BROWSER_QUIT;
    }
    if (result.action == BROWSER_FILE)
        drive_select_file(result.file_index);
    else
    {
        drive_disable();
        CEPackageCloseDirect();
    }
    release_menu_keys();
    gfx_SetTextScale(1, 1);
    gfx_SetTextFGColor(1);
    gfx_SetTextBGColor(0);
    gfx_SetTextTransparentColor(255);
    gfx_SetTransparentColor(0);
    gfx_FillScreen(0);
    free(screen);
    return result;
}

BrowserResult browser_show(uint8_t rom_count, const char *package_id,
                           uint8_t selected_file)
{
    BrowserScreen *screen;
    BrowserResult result = {BROWSER_BASIC, 0};
    uint8_t file_count = drive_file_count();
    bool held = false;
    if (!file_count)
        return result;
    palette_init();
    screen = calloc(1, sizeof(*screen));
    if (!screen)
    {
        result.action = notice("Browser unavailable", "BASIC")
                      ? BROWSER_BASIC : BROWSER_QUIT;
        drive_disable();
        CEPackageCloseDirect();
        release_menu_keys();
        gfx_SetTextFGColor(1);
        gfx_SetTextBGColor(0);
        gfx_SetTransparentColor(0);
        gfx_FillScreen(0);
        return result;
    }
    screen->rom_count = rom_count;
    screen->package_id = package_id;
    screen->identified = drive_identity(&screen->identity);
    if (screen->identified)
    {
        browser_preferences_load(&screen->identity, file_count, &screen->original);
    }
    browser_model_init(&screen->model, file_count, &screen->original);
    if (selected_file < file_count)
        browser_model_move(&screen->model, selected_file);
    draw_screen(screen);
    release_menu_keys();
    for (;;)
    {
        uint16_t keys;
        uint16_t previous_top = screen->model.top;
        uint16_t previous_row = screen->model.selected;
        bool changed = false;
        bool all_rows = false;
        kb_Scan();
        keys = menu_keys();
        if (!keys)
        {
            held = false;
            continue;
        }
        if (held)
            continue;
        held = true;
        if (keys & KEY_QUIT)
        {
            result.action = BROWSER_QUIT;
            return finish(screen, result);
        }
        if (keys & KEY_SETTINGS)
        {
            io_show_key_settings();
            draw_screen(screen);
            continue;
        }
        if (keys & KEY_BACK)
        {
            result.action = BROWSER_BACK;
            return finish(screen, result);
        }
        if (keys & KEY_OPEN)
        {
            uint16_t entry = browser_model_entry(&screen->model);
            DriveFileInfo info;
            if (entry && !drive_file_info((uint8_t)(entry - 1), &info))
                continue;
            result.action = BROWSER_FILE;
            result.file_index = (uint8_t)(entry - 1);
            return finish(screen, result);
        }
        if (keys & KEY_UP) changed = browser_model_move(&screen->model, -1);
        else if (keys & KEY_DOWN) changed = browser_model_move(&screen->model, 1);
        else if (keys & KEY_LEFT) changed = browser_model_move(&screen->model, -BROWSER_ROWS);
        else if (keys & KEY_RIGHT) changed = browser_model_move(&screen->model, BROWSER_ROWS);
        else if (keys & KEY_MARK)
            changed = all_rows = browser_model_launcher(&screen->model);
        if (changed)
        {
            if (all_rows || previous_top != screen->model.top)
                draw_list(screen);
            else
            {
                draw_row(screen, previous_row);
                draw_row(screen, screen->model.selected);
            }
            draw_information(screen);
            draw_footer(screen);
        }
    }
}