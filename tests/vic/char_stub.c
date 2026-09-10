#include <string.h>

#include "../../src/vic_char.h"

_Alignas(256) uint8_t vic_char_hires_lut[16][16][4];
_Alignas(256) uint8_t vic_char_mc_lut[8][16][4];

void vic_char_expand_rows(void)
{
    if (vic_char_cells == 0 || vic_char_cells > 40 ||
        vic_char_rows == 0 || vic_char_rows > 8)
        return;

    for (unsigned int cell_index = 0; cell_index < vic_char_cells; ++cell_index)
    {
        uint8_t color = vic_char_colram[cell_index] & 15;
        uint8_t (*table)[4] =
            vic_char_multicolor != 0 && (color & 8) != 0
                ? vic_char_mc_lut[color & 7] : vic_char_hires_lut[color];
        const uint8_t *glyph = vic_char_glyphs + (unsigned int)vic_char_screen[cell_index] * 8;
        for (unsigned int row_index = 0; row_index < vic_char_rows; ++row_index)
        {
            uint8_t data = glyph[row_index];
            uint8_t *destination = vic_char_dst + row_index * 320 + cell_index * 8;
            memcpy(destination, table[data >> 4], 4);
            memcpy(destination + 4, table[data & 15], 4);
        }
    }
}