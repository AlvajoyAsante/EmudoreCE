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

#ifndef EMUDORE_CHAR_ASM
#define EMUDORE_CHAR_ASM 1
#endif

#ifndef EMUDORE_VIC_TRACE
#ifdef NDEBUG
#define EMUDORE_VIC_TRACE 0
#else
#define EMUDORE_VIC_TRACE 1
#endif
#endif

/**
 * Reset the VIC-II to its power-on state.
 *
 * Raster timing, control registers, sprite latches and deferred-render state
 * are all reset so the first visible frame starts from a deterministic base.
 */
void vic_reset(void);
/**
 * Advance the VIC by one raster line.
 *
 * This processes raster timing, IRQ generation, frame completion, sprite
 * state, and any deferred rendering that must catch up before the next line.
 *
 * @return false only when a fatal stop path ends emulation; true otherwise.
 */
bool vic_emulate(void);
/**
 * Self-test the optional assembly scanline expanders.
 *
 * @return Bitmask: bit0 hires bitmap, bit1 multicolor bitmap, bit2 character
 *         rows. Each bit enables the corresponding tested kernel.
 */
uint8_t vic_row_asm_selftest(void);
/** Return the enabled row-kernel bitmask without running the tests again. */
uint8_t vic_asm_status(void);
/**
 * Write one VIC register.
 *
 * @param r Register index after VIC mirror folding.
 * @param v Byte value to write.
 */
void    vic_write_register(uint8_t r, uint8_t v);
/**
 * Read one VIC register.
 *
 * @param r Register index after VIC mirror folding.
 * @return Current register or latch value as exposed on the CPU bus.
 */
uint8_t vic_read_register(uint8_t r);
/** Inspect a VIC register without read side effects or busy-poll detection. */
uint8_t vic_peek_register(uint8_t r);
/**
 * Return the number of frames completed since reset.
 *
 * @return Monotonic frame counter used by boot hiding and pacing logic.
 */
unsigned int vic_frames(void);

/* diagnostics: unsupported graphic mode that stopped vic_emulate */
extern uint8_t vic_bad_mode;

/* diagnostics: display configuration snapshot printed by main() at exit */
typedef struct
{
   /* raw control registers and decoded mode snapshot */
    uint8_t cr1, cr2, mem_ptrs, border, bg0, mode, sprite_en, irq_en;
   /* sprite-wide enable/size/priority/multicolor control */
    uint8_t spr_dh, spr_dw, spr_prio, spr_mc;
   /* per-sprite positions and shared X MSB bits */
    uint8_t spr_x[8], spr_y[8], spr_msbx;
      uint8_t spr_ptr[8], spr_color[8], spr_shared[2];
   /* decoded memory bases from the current VIC bank state */
    uint16_t screen_mem, char_mem, bitmap_mem, vic_base;
   /* -1 when disabled, otherwise the programmed raster IRQ line */
    int raster_irq;
} VicDebug;
/**
 * Capture a debug snapshot of the current VIC state.
 *
 * @param d Output structure to fill.
 */
void vic_debug_snapshot(VicDebug *d);

/** Request that the next frame be rendered even if frame skipping is active. */
void vic_request_render(void);
/** Set the CPU/RAM fast-forward multiplier, clamped to 1..200. */
void vic_set_cpu_turbo(uint8_t multiplier);
/** Adapt the CPU/RAM multiplier to the completed-frame cadence. */
void vic_set_cpu_turbo_adaptive(bool enabled);
/** Suppress normal VIC presentation while an alternate diagnostic view is active. */
void vic_set_render_suppressed(bool enabled);
/** Enable adaptive rendering cadence; false renders every emulated frame. */
void vic_set_frame_skip(bool enabled);
/** Report whether adaptive rendering cadence is enabled. */
bool vic_frame_skip_enabled(void);
/**
 * Hide or reveal video output during boot/autoload choreography.
 *
 * @param hidden true to suppress visible rendering; false to unhide it.
 */
void vic_set_boot_hidden(bool hidden);
/**
 * Feed the raster busy-poll detector.
 *
 * @param id Stream identifier so different poll sources do not alias.
 */
void vic_poll_check(uint8_t id);

#ifndef NDEBUG
/* render-path profile: 0=wrap-flush ticks 1=char-row ticks 2=renders
   3=char-row calls 4=process_line calls */
extern unsigned long vic_prof[5];
#endif

/**
 * Flush deferred rendering up to the current raster position.
 *
 * Call this before display-affecting state changes so already-passed lines are
 * drawn with the old register state and future lines see the new one.
 */
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
