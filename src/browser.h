#ifndef EMUDORE_BROWSER_H
#define EMUDORE_BROWSER_H

#include <stdbool.h>
#include <stdint.h>

#include "CEPackage.h"

typedef enum
{
    BROWSER_QUIT,
    BROWSER_FILE,
    BROWSER_BASIC,
    BROWSER_BACK
} BrowserAction;

typedef struct
{
    BrowserAction action;
    uint8_t file_index;
} BrowserResult;

/* Shows the browser's disk root and returns the selected action. */
BrowserResult browser_select_disk(CEPackageDisk *disk, const char *selected_id);
BrowserResult browser_show(uint8_t rom_count, const char *package_id,
                           uint8_t selected_file);

#endif