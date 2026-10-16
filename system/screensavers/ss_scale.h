#ifndef BLESK_SS_SCALE_H
#define BLESK_SS_SCALE_H

/* Exact nearest-neighbour mapping, with divisions outside the pixel loop.
 * Repeated output rows reuse the already expanded row in cached RAM. */
static void ss_scale_rgb(gui_surface_t *surface, const uint32_t *pixels,
                         uint16_t width, uint16_t height) {
    uint32_t xstep, xrem, ystep, yrem, sy = 0U, yerror = 0U;
    uint32_t previous_y = 0xFFFFFFFFU;
    if (!surface || !surface->pixels || !pixels || !width || !height ||
        !surface->width || !surface->height || surface->pitch < surface->width)
        return;
    xstep = width / surface->width;
    xrem = width % surface->width;
    ystep = height / surface->height;
    yrem = height % surface->height;
    for (uint32_t y = 0; y < surface->height; y++) {
        uint32_t *dst = surface->pixels + y * surface->pitch;
        if (sy == previous_y) {
            const uint32_t *src = dst - surface->pitch;
            uint32_t count = surface->width;
            __asm__ volatile ("cld; rep movsl"
                              : "+D"(dst), "+S"(src), "+c"(count)
                              : : "memory");
        } else {
            const uint32_t *src = pixels + sy * width;
            uint32_t sx = 0U, xerror = 0U;
            for (uint32_t x = 0; x < surface->width; x++) {
                dst[x] = src[sx] & 0x00FFFFFFU;
                sx += xstep;
                xerror += xrem;
                if (xerror >= surface->width) {
                    xerror -= surface->width;
                    sx++;
                }
            }
        }
        previous_y = sy;
        sy += ystep;
        yerror += yrem;
        if (yerror >= surface->height) {
            yerror -= surface->height;
            sy++;
        }
    }
}
#endif
