#ifndef EMUDORE_BROWSER_MODEL_H
#define EMUDORE_BROWSER_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define BROWSER_ROWS 10
#define BROWSER_FLAG_BYTES 32
#define BROWSER_ALL_FILES 0
#define BROWSER_LAUNCHERS 1

typedef struct
{
    uint8_t launchers[BROWSER_FLAG_BYTES];
    uint8_t hidden[BROWSER_FLAG_BYTES];
    uint8_t view;
} BrowserPreferences;

typedef struct
{
    BrowserPreferences preferences;
    uint16_t entries[256];
    uint16_t count;
    uint16_t selected;
    uint16_t top;
    uint8_t file_count;
} BrowserModel;

bool browser_flag(const uint8_t *flags, uint8_t index);
void browser_model_init(BrowserModel *model, uint8_t file_count,
                        const BrowserPreferences *preferences);
bool browser_model_move(BrowserModel *model, int distance);
void browser_model_view(BrowserModel *model);
bool browser_model_launcher(BrowserModel *model);
bool browser_model_hide(BrowserModel *model);
uint16_t browser_model_entry(const BrowserModel *model);

#endif