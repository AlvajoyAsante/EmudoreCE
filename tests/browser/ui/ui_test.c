#include "mock.h"
#include "snapshot.h"
#include <graphx.h>
#include <keypadc.h>
#include <stdio.h>
#include <string.h>

#define browser_show(rom_count) browser_show((rom_count), CE_PACKAGE_DISK, 0)

static void acrojet_fixture(void)
{
    static const char *const names[] = {
        "ACROJET", "PART1", "PART2", "PART3", "TITLE.OBJ", "PANEL.OBJ",
        "MUSIC.OBJ", "TRAINER", "SCENERY.OBJ", "TERRAIN.OBJ", "SPRITES.OBJ",
        "FONT.OBJ", "SCORES", "OPTIONS", "HELP", "REPLAY", "LANDING.OBJ",
        "FLIGHT.OBJ", "FINAL.OBJ"
    };
    ui_reset(19);
    for (unsigned int index = 0; index < 19; ++index)
        strcpy(ui.files[index].info.name, names[index]);
}

static void no_files(void)
{
    ui_reset(0);
    memset(gfx_vram, 0, UI_PIXELS);
    gfx_palette[0] = gfx_RGBTo1555(0, 0, 0);
    gfx_palette[1] = gfx_RGBTo1555(255, 255, 255);
    kb_Data[1] = 0x40;
    UI_CHECK(browser_show(3).action == BROWSER_BASIC);
    UI_CHECK(ui.scans == 0 && ui.draws == 0 && ui.allocations == 0);
    UI_CHECK(ui.saves == 0 && ui.selects == 0);
    UI_CHECK(ui.disables == 0 && ui.closes == 0);
    UI_CHECK(kb_IsDown(kb_KeyMode) == 0x40);
    ui_verify_return();
}

static void basic_without_edits(void)
{
    ui_reset(19);
    ui_queue(UI_NONE);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_BASIC);
    UI_CHECK(ui.saves == 0 && ui.selects == 0);
    UI_CHECK(ui.disables == 1 && ui.closes == 1);
    ui_verify_return();
}

static void clear_cleanup(void)
{
    ui_reset(19);
    ui_queue(UI_NONE);
    ui_tap(UI_CLEAR);
    UI_CHECK(browser_show(3).action == BROWSER_QUIT);
    UI_CHECK(ui.saves == 0 && ui.selects == 0);
    UI_CHECK(ui.disables == 1 && ui.closes == 1);
    ui_verify_return();
}

static void check_last_19(void)
{
    UI_CHECK(strcmp(ui_text_at(6, 226)->text, "20/20") == 0);
    UI_CHECK(strcmp(ui_text_at(27, 205)->text, "FINAL.OBJ") == 0);
    UI_CHECK(ui_text_at(27, 205)->background == 3);
}

static void check_last_255(void)
{
    UI_CHECK(strcmp(ui_text_at(6, 226)->text, "256/256") == 0);
    UI_CHECK(strcmp(ui_text_at(27, 205)->text, "PRG254") == 0);
    UI_CHECK(ui_text_at(27, 205)->background == 3);
}

static void launch_last(uint8_t count, unsigned int key, unsigned int steps,
                        void (*inspect)(void))
{
    BrowserResult result;
    if (count == 19)
        acrojet_fixture();
    else
        ui_reset(count);
    ui_queue(UI_NONE);
    for (unsigned int step = 0; step < steps; ++step)
        ui_tap(key);
    ui_probe(inspect);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == count - 1);
    UI_CHECK(ui.selects == 1 && ui.selected == count - 1 && ui.saves == 0);
    ui_verify_return();
}

static void last_19_down(void) { launch_last(19, UI_DOWN, 19, check_last_19); }
static void last_19_page(void) { launch_last(19, UI_RIGHT, 2, check_last_19); }
static void last_255_page(void) { launch_last(255, UI_RIGHT, 26, check_last_255); }

static void check_long_information(void)
{
    UI_CHECK(strcmp(ui_text_at(27, 61)->text, "WWWWWWWWWWWWWWWW") == 0);
    UI_CHECK(ui_text_at(27, 61)->width == 128);
    UI_CHECK(strcmp(ui_text_at(198, 88)->text, "WWWWWWWWWWWWWW") == 0);
    UI_CHECK(strcmp(ui_text_at(198, 100)->text, "WW") == 0);
    UI_CHECK(ui_has_text("49154 bytes"));
    UI_CHECK(ui_has_text("$A0F2"));
    UI_CHECK(strcmp(ui_text_at(292, 190)->text, "Yes") == 0);
    UI_CHECK(ui_has_sprite(3, 177, 61));
    ui_verify_guards();
}

static void long_information(void)
{
    ui_reset(19);
    strcpy(ui.files[0].info.name, "WWWWWWWWWWWWWWWW");
    ui.files[0].info.size = 49154;
    ui.files[0].info.load_address = 0xa0f2;
    ui.files[0].lock = BROWSER_LOCK_YES;
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_probe(check_long_information);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_FILE);
    UI_CHECK(ui.saves == 0 && ui.selected == 0);
    ui_verify_return();
}

