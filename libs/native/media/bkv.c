#include "video_internal.h"

#define BKV1_HEADER_SIZE 40U
#define BKV2_HEADER_SIZE 48U
#define BKV_FLAG_LOOP 0x0001U
#define BKV2_CODEC_TILE_DELTA 1U
#define BKV2_FRAME_DELTA 0U
#define BKV2_FRAME_KEY   1U

static bool bkv1_open(vv_video_t *video) {
    const uint8_t *b = video->data;
    uint16_t header_size;
    uint32_t frame_count;
    uint32_t index_offset;
    uint32_t data_offset;
    uint32_t declared_size;
    uint32_t pixels;
    uint32_t i;

    if (video->size < BKV1_HEADER_SIZE) return false;
    header_size = vv_rd16(b + 4);
    video->width = vv_rd16(b + 6);
    video->height = vv_rd16(b + 8);
    video->fps_num = vv_rd16(b + 10);
    video->fps_den = vv_rd16(b + 12);
    video->bkv.flags = vv_rd16(b + 14);
    frame_count = vv_rd32(b + 16);
    index_offset = vv_rd32(b + 20);
    data_offset = vv_rd32(b + 24);
    declared_size = vv_rd32(b + 32);

    if (header_size < BKV1_HEADER_SIZE || header_size > video->size ||
        !video->width || !video->height || video->width > VV_MAX_WIDTH ||
        video->height > VV_MAX_HEIGHT || !video->fps_num || !video->fps_den ||
        video->fps_num > 240U || !frame_count || frame_count > VV_MAX_FRAMES)
        return false;
    if (declared_size && declared_size != video->size) return false;
    if (!vv_range_ok(index_offset, frame_count * 4U, video->size) ||
        data_offset < header_size || data_offset > video->size) return false;

    pixels = (uint32_t)video->width * video->height;
    if (!pixels || pixels > (uint32_t)VV_MAX_WIDTH * VV_MAX_HEIGHT) return false;

    for (i = 0; i < frame_count; i++) {
        uint32_t off = vv_rd32(b + index_offset + i * 4U);
        uint32_t payload;
        if (!vv_range_ok(off, 4U, video->size) || off < data_offset) return false;
        payload = vv_rd32(b + off);
        if (!payload || (payload & 3U) ||
            !vv_range_ok(off + 4U, payload, video->size)) return false;
    }

    video->bkv.canvas = (uint16_t *)bk_sys_alloc(pixels * 2U);
    if (!video->bkv.canvas) return false;
    video->format = VV_FORMAT_BKV1;
    video->frame_count = frame_count;
    video->loop_default = (video->bkv.flags & BKV_FLAG_LOOP) != 0U;
    video->bkv.version = 1U;
    video->bkv.index_offset = index_offset;
    video->bkv.data_offset = data_offset;
    return true;
}

static bool bkv2_frame_info(const vv_video_t *video, uint32_t frame,
                            uint32_t *offset, uint8_t *type,
                            uint16_t *count) {
    uint32_t off;
    uint32_t payload;
    if (!video || frame >= video->frame_count) return false;
    off = vv_rd32(video->data + video->bkv.index_offset + frame * 4U);
    if (!vv_range_ok(off, 8U, video->size)) return false;
    payload = vv_rd32(video->data + off);
    if (payload < 4U || !vv_range_ok(off + 4U, payload, video->size)) return false;
    if (offset) *offset = off;
    if (type) *type = video->data[off + 4U];
    if (count) *count = vv_rd16(video->data + off + 6U);
    return true;
}

static bool bkv2_open(vv_video_t *video) {
    const uint8_t *b = video->data;
    uint16_t header_size;
    uint16_t codec;
    uint8_t block_w, block_h;
    uint32_t frame_count, index_offset, data_offset, declared_size;
    uint32_t cols, rows, blocks;
    uint32_t i;

    if (video->size < BKV2_HEADER_SIZE) return false;
    header_size = vv_rd16(b + 4);
    video->width = vv_rd16(b + 6);
    video->height = vv_rd16(b + 8);
    video->fps_num = vv_rd16(b + 10);
    video->fps_den = vv_rd16(b + 12);
    video->bkv.flags = vv_rd16(b + 14);
    codec = vv_rd16(b + 16);
    block_w = b[18];
    block_h = b[19];
    video->bkv.key_interval = vv_rd16(b + 20);
    frame_count = vv_rd32(b + 24);
    index_offset = vv_rd32(b + 28);
    data_offset = vv_rd32(b + 32);
    declared_size = vv_rd32(b + 36);

    if (header_size < BKV2_HEADER_SIZE || header_size > video->size ||
        !video->width || !video->height || video->width > VV_MAX_WIDTH ||
        video->height > VV_MAX_HEIGHT || !video->fps_num || !video->fps_den ||
        video->fps_num > 240U || !frame_count || frame_count > VV_MAX_FRAMES ||
        codec != BKV2_CODEC_TILE_DELTA ||
        (block_w != 4U && block_w != 8U && block_w != 16U && block_w != 32U) ||
        (block_h != 4U && block_h != 8U && block_h != 16U && block_h != 32U))
        return false;
    if (declared_size && declared_size != video->size) return false;
    if (!vv_range_ok(index_offset, frame_count * 4U, video->size) ||
        data_offset < header_size || data_offset > video->size) return false;

    cols = ((uint32_t)video->width + block_w - 1U) / block_w;
    rows = ((uint32_t)video->height + block_h - 1U) / block_h;
    blocks = cols * rows;
    if (!blocks || blocks > 65535U) return false;

    video->format = VV_FORMAT_BKV2;
    video->frame_count = frame_count;
    video->loop_default = (video->bkv.flags & BKV_FLAG_LOOP) != 0U;
    video->bkv.version = 2U;
    video->bkv.codec = codec;
    video->bkv.block_w = block_w;
    video->bkv.block_h = block_h;
    video->bkv.block_cols = cols;
    video->bkv.block_rows = rows;
    video->bkv.block_count = blocks;
    video->bkv.index_offset = index_offset;
    video->bkv.data_offset = data_offset;
    video->bkv.blocks = (uint16_t *)bk_sys_alloc(blocks * 2U);
    if (!video->bkv.blocks) return false;
    vv_memzero(video->bkv.blocks, blocks * 2U);

    for (i = 0; i < frame_count; i++) {
        uint32_t off;
        uint8_t type;
        uint16_t count;
        uint32_t payload;
        uint32_t r;
        if (!bkv2_frame_info(video, i, &off, &type, &count)) return false;
        payload = vv_rd32(b + off);
        if (type != BKV2_FRAME_DELTA && type != BKV2_FRAME_KEY) return false;
        if (i == 0U && type != BKV2_FRAME_KEY) return false;
        if (type == BKV2_FRAME_KEY && count != blocks) return false;
        if ((uint32_t)count * 4U + 4U != payload) return false;
        for (r = 0; r < count; r++) {
            uint16_t index = vv_rd16(b + off + 8U + r * 4U);
            if ((uint32_t)index >= blocks) return false;
        }
    }
    return true;
}

