#include "video_internal.h"

#define FCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))
#define BI_RGB       0U
#define BI_BITFIELDS 3U
#define WAVE_PCM     1U

typedef struct {
    bool have_video;
    uint32_t video_stream;
    uint32_t video_handler;
    uint32_t scale;
    uint32_t rate;
    uint32_t length;
    uint32_t video_strf_offset;
    uint32_t video_strf_size;

    bool have_audio;
    uint32_t audio_stream;
    uint32_t audio_strf_offset;
    uint32_t audio_strf_size;
} avi_stream_info_t;

static uint32_t popcount32(uint32_t v) {
    uint32_t n = 0;
    while (v) { n += v & 1U; v >>= 1; }
    return n;
}

static uint32_t ctz32(uint32_t v) {
    uint32_t n = 0;
    if (!v) return 0;
    while (!(v & 1U)) { n++; v >>= 1; }
    return n;
}

static uint8_t mask_to_8(uint32_t value, uint32_t mask) {
    uint32_t shift, bits, component, maxv;
    if (!mask) return 0;
    shift = ctz32(mask);
    bits = popcount32(mask >> shift);
    component = (value & mask) >> shift;
    if (bits >= 8U) return (uint8_t)(component >> (bits - 8U));
    maxv = (1U << bits) - 1U;
    return (uint8_t)((component * 255U + maxv / 2U) / maxv);
}

static bool fourcc_is_mjpeg(uint32_t value) {
    return value == FCC('M','J','P','G') || value == FCC('J','P','E','G') ||
           value == FCC('m','j','p','g') || value == FCC('j','p','e','g');
}

static void parse_strl(vv_video_t *video, uint32_t start, uint32_t end,
                       uint32_t stream_index, avi_stream_info_t *out) {
    uint32_t pos = start;
    uint32_t strh_off = 0, strh_size = 0, strf_off = 0, strf_size = 0;
    uint32_t type;
    while (pos + 8U <= end) {
        uint32_t id = vv_rd32(video->data + pos);
        uint32_t sz = vv_rd32(video->data + pos + 4U);
        uint32_t data = pos + 8U;
        uint32_t next;
        if (!vv_range_ok(data, sz, video->size) || data + sz > end) break;
        next = data + ((sz + 1U) & ~1U);
        if (id == FCC('s','t','r','h')) { strh_off = data; strh_size = sz; }
        else if (id == FCC('s','t','r','f')) { strf_off = data; strf_size = sz; }
        if (next <= pos) break;
        pos = next;
    }
    if (!strh_off || strh_size < 48U || !strf_off) return;
    type = vv_rd32(video->data + strh_off);
    if (type == FCC('v','i','d','s') && !out->have_video && strf_size >= 40U) {
        out->have_video = true;
        out->video_stream = stream_index;
        out->video_handler = vv_rd32(video->data + strh_off + 4U);
        out->scale = vv_rd32(video->data + strh_off + 20U);
        out->rate = vv_rd32(video->data + strh_off + 24U);
        out->length = vv_rd32(video->data + strh_off + 32U);
        out->video_strf_offset = strf_off;
        out->video_strf_size = strf_size;
    } else if (type == FCC('a','u','d','s') && !out->have_audio && strf_size >= 16U) {
        out->have_audio = true;
        out->audio_stream = stream_index;
        out->audio_strf_offset = strf_off;
        out->audio_strf_size = strf_size;
    }
}

static bool parse_hdrl(vv_video_t *video, uint32_t start, uint32_t end,
                       avi_stream_info_t *out) {
    uint32_t pos = start;
    uint32_t stream_index = 0;
    while (pos + 8U <= end) {
        uint32_t id = vv_rd32(video->data + pos);
        uint32_t sz = vv_rd32(video->data + pos + 4U);
        uint32_t data = pos + 8U;
        uint32_t next;
        if (!vv_range_ok(data, sz, video->size) || data + sz > end) break;
        next = data + ((sz + 1U) & ~1U);
        if (id == FCC('L','I','S','T') && sz >= 4U) {
            uint32_t type = vv_rd32(video->data + data);
            if (type == FCC('s','t','r','l')) {
                parse_strl(video, data + 4U, data + sz, stream_index, out);
                stream_index++;
            }
        }
        if (next <= pos) break;
        pos = next;
    }
    return out->have_video;
}