static void idle_and_held_down(void)
{
    BrowserResult result;
    ui_reset(19);
    ui_queue(UI_NONE);
    for (unsigned int poll = 0; poll < 12; ++poll)
        ui_quiet(UI_NONE);
    ui_queue(UI_DOWN);
    for (unsigned int poll = 0; poll < 12; ++poll)
        ui_quiet(UI_DOWN);
    ui_quiet(UI_NONE);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 0);
    UI_CHECK(ui.selects == 1 && ui.saves == 0);
    ui_verify_return();
}

static void flag(uint8_t *flags, uint8_t index)
{
    flags[index / 8] |= (uint8_t)(1u << (index % 8));
}

static void check_save_order(void)
{
    UI_CHECK(ui.saves == 1 && ui.invalidations == 1);
    UI_CHECK(ui.gc_installs == 1 && ui.gc_clears == 1);
    UI_CHECK(ui_call_position(UI_INVALIDATE) < ui_call_position(UI_CLOSE));
    UI_CHECK(ui_call_position(UI_CLOSE) < ui_call_position(UI_GC_INSTALL));
    UI_CHECK(ui_call_position(UI_GC_INSTALL) < ui_call_position(UI_SAVE_START));
    UI_CHECK(ui_call_position(UI_SAVE_START) < ui_call_position(UI_SAVE_END));
    UI_CHECK(ui_call_position(UI_SAVE_END) < ui_call_position(UI_GC_CLEAR));
    UI_CHECK(ui.saved_count == ui.file_count);
    UI_CHECK(ui.saved_identity.size == ui.identity.size);
    UI_CHECK(ui.saved_identity.crc32 == ui.identity.crc32);
    if (ui.remaps) {
        UI_CHECK(ui_call_position(UI_GC_CLEAR) < ui_call_position(UI_REMAP));
        if (ui.selects)
            UI_CHECK(ui_call_position(UI_REMAP) < ui_call_position(UI_SELECT));
    }
}

static void save_with_gc(void)
{
    BrowserResult result;
    ui_reset(19);
    ui.simulate_gc = 2;
    ui_queue(UI_NONE);
    ui_tap(UI_RIGHT);
    ui_tap(UI_MARK);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 9);
    UI_CHECK(ui.selects == 1 && ui.selected == 9 && ui.remaps == 1);
    UI_CHECK(browser_flag(ui.saved_preferences.launchers, 9));
    UI_CHECK(ui.gc_cycles == 2 && ui.gfx_begins == 2 && ui.gfx_ends == 2);
    UI_CHECK(ui.palette_sets == 3);
    check_save_order();
    ui_verify_return();
}

static void check_save_failed_notice(void)
{
    UI_CHECK(ui_has_text("Preferences not saved"));
    UI_CHECK(ui_has_text("Continue"));
    UI_CHECK(ui.save_complete && ui.remaps == 1 && ui.selects == 0);
}

static void check_ram_notice(void)
{
    UI_CHECK(ui_has_text("Preferences saved in RAM"));
    UI_CHECK(ui_has_text("Continue"));
    UI_CHECK(ui.save_complete && ui.remaps == 1 && ui.selects == 0);
}

static void failed_save(BrowserSaveResult saved, unsigned int answer)
{
    BrowserResult result;
    ui_reset(19);
    ui.save_result = saved;
    ui.simulate_gc = 1;
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_tap(UI_MARK);
    ui_tap(UI_ENTER);
    ui_probe(saved == BROWSER_SAVE_RAM_ONLY ? check_ram_notice : check_save_failed_notice);
    ui_quiet(UI_NONE);
    ui_quiet(UI_NONE);
    ui_tap(answer);
    result = browser_show(3);
    UI_CHECK(result.action == (answer == UI_ENTER ? BROWSER_FILE : BROWSER_QUIT));
    UI_CHECK(ui.remaps == 1 && ui.gc_cycles == 1);
    UI_CHECK(ui.selects == (answer == UI_ENTER ? 1u : 0u));
    UI_CHECK(browser_flag(ui.saved_preferences.launchers, 0));
    check_save_order();
    ui_verify_return();
}

static void save_failed_continue(void) { failed_save(BROWSER_SAVE_FAILED, UI_ENTER); }
static void save_failed_quit(void) { failed_save(BROWSER_SAVE_FAILED, UI_CLEAR); }
static void save_ram_continue(void) { failed_save(BROWSER_SAVE_RAM_ONLY, UI_ENTER); }
static void save_ram_quit(void) { failed_save(BROWSER_SAVE_RAM_ONLY, UI_CLEAR); }

