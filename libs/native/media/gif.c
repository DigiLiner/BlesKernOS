#include "video_internal.h"

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
    uint32_t block_remaining;
    bool ended;
    uint32_t bit_buffer;
    uint8_t bit_count;
} gif_stream_t;

static bool gif_skip_subblocks(const uint8_t *data, uint32_t size, uint32_t *pos) {
    uint32_t p = *pos;
    while (p < size) {
        uint8_t n = data[p++];
        if (!n) { *pos = p; return true; }
        if (!vv_range_ok(p, n, size)) return false;
        p += n;
    }
    return false;
}

static uint16_t gif_color565(const uint8_t *p) {
    return vv_bgr_to_rgb565(p[2], p[1], p[0]);
}

static bool gif_read_palette(const uint8_t *data, uint32_t size, uint32_t pos,
                             uint16_t *palette, uint32_t count) {
    uint32_t i;
    if (!palette || count > 256U || !vv_range_ok(pos, count * 3U, size)) return false;
    for (i = 0; i < count; i++) palette[i] = gif_color565(data + pos + i * 3U);
    return true;
}

static bool gif_scan(vv_video_t *video, vv_gif_frame_t *frames,
                     uint32_t capacity, uint32_t *frame_count,
                     bool *loop_default, bool *has_restore) {
    const uint8_t *data = video->data;
    uint32_t size = video->size;
    uint32_t pos;
    uint8_t packed;
    uint32_t gct_count = 0;
    uint16_t pending_delay = 10U;
    uint8_t pending_disposal = 0U;
    int32_t pending_transparent = -1;

    if (size < 13U) return false;
    packed = data[10U];
    pos = 13U;
    if (packed & 0x80U) {
        gct_count = 1U << ((packed & 7U) + 1U);
        if (!vv_range_ok(pos, gct_count * 3U, size)) return false;
        pos += gct_count * 3U;
    }

    *frame_count = 0U;
    *loop_default = false;
    *has_restore = false;

    while (pos < size) {
        uint8_t marker = data[pos++];
        if (marker == 0x3BU) return *frame_count > 0U;
        if (marker == 0x21U) {
            uint8_t label;
            if (pos >= size) return false;
            label = data[pos++];
            if (label == 0xF9U) {
                uint8_t n, gce_packed;
                if (pos >= size) return false;
                n = data[pos++];
                if (n != 4U || !vv_range_ok(pos, 4U, size)) return false;
                gce_packed = data[pos];
                pending_disposal = (uint8_t)((gce_packed >> 2) & 7U);
                pending_delay = vv_rd16(data + pos + 1U);
                if (!pending_delay) pending_delay = 10U;
                pending_transparent = (gce_packed & 1U) ? (int32_t)data[pos + 3U] : -1;
                pos += 4U;
                if (pos >= size || data[pos++] != 0U) return false;
            } else if (label == 0xFFU) {
                uint8_t n;
                if (pos >= size) return false;
                n = data[pos++];
                if (!vv_range_ok(pos, n, size)) return false;
                if (n == 11U && pos + 11U <= size &&
                    data[pos] == 'N' && data[pos+1] == 'E' && data[pos+2] == 'T' &&
                    data[pos+3] == 'S' && data[pos+4] == 'C' && data[pos+5] == 'A' &&
                    data[pos+6] == 'P' && data[pos+7] == 'E' && data[pos+8] == '2' &&
                    data[pos+9] == '.' && data[pos+10] == '0') {
                    uint32_t q = pos + n;
                    if (q + 5U <= size && data[q] == 3U && data[q+1] == 1U) {
                        uint16_t loops = vv_rd16(data + q + 2U);
                        if (loops == 0U || loops > 1U) *loop_default = true;
                    }
                }
                pos += n;
                if (!gif_skip_subblocks(data, size, &pos)) return false;
            } else {
                if (!gif_skip_subblocks(data, size, &pos)) return false;
            }
            continue;
        }
        if (marker == 0x2CU) {
            uint32_t image_off = pos - 1U;
            uint16_t left, top, width, height;
            uint8_t ipacked;
            uint32_t lct_count = 0;
            if (!vv_range_ok(pos, 9U, size)) return false;
            left = vv_rd16(data + pos);
            top = vv_rd16(data + pos + 2U);
            width = vv_rd16(data + pos + 4U);
            height = vv_rd16(data + pos + 6U);
            ipacked = data[pos + 8U];
            pos += 9U;
            if (!width || !height || (uint32_t)left + width > video->width ||
                (uint32_t)top + height > video->height) return false;
            if (ipacked & 0x80U) {
                lct_count = 1U << ((ipacked & 7U) + 1U);
                if (!vv_range_ok(pos, lct_count * 3U, size)) return false;
                pos += lct_count * 3U;
            }
            if (pos >= size) return false;
            pos++; /* LZW minimum code size */
            if (!gif_skip_subblocks(data, size, &pos)) return false;
            if (frames && *frame_count < capacity) {
                frames[*frame_count].image_offset = image_off;
                frames[*frame_count].delay_cs = pending_delay;
                frames[*frame_count].disposal = pending_disposal;
                frames[*frame_count].transparent_index = pending_transparent;
                frames[*frame_count].left = left;
                frames[*frame_count].top = top;
                frames[*frame_count].width = width;
                frames[*frame_count].height = height;
            }
            if (pending_disposal == 3U) *has_restore = true;
            (*frame_count)++;
            if (*frame_count > VV_MAX_FRAMES) return false;
            pending_delay = 10U;
            pending_disposal = 0U;
            pending_transparent = -1;
            continue;
        }
        return false;
    }
    return false;
}