static bool parse_stream_number(uint32_t id, uint32_t *stream) {
    uint8_t a = (uint8_t)(id & 0xFFU);
    uint8_t b = (uint8_t)((id >> 8) & 0xFFU);
    if (!stream || a < '0' || a > '9' || b < '0' || b > '9') return false;
    /* AVI chunk stream prefixes are two ASCII decimal digits (00..99). */
    *stream = (uint32_t)(a - '0') * 10U + (uint32_t)(b - '0');
    return true;
}

static bool video_chunk_matches(uint32_t id, uint32_t stream) {
    uint8_t c = (uint8_t)((id >> 16) & 0xFFU);
    uint8_t d = (uint8_t)((id >> 24) & 0xFFU);
    uint32_t parsed;
    return c == 'd' && (d == 'b' || d == 'c') &&
           parse_stream_number(id, &parsed) && parsed == stream;
}

static bool audio_chunk_matches(uint32_t id, uint32_t stream) {
    uint8_t c = (uint8_t)((id >> 16) & 0xFFU);
    uint8_t d = (uint8_t)((id >> 24) & 0xFFU);
    uint32_t parsed;
    return c == 'w' && d == 'b' &&
           parse_stream_number(id, &parsed) && parsed == stream;
}

/* Seekable AVI reader: only the RIFF headers, index and current compressed
 * frame are resident. This also bounds memory when videos are large. */
#define AVI_HEADER_LIMIT (1024U * 1024U)
#define AVI_FRAME_LIMIT  (16U * 1024U * 1024U)

static bool avi_read_at(int fd, uint32_t offset, void *buffer, uint32_t length) {
    uint8_t *p = (uint8_t *)buffer;
    if (bk_file_seek(fd, (int32_t)offset, 0U) < 0) return false;
    while (length) {
        int read = bk_file_read(fd, p, length);
        if (read <= 0) return false;
        p += (uint32_t)read;
        length -= (uint32_t)read;
    }
    return true;
}

/* Index nested rec/movi LISTs with fixed-size header reads, never map movi. */
static bool scan_movi_file(int fd, uint32_t start, uint32_t end,
                           const avi_stream_info_t *si, vv_video_t *video,
                           uint32_t *frame_count, uint32_t *audio_count,
                           uint32_t *audio_bytes, uint32_t depth) {
    uint32_t pos = start;
    if (depth > 12U) return false;
    while (pos <= end && end - pos >= 8U) {
        uint8_t hdr[12];
        uint32_t id, sz, off, next;
        if (!avi_read_at(fd, pos, hdr, 8U)) return false;
        id = vv_rd32(hdr);
        sz = vv_rd32(hdr + 4U);
        off = pos + 8U;
        if (sz > end - off || sz > 0xFFFFFFFEU) return false;
        next = off + ((sz + 1U) & ~1U);
        if (id == FCC('L','I','S','T') && sz >= 4U) {
            if (!scan_movi_file(fd, off + 4U, off + sz, si, video,
                                frame_count, audio_count, audio_bytes,
                                depth + 1U)) return false;
        } else if (video_chunk_matches(id, si->video_stream)) {
            if (*frame_count >= VV_MAX_FRAMES) return false;
            if (video->avi.frames) {
                video->avi.frames[*frame_count].offset = off;
                video->avi.frames[*frame_count].size = sz;
            }
            (*frame_count)++;
        } else if (si->have_audio && audio_chunk_matches(id, si->audio_stream)) {
            if (*audio_count >= VV_MAX_FRAMES) return false;
            if (video->avi.audio_chunks) {
                video->avi.audio_chunks[*audio_count].offset = off;
                video->avi.audio_chunks[*audio_count].size = sz;
            }
            (*audio_count)++;
            if (*audio_bytes <= 0xFFFFFFFFU - sz) *audio_bytes += sz;
        }
        if (next <= pos) return false;
        pos = next;
    }
    return true;
}