static void filtered_original_index(void)
{
    const uint8_t indices[] = {2, 7, 18};
    BrowserResult result;
    ui_reset(19);
    ui.preferences.view = BROWSER_LAUNCHERS;
    for (size_t index = 0; index < sizeof(indices); ++index)
        flag(ui.preferences.launchers, indices[index]);
    ui_allow_indices(indices, sizeof(indices));
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_tap(UI_DOWN);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 7);
    UI_CHECK(ui.selects == 1 && ui.selected == 7 && ui.saves == 0);
    UI_CHECK(ui.name_indices[0] == 2 && ui.name_indices[1] == 7 && ui.name_indices[2] == 18);
    ui_verify_return();
}

static void allow_launchers(void)
{
    const uint8_t indices[] = {2, 7, 18};
    ui_allow_indices(indices, sizeof(indices));
}

static void allow_all(void)
{
    for (unsigned int index = 0; index < ui.file_count; ++index)
        ui.allowed_indices[index] = true;
}

static void launcher_fixture(void)
{
    ui_reset(19);
    flag(ui.preferences.launchers, 2);
    flag(ui.preferences.launchers, 7);
    flag(ui.preferences.launchers, 18);
}

static void check_launcher_seven(void)
{
    UI_CHECK(strcmp(ui_text_at(6, 226)->text, "3/4") == 0);
    UI_CHECK(strcmp(ui_text_at(27, 61)->text, "PRG002") == 0);
    UI_CHECK(strcmp(ui_text_at(27, 79)->text, "PRG007") == 0);
    UI_CHECK(ui_text_at(27, 79)->background == 3);
    UI_CHECK(strcmp(ui_text_at(27, 97)->text, "PRG018") == 0);
    UI_CHECK(strcmp(ui_text_at(198, 88)->text, "PRG007") == 0);
    UI_CHECK(ui_text_at(8, 25)->background == 1);
}

static void check_all_seven(void)
{
    UI_CHECK(strcmp(ui_text_at(6, 226)->text, "9/20") == 0);
    UI_CHECK(strcmp(ui_text_at(27, 187)->text, "PRG007") == 0);
    UI_CHECK(ui_text_at(27, 187)->background == 3);
    UI_CHECK(strcmp(ui_text_at(198, 88)->text, "PRG007") == 0);
    UI_CHECK(ui_text_at(104, 25)->background == 1);
}

static void view_preserves_index(void)
{
    BrowserResult result;
    launcher_fixture();
    ui_queue(UI_NONE);
    for (unsigned int step = 0; step < 8; ++step)
        ui_tap(UI_DOWN);
    ui_probe(allow_launchers);
    ui_tap(UI_VIEW);
    ui_probe(check_launcher_seven);
    ui_probe(allow_all);
    ui_tap(UI_VIEW);
    ui_probe(check_all_seven);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 7);
    UI_CHECK(ui.saves == 0 && ui.invalidations == 0);
    ui_verify_return();
}

static void check_removed_middle(void)
{
    UI_CHECK(strcmp(ui_text_at(6, 226)->text, "3/3") == 0);
    UI_CHECK(strcmp(ui_text_at(27, 79)->text, "PRG018") == 0);
    UI_CHECK(ui_text_at(27, 79)->background == 3);
    UI_CHECK(!ui_has_text("PRG007"));
}

static void hide_launcher_keeps_assignment(void)
{
    BrowserResult result;
    launcher_fixture();
    ui.preferences.view = BROWSER_LAUNCHERS;
    flag(ui.preferences.hidden, 10);
    allow_launchers();
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_tap(UI_DOWN);
    ui_tap(UI_HIDE);
    ui_probe(check_removed_middle);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 18);
    UI_CHECK(browser_flag(ui.saved_preferences.hidden, 7));
    UI_CHECK(browser_flag(ui.saved_preferences.hidden, 10));
    UI_CHECK(memcmp(ui.saved_preferences.launchers, ui.preferences.launchers,
                    BROWSER_FLAG_BYTES) == 0);
    check_save_order();
    ui_verify_return();
}

static void unmark_middle_nearest(void)
{
    BrowserResult result;
    launcher_fixture();
    ui.preferences.view = BROWSER_LAUNCHERS;
    allow_launchers();
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_tap(UI_DOWN);
    ui_tap(UI_MARK);
    ui_probe(check_removed_middle);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 18);
    UI_CHECK(!browser_flag(ui.saved_preferences.launchers, 7));
    UI_CHECK(!browser_flag(ui.saved_preferences.hidden, 7));
    check_save_order();
    ui_verify_return();
}

static void check_removed_last(void)
{
    UI_CHECK(strcmp(ui_text_at(6, 226)->text, "3/3") == 0);
    UI_CHECK(strcmp(ui_text_at(27, 79)->text, "PRG007") == 0);
    UI_CHECK(ui_text_at(27, 79)->background == 3);
    UI_CHECK(!ui_has_text("PRG018"));
}

static void unmark_last_nearest(void)
{
    BrowserResult result;
    launcher_fixture();
    ui.preferences.view = BROWSER_LAUNCHERS;
    allow_launchers();
    ui_queue(UI_NONE);
    ui_tap(UI_RIGHT);
    ui_tap(UI_MARK);
    ui_probe(check_removed_last);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 7);
    UI_CHECK(!browser_flag(ui.saved_preferences.launchers, 18));
    check_save_order();
    ui_verify_return();
}

