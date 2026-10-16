#include "video_internal.h"
#include "../third_party/pl_mpeg.h"

#define VV_MPEG_PROBE_BYTES (512U * 1024U)
#define VV_MPEG_AUDIO_CACHE 1152U

/* File-backed pl_mpeg. Three separate cursors are used for scanning, video
 * and audio: random seeking one decoder must not disturb the other. */
typedef struct {
    int fd;
    uint32_t length;
} vv_mpeg_file_t;

static void mpeg_file_load(plm_buffer_t *buffer, void *user) {
    vv_mpeg_file_t *file = (vv_mpeg_file_t *)user;
    uint8_t scratch[8192];
    int count;
    if (!file || file->fd < 0) return;
    count = bk_file_read(file->fd, scratch, sizeof(scratch));
    if (count > 0) (void)plm_buffer_write(buffer, scratch, (size_t)count);
    else {
        /* plm_buffer_signal_end() rewrites total_size, even in FILE mode.
         * EOF is detected by the no-progress read; avoid changing file size
         * or corrupting later duration/seek calculations. */
        (void)buffer;
    }
}

static void mpeg_file_seek(plm_buffer_t *buffer, size_t offset, void *user) {
    vv_mpeg_file_t *file = (vv_mpeg_file_t *)user;
    (void)buffer;
    if (file && file->fd >= 0 && offset <= file->length)
        (void)bk_file_seek(file->fd, (int32_t)offset, 0U);
}

static size_t mpeg_file_tell(plm_buffer_t *buffer, void *user) {
    vv_mpeg_file_t *file = (vv_mpeg_file_t *)user;
    int32_t position;
    (void)buffer;
    if (!file || file->fd < 0) return 0U;
    position = bk_file_tell(file->fd);
    return position > 0 ? (size_t)position : 0U;
}

static plm_buffer_t *mpeg_file_buffer(vv_mpeg_file_t *file) {
    return plm_buffer_create_with_callbacks(mpeg_file_load, mpeg_file_seek,
                                             mpeg_file_tell, file->length, file);
}

static plm_t *mpeg_file_decoder(vv_mpeg_file_t *file) {
    plm_buffer_t *buffer = mpeg_file_buffer(file);
    if (!buffer) return NULL;
    return plm_create_with_buffer(buffer, 1);
}

static vv_mpeg_file_t *mpeg_file_open(const char *path, uint32_t size) {
    vv_mpeg_file_t *file;
    int fd;
    if (!path || size > 0x7FFFFFFFU) return NULL;
    fd = bk_file_open(path, BK_FILE_READ_ONLY);
    if (fd < 0) return NULL;
    file = (vv_mpeg_file_t *)bk_sys_alloc(sizeof(*file));
    if (!file) { (void)bk_file_close(fd); return NULL; }
    file->fd = fd;
    file->length = size;
    return file;
}

static void mpeg_file_close(vv_mpeg_file_t *file) {
    if (!file) return;
    if (file->fd >= 0) (void)bk_file_close(file->fd);
    bk_sys_free(file);
}

static plm_t *mpeg_video_plm(vv_video_t *video) {
    return (plm_t *)video->mpeg.video_decoder;
}

static plm_t *mpeg_audio_plm(vv_video_t *video) {
    return (plm_t *)video->mpeg.audio_decoder;
}

static const plm_frame_t *mpeg_frame(const vv_video_t *video) {
    return (const plm_frame_t *)video->mpeg.decoded_frame;
}

static uint32_t mpeg_probe_size(const vv_video_t *video) {
    return video->size < VV_MPEG_PROBE_BYTES ? video->size : VV_MPEG_PROBE_BYTES;
}

static bool mpeg_rate_from_code(uint8_t code, uint32_t *num, uint32_t *den) {
    if (!num || !den) return false;
    switch (code) {
        case 1: *num = 24000U; *den = 1001U; return true;
        case 2: *num = 24U;    *den = 1U;    return true;
        case 3: *num = 25U;    *den = 1U;    return true;
        case 4: *num = 30000U; *den = 1001U; return true;
        case 5: *num = 30U;    *den = 1U;    return true;
        case 6: *num = 50U;    *den = 1U;    return true;
        case 7: *num = 60000U; *den = 1001U; return true;
        case 8: *num = 60U;    *den = 1U;    return true;
        default: return false;
    }
}

/* Walk only the demuxed video elementary stream. This avoids mistaking a
 * start-code-looking byte sequence inside MP2/private data for a video header.
 * The rolling state and pending sequence bytes intentionally survive PES
 * boundaries because MPEG start codes may be split between two video packets. */
