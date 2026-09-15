#ifndef UI_GRAPHX_H
#define UI_GRAPHX_H

#include <stdint.h>

typedef struct gfx_sprite_t {
    uint8_t width;
    uint8_t height;
    uint8_t data[];
} gfx_sprite_t;

extern uint8_t *const gfx_vram;
extern uint16_t *const gfx_palette;

#define gfx_RGBTo1555(red, green, blue) \
    ((uint16_t)(((((red) & 255) >> 3) << 10) | \
                ((((green) & 255) >> 3) << 5) | (((blue) & 255) >> 3)))

void gfx_Begin(void);
void gfx_End(void);
void gfx_SetPalette(const void *palette, unsigned int size, uint8_t offset);
uint8_t gfx_SetColor(uint8_t color);
uint8_t gfx_SetTransparentColor(uint8_t color);
void gfx_SetTextScale(uint8_t width_scale, uint8_t height_scale);
uint8_t gfx_SetTextFGColor(uint8_t color);
uint8_t gfx_SetTextBGColor(uint8_t color);
uint8_t gfx_SetTextTransparentColor(uint8_t color);
unsigned int gfx_GetStringWidth(const char *text);
void gfx_PrintStringXY(const char *text, int left, int top);
void gfx_FillScreen(uint8_t color);
void gfx_FillRectangle_NoClip(unsigned int left, uint8_t top,
                              unsigned int width, uint8_t height);
void gfx_ScaledTransparentSprite_NoClip(const gfx_sprite_t *sprite,
                                       unsigned int left, uint8_t top,
                                       uint8_t width_scale, uint8_t height_scale);

#endif