static void check_only_basic(void)
{
    UI_CHECK(strcmp(ui_text_at(6, 226)->text, "1/1") == 0);
    UI_CHECK(strcmp(ui_text_at(27, 43)->text, "BASIC") == 0);
    UI_CHECK(ui_text_at(27, 43)->background == 3);
    UI_CHECK(!ui_has_text("Mark") && !ui_has_text("Hide"));
}

static void unmark_only_launcher(void)
{
    const uint8_t indices[] = {7};
    ui_reset(19);
    ui.preferences.view = BROWSER_LAUNCHERS;
    flag(ui.preferences.launchers, 7);
    ui_allow_indices(indices, sizeof(indices));
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_tap(UI_MARK);
    ui_probe(check_only_basic);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_BASIC);
    UI_CHECK(ui.selects == 0 && ui.remaps == 0 && ui.disables == 1);
    UI_CHECK(!browser_flag(ui.saved_preferences.launchers, 7));
    check_save_order();
    ui_verify_return();
}

static void check_hidden_all(void)
{
    check_all_seven();
    UI_CHECK(ui_has_sprite(4, 165, 187));
    UI_CHECK(ui_has_sprite(4, 165, 151));
    UI_CHECK(strcmp(ui_text_at(292, 206)->text, "Yes") == 0);
    UI_CHECK(ui_has_text("Unhide"));
}

static void check_unhidden_all(void)
{
    check_all_seven();
    UI_CHECK(!ui_has_sprite(4, 165, 187));
    UI_CHECK(ui_has_sprite(4, 165, 151));
    UI_CHECK(strcmp(ui_text_at(300, 206)->text, "No") == 0);
    UI_CHECK(ui_has_text("Hide"));
}

static void all_shows_and_unhides(void)
{
    BrowserResult result;
    launcher_fixture();
    flag(ui.preferences.hidden, 7);
    flag(ui.preferences.hidden, 5);
    ui_queue(UI_NONE);
    for (unsigned int step = 0; step < 8; ++step)
        ui_tap(UI_DOWN);
    ui_probe(check_hidden_all);
    ui_tap(UI_HIDE);
    ui_probe(check_unhidden_all);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 7);
    UI_CHECK(!browser_flag(ui.saved_preferences.hidden, 7));
    UI_CHECK(browser_flag(ui.saved_preferences.hidden, 5));
    UI_CHECK(browser_flag(ui.saved_preferences.launchers, 7));
    check_save_order();
    ui_verify_return();
}

static void empty_launchers_basic_immutable(void)
{
    ui_reset(19);
    ui.preferences.view = BROWSER_LAUNCHERS;
    for (unsigned int index = 0; index < ui.file_count; ++index) {
        flag(ui.preferences.launchers, (uint8_t)index);
        flag(ui.preferences.hidden, (uint8_t)index);
    }
    ui_allow_indices(NULL, 0);
    ui_queue(UI_NONE);
    ui_probe(check_only_basic);
    ui_quiet(UI_MARK);
    ui_quiet(UI_NONE);
    ui_quiet(UI_HIDE);
    ui_quiet(UI_NONE);
    ui_quiet(UI_DOWN);
    ui_quiet(UI_NONE);
    ui_quiet(UI_RIGHT);
    ui_quiet(UI_NONE);
    ui_probe(check_only_basic);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_BASIC);
    UI_CHECK(ui.saves == 0 && ui.names == 0 && ui.infos == 0);
    ui_verify_return();
}

static void flags_toggled_back(void)
{
    BrowserResult result;
    ui_reset(19);
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_tap(UI_MARK);
    ui_tap(UI_HIDE);
    ui_tap(UI_HIDE);
    ui_tap(UI_MARK);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 0);
    UI_CHECK(ui.saves == 0 && ui.invalidations == 0 && ui.gc_installs == 0);
    ui_verify_return();
}

static void pages_left_and_bounds(void)
{
    acrojet_fixture();
    ui_queue(UI_NONE);
    ui_quiet(UI_UP);
    ui_quiet(UI_NONE);
    ui_quiet(UI_LEFT);
    ui_quiet(UI_NONE);
    ui_tap(UI_RIGHT);
    ui_tap(UI_RIGHT);
    ui_quiet(UI_DOWN);
    ui_quiet(UI_NONE);
    ui_quiet(UI_RIGHT);
    ui_quiet(UI_NONE);
    ui_probe(check_last_19);
    ui_tap(UI_LEFT);
    ui_tap(UI_LEFT);
    ui_quiet(UI_LEFT);
    ui_quiet(UI_NONE);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_BASIC);
    UI_CHECK(ui.saves == 0);
    ui_verify_return();
}

static void last_255_down(void) { launch_last(255, UI_DOWN, 255, check_last_255); }