static bool mpeg_scan_video(vv_video_t *video, vv_mpeg_file_t *source,
                            uint32_t *picture_count,
                            uint32_t *fps_num, uint32_t *fps_den) {
    plm_buffer_t *buffer;
    plm_demux_t *demux;
    plm_packet_t *packet;
    uint32_t state = 0xFFFFFFFFU;
    uint32_t count = 0U;
    uint32_t i;
    uint8_t seq_bytes[4];
    uint8_t seq_pos = 0U;
    uint8_t rate_code = 0U;

    if (!video || !picture_count || !fps_num || !fps_den) return false;
    buffer = source ? mpeg_file_buffer(source) :
             plm_buffer_create_with_memory((uint8_t *)video->data,
                                           (size_t)video->size, 0);
    if (!buffer) return false;
    demux = plm_demux_create(buffer, 1);
    if (!demux) {
        plm_buffer_destroy(buffer);
        return false;
    }
    (void)plm_demux_probe(demux, (size_t)mpeg_probe_size(video));

    while ((packet = plm_demux_decode(demux)) != NULL) {
        if (packet->type != PLM_DEMUX_PACKET_VIDEO_1) continue;
        for (i = 0U; i < (uint32_t)packet->length; ++i) {
            uint8_t byte = packet->data[i];
            if (seq_pos && seq_pos <= 4U) {
                seq_bytes[seq_pos - 1U] = byte;
                seq_pos++;
                if (seq_pos == 5U) {
                    uint8_t candidate = (uint8_t)(seq_bytes[3] & 0x0FU);
                    rate_code = (candidate >= 1U && candidate <= 8U)
                        ? candidate : 0U;
                    seq_pos = 0U;
                }
            }
            state = (state << 8) | byte;
            if (state == 0x000001B3U && !rate_code) {
                seq_pos = 1U;
            } else if (state == 0x00000100U) {
                if (count == VV_MAX_FRAMES) {
                    plm_demux_destroy(demux);
                    return false;
                }
                count++;
            }
        }
    }
    plm_demux_destroy(demux);
    if (!count || !mpeg_rate_from_code(rate_code, fps_num, fps_den))
        return false;
    *picture_count = count;
    return true;
}

static void mpeg_reset_audio_cache(vv_video_t *video) {
    video->mpeg.audio_cache_pos = 0U;
    video->mpeg.audio_cache_count = 0U;
}

static bool mpeg_allocate_frame_image(vv_video_t *video) {
    uint32_t pixels;
    if (video->frame_image.pixels) return true;
    pixels = (uint32_t)video->width * (uint32_t)video->height;
    if (!pixels || pixels > VV_FRAME_IMAGE_MAX_PIXELS) return false;
    if (pixels > 0x3FFFFFFFU) return false;
    video->frame_image.pixels = (uint32_t *)bk_sys_alloc(pixels * 4U);
    if (!video->frame_image.pixels) return false;
    vv_memzero(video->frame_image.pixels, pixels * 4U);
    video->frame_image.width = video->width;
    video->frame_image.height = video->height;
    video->frame_image_owned = true;
    return true;
}

