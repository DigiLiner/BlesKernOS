#ifndef BK_RGB565_H
#define BK_RGB565_H
#include "types.h"
static inline uint16_t bk_rgb565_encode(uint32_t rgb) {
    return (uint16_t)(((rgb>>8)&0xF800U)|((rgb>>5)&0x07E0U)|((rgb>>3)&0x001FU));
}
static inline uint32_t bk_rgb565_decode(uint16_t pixel) {
    uint32_t r=(pixel>>11)&31U,g=(pixel>>5)&63U,b=pixel&31U;
    return (((r<<3)|(r>>2))<<16)|(((g<<2)|(g>>4))<<8)|(b<<3)|(b>>2);
}
/* Use aligned dword stores for two pixels; never read uncached VRAM. */
static inline void bk_rgb565_upload(volatile uint16_t *dst,const uint32_t *src,uint32_t count) {
    if(count && ((uintptr_t)dst&3U)){*dst++=bk_rgb565_encode(*src++);count--;}
    while(count>=2U) {
        *(volatile uint32_t *)(uintptr_t)dst=bk_rgb565_encode(src[0])|((uint32_t)bk_rgb565_encode(src[1])<<16);
        dst+=2;src+=2;count-=2;
    }
    if(count)*dst=bk_rgb565_encode(*src);
}
#endif