static void initial_held_enter(void)
{
    BrowserResult result;
    ui_reset(19);
    ui_queue(UI_ENTER);
    ui_quiet(UI_ENTER);
    ui_quiet(UI_ENTER);
    ui_quiet(UI_NONE);
    ui_tap(UI_DOWN);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 0);
    UI_CHECK(ui.saves == 0);
    ui_verify_return();
}

static void chord_requires_release(void)
{
    BrowserResult result;
    ui_reset(19);
    ui_queue(UI_NONE);
    ui_queue(UI_DOWN);
    ui_quiet(UI_DOWN | UI_MARK);
    ui_quiet(UI_MARK);
    ui_quiet(UI_ENTER);
    ui_quiet(UI_NONE);
    ui_tap(UI_DOWN);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 1);
    UI_CHECK(ui.saves == 0);
    ui_verify_return();
}

static void mode_enter_handoff(void)
{
    BrowserResult result;
    ui_reset(19);
    ui_queue(UI_MODE);
    ui_queue(UI_DOWN | UI_MODE);
    ui_queue(UI_MODE);
    ui_queue(UI_ENTER | UI_MODE);
    ui_queue(UI_ENTER | UI_MODE);
    ui_queue(UI_MODE);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 0);
    UI_CHECK(kb_IsDown(kb_KeyMode) == 0x40);
    UI_CHECK(kb_IsDown(kb_KeyEnter) == 0);
    ui_verify_return();
}

static void held_mark_only_once(void)
{
    ui_reset(19);
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_queue(UI_MARK);
    for (unsigned int poll = 0; poll < 9; ++poll)
        ui_quiet(UI_MARK);
    ui_quiet(UI_NONE);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_FILE);
    UI_CHECK(browser_flag(ui.saved_preferences.launchers, 0));
    check_save_order();
    ui_verify_return();
}

static void check_invalid_information(void)
{
    UI_CHECK(ui_has_text("Invalid PRG"));
    UI_CHECK(strcmp(ui_text_at(27, 61)->text, "Invalid entry") == 0);
    UI_CHECK(ui.selects == 0);
}

static void invalid_prg_never_opens(void)
{
    BrowserResult result;
    ui_reset(2);
    ui.files[0].valid = ui.files[0].name_valid = false;
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_probe(check_invalid_information);
    ui_tap(UI_ENTER);
    ui_probe(check_invalid_information);
    ui_tap(UI_DOWN);
    ui_tap(UI_ENTER);
    result = browser_show(3);
    UI_CHECK(result.action == BROWSER_FILE && result.file_index == 1);
    UI_CHECK(ui.selects == 1 && ui.selected == 1 && ui.saves == 0);
    ui_verify_return();
}

static void check_lock_no(void)
{
    UI_CHECK(strcmp(ui_text_at(300, 190)->text, "No") == 0);
    UI_CHECK(!ui_has_sprite(3, 177, 61));
}

static void check_lock_unknown(void)
{
    UI_CHECK(strcmp(ui_text_at(260, 190)->text, "Unknown") == 0);
    UI_CHECK(!ui_has_sprite(3, 177, 61));
}

static void lock_fixture(uint8_t lock, void (*inspect)(void))
{
    ui_reset(1);
    ui.files[0].lock = lock;
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_probe(inspect);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_FILE);
    UI_CHECK(ui.saves == 0 && ui.files[0].lock == lock);
    ui_verify_return();
}

static void lock_no(void) { lock_fixture(BROWSER_LOCK_NO, check_lock_no); }
static void lock_unknown(void) { lock_fixture(BROWSER_LOCK_UNKNOWN, check_lock_unknown); }

static void check_locked_hidden(void)
{
    UI_CHECK(strcmp(ui_text_at(292, 190)->text, "Yes") == 0);
    UI_CHECK(strcmp(ui_text_at(292, 206)->text, "Yes") == 0);
    UI_CHECK(ui_has_sprite(1, 5, 57));
    UI_CHECK(ui_has_sprite(4, 165, 61) && ui_has_sprite(3, 177, 61));
}

static void lock_read_only(void)
{
    ui_reset(1);
    ui.files[0].lock = BROWSER_LOCK_YES;
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_tap(UI_MARK);
    ui_tap(UI_HIDE);
    ui_probe(check_locked_hidden);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_FILE);
    UI_CHECK(ui.files[0].lock == BROWSER_LOCK_YES);
    UI_CHECK(browser_flag(ui.saved_preferences.launchers, 0));
    UI_CHECK(browser_flag(ui.saved_preferences.hidden, 0));
    check_save_order();
    ui_verify_return();
}

static void check_header_only_info(void)
{
    UI_CHECK(ui_has_text("2 bytes"));
    UI_CHECK(ui_has_text("$0000"));
}

static void header_only_size(void)
{
    ui_reset(1);
    ui.files[0].info.size = 2;
    ui.files[0].info.load_address = 0;
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_probe(check_header_only_info);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_FILE);
    UI_CHECK(ui.saves == 0);
    ui_verify_return();
}