bool vv_bkv_open(vv_video_t *video) {
    if (!video || !video->data || video->size < 4U) return false;
    if (video->data[3] == '1') return bkv1_open(video);
    if (video->data[3] == '2') return bkv2_open(video);
    return false;
}

void vv_bkv_close(vv_video_t *video) {
    if (!video) return;
    if (video->bkv.canvas) bk_sys_free(video->bkv.canvas);
    if (video->bkv.blocks) bk_sys_free(video->bkv.blocks);
    video->bkv.canvas = NULL;
    video->bkv.blocks = NULL;
}

static bool bkv1_prepare(vv_video_t *video, uint32_t frame) {
    uint32_t off = vv_rd32(video->data + video->bkv.index_offset + frame * 4U);
    uint32_t payload = vv_rd32(video->data + off);
    const uint8_t *p = video->data + off + 4U;
    const uint8_t *end = p + payload;
    uint32_t pixels = (uint32_t)video->width * video->height;
    uint32_t pos = 0;
    while (p < end) {
        uint16_t run, color;
        uint32_t j;
        if ((uint32_t)(end - p) < 4U) return false;
        run = vv_rd16(p);
        color = vv_rd16(p + 2U);
        p += 4U;
        if (!run || (uint32_t)run > pixels - pos) return false;
        for (j = 0; j < run; j++) video->bkv.canvas[pos + j] = color;
        pos += run;
    }
    return pos == pixels;
}

static bool bkv2_apply(vv_video_t *video, uint32_t frame) {
    uint32_t off;
    uint8_t type;
    uint16_t count;
    uint32_t i;
    if (!bkv2_frame_info(video, frame, &off, &type, &count)) return false;
    if (type == BKV2_FRAME_KEY)
        vv_memzero(video->bkv.blocks, video->bkv.block_count * 2U);
    for (i = 0; i < count; i++) {
        uint16_t index = vv_rd16(video->data + off + 8U + i * 4U);
        uint16_t color = vv_rd16(video->data + off + 10U + i * 4U);
        if ((uint32_t)index >= video->bkv.block_count) return false;
        video->bkv.blocks[index] = color;
    }
    return true;
}

static bool bkv2_prepare(vv_video_t *video, uint32_t frame) {
    uint32_t start;
    uint32_t i;
    if (video->frame_prepared && frame == video->prepared_frame + 1U) {
        return bkv2_apply(video, frame);
    }
    start = frame;
    while (start > 0U) {
        uint8_t type;
        if (!bkv2_frame_info(video, start, NULL, &type, NULL)) return false;
        if (type == BKV2_FRAME_KEY) break;
        start--;
    }
    for (i = start; i <= frame; i++) {
        if (!bkv2_apply(video, i)) return false;
    }
    return true;
}

bool vv_bkv_prepare(vv_video_t *video, uint32_t frame) {
    if (!video || frame >= video->frame_count) return false;
    if (video->format == VV_FORMAT_BKV1) return bkv1_prepare(video, frame);
    if (video->format == VV_FORMAT_BKV2) return bkv2_prepare(video, frame);
    return false;
}

uint16_t vv_bkv_sample(const vv_video_t *video, uint32_t x, uint32_t y) {
    if (!video || x >= video->width || y >= video->height) return 0;
    if (video->format == VV_FORMAT_BKV1) {
        if (!video->bkv.canvas) return 0;
        return video->bkv.canvas[y * (uint32_t)video->width + x];
    }
    if (video->format == VV_FORMAT_BKV2) {
        uint32_t bx, by, index;
        if (!video->bkv.blocks) return 0;
        bx = x / video->bkv.block_w;
        by = y / video->bkv.block_h;
        index = by * video->bkv.block_cols + bx;
        if (index >= video->bkv.block_count) return 0;
        return video->bkv.blocks[index];
    }
    return 0;
}