static void scan_movi(vv_video_t *video, uint32_t start, uint32_t end,
                      const avi_stream_info_t *si,
                      vv_avi_frame_t *frames, uint32_t frame_capacity,
                      uint32_t *frame_count,
                      vv_avi_audio_chunk_t *audio, uint32_t audio_capacity,
                      uint32_t *audio_count, uint32_t *audio_bytes) {
    uint32_t pos = start;
    while (pos + 8U <= end) {
        uint32_t id = vv_rd32(video->data + pos);
        uint32_t sz = vv_rd32(video->data + pos + 4U);
        uint32_t data = pos + 8U;
        uint32_t next;
        if (!vv_range_ok(data, sz, video->size) || data + sz > end) break;
        next = data + ((sz + 1U) & ~1U);
        if (id == FCC('L','I','S','T') && sz >= 4U) {
            scan_movi(video, data + 4U, data + sz, si,
                      frames, frame_capacity, frame_count,
                      audio, audio_capacity, audio_count, audio_bytes);
        } else if (video_chunk_matches(id, si->video_stream)) {
            if (frames && *frame_count < frame_capacity) {
                frames[*frame_count].offset = data;
                frames[*frame_count].size = sz;
            }
            (*frame_count)++;
        } else if (si->have_audio &&
                   audio_chunk_matches(id, si->audio_stream)) {
            if (audio && *audio_count < audio_capacity) {
                audio[*audio_count].offset = data;
                audio[*audio_count].size = sz;
            }
            (*audio_count)++;
            if (audio_bytes && *audio_bytes <= 0xFFFFFFFFU - sz)
                *audio_bytes += sz;
        }
        if (next <= pos) break;
        pos = next;
    }
}

static bool setup_video(vv_video_t *video, const avi_stream_info_t *si) {
    const uint8_t *p = video->data + si->video_strf_offset;
    uint32_t header = vv_rd32(p);
    int32_t width, height;
    uint16_t planes, bpp;
    uint32_t compression;
    uint32_t colors_used;
    uint32_t palette_off;
    uint32_t i;
    if (header < 40U || si->video_strf_size < header) return false;
    width = vv_rd32s(p + 4U);
    height = vv_rd32s(p + 8U);
    planes = vv_rd16(p + 12U);
    bpp = vv_rd16(p + 14U);
    compression = vv_rd32(p + 16U);
    colors_used = vv_rd32(p + 32U);
    if (width <= 0 || height == 0 || width > (int32_t)VV_MAX_WIDTH ||
        height > (int32_t)VV_MAX_HEIGHT || height < -(int32_t)VV_MAX_HEIGHT ||
        planes != 1U) return false;

    video->width = (uint16_t)width;
    video->height = (uint16_t)(height < 0 ? -height : height);
    video->avi.dib_height = height;
    video->avi.bpp = bpp;
    video->avi.compression = compression;
    video->avi.handler = si->video_handler;
    video->avi.mjpeg = fourcc_is_mjpeg(compression) ||
                       fourcc_is_mjpeg(si->video_handler);

    if (video->avi.mjpeg) {
        /* Standard MJPEG AVI stores a complete JPEG image in every ##dc chunk.
         * Decoding is delegated to the public BlesKernOS JPEG API. */
        return true;
    }

    if ((bpp != 8U && bpp != 16U && bpp != 24U && bpp != 32U) ||
        (compression != BI_RGB && compression != BI_BITFIELDS)) return false;
    if (bpp == 8U && compression != BI_RGB) return false;

    video->avi.stride = (((uint32_t)video->width * bpp + 31U) / 32U) * 4U;
    video->avi.red_mask = bpp == 16U ? 0x7C00U : 0x00FF0000U;
    video->avi.green_mask = bpp == 16U ? 0x03E0U : 0x0000FF00U;
    video->avi.blue_mask = bpp == 16U ? 0x001FU : 0x000000FFU;

    if (compression == BI_BITFIELDS) {
        if (header >= 52U) {
            video->avi.red_mask = vv_rd32(p + 40U);
            video->avi.green_mask = vv_rd32(p + 44U);
            video->avi.blue_mask = vv_rd32(p + 48U);
        } else if (si->video_strf_size >= header + 12U) {
            video->avi.red_mask = vv_rd32(p + header);
            video->avi.green_mask = vv_rd32(p + header + 4U);
            video->avi.blue_mask = vv_rd32(p + header + 8U);
        } else return false;
    }

    if (bpp == 8U) {
        uint32_t count = colors_used ? colors_used : 256U;
        if (count > 256U) count = 256U;
        palette_off = header;
        if (!vv_range_ok(si->video_strf_offset + palette_off, count * 4U, video->size) ||
            palette_off + count * 4U > si->video_strf_size) return false;
        for (i = 0; i < count; i++) {
            const uint8_t *q = p + palette_off + i * 4U;
            video->avi.palette[i] = ((uint32_t)q[2] << 16) |
                                    ((uint32_t)q[1] << 8) | q[0];
        }
        video->avi.palette_count = (uint16_t)count;
    }
    return true;
}