static void check_disk_notice(void)
{
    UI_CHECK(ui_has_text("Disk unavailable") && ui_has_text("BASIC"));
    UI_CHECK(ui.selects == 0 && ui.remaps == 1 && ui.save_complete);
}

static void remap_failure(unsigned int fault, unsigned int answer)
{
    ui_reset(19);
    ui.simulate_gc = 1;
    if (fault == 0) ui.remap_ok = false;
    if (fault == 1) ui.remap_identified = false;
    if (fault == 2) ++ui.remap_identity.size;
    if (fault == 3) ++ui.remap_identity.crc32;
    if (fault == 4) --ui.remap_count;
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_tap(UI_MARK);
    ui_tap(UI_ENTER);
    ui_probe(check_disk_notice);
    ui_quiet(UI_NONE);
    ui_tap(answer);
    UI_CHECK(browser_show(3).action == (answer == UI_ENTER ? BROWSER_BASIC : BROWSER_QUIT));
    UI_CHECK(ui.selects == 0 && ui.disables == 1 && ui.closes == 2);
    UI_CHECK(ui.remaps == 1);
    check_save_order();
    ui_verify_return();
}

static void remap_basic(void) { remap_failure(0, UI_ENTER); }
static void remap_quit(void) { remap_failure(0, UI_CLEAR); }
static void remap_identity_missing(void) { remap_failure(1, UI_ENTER); }
static void remap_size_mismatch(void) { remap_failure(2, UI_ENTER); }
static void remap_crc_mismatch(void) { remap_failure(3, UI_ENTER); }
static void remap_count_mismatch(void) { remap_failure(4, UI_ENTER); }

static void failed_save_and_remap(void)
{
    ui_reset(19);
    ui.save_result = BROWSER_SAVE_FAILED;
    ui.remap_ok = false;
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_tap(UI_MARK);
    ui_tap(UI_ENTER);
    ui_probe(check_disk_notice);
    ui_tap(UI_ENTER);
    ui_probe(check_save_failed_notice);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_BASIC);
    UI_CHECK(ui.selects == 0 && ui.remaps == 1 && ui.disables == 1);
    check_save_order();
    ui_verify_return();
}

static void save_view_on_quit(void)
{
    ui_reset(19);
    ui.simulate_gc = 1;
    ui_queue(UI_NONE);
    ui_tap(UI_VIEW);
    ui_tap(UI_CLEAR);
    UI_CHECK(browser_show(3).action == BROWSER_QUIT);
    UI_CHECK(ui.saved_preferences.view == BROWSER_LAUNCHERS);
    UI_CHECK(ui.remaps == 0 && ui.selects == 0 && ui.disables == 1);
    check_save_order();
    ui_verify_return();
}

static void check_quit_notice(void)
{
    UI_CHECK(ui_has_text("Preferences not saved") && ui_has_text("Quit"));
    UI_CHECK(ui.saves == 1 && ui.remaps == 0);
}

static void save_failure_on_quit(void)
{
    ui_reset(19);
    ui.save_result = BROWSER_SAVE_FAILED;
    ui_queue(UI_NONE);
    ui_tap(UI_VIEW);
    ui_tap(UI_CLEAR);
    ui_probe(check_quit_notice);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_QUIT);
    UI_CHECK(ui.remaps == 0 && ui.selects == 0 && ui.disables == 1);
    check_save_order();
    ui_verify_return();
}

static void check_unidentified_defaults(void)
{
    check_lock_unknown();
    UI_CHECK(ui_has_text("Unclassified"));
    UI_CHECK(strcmp(ui_text_at(300, 206)->text, "No") == 0);
    UI_CHECK(strcmp(ui_text_at(6, 226)->text, "2/20") == 0);
    UI_CHECK(ui.preference_loads == 0 && ui.info_loads == 0);
}

static void unidentified_fixture(void)
{
    ui_reset(19);
    ui.identified = false;
    ui.preferences.view = BROWSER_LAUNCHERS;
    flag(ui.preferences.launchers, 0);
    flag(ui.preferences.hidden, 0);
    ui.files[0].lock = BROWSER_LOCK_YES;
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_probe(check_unidentified_defaults);
}

static void unidentified_no_edits(void)
{
    unidentified_fixture();
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_FILE);
    UI_CHECK(ui.saves == 0 && ui.invalidations == 0 && ui.remaps == 0);
    UI_CHECK(ui.selects == 1 && ui.selected == 0);
    ui_verify_return();
}

static void check_unidentified_notice(void)
{
    UI_CHECK(ui_has_text("Preferences not saved") && ui_has_text("Continue"));
    UI_CHECK(ui.saves == 0 && ui.invalidations == 0 && ui.remaps == 0 && ui.selects == 0);
}

static void unidentified_edits_no_write(void)
{
    unidentified_fixture();
    ui_tap(UI_MARK);
    ui_tap(UI_ENTER);
    ui_probe(check_unidentified_notice);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_FILE);
    UI_CHECK(ui.saves == 0 && ui.invalidations == 0 && ui.remaps == 0);
    UI_CHECK(ui.selects == 1 && ui.selected == 0);
    ui_verify_return();
}

