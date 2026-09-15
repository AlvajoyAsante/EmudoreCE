#ifndef EMUDORE_BROWSER_STORAGE_H
#define EMUDORE_BROWSER_STORAGE_H

#include "browser_model.h"
#include "drive.h"

#define BROWSER_LOCK_UNKNOWN 0
#define BROWSER_LOCK_NO 1
#define BROWSER_LOCK_YES 2

void browser_info_load(const DriveIdentity *identity, uint8_t file_count,
                       uint8_t *locks);
void browser_preferences_load(const DriveIdentity *identity, uint8_t file_count,
                              BrowserPreferences *preferences);

typedef enum
{
    BROWSER_SAVE_OK,
    BROWSER_SAVE_FAILED,
    BROWSER_SAVE_RAM_ONLY
} BrowserSaveResult;

/**
 * Saves synchronously. RAM_ONLY means verified data remains in RAM.
 * Caller must invalidate drive/package mappings and install GC callbacks first,
 * then remap after every result, including failure.
 */
BrowserSaveResult browser_preferences_save(const DriveIdentity *identity,
                                          uint8_t file_count,
                                          const BrowserPreferences *preferences);

#endif