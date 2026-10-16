#ifndef BLES_ARCH_ARM32_RPI3_FRAMEBUFFER_H
#define BLES_ARCH_ARM32_RPI3_FRAMEBUFFER_H

#include "../../../kernel/include/types.h"

typedef struct {
    volatile uint8_t *base;
    uint32_t bus_address;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t depth;
    uint32_t pixel_order; /* 0=BGR, 1=RGB, firmware-defined */
} rpi3_framebuffer_t;

int rpi3_framebuffer_init(uint32_t width, uint32_t height, uint32_t depth);
const rpi3_framebuffer_t *rpi3_framebuffer_get(void);
void rpi3_framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t rgb);
void rpi3_framebuffer_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void rpi3_framebuffer_clear(uint32_t rgb);
void rpi3_framebuffer_flush(void);
void rpi3_framebuffer_test_pattern(void);

#endif