static void setup_audio(vv_video_t *video, const avi_stream_info_t *si) {
    const uint8_t *p;
    uint16_t format, channels, align, bits;
    uint32_t rate, avg;
    if (!si->have_audio || si->audio_strf_size < 16U) return;
    p = video->data + si->audio_strf_offset;
    format = vv_rd16(p + 0U);
    channels = vv_rd16(p + 2U);
    rate = vv_rd32(p + 4U);
    avg = vv_rd32(p + 8U);
    align = vv_rd16(p + 12U);
    bits = vv_rd16(p + 14U);
    video->avi.audio_stream = si->audio_stream;
    video->avi.audio_format = format;
    video->avi.audio_channels = channels;
    video->avi.audio_sample_rate = rate;
    video->avi.audio_bytes_per_sec = avg;
    video->avi.audio_block_align = align;
    video->avi.audio_bits = bits;

    if (format != WAVE_PCM || (channels != 1U && channels != 2U) ||
        (bits != 8U && bits != 16U) || rate < 4000U || rate > 44100U)
        return;
    if (!align) align = (uint16_t)(channels * (bits / 8U));
    if (align != channels * (bits / 8U)) return;
    if (!avg) avg = rate * align;
    video->avi.audio_block_align = align;
    video->avi.audio_bytes_per_sec = avg;
    video->avi.audio_supported = true;
}