static void gif_stream_init(gif_stream_t *s, const uint8_t *data,
                            uint32_t size, uint32_t pos) {
    s->data = data; s->size = size; s->pos = pos;
    s->block_remaining = 0U; s->ended = false;
    s->bit_buffer = 0U; s->bit_count = 0U;
}

static bool gif_stream_byte(gif_stream_t *s, uint8_t *out) {
    if (s->ended) return false;
    if (!s->block_remaining) {
        uint8_t n;
        if (s->pos >= s->size) return false;
        n = s->data[s->pos++];
        if (!n) { s->ended = true; return false; }
        if (!vv_range_ok(s->pos, n, s->size)) return false;
        s->block_remaining = n;
    }
    *out = s->data[s->pos++];
    s->block_remaining--;
    return true;
}

static bool gif_stream_code(gif_stream_t *s, uint8_t code_bits, uint16_t *code) {
    uint8_t b;
    while (s->bit_count < code_bits) {
        if (!gif_stream_byte(s, &b)) return false;
        s->bit_buffer |= (uint32_t)b << s->bit_count;
        s->bit_count = (uint8_t)(s->bit_count + 8U);
    }
    *code = (uint16_t)(s->bit_buffer & ((1U << code_bits) - 1U));
    s->bit_buffer >>= code_bits;
    s->bit_count = (uint8_t)(s->bit_count - code_bits);
    return true;
}

typedef struct {
    vv_video_t *video;
    const vv_gif_frame_t *meta;
    const uint16_t *palette;
    uint32_t palette_count;
    uint32_t out_count;
    uint16_t x;
    uint16_t y;
    uint8_t interlace_pass;
    bool interlaced;
} gif_output_t;

static void gif_output_advance_row(gif_output_t *o) {
    if (!o->interlaced) { o->y++; return; }
    if (o->interlace_pass == 0U) {
        o->y = (uint16_t)(o->y + 8U);
        if (o->y >= o->meta->height) { o->interlace_pass = 1U; o->y = 4U; }
    } else if (o->interlace_pass == 1U) {
        o->y = (uint16_t)(o->y + 8U);
        if (o->y >= o->meta->height) { o->interlace_pass = 2U; o->y = 2U; }
    } else if (o->interlace_pass == 2U) {
        o->y = (uint16_t)(o->y + 4U);
        if (o->y >= o->meta->height) { o->interlace_pass = 3U; o->y = 1U; }
    } else {
        o->y = (uint16_t)(o->y + 2U);
    }
}

