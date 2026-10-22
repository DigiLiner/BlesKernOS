#ifndef VIEWER_VIDEO_H
#define VIEWER_VIDEO_H

#include <bleskernos_api.h>

#define VV_MAX_WIDTH  1920U
#define VV_MAX_HEIGHT 1080U
#define VV_MAX_FRAMES 200000U
#define VV_FRAME_IMAGE_MAX_PIXELS (1024U * 768U)

typedef enum {
    VV_FORMAT_NONE = 0,
    VV_FORMAT_BKV1,
    VV_FORMAT_BKV2,
    VV_FORMAT_AVI,
    VV_FORMAT_GIF,
    VV_FORMAT_MPEG1
} vv_format_t;

typedef struct {
    uint32_t offset;
    uint32_t size;
} vv_avi_frame_t;

typedef struct {
    uint32_t offset;
    uint32_t size;
} vv_avi_audio_chunk_t;

typedef struct {
    uint32_t image_offset;
    uint16_t delay_cs;
    uint8_t disposal;
    int32_t transparent_index;
    uint16_t left, top, width, height;
} vv_gif_frame_t;

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint16_t width;
    uint16_t height;
    uint32_t frame_count;
    uint32_t fps_num;
    uint32_t fps_den;
    bool loop_default;
    vv_format_t format;
    uint32_t prepared_frame;
    bool frame_prepared;

    /* Optional full-color frame cache used by the public GUI image API. */
    bk_gui_image_t frame_image;
    uint32_t frame_image_frame;
    bool frame_image_valid;
    bool frame_image_owned;

    struct {
        uint16_t version;
        uint16_t flags;
        uint16_t codec;
        uint8_t block_w;
        uint8_t block_h;
        uint16_t key_interval;
        uint32_t index_offset;
        uint32_t data_offset;
        uint16_t *canvas;
        uint16_t *blocks;
        uint32_t block_cols;
        uint32_t block_rows;
        uint32_t block_count;
    } bkv;

    struct {
        vv_avi_frame_t *frames;
        uint32_t frame_capacity;
        uint16_t bpp;
        uint32_t compression;
        uint32_t handler;
        bool mjpeg;
        bool streamed;
        int file_fd;
        uint8_t *frame_payload;
        uint32_t frame_payload_capacity;
        uint32_t frame_payload_size;
        uint8_t *audio_cache;
        uint32_t audio_cache_start;
        uint32_t audio_cache_size;
        int32_t dib_height;
        uint32_t stride;
        uint32_t palette[256];
        uint16_t palette_count;
        uint32_t red_mask;
        uint32_t green_mask;
        uint32_t blue_mask;
        bk_gui_image_t mjpeg_image;
        bool mjpeg_image_valid;

        vv_avi_audio_chunk_t *audio_chunks;
        uint32_t audio_chunk_count;
        uint32_t audio_stream;
        uint32_t audio_data_bytes;
        uint32_t audio_sample_rate;
        uint32_t audio_bytes_per_sec;
        uint16_t audio_format;
        uint16_t audio_channels;
        uint16_t audio_bits;
        uint16_t audio_block_align;
        bool audio_supported;
    } avi;

    struct {
        void *video_decoder;
        void *audio_decoder;
        void *video_file;
        void *audio_file;
        const void *decoded_frame;
        uint32_t decoded_index;
        bool decoded_valid;
        uint16_t audio_sample_rate;
        uint16_t audio_source_rate;
        bool audio_supported;
        uint16_t audio_cache_pos;
        uint16_t audio_cache_count;
        uint8_t audio_cache[1152];
    } mpeg;

    struct {
        vv_gif_frame_t *frames;
        uint16_t *canvas;
        uint16_t *restore;
        uint8_t *indices;
        uint32_t pixel_count;
        uint16_t global_palette[256];
        uint16_t global_palette_count;
        uint16_t background_color;
        bool has_restore_disposal;
        uint16_t prefix[4096];
        uint8_t suffix[4096];
        uint8_t stack[4096];
    } gif;
} vv_video_t;

bool vv_open(vv_video_t *video, const void *data, uint32_t size);
/* MPEG-1 PS disk-streaming, with independent seekable video/audio cursors. */
bool vv_open_mpeg_stream(vv_video_t *video, const char *path, uint32_t size);
bool vv_open_avi_stream(vv_video_t *video, const char *path, uint32_t size);
void vv_close(vv_video_t *video);
bool vv_prepare_frame(vv_video_t *video, uint32_t frame);
uint16_t vv_sample_rgb565(const vv_video_t *video, uint32_t x, uint32_t y);
uint32_t vv_frame_duration_ms(const vv_video_t *video, uint32_t frame);
uint32_t vv_total_duration_ms(const vv_video_t *video);
uint32_t vv_frame_time_ms(const vv_video_t *video, uint32_t frame);
const char *vv_format_name(const vv_video_t *video);
bool vv_is_high_performance_native(const vv_video_t *video);
uint32_t vv_native_block_width(const vv_video_t *video);
uint32_t vv_native_block_height(const vv_video_t *video);
uint32_t vv_native_block_cols(const vv_video_t *video);
uint32_t vv_native_block_rows(const vv_video_t *video);
uint16_t vv_native_block_color(const vv_video_t *video, uint32_t block_index);

/* Returns a full-color XRGB frame when one can be produced economically.
 * The returned image is owned by vv_video_t and remains valid until the next
 * frame is prepared or vv_close() is called. */
const bk_gui_image_t *vv_frame_image(vv_video_t *video);

/* Audio helper. AVI PCM and MPEG-1 Layer II are converted to unsigned
 * 8-bit mono for BlesKernOS. MPEG Layer II at 48 kHz is downsampled to
 * 24 kHz so it also works through the SB16 backend (44.1 kHz maximum). */
bool vv_audio_available(const vv_video_t *video);
uint16_t vv_audio_sample_rate(const vv_video_t *video);
void vv_audio_seek_ms(vv_video_t *video, uint32_t time_ms,
                      uint32_t *chunk_index, uint32_t *chunk_offset);
uint32_t vv_audio_decode_u8_mono(vv_video_t *video,
                                 uint32_t *chunk_index,
                                 uint32_t *chunk_offset,
                                 uint8_t *output, uint32_t capacity);

#endif