bool vv_avi_open(vv_video_t *video) {
    uint32_t riff_end;
    uint32_t pos = 12U;
    uint32_t hdrl_start = 0, hdrl_end = 0, movi_start = 0, movi_end = 0;
    avi_stream_info_t si;
    uint32_t frame_count = 0, audio_count = 0, audio_capacity = 0, audio_bytes = 0;
    vv_memzero(&si, sizeof(si));
    if (!video || video->size < 12U) return false;
    riff_end = 8U + vv_rd32(video->data + 4U);
    if (riff_end > video->size) riff_end = video->size;
    while (pos + 8U <= riff_end) {
        uint32_t id = vv_rd32(video->data + pos);
        uint32_t sz = vv_rd32(video->data + pos + 4U);
        uint32_t data = pos + 8U;
        uint32_t next;
        if (!vv_range_ok(data, sz, video->size) || data + sz > riff_end) break;
        next = data + ((sz + 1U) & ~1U);
        if (id == FCC('L','I','S','T') && sz >= 4U) {
            uint32_t type = vv_rd32(video->data + data);
            if (type == FCC('h','d','r','l')) {
                hdrl_start = data + 4U; hdrl_end = data + sz;
            } else if (type == FCC('m','o','v','i')) {
                movi_start = data + 4U; movi_end = data + sz;
            }
        }
        if (next <= pos) break;
        pos = next;
    }
    if (!hdrl_start || !movi_start ||
        !parse_hdrl(video, hdrl_start, hdrl_end, &si)) return false;
    if (!setup_video(video, &si)) return false;
    /* Set early so vv_close() can reclaim partially opened AVI state. */
    video->format = VV_FORMAT_AVI;
    setup_audio(video, &si);

    scan_movi(video, movi_start, movi_end, &si,
              NULL, 0U, &frame_count,
              NULL, 0U, &audio_count, &audio_bytes);
    if (!frame_count || frame_count > VV_MAX_FRAMES) return false;

    video->avi.frames = (vv_avi_frame_t *)bk_sys_alloc(
        frame_count * sizeof(vv_avi_frame_t));
    if (!video->avi.frames) return false;
    video->avi.frame_capacity = frame_count;

    if (video->avi.audio_supported && audio_count) {
        audio_capacity = audio_count;
        video->avi.audio_chunks = (vv_avi_audio_chunk_t *)bk_sys_alloc(
            audio_capacity * sizeof(vv_avi_audio_chunk_t));
        if (!video->avi.audio_chunks) {
            video->avi.audio_supported = false;
            audio_count = 0U;
        }
    }

    frame_count = 0U;
    audio_count = 0U;
    audio_bytes = 0U;
    scan_movi(video, movi_start, movi_end, &si,
              video->avi.frames, video->avi.frame_capacity, &frame_count,
              video->avi.audio_chunks,
              video->avi.audio_chunks ? audio_capacity : 0U,
              &audio_count, &audio_bytes);
    if (!frame_count) return false;

    video->frame_count = frame_count;
    video->fps_num = si.rate ? si.rate : 10U;
    video->fps_den = si.scale ? si.scale : 1U;
    if (video->fps_num > 1000000U || !video->fps_den) return false;
    video->avi.audio_chunk_count = video->avi.audio_chunks ? audio_count : 0U;
    video->avi.audio_data_bytes = video->avi.audio_chunks ? audio_bytes : 0U;
    video->format = VV_FORMAT_AVI;
    video->loop_default = false;
    return true;
}