static bool gif_output_index(gif_output_t *o, uint8_t index) {
    uint32_t dx, dy;
    if (o->out_count >= (uint32_t)o->meta->width * o->meta->height) return false;
    if (o->y >= o->meta->height) return false;
    dx = (uint32_t)o->meta->left + o->x;
    dy = (uint32_t)o->meta->top + o->y;
    if ((int32_t)index != o->meta->transparent_index && index < o->palette_count)
        o->video->gif.canvas[dy * o->video->width + dx] = o->palette[index];
    o->out_count++;
    o->x++;
    if (o->x >= o->meta->width) {
        o->x = 0U;
        gif_output_advance_row(o);
    }
    return true;
}

static bool gif_decode_image(vv_video_t *video, const vv_gif_frame_t *meta) {
    const uint8_t *data = video->data;
    uint32_t size = video->size;
    uint32_t pos = meta->image_offset + 1U;
    uint8_t packed;
    uint16_t local_palette[256];
    const uint16_t *palette = video->gif.global_palette;
    uint32_t palette_count = video->gif.global_palette_count;
    uint32_t lct_count = 0;
    uint8_t min_code_size;
    uint16_t clear_code, end_code, next_code, old_code = 0, code, in_code;
    uint8_t code_size;
    uint8_t first = 0;
    bool have_old = false;
    gif_stream_t stream;
    gif_output_t out;

    if (!vv_range_ok(pos, 9U, size)) return false;
    packed = data[pos + 8U];
    pos += 9U;
    if (packed & 0x80U) {
        lct_count = 1U << ((packed & 7U) + 1U);
        if (!gif_read_palette(data, size, pos, local_palette, lct_count)) return false;
        palette = local_palette;
        palette_count = lct_count;
        pos += lct_count * 3U;
    }
    if (!palette_count || pos >= size) return false;
    min_code_size = data[pos++];
    if (min_code_size < 2U || min_code_size > 8U) return false;
    clear_code = (uint16_t)(1U << min_code_size);
    end_code = (uint16_t)(clear_code + 1U);
    next_code = (uint16_t)(end_code + 1U);
    code_size = (uint8_t)(min_code_size + 1U);

    gif_stream_init(&stream, data, size, pos);
    out.video = video; out.meta = meta; out.palette = palette;
    out.palette_count = palette_count; out.out_count = 0U;
    out.x = 0U; out.y = 0U; out.interlace_pass = 0U;
    out.interlaced = (packed & 0x40U) != 0U;

    while (gif_stream_code(&stream, code_size, &code)) {
        uint32_t stack_count = 0U;
        if (code == clear_code) {
            next_code = (uint16_t)(end_code + 1U);
            code_size = (uint8_t)(min_code_size + 1U);
            have_old = false;
            continue;
        }
        if (code == end_code) break;
        if (!have_old) {
            if (code >= clear_code || !gif_output_index(&out, (uint8_t)code)) return false;
            first = (uint8_t)code;
            old_code = code;
            have_old = true;
            continue;
        }

        in_code = code;
        if (code >= next_code) {
            if (code != next_code || stack_count >= 4096U) return false;
            video->gif.stack[stack_count++] = first;
            code = old_code;
        }
        while (code >= clear_code) {
            if (code >= next_code || stack_count >= 4096U) return false;
            video->gif.stack[stack_count++] = video->gif.suffix[code];
            code = video->gif.prefix[code];
        }
        first = (uint8_t)code;
        if (stack_count >= 4096U) return false;
        video->gif.stack[stack_count++] = first;
        while (stack_count) {
            if (!gif_output_index(&out, video->gif.stack[--stack_count])) return false;
        }
        if (next_code < 4096U) {
            video->gif.prefix[next_code] = old_code;
            video->gif.suffix[next_code] = first;
            next_code++;
            if (next_code == (uint16_t)(1U << code_size) && code_size < 12U)
                code_size++;
        }
        old_code = in_code;
        if (out.out_count == (uint32_t)meta->width * meta->height) break;
    }
    return out.out_count == (uint32_t)meta->width * meta->height;
}

static void gif_fill_rect(vv_video_t *video, const vv_gif_frame_t *m, uint16_t color) {
    uint32_t y, x;
    for (y = 0; y < m->height; y++) {
        uint32_t base = ((uint32_t)m->top + y) * video->width + m->left;
        for (x = 0; x < m->width; x++) video->gif.canvas[base + x] = color;
    }
}

