#include "browser_model.h"
#include <string.h>

bool browser_flag(const uint8_t *flags, uint8_t index)
{
    return (flags[index / 8] & (uint8_t)(1u << (index % 8))) != 0;
}

static void keep_visible(BrowserModel *model)
{
    uint16_t maximum_top = model->count > BROWSER_ROWS
                         ? model->count - BROWSER_ROWS : 0;
    if (model->selected < model->top)
        model->top = model->selected;
    if (model->selected >= model->top + BROWSER_ROWS)
        model->top = model->selected - BROWSER_ROWS + 1;
    if (model->top > maximum_top)
        model->top = maximum_top;
}

static void rebuild(BrowserModel *model, uint16_t previous_entry)
{
    uint16_t index;
    uint16_t previous_row = model->selected;
    bool found = previous_entry == 0;

    model->count = 1;
    model->entries[0] = 0;
    if (found)
        model->selected = 0;
    for (index = 0; index < model->file_count; ++index)
    {
        if (model->preferences.view == BROWSER_LAUNCHERS &&
            (!browser_flag(model->preferences.launchers, (uint8_t)index) ||
             browser_flag(model->preferences.hidden, (uint8_t)index)))
            continue;
        model->entries[model->count] = index + 1;
        if (index + 1 == previous_entry)
        {
            model->selected = model->count;
            found = true;
        }
        ++model->count;
    }
    if (!found)
        model->selected = previous_row < model->count
                        ? previous_row : model->count - 1;
    keep_visible(model);
}

void browser_model_init(BrowserModel *model, uint8_t file_count,
                        const BrowserPreferences *preferences)
{
    memset(model, 0, sizeof(*model));
    if (preferences)
        model->preferences = *preferences;
    if (model->preferences.view > BROWSER_LAUNCHERS)
        model->preferences.view = BROWSER_ALL_FILES;
    model->file_count = file_count;
    rebuild(model, 0);
}

uint16_t browser_model_entry(const BrowserModel *model)
{
    return model->entries[model->selected];
}

bool browser_model_move(BrowserModel *model, int distance)
{
    int next = (int)model->selected + distance;
    uint16_t previous = model->selected;
    if (next < 0)
        next = 0;
    if (next >= model->count)
        next = model->count - 1;
    model->selected = (uint16_t)next;
    keep_visible(model);
    return model->selected != previous;
}

void browser_model_view(BrowserModel *model)
{
    uint16_t entry = browser_model_entry(model);
    model->preferences.view ^= 1;
    rebuild(model, entry);
}

static bool toggle_flag(BrowserModel *model, uint8_t *flags)
{
    uint16_t entry = browser_model_entry(model);
    uint8_t index;
    if (!entry)
        return false;
    index = (uint8_t)(entry - 1);
    flags[index / 8] ^= (uint8_t)(1u << (index % 8));
    rebuild(model, entry);
    return true;
}

bool browser_model_launcher(BrowserModel *model)
{
    return toggle_flag(model, model->preferences.launchers);
}

bool browser_model_hide(BrowserModel *model)
{
    return toggle_flag(model, model->preferences.hidden);
}