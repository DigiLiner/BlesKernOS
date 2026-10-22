#include <bleskernos_media_lbs.h>
#include "../../libs/native/media/video.h"
#include "../../libs/native/media/render.h"
#include <string.h>
#include "resources.h"
struct bk_media_video {
    vv_video_t video;
    uint32_t chunk, offset;
};
const char *bk_media_formats(void) {
    return "BMP PNG JPEG SVG GIF BKV1 BKV2 AVI(DIB/MJPEG/PCM) MPEG1/MP2";
}
bk_media_video_t *bk_media_video_open(const void *data, uint32_t size) {
    bk_media_video_t *v;
    if (!data || !size)
        return 0;
    v = bk_sys_alloc(sizeof(*v));
    if (!v)
        return 0;
    memset(v, 0, sizeof(*v));
    if (!vv_open(&v->video, data, size)) {
        vv_close(&v->video);
        bk_sys_free(v);
        return 0;
    }
    bk_lbs_resource_acquire();
    return v;
}
void bk_media_video_close(bk_media_video_t *v) {
    if (v) {
        vv_close(&v->video);
        bk_sys_free(v);
        bk_lbs_resource_release();
    }
}
int bk_media_video_frame(bk_media_video_t *v, uint32_t frame, bk_gui_image_t *out) {
    const bk_gui_image_t *image;
    if (!v || !out)
        return BK_LBS_INVALID;
    if (!vv_prepare_frame(&v->video, frame))
        return -5;
    image = vv_frame_image(&v->video);
    if (!image)
        return BK_LBS_UNSUPPORTED;
    *out = *image;
    return 0;
}
uint32_t bk_media_video_frames(bk_media_video_t *v) {
    return v ? v->video.frame_count : 0;
}
uint32_t bk_media_audio_read(bk_media_video_t *v, uint8_t *out, uint32_t n) {
    return v && out ? vv_audio_decode_u8_mono(&v->video, &v->chunk, &v->offset, out, n) : 0;
}
uint32_t bk_media_audio_rate(bk_media_video_t *v) {
    return v ? vv_audio_sample_rate(&v->video) : 0;
}

bk_media_video_t *bk_media_video_open_path(const char *path, uint32_t size, uint32_t format) {
    if (!path || !*path || (format != BK_MEDIA_AVI && format != BK_MEDIA_MPEG1)) { bk_sys_log("[MEDIA] invalid stream arguments\n"); return 0; }
    bk_media_video_t *v = bk_sys_alloc(sizeof(*v));
    if (!v) { bk_sys_log("[MEDIA] stream allocation failed\n"); return 0; }
    memset(v, 0, sizeof(*v));
    bool ok = format == BK_MEDIA_AVI ? vv_open_avi_stream(&v->video, path, size)
                                    : vv_open_mpeg_stream(&v->video, path, size);
    if (!ok) { vv_close(&v->video); bk_sys_free(v); return 0; }
    bk_lbs_resource_acquire();
    return v;
}
int bk_media_video_info(bk_media_video_t *v, bk_media_video_info_t *out) {
    if (!v || !out || out->size < sizeof(*out)) return BK_LBS_INVALID;
    bk_media_video_info_t info = {sizeof(info), v->video.width, v->video.height,
        v->video.frame_count, v->video.fps_num, v->video.fps_den, v->video.format,
        v->video.loop_default, vv_total_duration_ms(&v->video),
        vv_audio_available(&v->video), vv_audio_sample_rate(&v->video)};
    *out = info;
    return 0;
}
bool bk_media_video_prepare(bk_media_video_t *v, uint32_t frame) {
    return v && vv_prepare_frame(&v->video, frame);
}
void bk_media_video_invalidate(bk_media_video_t *v) {
    if (v) { v->video.frame_prepared = false; v->video.frame_image_valid = false; }
}
uint32_t bk_media_video_frame_duration(bk_media_video_t *v, uint32_t frame) {
    return v ? vv_frame_duration_ms(&v->video, frame) : 0;
}
uint32_t bk_media_video_frame_time(bk_media_video_t *v, uint32_t frame) {
    return v ? vv_frame_time_ms(&v->video, frame) : 0;
}
const char *bk_media_video_format_name(bk_media_video_t *v) {
    return v ? vv_format_name(&v->video) : "";
}
const char *bk_media_quality_name(bk_media_quality_t q) { return vr_quality_name(q); }
bool bk_media_video_draw(bk_gui_surface_t *surface, bk_media_video_t *v,
                        bk_gui_rect_t target, bk_media_quality_t quality,
                        bk_media_render_stats_t *stats) {
    return v && quality < BK_MEDIA_QUALITY_COUNT &&
           vr_draw_frame(surface, &v->video, target, quality, stats);
}
void bk_media_audio_seek(bk_media_video_t *v, uint32_t ms,
                        uint32_t *chunk, uint32_t *offset) {
    if (v && chunk && offset) vv_audio_seek_ms(&v->video, ms, chunk, offset);
}
uint32_t bk_media_audio_decode(bk_media_video_t *v, uint32_t *chunk,
                              uint32_t *offset, uint8_t *out, uint32_t capacity) {
    return v && chunk && offset && out ?
        vv_audio_decode_u8_mono(&v->video, chunk, offset, out, capacity) : 0;
}
