#ifndef BLESK_SS_SCALE_H
#define BLESK_SS_SCALE_H

/* Local RGB drawing: saver frames already live in the callback's writable
 * surface. Avoid a kernel/API transition for every pixel. */
static inline bool ss_rgb_clip(const gui_surface_t *surface, gui_rect_t rect,
                               gui_rect_t *visible) {
    if (!surface || !surface->pixels || rect.w <= 0 || rect.h <= 0) return false;
    int width = surface->storage_width ? surface->storage_width : surface->width;
    int height = surface->storage_height ? surface->storage_height : surface->height;
    int left = rect.x, top = rect.y, right = rect.x + rect.w, bottom = rect.y + rect.h;
    if (left < surface->clip.x) left = surface->clip.x;
    if (top < surface->clip.y) top = surface->clip.y;
    if (right > surface->clip.x + surface->clip.w) right = surface->clip.x + surface->clip.w;
    if (bottom > surface->clip.y + surface->clip.h) bottom = surface->clip.y + surface->clip.h;
    if (left < surface->origin_x) left = surface->origin_x;
    if (top < surface->origin_y) top = surface->origin_y;
    if (right > surface->origin_x + width) right = surface->origin_x + width;
    if (bottom > surface->origin_y + height) bottom = surface->origin_y + height;
    if (left >= right || top >= bottom || surface->pitch < (uint32_t)width) return false;
    *visible = (gui_rect_t){left, top, right-left, bottom-top};
    return true;
}

static inline void ss_putpixel_rgb(gui_surface_t *surface, int x, int y, uint32_t color) {
    gui_rect_t visible;
    if (ss_rgb_clip(surface, (gui_rect_t){x,y,1,1}, &visible))
        surface->pixels[(uint32_t)(y-surface->origin_y)*surface->pitch +
                        (uint32_t)(x-surface->origin_x)] = color & 0x00FFFFFFU;
}

/* Exact nearest-neighbour mapping, including offset/clipped previews. Keep
 * divisions outside the pixel loop and reuse repeated expanded rows. */
static void ss_blit_rgb(gui_surface_t *surface, const uint32_t *pixels,
                        uint16_t width, uint16_t height, gui_rect_t destination) {
    gui_rect_t visible;
    if (!pixels || !width || !height || !ss_rgb_clip(surface,destination,&visible)) return;
    uint32_t dw = destination.w, dh = destination.h;
    uint32_t xstart = (uint32_t)(visible.x-destination.x)*width;
    uint32_t ystart = (uint32_t)(visible.y-destination.y)*height;
    uint32_t xstep=width/dw, xrem=width%dw, ystep=height/dh, yrem=height%dh;
    uint32_t sy=ystart/dh, yerror=ystart%dh, previous_y=0xFFFFFFFFU;
    for (int y=visible.y; y<visible.y+visible.h; y++) {
        uint32_t *dst=surface->pixels+(uint32_t)(y-surface->origin_y)*surface->pitch+
                      (uint32_t)(visible.x-surface->origin_x);
        if (sy == previous_y) {
            const uint32_t *src=dst-surface->pitch;
            uint32_t count=visible.w;
#ifdef BLES_ARCH_ARM32_RPI3
            while (count--) *dst++=*src++;
#else
            __asm__ volatile ("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(count) : : "memory");
#endif
        } else {
            const uint32_t *src=pixels+sy*width;
            uint32_t sx=xstart/dw, xerror=xstart%dw;
            for (int x=0;x<visible.w;x++) {
                dst[x]=src[sx]&0x00FFFFFFU;
                sx+=xstep;xerror+=xrem;
                if (xerror>=dw){xerror-=dw;sx++;}
            }
        }
        previous_y=sy;sy+=ystep;yerror+=yrem;
        if(yerror>=dh){yerror-=dh;sy++;}
    }
}

static inline void ss_scale_rgb(gui_surface_t *surface, const uint32_t *pixels,
                                uint16_t width, uint16_t height) {
    if (!surface) return;
    ss_blit_rgb(surface,pixels,width,height,(gui_rect_t){0,0,surface->width,surface->height});
}
#endif
