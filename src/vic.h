/*
 * emudore-CE: MOS VIC-II video chip (C port of vic.h).
 *
 * PAL 6569. Graphics constants are kept as #defines so the original formulas
 * read the same.
 */
#ifndef EMUDORE_CE_VIC_H
#define EMUDORE_CE_VIC_H

#include <stdint.h>
#include <stdbool.h>

void vic_reset(void);
bool vic_emulate(void);
void    vic_write_register(uint8_t r, uint8_t v);
uint8_t vic_read_register(uint8_t r);
unsigned int vic_frames(void);

/* timing / geometry */
#define VIC_SCREEN_LINES        312
#define VIC_SCREEN_COLS         504
#define VIC_VISIBLE_WIDTH       403
#define VIC_VISIBLE_HEIGHT      284
#define VIC_FIRST_VISIBLE_LINE  14
#define VIC_LAST_VISIBLE_LINE   298
#define VIC_LINE_CYCLES         63
#define VIC_BAD_LINE_CYCLES     23
#define VIC_SPRITE_PTRS_OFFSET  0x3f8

#define VIC_G_RES_X             320
#define VIC_G_RES_Y             200
#define VIC_G_COLS              40
#define VIC_G_ROWS              25
#define VIC_G_FIRST_LINE        56
#define VIC_G_LAST_LINE         256
#define VIC_G_FIRST_COL         42

#define VIC_SPRITE_WIDTH        24
#define VIC_SPRITE_HEIGHT       21
#define VIC_SPRITE_SIZE         64
#define VIC_SPRITES_FIRST_LINE  6
#define VIC_SPRITES_FIRST_COL   18

#endif /* EMUDORE_CE_VIC_H */
