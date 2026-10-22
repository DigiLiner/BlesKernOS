#ifndef BLESKERNOS_MEDIA_LBS_H
#define BLESKERNOS_MEDIA_LBS_H
#include <bleskernos_lbs.h>
#include <bleskernos_api.h>
typedef struct bk_media_video bk_media_video_t;
const char *bk_media_formats(void);
/* In-memory streams borrow data until close. */
bk_media_video_t *bk_media_video_open(const void *data, uint32_t size);
void bk_media_video_close(bk_media_video_t *);
int bk_media_video_frame(bk_media_video_t *, uint32_t frame, bk_gui_image_t *borrowed);
uint32_t bk_media_video_frames(bk_media_video_t *);
uint32_t bk_media_audio_read(bk_media_video_t *, uint8_t *out, uint32_t capacity);
uint32_t bk_media_audio_rate(bk_media_video_t *);
/* ABI 1 additions. Memory-open borrows its stream; path-open owns its file
 * descriptors. The context owns decoded frames. Serialize use of a context. */
enum { BK_MEDIA_NONE, BK_MEDIA_BKV1, BK_MEDIA_BKV2, BK_MEDIA_AVI,
       BK_MEDIA_GIF, BK_MEDIA_MPEG1 };
typedef struct {
    uint32_t size, width, height, frame_count, fps_num, fps_den, format;
    uint32_t loop_default, duration_ms, audio_available, audio_rate;
} bk_media_video_info_t;
typedef enum {
    BK_MEDIA_QUALITY_AUTO, BK_MEDIA_QUALITY_EXACT, BK_MEDIA_QUALITY_FINE,
    BK_MEDIA_QUALITY_FAST, BK_MEDIA_QUALITY_TURBO, BK_MEDIA_QUALITY_ULTRA,
    BK_MEDIA_QUALITY_COUNT
} bk_media_quality_t;
typedef struct {
    uint32_t rectangles, samples;
    uint8_t block_px;
} bk_media_render_stats_t;
/* format must be AVI or MPEG1 for disk streaming. */
bk_media_video_t *bk_media_video_open_path(const char *, uint32_t size, uint32_t format);
int bk_media_video_info(bk_media_video_t *, bk_media_video_info_t *);
bool bk_media_video_prepare(bk_media_video_t *, uint32_t frame);
void bk_media_video_invalidate(bk_media_video_t *);
uint32_t bk_media_video_frame_duration(bk_media_video_t *, uint32_t frame);
uint32_t bk_media_video_frame_time(bk_media_video_t *, uint32_t frame);
const char *bk_media_video_format_name(bk_media_video_t *);
const char *bk_media_quality_name(bk_media_quality_t);
bool bk_media_video_draw(bk_gui_surface_t *, bk_media_video_t *, bk_gui_rect_t,
                        bk_media_quality_t, bk_media_render_stats_t *);
void bk_media_audio_seek(bk_media_video_t *, uint32_t ms,
                        uint32_t *chunk, uint32_t *offset);
uint32_t bk_media_audio_decode(bk_media_video_t *, uint32_t *chunk,
                              uint32_t *offset, uint8_t *, uint32_t capacity);
#endif