bool vv_avi_open_stream(vv_video_t *video, const char *path, uint32_t size) {
    int fd = -1;
    uint8_t hdr[12];
    uint32_t pos = 12U, riff_end, hdrl_start = 0, hdrl_end = 0;
    uint32_t movi_start = 0, movi_end = 0, metadata_end;
    uint32_t frames = 0, audios = 0, audio_bytes = 0;
    avi_stream_info_t si;
    if (!video || !path || size < 32U) return false;
    vv_memzero(&si, sizeof(si));
    fd = bk_file_open(path, BK_FILE_READ_ONLY);
    if (fd < 0) return false;
    video->avi.file_fd = fd;
    video->avi.streamed = true;
    video->format = VV_FORMAT_AVI; /* close descriptor on partial failure */
    if (!avi_read_at(fd, 0, hdr, 12U) ||
        vv_rd32(hdr) != FCC('R','I','F','F') ||
        vv_rd32(hdr + 8U) != FCC('A','V','I',' ')) return false;
    riff_end = vv_rd32(hdr + 4U);
    if (riff_end > size - 8U) riff_end = size - 8U;
    riff_end += 8U;
    while (pos < riff_end && riff_end - pos >= 8U) {
        uint32_t id, len, off, next;
        if (!avi_read_at(fd, pos, hdr, 8U)) return false;
        id = vv_rd32(hdr); len = vv_rd32(hdr + 4U);
        off = pos + 8U;
        if (len > riff_end - off || len > 0xFFFFFFFEU) return false;
        next = off + ((len + 1U) & ~1U);
        if (id == FCC('L','I','S','T') && len >= 4U) {
            uint32_t type;
            if (!avi_read_at(fd, pos + 8U, hdr + 8U, 4U)) return false;
            type = vv_rd32(hdr + 8U);
            if (type == FCC('h','d','r','l')) {
                hdrl_start = off + 4U;
                hdrl_end = off + len;
            } else if (type == FCC('m','o','v','i')) {
                movi_start = off + 4U;
                movi_end = off + len;
            }
        }
        if (next <= pos) return false;
        pos = next;
    }
    if (!hdrl_start || !movi_start || hdrl_end > AVI_HEADER_LIMIT ||
        hdrl_end > size || movi_end > size) return false;
    metadata_end = hdrl_end;
    video->data = (uint8_t *)bk_sys_alloc(metadata_end);
    if (!video->data) return false;
    if (!avi_read_at(fd, 0, (void *)video->data, metadata_end)) return false;
    video->size = metadata_end;
    if (!parse_hdrl(video, hdrl_start, hdrl_end, &si) ||
        !setup_video(video, &si)) return false;
    setup_audio(video, &si);
    if (video->avi.audio_supported) {
        video->avi.audio_cache = (uint8_t *)bk_sys_alloc(4096U);
        if (!video->avi.audio_cache) video->avi.audio_supported = false;
    }
    /* Once header parsing is finished, indexed offsets refer to file size. */
    video->size = size;
    if (!scan_movi_file(fd, movi_start, movi_end, &si, video,
                        &frames, &audios, &audio_bytes, 0U) || !frames)
        return false;
    video->avi.frames = (vv_avi_frame_t *)bk_sys_alloc(
        frames * sizeof(vv_avi_frame_t));
    if (!video->avi.frames) return false;
    video->avi.frame_capacity = frames;
    if (video->avi.audio_supported && audios) {
        video->avi.audio_chunks = (vv_avi_audio_chunk_t *)bk_sys_alloc(
            audios * sizeof(vv_avi_audio_chunk_t));
        if (!video->avi.audio_chunks) video->avi.audio_supported = false;
    }
    frames = 0; audios = 0; audio_bytes = 0;
    if (!scan_movi_file(fd, movi_start, movi_end, &si, video,
                        &frames, &audios, &audio_bytes, 0U)) return false;
    video->frame_count = frames;
    video->fps_num = si.rate ? si.rate : 10U;
    video->fps_den = si.scale ? si.scale : 1U;
    if (video->fps_num > 1000000U || !video->fps_den) return false;
    video->avi.audio_chunk_count = video->avi.audio_chunks ? audios : 0U;
    video->avi.audio_data_bytes = video->avi.audio_chunks ? audio_bytes : 0U;
    video->loop_default = false;
    return true;
}

void vv_avi_close(vv_video_t *video) {
    if (!video) return;
    if (video->avi.mjpeg_image_valid) {
        bk_gui_image_free(&video->avi.mjpeg_image);
        video->avi.mjpeg_image_valid = false;
    }
    if (video->avi.audio_cache) bk_sys_free(video->avi.audio_cache);
    video->avi.audio_cache = NULL;
    if (video->avi.frame_payload) bk_sys_free(video->avi.frame_payload);
    video->avi.frame_payload = NULL;
    if (video->avi.streamed) {
        (void)bk_file_close(video->avi.file_fd);
        if (video->data) bk_sys_free((void *)video->data);
        video->data = NULL;
    }
    if (video->avi.frames) bk_sys_free(video->avi.frames);
    if (video->avi.audio_chunks) bk_sys_free(video->avi.audio_chunks);
    video->avi.frames = NULL;
    video->avi.audio_chunks = NULL;
    video->avi.audio_chunk_count = 0U;
}

