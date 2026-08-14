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
/* Verifies the eZ80 scanline rows; returns bit0 = hires OK, bit1 = MC OK. */
uint8_t vic_row_asm_selftest(void);
void    vic_write_register(uint8_t r, uint8_t v);
uint8_t vic_read_register(uint8_t r);
unsigned int vic_frames(void);

/* diagnostics: unsupported graphic mode that stopped vic_emulate */
extern uint8_t vic_bad_mode;

/* diagnostics: display configuration snapshot printed by main() at exit */
typedef struct
{
    uint8_t cr1, cr2, mem_ptrs, border, bg0, mode, sprite_en, irq_en;
    uint8_t spr_dh, spr_dw, spr_prio, spr_mc;
    uint8_t spr_x[8], spr_y[8], spr_msbx;
    uint16_t screen_mem, char_mem, bitmap_mem, vic_base;
    int raster_irq;
} VicDebug;
void vic_debug_snapshot(VicDebug *d);

/* ask for the next frame to be rendered (used on keyboard activity) */
void vic_request_render(void);

/* lazy rendering: draw pending raster lines with the current register state
   (called before display-affecting VIC writes and CIA2 bank switches) */
void vic_render_catchup(void);

/* rolling sprite/control write trace for multiplexer diagnosis */
#define VIC_WTRACE_MAX 64
typedef struct { uint16_t line; uint8_t reg, val; } VicWriteTrace;
extern VicWriteTrace vic_wtrace[VIC_WTRACE_MAX];
extern uint8_t vic_wtrace_pos;

/* cpu.cycles value at which the next raster line is processed; read by
   c64_run's CPU batching loop */
extern unsigned int vic_raster_due;

/* timing / geometry */
#define VIC_SCREEN_LINES        312
#define VIC_SCREEN_COLS         504
#define VIC_VISIBLE_WIDTH       403
#define VIC_VISIBLE_HEIGHT      284
/* Hardware-true PAL geometry: text window (25 rows, yscroll=3) spans raster
   lines 51-250, so games' raster splits land on the intended picture bands.
   LCD mapping: fb_y = raster - VIC_FIRST_VISIBLE_LINE, LCD_y = fb_y - IO_CROP_Y
   keeps the text area centred (20-px letterbox bands). */
#define VIC_FIRST_VISIBLE_LINE  9
#define VIC_LAST_VISIBLE_LINE   293
#define VIC_LINE_CYCLES         63
#define VIC_BAD_LINE_CYCLES     23
#define VIC_SPRITE_PTRS_OFFSET  0x3f8

#define VIC_G_RES_X             320
#define VIC_G_RES_Y             200
#define VIC_G_COLS              40
#define VIC_G_ROWS              25
#define VIC_G_FIRST_LINE        51
#define VIC_G_LAST_LINE         251
#define VIC_G_FIRST_COL         42

#define VIC_SPRITE_WIDTH        24
#define VIC_SPRITE_HEIGHT       21
#define VIC_SPRITE_SIZE         64
/* sprite Y=50/X=24 aligns with the text window top-left (raster 51) */
#define VIC_SPRITES_FIRST_LINE  1
#define VIC_SPRITES_FIRST_COL   19

#endif /* EMUDORE_CE_VIC_H */
