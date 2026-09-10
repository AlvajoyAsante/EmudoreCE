/*
 * emudore-CE: calculator screen + keypad that replace the original SDL-based
 * IO class. Everything SDL, std::queue, std::chrono and std::thread is gone.
 *
 * The VIC composes 1:1 rows off-screen, then copies completed rows into the
 * visible graphx buffer. This is not a full-frame buffer swap.
 */
#ifndef EMUDORE_CE_IO_H
#define EMUDORE_CE_IO_H

#include <stdint.h>
#include <stdbool.h>

/* Visible C64 pixel area (the 320x200 text/bitmap area plus its border).
   The TI-84+CE LCD is 320x240, so the C64 image is centered. */
#define C64_VISIBLE_COLS 403
#define C64_VISIBLE_ROWS 284
#define C64_ORIGIN_X     0
#define C64_ORIGIN_Y     0

/* Crop offsets from C64 frame coords to LCD coords (LCD_x = x - IO_CROP_X). */
#define IO_CROP_X 43
#define IO_CROP_Y 22

#ifndef IO_SHOW_FPS_OVERLAY
#define IO_SHOW_FPS_OVERLAY 0
#endif

/**
 * Initialize the calculator-facing video and input layer.
 *
 * This prepares the graphx framebuffer view, caches per-row pointers and
 * resets keypad-derived state used by the CIAs and boot UI.
 */
void io_init(void);
/** Shut the IO layer down. Presently retained for symmetry with io_init(). */
void io_shutdown(void);
/**
 * Poll the CE keypad and update input-facing state.
 *
 * This rebuilds the keyboard matrix, updates joystick state and cached border
 * indicators, and tracks CLEAR as a host-side quit request. GRAPH blocks in
 * Settings with timer 1 stopped until the user resumes or quits.
 *
 * @return false when the user requested quit; true otherwise.
 */
bool io_emulate(void);

/**
 * Write one framebuffer pixel.
 *
 * @param x C64-visible-frame X coordinate before LCD crop.
 * @param y C64-visible-frame Y coordinate before LCD crop.
 * @param color C64 palette index, optionally ORed with IO_TAG_* bits.
 */
void io_screen_update_pixel(int x, int y, uint8_t color);
/**
 * Fill a horizontal pixel run.
 *
 * @param x Left edge in C64-visible-frame coordinates.
 * @param y Row in C64-visible-frame coordinates.
 * @param w Width in pixels.
 * @param color C64 palette index, optionally ORed with IO_TAG_* bits.
 */
void io_screen_draw_rect(int x, int y, int w, uint8_t color);
/**
 * Paint one whole border-aligned scanline.
 *
 * @param y C64-visible-frame row before LCD crop.
 * @param color Border palette index.
 */
void io_screen_draw_border(int y, uint8_t color);
/**
 * Read back one framebuffer pixel.
 *
 * @param x C64-visible-frame X coordinate before LCD crop.
 * @param y C64-visible-frame Y coordinate before LCD crop.
 * @return Stored pixel byte, including IO_TAG_* bits, or 0 when off-screen.
 */
uint8_t io_screen_peek(int x, int y);
#define IO_TAG_FOREGROUND 0x20
#define IO_TAG_SPRITE     0x10

/** Rebuild all 256 palette entries as the repeating 16-colour C64 pattern. */
void io_reload_palette(void);
/**
 * Inspect one graphx palette entry.
 *
 * @param index Palette slot to sample.
 * @return Raw graphx 1555 color value stored at that slot.
 */
uint16_t io_palette_probe(uint8_t index);
/**
 * Get a writable framebuffer row pointer for the text area renderer.
 *
 * @param y C64 raster row before LCD crop.
 * @return Pointer whose element 0 is the first text-area pixel, or NULL when
 *         the requested row is outside the visible LCD crop.
 */
uint8_t *io_text_row(int y);
/** End-of-frame hook; completed row compositions are already on the LCD. */
void io_screen_refresh(void);
/**
 * Draw or suppress the host-side FPS overlay.
 *
 * @param can_draw true when the caller permits overlay drawing this frame.
 */
#if IO_SHOW_FPS_OVERLAY
void io_fps_overlay(bool can_draw);
#else
#define io_fps_overlay(can_draw) ((void)(can_draw))
#endif

/**
 * Read one active-low C64 keyboard matrix column.
 *
 * @param col Column number 0-7 selected by CIA #1 PRA.
 * @return Active-low row bits for that column.
 */
uint8_t io_keyboard_matrix_row(int col);

/**
 * Read joystick port 2 state.
 *
 * @return Active-low bits where bit0=up, bit1=down, bit2=left,
 *         bit3=right, bit4=fire.
 */
uint8_t io_joystick2(void);

/**
 * Report whether the host requested exit.
 *
 * @return true once CLEAR has been pressed on the calculator keypad.
 */
bool io_quit_requested(void);

#endif /* EMUDORE_CE_IO_H */