bool vv_avi_prepare(vv_video_t *video, uint32_t frame) {
    uint32_t needed;
    const vv_avi_frame_t *f;
    if (!video || frame >= video->frame_count || !video->avi.frames) return false;
    f = &video->avi.frames[frame];
    if (video->avi.streamed) {
        uint8_t *new_buffer;
        if (!f->size || f->size > AVI_FRAME_LIMIT) return false;
        if (f->size > video->avi.frame_payload_capacity) {
            new_buffer = (uint8_t *)bk_sys_alloc(f->size);
            if (!new_buffer) return false;
            if (video->avi.frame_payload) bk_sys_free(video->avi.frame_payload);
            video->avi.frame_payload = new_buffer;
            video->avi.frame_payload_capacity = f->size;
        }
        if (!avi_read_at(video->avi.file_fd, f->offset,
                         video->avi.frame_payload, f->size)) return false;
        video->avi.frame_payload_size = f->size;
    }
    if (video->avi.mjpeg) {
        if (video->avi.mjpeg_image_valid) {
            bk_gui_image_free(&video->avi.mjpeg_image);
            video->avi.mjpeg_image_valid = false;
        }
        vv_memzero(&video->avi.mjpeg_image, sizeof(video->avi.mjpeg_image));
        if (!f->size || !vv_range_ok(f->offset, f->size, video->size)) return false;
        if (!bk_gui_image_decode_jpeg(&video->avi.mjpeg_image,
                                      video->avi.streamed ? video->avi.frame_payload :
                                      video->data + f->offset, f->size))
            return false;
        if (!video->avi.mjpeg_image.pixels || !video->avi.mjpeg_image.width ||
            !video->avi.mjpeg_image.height) {
            bk_gui_image_free(&video->avi.mjpeg_image);
            return false;
        }
        video->avi.mjpeg_image_valid = true;
        return true;
    }
    needed = video->avi.stride * (uint32_t)video->height;
    return needed <= f->size;
}

uint16_t vv_avi_sample(const vv_video_t *video, uint32_t x, uint32_t y) {
    const vv_avi_frame_t *f;
    const uint8_t *row;
    uint32_t sy;
    if (!video || !video->frame_prepared || !video->avi.frames ||
        x >= video->width || y >= video->height) return 0;

    if (video->avi.mjpeg) {
        const bk_gui_image_t *image = &video->avi.mjpeg_image;
        uint32_t sx, my, pixel;
        if (!video->avi.mjpeg_image_valid || !image->pixels) return 0;
        sx = (uint32_t)x * image->width / video->width;
        my = (uint32_t)y * image->height / video->height;
        if (sx >= image->width) sx = image->width - 1U;
        if (my >= image->height) my = image->height - 1U;
        pixel = image->pixels[my * image->width + sx];
        return vv_xrgb_to_rgb565(pixel);
    }

    f = &video->avi.frames[video->prepared_frame];
    sy = video->avi.dib_height > 0 ? (uint32_t)video->height - 1U - y : y;
    row = (video->avi.streamed ? video->avi.frame_payload :
           video->data + f->offset) + sy * video->avi.stride;
    if (video->avi.bpp == 8U) {
        uint8_t idx = row[x];
        uint32_t c = idx < video->avi.palette_count ? video->avi.palette[idx] : 0U;
        return vv_xrgb_to_rgb565(c);
    }
    if (video->avi.bpp == 16U) {
        uint32_t v = vv_rd16(row + x * 2U);
        uint8_t r = mask_to_8(v, video->avi.red_mask);
        uint8_t g = mask_to_8(v, video->avi.green_mask);
        uint8_t b = mask_to_8(v, video->avi.blue_mask);
        return vv_bgr_to_rgb565(b, g, r);
    }
    if (video->avi.bpp == 24U) {
        const uint8_t *p = row + x * 3U;
        return vv_bgr_to_rgb565(p[0], p[1], p[2]);
    }
    if (video->avi.bpp == 32U) {
        const uint8_t *p = row + x * 4U;
        if (video->avi.compression == BI_BITFIELDS) {
            uint32_t v = vv_rd32(p);
            uint8_t r = mask_to_8(v, video->avi.red_mask);
            uint8_t g = mask_to_8(v, video->avi.green_mask);
            uint8_t b = mask_to_8(v, video->avi.blue_mask);
            return vv_bgr_to_rgb565(b, g, r);
        }
        return vv_bgr_to_rgb565(p[0], p[1], p[2]);
    }
    return 0;
}

