/*
 * emudore-CE: calculator screen + keypad that replace the original SDL-based
 * IO class. Everything SDL, std::queue, std::chrono and std::thread is gone.
 *
 * The screen is drawn 1:1 (no scaling) into the graphx draw buffer and copied
 * to the LCD once per emulated frame.
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

void io_init(void);
void io_shutdown(void);
/* Scan the CE keypad and refresh the CIA keyboard matrix + CLEAR exit key. */
bool io_emulate(void);

/* Drawing entry points used by the VIC-II. */
void io_screen_update_pixel(int x, int y, uint8_t color);
void io_screen_draw_rect(int x, int y, int w, uint8_t color);
void io_screen_draw_border(int y, uint8_t color);
/* Direct framebuffer pointer for a C64 frame row (NULL if off-screen);
   dst[0] is the first text-area pixel. Used by the fast char renderer. */
uint8_t *io_text_row(int y);
/* Copy the finished frame to the LCD. */
void io_screen_refresh(void);

/* CIA1 keyboard matrix column read-back (active-low). */
uint8_t io_keyboard_matrix_row(int col);

/* Returns true once the user pressed CLEAR to quit. */
bool io_quit_requested(void);

#endif /* EMUDORE_CE_IO_H */