static void gif_apply_disposal(vv_video_t *video, const vv_gif_frame_t *m) {
    if (m->disposal == 2U) {
        gif_fill_rect(video, m, video->gif.background_color);
    } else if (m->disposal == 3U && video->gif.restore) {
        vv_memcpy(video->gif.canvas, video->gif.restore, video->gif.pixel_count * 2U);
    }
}

bool vv_gif_open(vv_video_t *video) {
    uint8_t packed, bg;
    uint32_t gct_count = 0;
    uint32_t pos = 13U;
    uint32_t count = 0;
    bool loop = false, restore = false;
    uint32_t pixels;

    if (!video || video->size < 13U) return false;
    video->width = vv_rd16(video->data + 6U);
    video->height = vv_rd16(video->data + 8U);
    packed = video->data[10U];
    bg = video->data[11U];
    if (!video->width || !video->height || video->width > VV_MAX_WIDTH ||
        video->height > VV_MAX_HEIGHT) return false;
    pixels = (uint32_t)video->width * video->height;
    video->gif.pixel_count = pixels;
    if (packed & 0x80U) {
        gct_count = 1U << ((packed & 7U) + 1U);
        if (!gif_read_palette(video->data, video->size, pos,
                              video->gif.global_palette, gct_count)) return false;
        video->gif.global_palette_count = (uint16_t)gct_count;
        if (bg < gct_count) video->gif.background_color = video->gif.global_palette[bg];
    }
    if (!gif_scan(video, NULL, 0U, &count, &loop, &restore) || !count) return false;
    video->format = VV_FORMAT_GIF;
    video->gif.frames = (vv_gif_frame_t *)bk_sys_alloc(count * sizeof(vv_gif_frame_t));
    video->gif.canvas = (uint16_t *)bk_sys_alloc(pixels * 2U);
    if (!video->gif.frames || !video->gif.canvas) return false;
    if (restore) {
        video->gif.restore = (uint16_t *)bk_sys_alloc(pixels * 2U);
        if (!video->gif.restore) return false;
    }
    count = 0U;
    if (!gif_scan(video, video->gif.frames, VV_MAX_FRAMES, &count, &loop, &restore))
        return false;
    video->frame_count = count;
    video->fps_num = 0U;
    video->fps_den = 1U;
    video->loop_default = loop;
    video->gif.has_restore_disposal = restore;
    video->format = VV_FORMAT_GIF;
    return true;
}

void vv_gif_close(vv_video_t *video) {
    if (!video) return;
    if (video->gif.frames) bk_sys_free(video->gif.frames);
    if (video->gif.canvas) bk_sys_free(video->gif.canvas);
    if (video->gif.restore) bk_sys_free(video->gif.restore);
    if (video->gif.indices) bk_sys_free(video->gif.indices);
    video->gif.frames = NULL; video->gif.canvas = NULL;
    video->gif.restore = NULL; video->gif.indices = NULL;
}

bool vv_gif_prepare(vv_video_t *video, uint32_t frame) {
    uint32_t i, p;
    if (!video || !video->gif.frames || !video->gif.canvas || frame >= video->frame_count)
        return false;
    for (p = 0; p < video->gif.pixel_count; p++)
        video->gif.canvas[p] = video->gif.background_color;
    for (i = 0; i <= frame; i++) {
        vv_gif_frame_t *m = &video->gif.frames[i];
        if (m->disposal == 3U && video->gif.restore)
            vv_memcpy(video->gif.restore, video->gif.canvas, video->gif.pixel_count * 2U);
        if (!gif_decode_image(video, m)) return false;
        if (i < frame) gif_apply_disposal(video, m);
    }
    return true;
}

uint16_t vv_gif_sample(const vv_video_t *video, uint32_t x, uint32_t y) {
    if (!video || !video->gif.canvas || x >= video->width || y >= video->height) return 0;
    return video->gif.canvas[y * (uint32_t)video->width + x];
}

uint32_t vv_gif_duration_ms(const vv_video_t *video, uint32_t frame) {
    uint32_t ms;
    if (!video || !video->gif.frames || frame >= video->frame_count) return 100U;
    ms = (uint32_t)video->gif.frames[frame].delay_cs * 10U;
    if (ms < 20U) ms = 20U;
    if (ms > 10000U) ms = 10000U;
    return ms;
}