void vv_avi_audio_seek_ms(const vv_video_t *video, uint32_t time_ms,
                          uint32_t *chunk_index, uint32_t *chunk_offset) {
    uint32_t target;
    uint32_t i;
    if (chunk_index) *chunk_index = 0U;
    if (chunk_offset) *chunk_offset = 0U;
    if (!video || !chunk_index || !chunk_offset || !video->avi.audio_supported ||
        !video->avi.audio_chunks || !video->avi.audio_chunk_count ||
        !video->avi.audio_bytes_per_sec) return;

    target = vv_muldiv_u32_sat(time_ms, video->avi.audio_bytes_per_sec, 1000U);
    if (target > video->avi.audio_data_bytes) target = video->avi.audio_data_bytes;
    if (video->avi.audio_block_align)
        target -= target % video->avi.audio_block_align;

    for (i = 0U; i < video->avi.audio_chunk_count; ++i) {
        uint32_t size = video->avi.audio_chunks[i].size;
        if (target < size) {
            *chunk_index = i;
            *chunk_offset = target;
            return;
        }
        target -= size;
    }
    *chunk_index = video->avi.audio_chunk_count;
    *chunk_offset = 0U;
}

static int32_t pcm16(const uint8_t *p) {
    int32_t v = (int32_t)vv_rd16(p);
    if (v & 0x8000) v -= 65536;
    return v;
}

uint32_t vv_avi_audio_decode_u8_mono(vv_video_t *video,
                                     uint32_t *chunk_index,
                                     uint32_t *chunk_offset,
                                     uint8_t *output, uint32_t capacity) {
    uint32_t produced = 0U;
    uint32_t block;
    if (!video || !chunk_index || !chunk_offset || !output || !capacity ||
        !video->avi.audio_supported || !video->avi.audio_chunks) return 0U;
    block = video->avi.audio_block_align;
    if (!block) return 0U;

    while (*chunk_index < video->avi.audio_chunk_count && produced < capacity) {
        const vv_avi_audio_chunk_t *chunk = &video->avi.audio_chunks[*chunk_index];
        uint32_t off = *chunk_offset;
        if (off > chunk->size) off = chunk->size;
        while (off + block <= chunk->size && produced < capacity) {
            const uint8_t *p;
            uint32_t absolute = chunk->offset + off;
            if (video->avi.streamed) {
                uint32_t capacity = 4096U;
                if (block > capacity) return produced;
                if (absolute < video->avi.audio_cache_start ||
                    absolute - video->avi.audio_cache_start + block >
                    video->avi.audio_cache_size) {
                    uint32_t bytes = chunk->size - off;
                    if (bytes > capacity) bytes = capacity;
                    if (!avi_read_at(video->avi.file_fd, absolute,
                                     video->avi.audio_cache, bytes)) return produced;
                    video->avi.audio_cache_start = absolute;
                    video->avi.audio_cache_size = bytes;
                }
                p = video->avi.audio_cache +
                    (absolute - video->avi.audio_cache_start);
            } else p = video->data + absolute;
            uint8_t sample;
            if (video->avi.audio_bits == 8U) {
                if (video->avi.audio_channels == 1U) sample = p[0];
                else sample = (uint8_t)(((uint32_t)p[0] + (uint32_t)p[1]) / 2U);
            } else {
                int32_t s;
                if (video->avi.audio_channels == 1U) s = pcm16(p);
                else s = (pcm16(p) + pcm16(p + 2U)) / 2;
                s = (s + 32768) >> 8;
                if (s < 0) s = 0;
                if (s > 255) s = 255;
                sample = (uint8_t)s;
            }
            output[produced++] = sample;
            off += block;
        }
        if (off + block > chunk->size) {
            (*chunk_index)++;
            *chunk_offset = 0U;
        } else {
            *chunk_offset = off;
        }
    }
    return produced;
}
