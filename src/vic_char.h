#ifndef EMUDORE_CE_VIC_CHAR_H
#define EMUDORE_CE_VIC_CHAR_H

#include <stdbool.h>
#include <stdint.h>

extern uint8_t *vic_char_dst;
extern const uint8_t *vic_char_screen, *vic_char_colram, *vic_char_glyphs;
extern uint8_t vic_char_cells, vic_char_rows, vic_char_multicolor;

/** Set the three shared 4-bit palette colors before expanding any cells. */
void vic_char_set_colors(uint8_t bg0, uint8_t bg1, uint8_t bg2);

/**
 * Expand full cells only, starting at vic_char_dst with a 320-byte row stride.
 * vic_char_glyphs is the charset base plus the starting row within a glyph;
 * screen codes index it with code * 8. Rows advance by one glyph byte.
 * The caller must keep all rows within one glyph and all cells within each
 * destination row. Counts outside cells=1..40 or rows=1..8 are no-ops.
 * Nonzero multicolor enables mixed MC/hires selection by color RAM bit 3.
 * Inputs are not advanced. This entry point is not reentrant; ECM is excluded.
 */
void vic_char_expand_rows(void);

/**
 * Guarded reference/assembly comparison using 4816 temporary heap bytes.
 * Restores inputs and prior color-cache validity; allocation failure is false.
 * Does not initialize video or access the LCD.
 */
bool vic_char_selftest(void);

#endif