static bool mpeg_open_impl(vv_video_t *video, const char *path) {
    plm_t *decoder = NULL;
    plm_t *audio = NULL;
    uint32_t pictures;
    int width, height;
    int sample_rate;
    vv_mpeg_file_t *vfile = NULL, *afile = NULL, *scanfile = NULL;

    if (!video || video->size < 16U) return false;
    /* MPEG-1 Program Stream pack start code. Raw .m1v is intentionally not
     * accepted here: .MPG/.MPEG support means an MPEG-PS container. */
    if (video->data && (video->data[0] != 0x00U || video->data[1] != 0x00U ||
        video->data[2] != 0x01U || video->data[3] != 0xBAU))
        return false;

    if (path) {
        vfile = mpeg_file_open(path, video->size);
        scanfile = mpeg_file_open(path, video->size);
        if (!vfile || !scanfile) goto fail;
    }
    decoder = path ? mpeg_file_decoder(vfile) :
              plm_create_with_memory((uint8_t *)video->data,
                                     (size_t)video->size, 0);
    if (!decoder) goto fail;
    (void)plm_probe(decoder, (size_t)mpeg_probe_size(video));
    if (plm_get_num_video_streams(decoder) < 1) goto fail;
    plm_set_audio_enabled(decoder, 0);

    width = plm_get_width(decoder);
    height = plm_get_height(decoder);
    if (width <= 0 || height <= 0 || width > (int)VV_MAX_WIDTH ||
        height > (int)VV_MAX_HEIGHT)
        goto fail;
    if ((uint32_t)width > 0xFFFFFFFFU / (uint32_t)height) goto fail;

    if (!mpeg_scan_video(video, scanfile, &pictures,
                         &video->fps_num, &video->fps_den))
        goto fail;
    mpeg_file_close(scanfile);
    scanfile = NULL;

    video->format = VV_FORMAT_MPEG1;
    video->width = (uint16_t)width;
    video->height = (uint16_t)height;
    video->frame_count = pictures;
    video->loop_default = false;
    video->mpeg.video_decoder = decoder;
    video->mpeg.video_file = vfile;
    vfile = NULL;
    video->mpeg.decoded_frame = NULL;
    video->mpeg.decoded_index = 0U;
    video->mpeg.decoded_valid = false;
    decoder = NULL;

    /* A second decoder keeps MP2 position independent from video seeking.
     * This costs memory, but makes pause/seek/frame-drop behavior deterministic. */
    if (path) afile = mpeg_file_open(path, video->size);
    audio = path ? (afile ? mpeg_file_decoder(afile) : NULL) :
            plm_create_with_memory((uint8_t *)video->data,
                                   (size_t)video->size, 0);
    if (audio) {
        (void)plm_probe(audio, (size_t)mpeg_probe_size(video));
        if (plm_get_num_audio_streams(audio) > 0) {
            sample_rate = plm_get_samplerate(audio);
            /* MPEG-1 Layer II uses 32/44.1/48 kHz. SB16 in BlesKernOS
             * accepts up to 44.1 kHz, so 48 kHz is decoded normally and
             * downsampled 2:1 to 24 kHz before reaching the public PCM API. */
            if (sample_rate == 32000 || sample_rate == 44100 ||
                sample_rate == 48000) {
                video->mpeg.audio_decoder = audio;
                video->mpeg.audio_file = afile;
                afile = NULL;
                video->mpeg.audio_sample_rate =
                    (uint16_t)(sample_rate == 48000 ? 24000 : sample_rate);
                video->mpeg.audio_source_rate = (uint16_t)sample_rate;
                video->mpeg.audio_supported = true;
                plm_set_video_enabled(audio, 0);
                audio = NULL;
            }
        }
    }
    if (audio) plm_destroy(audio);
    mpeg_file_close(afile);
    mpeg_reset_audio_cache(video);
    return true;

fail:
    if (audio) plm_destroy(audio);
    if (decoder) plm_destroy(decoder);
    mpeg_file_close(afile);
    mpeg_file_close(vfile);
    mpeg_file_close(scanfile);
    return false;
}


bool vv_mpeg_open(vv_video_t *video) {
    return mpeg_open_impl(video, NULL);
}

bool vv_mpeg_open_stream(vv_video_t *video, const char *path) {
    return mpeg_open_impl(video, path);
}

void vv_mpeg_close(vv_video_t *video) {
    if (!video) return;
    if (video->mpeg.video_decoder)
        plm_destroy((plm_t *)video->mpeg.video_decoder);
    if (video->mpeg.audio_decoder)
        plm_destroy((plm_t *)video->mpeg.audio_decoder);
    mpeg_file_close((vv_mpeg_file_t *)video->mpeg.video_file);
    mpeg_file_close((vv_mpeg_file_t *)video->mpeg.audio_file);
    video->mpeg.video_file = NULL;
    video->mpeg.audio_file = NULL;
    video->mpeg.video_decoder = NULL;
    video->mpeg.audio_decoder = NULL;
    video->mpeg.decoded_frame = NULL;
    video->mpeg.decoded_valid = false;
    video->mpeg.audio_supported = false;
    video->mpeg.audio_sample_rate = 0U;
    video->mpeg.audio_source_rate = 0U;
    mpeg_reset_audio_cache(video);
}

bool vv_mpeg_prepare(vv_video_t *video, uint32_t frame_index) {
    plm_t *decoder;
    plm_frame_t *frame;
    double seconds;
    if (!video || video->format != VV_FORMAT_MPEG1 ||
        frame_index >= video->frame_count)
        return false;
    decoder = mpeg_video_plm(video);
    if (!decoder) return false;

    if (video->mpeg.decoded_valid &&
        frame_index == video->mpeg.decoded_index + 1U) {
        frame = plm_decode_video(decoder);
    } else if (!video->mpeg.decoded_valid && frame_index == 0U) {
        frame = plm_decode_video(decoder);
    } else {
        seconds = ((double)frame_index * (double)video->fps_den) /
                  (double)video->fps_num;
        frame = plm_seek_frame(decoder, seconds, 1);
    }
    if (!frame) return false;
    video->mpeg.decoded_frame = frame;
    video->mpeg.decoded_index = frame_index;
    video->mpeg.decoded_valid = true;
    return true;
}

static uint8_t mpeg_clamp8(int value) {
    if (value < 0) return 0U;
    if (value > 255) return 255U;
    return (uint8_t)value;
}