static void check_allocation_notice(void)
{
    UI_CHECK(ui_has_text("Browser unavailable") && ui_has_text("BASIC"));
    UI_CHECK(ui.allocations == 0 && ui.saves == 0 && ui.selects == 0);
}

static void allocation_failure(unsigned int answer, bool require_transparency)
{
    ui_reset(19);
    ui.allocation_failure = true;
    ui_queue(UI_ENTER | UI_MODE);
    ui_queue(UI_MODE);
    ui_probe(check_allocation_notice);
    ui_queue(answer | UI_MODE);
    ui_queue(UI_MODE);
    UI_CHECK(browser_show(3).action == (answer == UI_ENTER ? BROWSER_BASIC : BROWSER_QUIT));
    UI_CHECK(ui.allocation_attempts == 1 && ui.allocations == 0 && ui.frees == 0);
    UI_CHECK(ui.saves == 0 && ui.selects == 0 && ui.disables == 1 && ui.closes == 1);
    UI_CHECK(kb_IsDown(kb_KeyMode) == 0x40);
    if (require_transparency)
        ui_verify_return();
    else
        ui_verify_handoff();
}

static void allocation_basic(void) { allocation_failure(UI_ENTER, false); }
static void allocation_quit(void) { allocation_failure(UI_CLEAR, false); }
static void allocation_basic_transparency(void) { allocation_failure(UI_ENTER, true); }
static void allocation_quit_transparency(void) { allocation_failure(UI_CLEAR, true); }

static void capture_all(void)
{
    check_long_information();
    UI_CHECK(ui_has_text("All Files") && ui_text_at(104, 25)->background == 1);
    ui_snapshot("all-files");
}

static void snapshot_all_files(void)
{
    acrojet_fixture();
    strcpy(ui.files[0].info.name, "WWWWWWWWWWWWWWWW");
    ui.files[0].info.size = 49154;
    ui.files[0].info.load_address = 0xa0f2;
    ui.files[0].lock = BROWSER_LOCK_YES;
    flag(ui.preferences.launchers, 0);
    flag(ui.preferences.hidden, 4);
    flag(ui.preferences.hidden, 5);
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_probe(capture_all);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_FILE);
    UI_CHECK(ui.saves == 0 && ui.selected == 0);
    ui_verify_return();
}

static void capture_launchers(void)
{
    UI_CHECK(ui_text_at(8, 25)->background == 1);
    UI_CHECK(strcmp(ui_text_at(6, 226)->text, "2/2") == 0);
    UI_CHECK(strcmp(ui_text_at(27, 61)->text, "ACROJET") == 0);
    UI_CHECK(ui_text_at(27, 61)->background == 3);
    UI_CHECK(!ui_has_text("PART1") && !ui_has_text("TITLE.OBJ") && !ui_has_text("PANEL.OBJ"));
    UI_CHECK(browser_flag(ui.preferences.hidden, 1));
    UI_CHECK(browser_flag(ui.preferences.hidden, 4));
    UI_CHECK(browser_flag(ui.preferences.hidden, 5));
    ui_snapshot("launchers");
}

static void snapshot_launchers(void)
{
    const uint8_t indices[] = {0};
    acrojet_fixture();
    ui.preferences.view = BROWSER_LAUNCHERS;
    ui.files[0].lock = BROWSER_LOCK_NO;
    flag(ui.preferences.launchers, 0);
    flag(ui.preferences.hidden, 1);
    flag(ui.preferences.hidden, 4);
    flag(ui.preferences.hidden, 5);
    ui_allow_indices(indices, sizeof(indices));
    ui_queue(UI_NONE);
    ui_tap(UI_DOWN);
    ui_probe(capture_launchers);
    ui_tap(UI_ENTER);
    UI_CHECK(browser_show(3).action == BROWSER_FILE);
    UI_CHECK(ui.saves == 0 && ui.selected == 0);
    ui_verify_return();
}

static void capture_last_page(void)
{
    check_last_255();
    ui_snapshot("last-page-255");
}

static void snapshot_last_page(void)
{
    launch_last(255, UI_RIGHT, 26, capture_last_page);
}

static bool run_test(void (*test)(void))
{
    if (setjmp(ui_jump) != 0)
        return false;
    test();
    ui_verify_guards();
    UI_CHECK(ui.scans == ui.queued);
    UI_CHECK(ui.allocation == NULL && ui.allocations == ui.frees);
    return true;
}

