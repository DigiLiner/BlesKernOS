#ifndef BLESKERNOS_RASTER_LBS_H
#define BLESKERNOS_RASTER_LBS_H
#include <stdint.h>
typedef struct { int x, y, w, h; } bk_raster_rect_t;
/* Caller-owned XRGB staging pixels, pitch in pixels. Storage may have a
 * desktop-coordinate origin. Caller guarantees pitch*storage_height capacity. */
typedef struct {
    uint32_t size;
    uint32_t *pixels;
    uint32_t pitch, storage_width, storage_height;
    int origin_x, origin_y;
    bk_raster_rect_t clip;
} bk_raster_view_t;
int bk_ngs_checker(bk_raster_view_t *, bk_raster_rect_t area);
/* Nearest scaling and legacy binary alpha (zero transparent, nonzero opaque). */
int bk_ngs_image_blit(bk_raster_view_t *, bk_raster_rect_t area,
                      bk_raster_rect_t destination, const uint32_t *pixels,
                      uint32_t width, uint32_t height);
#endif
