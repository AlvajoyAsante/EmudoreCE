#ifndef IO_TEST_GRAPHX_H
#define IO_TEST_GRAPHX_H

#include <stdint.h>

typedef int gfx_location_t;

extern uint8_t *const gfx_vram;
extern uint16_t *const gfx_palette;

#define gfx_RGBTo1555(red, green, blue) \
    ((uint16_t)(((((red) & 255) >> 3) << 10) | \
                ((((green) & 255) >> 3) << 5) | (((blue) & 255) >> 3)))

void gfx_SetTextScale(uint8_t width_scale, uint8_t height_scale);
uint8_t gfx_SetTextFGColor(uint8_t color);
uint8_t gfx_SetTextBGColor(uint8_t color);
uint8_t gfx_SetTextTransparentColor(uint8_t color);
unsigned int gfx_GetStringWidth(const char *text);
void gfx_PrintStringXY(const char *text, int left, int top);
uint8_t gfx_SetColor(uint8_t color);
void gfx_FillRectangle_NoClip(unsigned int left, uint8_t top,
                              unsigned int width, uint8_t height);
void gfx_Rectangle_NoClip(unsigned int left, uint8_t top,
                         unsigned int width, uint8_t height);

#endif