int main(int argc, char **argv)
{
    static const struct {
        const char *group, *name;
        void (*run)(void);
    } tests[] = {
        {"lifecycle", "no files immediate BASIC", no_files},
        {"lifecycle", "BASIC without edits", basic_without_edits},
        {"lifecycle", "CLEAR cleanup", clear_cleanup},
        {"navigation", "19 entries via Down", last_19_down},
        {"navigation", "19 entries via page", last_19_page},
        {"navigation", "255 entries last index 254", last_255_page},
        {"navigation", "255 entries via Down", last_255_down},
        {"navigation", "page left and navigation bounds", pages_left_and_bounds},
        {"information", "16 W name, lock, bytes and LOAD", long_information},
        {"information", "invalid PRG never opens", invalid_prg_never_opens},
        {"information", "known lock No", lock_no},
        {"information", "lock Unknown", lock_unknown},
        {"information", "lock remains read-only when marking/hiding", lock_read_only},
        {"information", "size includes two-byte header and LOAD keeps zeroes", header_only_size},
        {"input", "no idle or held-key redraw", idle_and_held_down},
        {"input", "initial held ENTER cannot autolaunch", initial_held_enter},
        {"input", "chord transition requires full menu release", chord_requires_release},
        {"input", "MODE+ENTER preserves raw MODE bitmask", mode_enter_handoff},
        {"input", "held ALPHA marks only once", held_mark_only_once},
        {"views", "filtered original drive index", filtered_original_index},
        {"views", "view round-trip preserves original index without save", view_preserves_index},
        {"views", "hide removes launcher but retains assignment", hide_launcher_keeps_assignment},
        {"views", "unmark middle selects nearest remaining row", unmark_middle_nearest},
        {"views", "unmark last selects preceding row", unmark_last_nearest},
        {"views", "unmark sole launcher recovers BASIC", unmark_only_launcher},
        {"views", "All Files shows hidden and permits unhide", all_shows_and_unhides},
        {"views", "all hidden keeps BASIC unhideable and unmarkable", empty_launchers_basic_immutable},
        {"views", "flags toggled back do not save", flags_toggled_back},
        {"storage", "save, two GC cycles, remap and select", save_with_gc},
        {"storage", "failed save notice then continue", save_failed_continue},
        {"storage", "failed save notice then quit", save_failed_quit},
        {"storage", "RAM_ONLY notice then continue", save_ram_continue},
        {"storage", "RAM_ONLY notice then quit", save_ram_quit},
        {"storage", "remap failure offers BASIC", remap_basic},
        {"storage", "remap failure offers CLEAR quit", remap_quit},
        {"storage", "remap identity missing", remap_identity_missing},
        {"storage", "remap size mismatch", remap_size_mismatch},
        {"storage", "remap CRC mismatch", remap_crc_mismatch},
        {"storage", "remap count mismatch", remap_count_mismatch},
        {"storage", "failed save and failed remap recover BASIC", failed_save_and_remap},
        {"storage", "view-only save on quit", save_view_on_quit},
        {"storage", "save failure waits for Quit acknowledgement", save_failure_on_quit},
        {"storage", "missing identity uses Unknown and default flags", unidentified_no_edits},
        {"storage", "missing identity edits never write or invalidate", unidentified_edits_no_write},
        {"allocation", "calloc failure BASIC cleanup and held MODE", allocation_basic},
        {"allocation", "calloc failure CLEAR cleanup and held MODE", allocation_quit},
        {"regression", "calloc failure BASIC restores sprite transparency", allocation_basic_transparency},
        {"regression", "calloc failure CLEAR restores sprite transparency", allocation_quit_transparency},
        {"snapshots", "All Files selected long locked filename", snapshot_all_files},
        {"snapshots", "Launchers game with hidden support files", snapshot_launchers},
        {"snapshots", "last page of 255 entries", snapshot_last_page}
    };
    unsigned int passed = 0, total = 0;
    const char *group = NULL;
    const char *excluded = NULL;
    size_t peak_bytes = 0;
    unsigned int allocations = 0, frees = 0, failures = 0;
    if (argc == 3 && strcmp(argv[1], "--group") == 0)
        group = argv[2];
    else if (argc == 3 && strcmp(argv[1], "--exclude") == 0)
        excluded = argv[2];
    else if (argc == 2 && strcmp(argv[1], "--snapshots") == 0) {
        ui_snapshots_enabled = true;
        group = "snapshots";
    }
    else if (argc != 1) {
        fprintf(stderr, "usage: ui_tests [--group GROUP | --exclude GROUP | --snapshots]\n");
        return 2;
    }
    for (unsigned int index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        bool success;
        if (group && strcmp(group, tests[index].group) != 0)
            continue;
        if (excluded && strcmp(excluded, tests[index].group) == 0)
            continue;
        ++total;
        success = run_test(tests[index].run);
        if (ui.peak_bytes > peak_bytes)
            peak_bytes = ui.peak_bytes;
        allocations += ui.allocations;
        frees += ui.frees;
        failures += ui.allocation_attempts - ui.allocations;
        if (success)
            ++passed;
        printf("%s [%s] %s\n", success ? "PASS" : "FAIL",
               tests[index].group, tests[index].name);
    }
    printf("browser UI: %u/%u passed, %u failed\n", passed, total, total - passed);
    printf("host browser calloc peak: %zu bytes (mock guards excluded)\n", peak_bytes);
        printf("browser heap: %u allocations, %u frees, %u injected calloc failures\n",
            allocations, frees, failures);
    return total && passed == total ? 0 : 1;
}