uint16_t vv_mpeg_sample(const vv_video_t *video, uint32_t x, uint32_t y) {
    const plm_frame_t *frame;
    int yy, cb, cr, c;
    uint8_t r, g, b;
    uint32_t yi, ci;
    if (!video || x >= video->width || y >= video->height) return 0U;
    frame = mpeg_frame(video);
    if (!frame) return 0U;
    yi = y * frame->y.width + x;
    ci = (y >> 1) * frame->cb.width + (x >> 1);
    yy = (int)frame->y.data[yi];
    cb = (int)frame->cb.data[ci] - 128;
    cr = (int)frame->cr.data[ci] - 128;
    c = yy - 16;
    if (c < 0) c = 0;
    r = mpeg_clamp8((298 * c + 409 * cr + 128) >> 8);
    g = mpeg_clamp8((298 * c - 100 * cb - 208 * cr + 128) >> 8);
    b = mpeg_clamp8((298 * c + 516 * cb + 128) >> 8);
    return (uint16_t)((((uint16_t)r >> 3) << 11) |
                      (((uint16_t)g >> 2) << 5) |
                      ((uint16_t)b >> 3));
}

const bk_gui_image_t *vv_mpeg_frame_image(vv_video_t *video) {
    plm_frame_t *frame;
    if (!video || video->format != VV_FORMAT_MPEG1 ||
        !video->mpeg.decoded_valid)
        return NULL;
    if (!mpeg_allocate_frame_image(video)) return NULL;
    if (video->frame_image_valid &&
        video->frame_image_frame == video->prepared_frame)
        return &video->frame_image;
    frame = (plm_frame_t *)video->mpeg.decoded_frame;
    if (!frame) return NULL;
    plm_frame_to_bgra(frame, (uint8_t *)video->frame_image.pixels,
                      (int)video->width * 4);
    video->frame_image_frame = video->prepared_frame;
    video->frame_image_valid = true;
    return &video->frame_image;
}

bool vv_mpeg_audio_available(const vv_video_t *video) {
    return video && video->format == VV_FORMAT_MPEG1 &&
           video->mpeg.audio_supported && video->mpeg.audio_decoder != NULL &&
           video->mpeg.audio_sample_rate != 0U;
}

void vv_mpeg_audio_seek_ms(vv_video_t *video, uint32_t time_ms) {
    plm_t *audio;
    double seconds;
    if (!vv_mpeg_audio_available(video)) return;
    audio = mpeg_audio_plm(video);
    mpeg_reset_audio_cache(video);
    if (time_ms == 0U) {
        plm_rewind(audio);
        return;
    }
    seconds = (double)time_ms / 1000.0;
    /* pl_mpeg's high-level seek is video-PTS based. Re-enable the video
     * packet path only while seeking, then keep sequential audio decode lean. */
    plm_set_video_enabled(audio, 1);
    if (!plm_seek(audio, seconds, 0)) plm_rewind(audio);
    plm_set_video_enabled(audio, 0);
}

static void mpeg_fill_audio_cache(vv_video_t *video, const plm_samples_t *samples) {
    uint32_t src, dst = 0U, step;
    if (!video || !samples) return;
    /* Taking every second decoded sample from a 48 kHz MP2 source is
     * intentionally cheap and keeps the output compatible with SB16. */
    step = video->mpeg.audio_source_rate == 48000U ? 2U : 1U;
    for (src = 0U; src < VV_MPEG_AUDIO_CACHE; src += step) {
        float mixed = (samples->interleaved[src * 2U] +
                       samples->interleaved[src * 2U + 1U]) * 0.5f;
        int value;
        if (mixed < -1.0f) mixed = -1.0f;
        if (mixed > 1.0f) mixed = 1.0f;
        value = (int)(mixed * 127.0f + 128.0f);
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        video->mpeg.audio_cache[dst++] = (uint8_t)value;
    }
    video->mpeg.audio_cache_pos = 0U;
    video->mpeg.audio_cache_count = (uint16_t)dst;
}

uint32_t vv_mpeg_audio_decode_u8_mono(vv_video_t *video,
                                      uint8_t *output, uint32_t capacity) {
    plm_t *audio;
    uint32_t written = 0U;
    if (!vv_mpeg_audio_available(video) || !output || !capacity) return 0U;
    audio = mpeg_audio_plm(video);
    while (written < capacity) {
        if (video->mpeg.audio_cache_pos < video->mpeg.audio_cache_count) {
            uint32_t avail = video->mpeg.audio_cache_count -
                             video->mpeg.audio_cache_pos;
            uint32_t take = capacity - written;
            if (take > avail) take = avail;
            vv_memcpy(output + written,
                      video->mpeg.audio_cache + video->mpeg.audio_cache_pos,
                      take);
            video->mpeg.audio_cache_pos += take;
            written += take;
            continue;
        }
        {
            plm_samples_t *samples = plm_decode_audio(audio);
            if (!samples) break;
            mpeg_fill_audio_cache(video, samples);
        }
    }
    return written;
}
