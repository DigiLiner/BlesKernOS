#ifndef VIEWER_RENDER_H
#define VIEWER_RENDER_H

#include <bleskernos_api.h>
#include "video.h"

typedef enum {
    VR_QUALITY_AUTO = 0,
    VR_QUALITY_EXACT,
    VR_QUALITY_FINE,
    VR_QUALITY_FAST,
    VR_QUALITY_TURBO,
    VR_QUALITY_ULTRA,
    VR_QUALITY_COUNT
} vr_quality_t;

typedef struct {
    uint32_t rectangles;
    uint32_t samples;
    uint8_t block_px;
} vr_stats_t;

uint32_t vr_rgb565_to_xrgb(uint16_t color);
const char *vr_quality_name(vr_quality_t quality);
uint8_t vr_quality_block(vr_quality_t quality, const vv_video_t *video,
                         bk_gui_rect_t target);
bool vr_draw_frame(bk_gui_surface_t *surface, vv_video_t *video,
                   bk_gui_rect_t target, vr_quality_t quality,
                   vr_stats_t *stats);

#endif
