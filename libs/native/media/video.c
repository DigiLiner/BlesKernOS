#include "video_internal.h"

uint16_t vv_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t vv_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int32_t vv_rd32s(const uint8_t *p) {
    return (int32_t)vv_rd32(p);
}

bool vv_range_ok(uint32_t offset, uint32_t length, uint32_t size) {
    return offset <= size && length <= size - offset;
}

uint16_t vv_xrgb_to_rgb565(uint32_t color) {
    uint32_t r = (color >> 16) & 0xFFU;
    uint32_t g = (color >> 8) & 0xFFU;
    uint32_t b = color & 0xFFU;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint32_t vv_rgb565_to_xrgb(uint16_t color) {
    uint32_t r = (uint32_t)((color >> 11) & 0x1FU);
    uint32_t g = (uint32_t)((color >> 5) & 0x3FU);
    uint32_t b = (uint32_t)(color & 0x1FU);
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xFF000000U | (r << 16) | (g << 8) | b;
}

uint32_t vv_muldiv_u32_sat(uint32_t a, uint32_t b, uint32_t divisor) {
    uint32_t a0, a1, b0, b1, p0, p1, p2, p3, middle;
    uint32_t hi, lo, q_hi = 0U, q_lo = 0U, rem = 0U;
    int bit;
    if (!divisor) return 0xFFFFFFFFU;
    /* 32x32 -> 64 using 16-bit limbs, then restoring divide by 32-bit.
     * All current callers use small divisors (<= 1,000,000), so rem<<1
     * cannot overflow before the subtraction step. */
    a0 = a & 0xFFFFU; a1 = a >> 16;
    b0 = b & 0xFFFFU; b1 = b >> 16;
    p0 = a0 * b0; p1 = a0 * b1; p2 = a1 * b0; p3 = a1 * b1;
    middle = (p0 >> 16) + (p1 & 0xFFFFU) + (p2 & 0xFFFFU);
    lo = (p0 & 0xFFFFU) | (middle << 16);
    hi = p3 + (p1 >> 16) + (p2 >> 16) + (middle >> 16);
    for (bit = 63; bit >= 0; --bit) {
        uint32_t in = bit >= 32 ? ((hi >> (bit - 32)) & 1U)
                                : ((lo >> bit) & 1U);
        rem = (rem << 1) | in;
        if (rem >= divisor) {
            rem -= divisor;
            if (bit >= 32) q_hi |= 1U << (bit - 32);
            else q_lo |= 1U << bit;
        }
    }
    return q_hi ? 0xFFFFFFFFU : q_lo;
}

uint16_t vv_bgr_to_rgb565(uint8_t b, uint8_t g, uint8_t r) {
    return (uint16_t)((((uint16_t)r >> 3) << 11) |
                      (((uint16_t)g >> 2) << 5) |
                      ((uint16_t)b >> 3));
}

void vv_memzero(void *pointer, uint32_t size) {
    uint8_t *p = (uint8_t *)pointer;
    while (p && size--) *p++ = 0;
}

void vv_memcpy(void *dst, const void *src, uint32_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (d && s && size--) *d++ = *s++;
}

bool vv_open(vv_video_t *video, const void *data, uint32_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    if (!video || !bytes || size < 6U) return false;
    vv_memzero(video, sizeof(*video));
    video->data = bytes;
    video->size = size;

    if (size >= 4U && bytes[0] == 'B' && bytes[1] == 'K' &&
        bytes[2] == 'V' && (bytes[3] == '1' || bytes[3] == '2')) {
        if (!vv_bkv_open(video)) goto fail;
    } else if (size >= 12U && bytes[0] == 'R' && bytes[1] == 'I' &&
               bytes[2] == 'F' && bytes[3] == 'F' &&
               bytes[8] == 'A' && bytes[9] == 'V' &&
               bytes[10] == 'I' && bytes[11] == ' ') {
        if (!vv_avi_open(video)) goto fail;
    } else if (bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' &&
               bytes[3] == '8' && (bytes[4] == '7' || bytes[4] == '9') &&
               bytes[5] == 'a') {
        if (!vv_gif_open(video)) goto fail;
    } else if (size >= 4U && bytes[0] == 0x00U && bytes[1] == 0x00U &&
               bytes[2] == 0x01U && bytes[3] == 0xBAU) {
        if (!vv_mpeg_open(video)) goto fail;
    } else {
        goto fail;
    }
    video->prepared_frame = 0;
    video->frame_prepared = false;
    video->frame_image_frame = 0;
    video->frame_image_valid = false;
    return true;

fail:
    vv_close(video);
    return false;
}

bool vv_open_mpeg_stream(vv_video_t *video, const char *path, uint32_t size) {
    int fd;
    uint8_t magic[5];
    if (!video || !path || size < 16U || size > 0x7FFFFFFFU) return false;
    fd = bk_file_open(path, BK_FILE_READ_ONLY);
    if (fd < 0) return false;
    if (bk_file_read(fd, magic, sizeof(magic)) != (int)sizeof(magic)) {
        (void)bk_file_close(fd);
        return false;
    }
    (void)bk_file_close(fd);
    /* pl_mpeg decodes MPEG-1 video in MPEG-1 PS only, not DVD MPEG-2. */
    if (magic[0] != 0 || magic[1] != 0 || magic[2] != 1 ||
        magic[3] != 0xBA || (magic[4] & 0xF0U) != 0x20U) return false;
    vv_memzero(video, sizeof(*video));
    video->size = size;
    if (!vv_mpeg_open_stream(video, path)) {
        vv_close(video);
        return false;
    }
    return true;
}

bool vv_open_avi_stream(vv_video_t *video, const char *path, uint32_t size) {
    if (!video || !path || size < 12U || size > 0x7FFFFFFFU) return false;
    vv_memzero(video, sizeof(*video));
    if (!vv_avi_open_stream(video, path, size)) {
        vv_close(video);
        return false;
    }
    return true;
}

void vv_close(vv_video_t *video) {
    if (!video) return;
    if (video->frame_image_owned && video->frame_image.pixels)
        bk_sys_free(video->frame_image.pixels);
    video->frame_image.pixels = NULL;
    video->frame_image.width = 0;
    video->frame_image.height = 0;
    video->frame_image_owned = false;
    video->frame_image_valid = false;

    if (video->format == VV_FORMAT_BKV1 || video->format == VV_FORMAT_BKV2)
        vv_bkv_close(video);
    else if (video->format == VV_FORMAT_AVI)
        vv_avi_close(video);
    else if (video->format == VV_FORMAT_GIF)
        vv_gif_close(video);
    else if (video->format == VV_FORMAT_MPEG1)
        vv_mpeg_close(video);
    video->format = VV_FORMAT_NONE;
}

bool vv_prepare_frame(vv_video_t *video, uint32_t frame) {
    bool ok = false;
    if (!video || frame >= video->frame_count) return false;
    if (video->frame_prepared && video->prepared_frame == frame) return true;
    if (video->format == VV_FORMAT_BKV1 || video->format == VV_FORMAT_BKV2)
        ok = vv_bkv_prepare(video, frame);
    else if (video->format == VV_FORMAT_AVI)
        ok = vv_avi_prepare(video, frame);
    else if (video->format == VV_FORMAT_GIF)
        ok = vv_gif_prepare(video, frame);
    else if (video->format == VV_FORMAT_MPEG1)
        ok = vv_mpeg_prepare(video, frame);
    if (ok) {
        video->prepared_frame = frame;
        video->frame_prepared = true;
        video->frame_image_valid = false;
    }
    return ok;
}

uint16_t vv_sample_rgb565(const vv_video_t *video, uint32_t x, uint32_t y) {
    if (!video || x >= video->width || y >= video->height) return 0;
    if (video->format == VV_FORMAT_BKV1 || video->format == VV_FORMAT_BKV2)
        return vv_bkv_sample(video, x, y);
    if (video->format == VV_FORMAT_AVI) return vv_avi_sample(video, x, y);
    if (video->format == VV_FORMAT_GIF) return vv_gif_sample(video, x, y);
    if (video->format == VV_FORMAT_MPEG1) return vv_mpeg_sample(video, x, y);
    return 0;
}

uint32_t vv_frame_duration_ms(const vv_video_t *video, uint32_t frame) {
    uint32_t value;
    if (!video) return 100U;
    if (video->format == VV_FORMAT_GIF) return vv_gif_duration_ms(video, frame);
    if (!video->fps_num || !video->fps_den) return 100U;
    value = 1000U * video->fps_den + video->fps_num / 2U;
    value /= video->fps_num;
    if (!value) value = 1;
    if (value > 10000U) value = 10000U;
    return (uint32_t)value;
}

uint32_t vv_frame_time_ms(const vv_video_t *video, uint32_t frame) {
    uint32_t total = 0U;
    uint32_t i;
    if (!video || !video->frame_count) return 0U;
    if (frame > video->frame_count) frame = video->frame_count;
    if (video->format != VV_FORMAT_GIF && video->fps_num && video->fps_den) {
        uint32_t factor;
        if (video->fps_den > 0xFFFFFFFFU / 1000U) return 0xFFFFFFFFU;
        factor = video->fps_den * 1000U;
        return vv_muldiv_u32_sat(frame, factor, video->fps_num);
    }
    for (i = 0U; i < frame; ++i) {
        uint32_t add = vv_frame_duration_ms(video, i);
        if (total > 0xFFFFFFFFU - add) return 0xFFFFFFFFU;
        total += add;
    }
    return total;
}

uint32_t vv_total_duration_ms(const vv_video_t *video) {
    uint32_t total = 0U;
    uint32_t i;
    if (!video || !video->frame_count) return 0U;
    if (video->format != VV_FORMAT_GIF && video->fps_num && video->fps_den) {
        uint32_t factor;
        if (video->fps_den > 0xFFFFFFFFU / 1000U) return 0xFFFFFFFFU;
        factor = video->fps_den * 1000U;
        return vv_muldiv_u32_sat(video->frame_count, factor, video->fps_num);
    }
    for (i = 0; i < video->frame_count; ++i) {
        uint32_t add = vv_frame_duration_ms(video, i);
        if (total > 0xFFFFFFFFU - add) return 0xFFFFFFFFU;
        total += add;
    }
    return total;
}

const char *vv_format_name(const vv_video_t *video) {
    if (!video) return "Unknown";
    switch (video->format) {
        case VV_FORMAT_BKV1: return "BKV1/RLE";
        case VV_FORMAT_BKV2: return "BKV2/TileDelta";
        case VV_FORMAT_AVI:
            return video->avi.mjpeg ? "AVI/Motion-JPEG" : "AVI/DIB";
        case VV_FORMAT_GIF:  return "GIF89a";
        case VV_FORMAT_MPEG1: return video->mpeg.audio_supported
            ? "MPEG-1/MP2" : "MPEG-1 Video";
        default: return "Unknown";
    }
}

bool vv_is_high_performance_native(const vv_video_t *video) {
    return video && video->format == VV_FORMAT_BKV2 && video->bkv.codec == 1U &&
           video->bkv.blocks != NULL;
}

uint32_t vv_native_block_width(const vv_video_t *video) {
    return vv_is_high_performance_native(video) ? video->bkv.block_w : 0U;
}
uint32_t vv_native_block_height(const vv_video_t *video) {
    return vv_is_high_performance_native(video) ? video->bkv.block_h : 0U;
}
uint32_t vv_native_block_cols(const vv_video_t *video) {
    return vv_is_high_performance_native(video) ? video->bkv.block_cols : 0U;
}
uint32_t vv_native_block_rows(const vv_video_t *video) {
    return vv_is_high_performance_native(video) ? video->bkv.block_rows : 0U;
}
uint16_t vv_native_block_color(const vv_video_t *video, uint32_t block_index) {
    if (!vv_is_high_performance_native(video) ||
        block_index >= video->bkv.block_count) return 0;
    return video->bkv.blocks[block_index];
}

const bk_gui_image_t *vv_frame_image(vv_video_t *video) {
    uint32_t pixels;
    uint32_t x, y;
    if (!video || !video->frame_prepared || !video->width || !video->height)
        return NULL;

    if (video->format == VV_FORMAT_AVI && video->avi.mjpeg) {
        return video->avi.mjpeg_image_valid ? &video->avi.mjpeg_image : NULL;
    }
    if (video->format == VV_FORMAT_MPEG1)
        return vv_mpeg_frame_image(video);

    /* BKV2 already has a faster native tile renderer. */
    if (vv_is_high_performance_native(video)) return NULL;
    if (video->format != VV_FORMAT_AVI && video->format != VV_FORMAT_GIF)
        return NULL;

    pixels = (uint32_t)video->width * (uint32_t)video->height;
    if (!pixels || pixels > VV_FRAME_IMAGE_MAX_PIXELS) return NULL;
    if (!video->frame_image.pixels) {
        video->frame_image.pixels = (uint32_t *)bk_sys_alloc(pixels * 4U);
        if (!video->frame_image.pixels) return NULL;
        video->frame_image.width = video->width;
        video->frame_image.height = video->height;
        video->frame_image_owned = true;
    }
    if (video->frame_image_valid &&
        video->frame_image_frame == video->prepared_frame)
        return &video->frame_image;

    for (y = 0; y < video->height; ++y) {
        for (x = 0; x < video->width; ++x) {
            uint16_t c = vv_sample_rgb565(video, x, y);
            video->frame_image.pixels[y * video->width + x] =
                vv_rgb565_to_xrgb(c);
        }
    }
    video->frame_image_frame = video->prepared_frame;
    video->frame_image_valid = true;
    return &video->frame_image;
}

bool vv_audio_available(const vv_video_t *video) {
    if (!video) return false;
    if (video->format == VV_FORMAT_AVI)
        return video->avi.audio_supported && video->avi.audio_chunk_count > 0U;
    if (video->format == VV_FORMAT_MPEG1)
        return vv_mpeg_audio_available(video);
    return false;
}

uint16_t vv_audio_sample_rate(const vv_video_t *video) {
    if (!vv_audio_available(video)) return 0U;
    if (video->format == VV_FORMAT_AVI) {
        if (video->avi.audio_sample_rate > 65535U) return 0U;
        return (uint16_t)video->avi.audio_sample_rate;
    }
    if (video->format == VV_FORMAT_MPEG1)
        return video->mpeg.audio_sample_rate;
    return 0U;
}

void vv_audio_seek_ms(vv_video_t *video, uint32_t time_ms,
                      uint32_t *chunk_index, uint32_t *chunk_offset) {
    if (!video) return;
    if (video->format == VV_FORMAT_AVI) {
        vv_avi_audio_seek_ms(video, time_ms, chunk_index, chunk_offset);
        return;
    }
    if (chunk_index) *chunk_index = 0U;
    if (chunk_offset) *chunk_offset = 0U;
    if (video->format == VV_FORMAT_MPEG1)
        vv_mpeg_audio_seek_ms(video, time_ms);
}

uint32_t vv_audio_decode_u8_mono(vv_video_t *video,
                                 uint32_t *chunk_index,
                                 uint32_t *chunk_offset,
                                 uint8_t *output, uint32_t capacity) {
    if (!video) return 0U;
    if (video->format == VV_FORMAT_AVI)
        return vv_avi_audio_decode_u8_mono(video, chunk_index, chunk_offset,
                                           output, capacity);
    if (video->format == VV_FORMAT_MPEG1)
        return vv_mpeg_audio_decode_u8_mono(video, output, capacity);
    return 0U;
}
