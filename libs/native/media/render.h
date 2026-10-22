#ifndef VIEWER_RENDER_H
#define VIEWER_RENDER_H

#include <bleskernos_api.h>
#include "video.h"
#include <bleskernos_media_lbs.h>

typedef bk_media_quality_t vr_quality_t;
typedef bk_media_render_stats_t vr_stats_t;
#define VR_QUALITY_AUTO BK_MEDIA_QUALITY_AUTO
#define VR_QUALITY_EXACT BK_MEDIA_QUALITY_EXACT
#define VR_QUALITY_FINE BK_MEDIA_QUALITY_FINE
#define VR_QUALITY_FAST BK_MEDIA_QUALITY_FAST
#define VR_QUALITY_TURBO BK_MEDIA_QUALITY_TURBO
#define VR_QUALITY_ULTRA BK_MEDIA_QUALITY_ULTRA
#define VR_QUALITY_COUNT BK_MEDIA_QUALITY_COUNT

uint32_t vr_rgb565_to_xrgb(uint16_t color);
const char *vr_quality_name(vr_quality_t quality);
uint8_t vr_quality_block(vr_quality_t quality, const vv_video_t *video,
                         bk_gui_rect_t target);
bool vr_draw_frame(bk_gui_surface_t *surface, vv_video_t *video,
                   bk_gui_rect_t target, vr_quality_t quality,
                   vr_stats_t *stats);

#endif
