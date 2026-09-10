#include "../../src/browser_model.h"
#include "../../src/crc32.h"
#include <assert.h>
#include <stdio.h>

static void test_acrojet_selection(void)
{
    BrowserModel model;
    browser_model_init(&model, 19, NULL);
    assert(model.count == 20);
    assert(browser_model_entry(&model) == 0);
    assert(!browser_model_launcher(&model));
    assert(!browser_model_hide(&model));
    assert(browser_model_move(&model, 19));
    assert(model.top == 10);
    assert(browser_model_entry(&model) - 1 == 18);
    assert(!browser_model_move(&model, 1));
    assert(browser_model_launcher(&model));
    browser_model_view(&model);
    assert(model.count == 2);
    assert(browser_model_entry(&model) - 1 == 18);
    assert(browser_model_hide(&model));
    assert(model.count == 1);
    assert(browser_model_entry(&model) == 0);
    assert(browser_flag(model.preferences.launchers, 18));
    browser_model_view(&model);
    assert(model.count == 20);
    browser_model_move(&model, 19);
    assert(browser_flag(model.preferences.hidden, 18));
    assert(browser_model_hide(&model));
    browser_model_view(&model);
    assert(browser_model_entry(&model) == 19);
}

static void test_counts_and_pages(void)
{
    static const uint8_t counts[] = {0, 1, 8, 9, 19, 144, 255};
    unsigned int count_index;
    for (count_index = 0; count_index < sizeof(counts); ++count_index)
    {
        BrowserModel model;
        uint16_t row;
        browser_model_init(&model, counts[count_index], NULL);
        assert(model.count == (uint16_t)counts[count_index] + 1);
        for (row = 0; row < model.count; ++row)
        {
            assert(browser_model_entry(&model) == row);
            assert(model.top <= model.selected);
            assert(model.selected < model.top + BROWSER_ROWS);
            browser_model_move(&model, 1);
        }
        browser_model_move(&model, -256);
        assert(model.top == 0 && model.selected == 0);
        browser_model_move(&model, BROWSER_ROWS);
        assert(model.selected == (counts[count_index] < BROWSER_ROWS
                               ? counts[count_index] : BROWSER_ROWS));
        browser_model_move(&model, -BROWSER_ROWS);
        assert(browser_model_entry(&model) == 0);
    }
}

static void test_preserved_preferences(void)
{
    BrowserModel model;
    BrowserPreferences saved = {{0}, {0}, BROWSER_LAUNCHERS};
    saved.launchers[0] = 0x05;
    saved.hidden[0] = 0x01;
    browser_model_init(&model, 3, &saved);
    assert(model.count == 2);
    browser_model_move(&model, 1);
    assert(browser_model_entry(&model) == 3);
    browser_model_view(&model);
    assert(model.selected == 3);
    assert(browser_model_entry(&model) == 3);
    browser_model_view(&model);
    browser_model_launcher(&model);
    assert(model.count == 1);
    assert(browser_model_entry(&model) == 0);
}

int main(void)
{
    const uint8_t vector[] = "123456789";
    assert(crc32_update(0, vector, 9) == 0xcbf43926UL);
    assert(crc32_update(crc32_update(0, vector, 4), vector + 4, 5) == 0xcbf43926UL);
    assert(crc32_update(0, vector, 0) == 0);
    test_acrojet_selection();
    test_counts_and_pages();
    test_preserved_preferences();
    puts("browser model: 4 tests passed (CRC32 and every row of 255 files)");
    return 0;
}