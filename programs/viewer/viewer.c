#include <bleskernos_theme.h>
#include <bleskernos_api.h>
#include "lib/video.h"
#include "lib/render.h"
#include "lib/retro_ui.h"

#define VIEWER_MIN_API 28U
#define VIEWER_FACE BK_THEME_FACE
#define VIEWER_TEXT BK_THEME_TEXT
#define VIEWER_DIM BK_THEME_MUTED
#define VIEWER_BLACK    0x00000000U
#define VIEWER_GREEN    0x00006020U

#define STATUS_H        20
#define INFO_H          22
#define TIMELINE_H      18
#define CONTROLS_H      46
#define MARGIN           7
#define AUDIO_BUFFER_SIZE 4096U

enum {
    MENU_FILE_OPEN = 100U,
    MENU_FILE_CLOSE,
    MENU_FILE_EXIT,
    MENU_PLAY_TOGGLE = 200U,
    MENU_PLAY_STOP,
    MENU_PLAY_FIRST,
    MENU_PLAY_PREV,
    MENU_PLAY_NEXT,
    MENU_PLAY_LAST,
    MENU_PLAY_LOOP,
    MENU_VIDEO_AUTO = 300U,
    MENU_VIDEO_EXACT,
    MENU_VIDEO_FINE,
    MENU_VIDEO_FAST,
    MENU_VIDEO_TURBO,
    MENU_VIDEO_ULTRA,
    MENU_AUDIO_MUTE = 400U,
    MENU_HELP_ABOUT = 500U
};

typedef struct {
    bk_gui_desktop_t *desktop;
    bk_gui_window_t *window;
    void *file_data;
    uint32_t file_size;
    vv_video_t video;
    bool loaded;
    bool playing;
    bool loop;
    uint32_t frame;
    uint32_t next_frame_ms;
    uint32_t dropped_frames;
    uint32_t total_duration_ms;
    vr_quality_t quality;
    vr_stats_t render_stats;
    char path[BK_PATH_MAX];
    char status[112];

    bk_gui_image_t play_icon;
    bk_gui_image_t pause_icon;
    bk_gui_image_t sound_icon;
    bool play_icon_loaded;
    bool pause_icon_loaded;
    bool sound_icon_loaded;

    uint8_t *audio_buffer;
    uint8_t volume;
    bool muted;
    bool audio_cursor_valid;
    uint32_t audio_chunk;
    uint32_t audio_chunk_offset;

    bool dragging_timeline;
    bool dragging_volume;
    bool resume_after_seek;
} viewer_state_t;

typedef struct {
    bk_gui_rect_t video;
    bk_gui_rect_t info;
    bk_gui_rect_t time;
    bk_gui_rect_t timeline;
    bk_gui_rect_t controls;
    bk_gui_rect_t first;
    bk_gui_rect_t prev;
    bk_gui_rect_t play;
    bk_gui_rect_t stop;
    bk_gui_rect_t next;
    bk_gui_rect_t last;
    bk_gui_rect_t quality;
    bk_gui_rect_t loop;
    bk_gui_rect_t sound;
    bk_gui_rect_t volume;
    bk_gui_rect_t status;
} viewer_layout_t;

static bool viewer_load(viewer_state_t *state, const char *path);
static void viewer_unload(viewer_state_t *state);
static void viewer_stop(viewer_state_t *state, bool rewind);
static void viewer_toggle_play(viewer_state_t *state);
static void viewer_cycle_quality(viewer_state_t *state);
static void viewer_open_dialog(viewer_state_t *state);

static void zero_memory(void *pointer, uint32_t size) {
    uint8_t *p = (uint8_t *)pointer;
    while (p && size--) *p++ = 0;
}

static void text_copy(char *dst, uint32_t capacity, const char *src) {
    uint32_t i = 0;
    if (!dst || !capacity) return;
    while (src && src[i] && i + 1U < capacity) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static char *append_text(char *out, char *end, const char *text) {
    while (out < end && text && *text) *out++ = *text++;
    if (out <= end) *out = '\0';
    return out;
}

static char *append_uint(char *out, char *end, uint32_t value) {
    char digits[11];
    uint32_t count = 0;
    if (!value) return append_text(out, end, "0");
    while (value && count < sizeof(digits)) {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (count && out < end) *out++ = digits[--count];
    if (out <= end) *out = '\0';
    return out;
}

static char *append_2digits(char *out, char *end, uint32_t value) {
    if (out < end) *out++ = (char)('0' + ((value / 10U) % 10U));
    if (out < end) *out++ = (char)('0' + (value % 10U));
    if (out <= end) *out = '\0';
    return out;
}

static void trim_argument(char *text) {
    uint32_t len, start = 0, i;
    if (!text) return;
    len = (uint32_t)bk_string_length(text);
    while (start < len && (text[start] == ' ' || text[start] == '\t')) start++;
    while (len > start && (text[len - 1] == ' ' || text[len - 1] == '\t')) len--;
    if (len > start + 1U && text[start] == '"' && text[len - 1] == '"') {
        start++; len--;
    }
    if (start) {
        for (i = 0; start + i < len; i++) text[i] = text[start + i];
        text[i] = '\0';
    } else text[len] = '\0';
}

static const char *path_basename(const char *path) {
    const char *base = path;
    const char *p = path;
    if (!path) return "";
    while (*p) {
        if (*p == '/' || *p == '\\') base = p + 1;
        p++;
    }
    return base;
}

static uint32_t viewer_frame_time_ms(const viewer_state_t *state) {
    if (!state || !state->loaded) return 0U;
    return vv_frame_time_ms(&state->video, state->frame);
}

static void format_time(uint32_t ms, char *out, uint32_t capacity) {
    uint32_t seconds = ms / 1000U;
    uint32_t hours = seconds / 3600U;
    uint32_t minutes = (seconds / 60U) % 60U;
    uint32_t secs = seconds % 60U;
    char *p, *end;
    if (!out || !capacity) return;
    out[0] = '\0';
    p = out; end = out + capacity - 1U;
    if (hours) {
        p = append_uint(p, end, hours);
        p = append_text(p, end, ":");
        p = append_2digits(p, end, minutes);
    } else {
        p = append_2digits(p, end, minutes);
    }
    p = append_text(p, end, ":");
    (void)append_2digits(p, end, secs);
}

static void viewer_layout(viewer_state_t *state, viewer_layout_t *layout) {
    bk_gui_rect_t c = {0, 0, 0, 0};
    int bottom, x, right, volume_w;
    (void)bk_gui_window_content_rect(state->window, &c);
    layout->status = (bk_gui_rect_t){c.x + 4, c.y + c.h - STATUS_H - 3,
                                     c.w - 8, STATUS_H};
    layout->controls = (bk_gui_rect_t){c.x + MARGIN,
        layout->status.y - CONTROLS_H - 4, c.w - MARGIN * 2, CONTROLS_H};
    layout->timeline = (bk_gui_rect_t){c.x + MARGIN,
        layout->controls.y - TIMELINE_H - 4, c.w - MARGIN * 2, TIMELINE_H};
    layout->info = (bk_gui_rect_t){c.x + MARGIN,
        layout->timeline.y - INFO_H - 2, c.w - MARGIN * 2, INFO_H};
    layout->time = (bk_gui_rect_t){layout->info.x, layout->info.y + 2, 112,
                                   INFO_H - 4};
    bottom = layout->info.y - 5;
    layout->video = (bk_gui_rect_t){c.x + MARGIN, c.y + MARGIN,
        c.w - MARGIN * 2, bottom - (c.y + MARGIN)};

    x = layout->controls.x + 7;
    layout->first = (bk_gui_rect_t){x, layout->controls.y + 9, 30, 28}; x += 33;
    layout->prev  = (bk_gui_rect_t){x, layout->controls.y + 9, 30, 28}; x += 33;
    layout->play  = (bk_gui_rect_t){x, layout->controls.y + 7, 58, 32}; x += 61;
    layout->stop  = (bk_gui_rect_t){x, layout->controls.y + 9, 42, 28}; x += 45;
    layout->next  = (bk_gui_rect_t){x, layout->controls.y + 9, 30, 28}; x += 33;
    layout->last  = (bk_gui_rect_t){x, layout->controls.y + 9, 30, 28};

    right = layout->controls.x + layout->controls.w - 7;
    volume_w = layout->controls.w >= 620 ? 86 : 58;
    layout->volume = (bk_gui_rect_t){right - volume_w, layout->controls.y + 11,
                                     volume_w, 24};
    right = layout->volume.x - 3;
    layout->sound = (bk_gui_rect_t){right - 34, layout->controls.y + 9, 34, 28};
    right = layout->sound.x - 5;
    layout->loop = (bk_gui_rect_t){right - 52, layout->controls.y + 9, 52, 28};
    right = layout->loop.x - 5;
    layout->quality = (bk_gui_rect_t){right - 84, layout->controls.y + 9, 84, 28};
}

static bk_gui_rect_t viewer_fit_video(const viewer_state_t *state,
                                      bk_gui_rect_t viewport) {
    uint32_t w, h;
    int draw_w, draw_h;
    if (!state || !state->loaded || !state->video.width || !state->video.height)
        return viewport;
    w = state->video.width; h = state->video.height;
    draw_w = viewport.w - 8;
    draw_h = (int)((uint32_t)draw_w * h / w);
    if (draw_h > viewport.h - 8) {
        draw_h = viewport.h - 8;
        draw_w = (int)((uint32_t)draw_h * w / h);
    }
    if (draw_w < 1) draw_w = 1;
    if (draw_h < 1) draw_h = 1;
    return (bk_gui_rect_t){viewport.x + (viewport.w - draw_w) / 2,
                           viewport.y + (viewport.h - draw_h) / 2,
                           draw_w, draw_h};
}

static void build_frame_text(viewer_state_t *state, char *out, uint32_t capacity) {
    char *p = out, *end = out + capacity - 1U;
    if (!capacity) return;
    *out = '\0';
    if (!state->loaded) return;
    p = append_text(p, end, "Frame ");
    p = append_uint(p, end, state->frame + 1U);
    p = append_text(p, end, "/");
    p = append_uint(p, end, state->video.frame_count);
    if (state->dropped_frames) {
        p = append_text(p, end, "  drop ");
        (void)append_uint(p, end, state->dropped_frames);
    }
}

static void build_video_info(viewer_state_t *state, char *out, uint32_t capacity) {
    char *p = out, *end = out + capacity - 1U;
    uint32_t fps_int = 0U, fps_frac = 0U;
    if (!capacity) return;
    *out = '\0';
    if (!state->loaded) {
        append_text(out, end, "BlesKernOS Viewer 3.2");
        return;
    }
    p = append_text(p, end, vv_format_name(&state->video));
    p = append_text(p, end, "  ");
    p = append_uint(p, end, state->video.width);
    p = append_text(p, end, "x");
    p = append_uint(p, end, state->video.height);
    if (state->video.format == VV_FORMAT_GIF) {
        p = append_text(p, end, "  variable fps");
    } else if (state->video.fps_den) {
        fps_int = state->video.fps_num / state->video.fps_den;
        fps_frac = ((state->video.fps_num % state->video.fps_den) * 10U) /
                   state->video.fps_den;
        p = append_text(p, end, "  ");
        p = append_uint(p, end, fps_int);
        if (fps_frac) {
            p = append_text(p, end, ".");
            p = append_uint(p, end, fps_frac);
        }
        p = append_text(p, end, " fps");
    }
    p = append_text(p, end, "  ");
    p = append_text(p, end, vr_quality_name(state->quality));
    if (state->render_stats.block_px > 1U) {
        p = append_text(p, end, "/");
        p = append_uint(p, end, state->render_stats.block_px);
        p = append_text(p, end, "px");
    } else if (state->render_stats.rectangles == 1U) {
        p = append_text(p, end, "/full");
    }
}

static void build_time_text(viewer_state_t *state, char *out, uint32_t capacity) {
    char a[16], b[16];
    char *p, *end;
    if (!out || !capacity) return;
    if (!state->loaded) { text_copy(out, capacity, "--:-- / --:--"); return; }
    format_time(viewer_frame_time_ms(state), a, sizeof(a));
    format_time(state->total_duration_ms, b, sizeof(b));
    out[0] = '\0'; p = out; end = out + capacity - 1U;
    p = append_text(p, end, a);
    p = append_text(p, end, " / ");
    (void)append_text(p, end, b);
}

static const char *viewer_audio_label(const viewer_state_t *state) {
    if (!state || !state->loaded || !vv_audio_available(&state->video))
        return "Video";
    if (!bk_sound_pcm_available()) return "PCM unavailable";
    return state->muted ? "Muted" : bk_sound_pcm_name();
}

static void viewer_paint(bk_gui_window_t *window UNUSED,
                         bk_gui_surface_t *surface, void *context) {
    viewer_state_t *state = (viewer_state_t *)context;
    viewer_layout_t l;
    bk_gui_rect_t c, video_target;
    char frame_text[72], info[112], qlabel[24], time_text[40];
    const char *movie_name;
    if (!state || !surface || !bk_gui_window_content_rect(state->window, &c)) return;
    viewer_layout(state, &l);
    bk_gui_surface_fill_rect(surface, c, VIEWER_FACE);

    /* Deep recessed picture well, typical of desktop media players of 1994-96. */
    rvui_bevel_panel(surface, l.video, true, 0x00303030U);
    bk_gui_surface_fill_rect(surface,
        (bk_gui_rect_t){l.video.x + 3, l.video.y + 3, l.video.w - 6, l.video.h - 6},
        VIEWER_BLACK);
    if (state->loaded) {
        video_target = viewer_fit_video(state,
            (bk_gui_rect_t){l.video.x + 4, l.video.y + 4, l.video.w - 8, l.video.h - 8});
        bk_gui_surface_fill_rect(surface, video_target, VIEWER_BLACK);
        if (!vv_prepare_frame(&state->video, state->frame) ||
            !vr_draw_frame(surface, &state->video, video_target,
                           state->quality, &state->render_stats)) {
            bk_gui_surface_draw_text(surface, l.video.x + 18, l.video.y + 18,
                "Video decode error", 0x00FF8080U, VIEWER_BLACK, false);
        }
    } else {
        const char *a = "No movie loaded";
        const char *b = "File > Open...  MPG/MPEG, AVI, BKV or animated GIF";
        int cx = l.video.x + l.video.w / 2;
        bk_gui_surface_draw_text(surface,
            cx - (int)bk_gui_text_width(a) / 2, l.video.y + l.video.h / 2 - 10,
            a, 0x00FFFFFFU, VIEWER_BLACK, false);
        bk_gui_surface_draw_text(surface,
            cx - (int)bk_gui_text_width(b) / 2, l.video.y + l.video.h / 2 + 8,
            b, 0x00A0A0A0U, VIEWER_BLACK, false);
    }

    build_time_text(state, time_text, sizeof(time_text));
    rvui_inset_text(surface, l.time, time_text, true);
    build_frame_text(state, frame_text, sizeof(frame_text));
    build_video_info(state, info, sizeof(info));
    if (frame_text[0])
        bk_gui_surface_draw_text(surface, l.time.x + l.time.w + 8, l.info.y + 7,
                                 frame_text, VIEWER_DIM, VIEWER_FACE, false);
    {
        int tx = l.info.x + l.info.w - (int)bk_gui_text_width(info);
        int min_tx = l.time.x + l.time.w + 120;
        if (tx < min_tx) tx = min_tx;
        bk_gui_surface_draw_text(surface, tx, l.info.y + 7,
                                 info, VIEWER_DIM, VIEWER_FACE, false);
    }

    rvui_slider(surface, l.timeline, state->loaded ? state->frame : 0U,
        state->loaded && state->video.frame_count > 1U ?
            state->video.frame_count - 1U : 1U, state->loaded);

    rvui_bevel_panel(surface, l.controls, false, VIEWER_FACE);
    rvui_button(surface, l.first, "|<", false, state->loaded);
    rvui_button(surface, l.prev, "<", false, state->loaded);
    rvui_icon_button(surface, l.play, state->playing ? "Pause" : "Play",
        state->playing ? (state->pause_icon_loaded ? &state->pause_icon : NULL)
                       : (state->play_icon_loaded ? &state->play_icon : NULL),
        state->playing, state->loaded);
    rvui_button(surface, l.stop, "Stop", false, state->loaded);
    rvui_button(surface, l.next, ">", false, state->loaded);
    rvui_button(surface, l.last, ">|", false, state->loaded);

    text_copy(qlabel, sizeof(qlabel), vr_quality_name(state->quality));
    rvui_button(surface, l.quality, qlabel, false, state->loaded);
    rvui_button(surface, l.loop, state->loop ? "Loop*" : "Loop",
                state->loop, state->loaded);
    rvui_icon_button(surface, l.sound, "",
        state->sound_icon_loaded ? &state->sound_icon : NULL,
        state->muted, state->loaded && vv_audio_available(&state->video));
    rvui_slider(surface, l.volume, state->muted ? 0U : state->volume,
                255U, state->loaded && vv_audio_available(&state->video));

    movie_name = state->loaded ? path_basename(state->path) : state->status;
    rvui_status_bar(surface, l.status, movie_name, viewer_audio_label(state));

    if (state->playing) {
        bk_gui_surface_fill_rect(surface,
            (bk_gui_rect_t){l.controls.x + 2, l.controls.y + 2, 3, l.controls.h - 4},
            VIEWER_GREEN);
    }
}

static void viewer_schedule_current(viewer_state_t *state, uint32_t now) {
    uint32_t duration = vv_frame_duration_ms(&state->video, state->frame);
    state->next_frame_ms = now + duration;
}

static void viewer_audio_stop(viewer_state_t *state) {
    if (!state) return;
    if (bk_sound_pcm_available()) bk_sound_stop();
    state->audio_cursor_valid = false;
}

static void viewer_audio_seek_to_video(viewer_state_t *state) {
    if (!state || !state->loaded || !vv_audio_available(&state->video)) return;
    vv_audio_seek_ms(&state->video, viewer_frame_time_ms(state),
                     &state->audio_chunk, &state->audio_chunk_offset);
    state->audio_cursor_valid = true;
}

static void viewer_set_frame(viewer_state_t *state, uint32_t frame) {
    if (!state || !state->loaded) return;
    if (frame >= state->video.frame_count) frame = state->video.frame_count - 1U;
    state->frame = frame;
    state->video.frame_prepared = false;
    state->video.frame_image_valid = false;
    viewer_schedule_current(state, bk_sys_uptime_ms());
    state->audio_cursor_valid = false;
    bk_gui_window_invalidate(state->window);
}

static void viewer_toggle_play(viewer_state_t *state) {
    if (!state || !state->loaded) return;
    if (!state->playing && state->frame + 1U >= state->video.frame_count)
        state->frame = 0U;
    state->playing = !state->playing;
    if (!state->playing) viewer_audio_stop(state);
    else {
        viewer_schedule_current(state, bk_sys_uptime_ms());
        viewer_audio_seek_to_video(state);
    }
    bk_gui_window_invalidate(state->window);
}

static void viewer_stop(viewer_state_t *state, bool rewind) {
    if (!state || !state->loaded) return;
    state->playing = false;
    viewer_audio_stop(state);
    if (rewind) {
        state->dropped_frames = 0U;
        viewer_set_frame(state, 0U);
    } else {
        bk_gui_window_invalidate(state->window);
    }
}

static void viewer_cycle_quality(viewer_state_t *state) {
    if (!state) return;
    state->quality = (vr_quality_t)(((uint32_t)state->quality + 1U) % VR_QUALITY_COUNT);
    bk_gui_window_invalidate(state->window);
}

static void viewer_set_quality(viewer_state_t *state, vr_quality_t quality) {
    if (!state || quality >= VR_QUALITY_COUNT) return;
    state->quality = quality;
    bk_gui_window_invalidate(state->window);
}

static void viewer_toggle_mute(viewer_state_t *state) {
    if (!state) return;
    state->muted = !state->muted;
    viewer_audio_stop(state);
    if (state->playing && !state->muted) viewer_audio_seek_to_video(state);
    bk_gui_window_invalidate(state->window);
}

static void viewer_seek_from_x(viewer_state_t *state, bk_gui_rect_t rect, int x) {
    int rel, span;
    uint32_t target;
    if (!state || !state->loaded || state->video.frame_count < 2U) return;
    span = rect.w - 10;
    if (span < 1) return;
    rel = x - (rect.x + 5);
    if (rel < 0) rel = 0;
    if (rel > span) rel = span;
    target = (uint32_t)rel * (state->video.frame_count - 1U) / (uint32_t)span;
    viewer_set_frame(state, target);
}

static void viewer_volume_from_x(viewer_state_t *state, bk_gui_rect_t rect, int x) {
    int rel, span;
    if (!state) return;
    span = rect.w - 10;
    if (span < 1) return;
    rel = x - (rect.x + 5);
    if (rel < 0) rel = 0;
    if (rel > span) rel = span;
    state->volume = (uint8_t)((uint32_t)rel * 255U / (uint32_t)span);
    state->muted = state->volume == 0U;
    viewer_audio_stop(state);
    if (state->playing && !state->muted) viewer_audio_seek_to_video(state);
    bk_gui_window_invalidate(state->window);
}

static bool viewer_event(bk_gui_window_t *window UNUSED,
                         const bk_gui_event_t *event, void *context) {
    viewer_state_t *state = (viewer_state_t *)context;
    viewer_layout_t l;
    if (!state || !event) return false;
    if (event->type == BK_GUI_EVENT_KEY) {
        if (event->ctrl && (event->key == 'o' || event->key == 'O')) {
            viewer_open_dialog(state);
            return true;
        }
        if (event->key == ' ' || event->key == 'p' || event->key == 'P') {
            viewer_toggle_play(state); return true;
        }
        if (event->key == 'q' || event->key == 'Q') {
            viewer_cycle_quality(state); return true;
        }
        if (event->key == 'm' || event->key == 'M') {
            viewer_toggle_mute(state); return true;
        }
        if (!state->loaded) return false;
        if (event->key == 'r' || event->key == 'R') {
            viewer_stop(state, true); return true;
        }
        if (event->key == 's' || event->key == 'S') {
            viewer_stop(state, false); return true;
        }
        if (event->key == 'l' || event->key == 'L') {
            state->loop = !state->loop;
            bk_gui_window_invalidate(state->window); return true;
        }
        if ((event->key == '[' || event->key == BK_KEY_LEFT) && state->frame > 0U) {
            viewer_stop(state, false); viewer_set_frame(state, state->frame - 1U); return true;
        }
        if ((event->key == ']' || event->key == BK_KEY_RIGHT) &&
            state->frame + 1U < state->video.frame_count) {
            viewer_stop(state, false); viewer_set_frame(state, state->frame + 1U); return true;
        }
        return false;
    }

    viewer_layout(state, &l);
    if (event->type == BK_GUI_EVENT_MOUSE_DOWN) {
        if (state->loaded && rvui_contains(l.timeline, event->x, event->y)) {
            state->dragging_timeline = true;
            state->resume_after_seek = state->playing;
            state->playing = false;
            viewer_audio_stop(state);
            viewer_seek_from_x(state, l.timeline, event->x);
            return true;
        }
        if (state->loaded && vv_audio_available(&state->video) &&
            rvui_contains(l.volume, event->x, event->y)) {
            state->dragging_volume = true;
            viewer_volume_from_x(state, l.volume, event->x);
            return true;
        }
        return false;
    }
    if (event->type == BK_GUI_EVENT_MOUSE_MOVE) {
        if (state->dragging_timeline) {
            viewer_seek_from_x(state, l.timeline, event->x); return true;
        }
        if (state->dragging_volume) {
            viewer_volume_from_x(state, l.volume, event->x); return true;
        }
        return false;
    }
    if (event->type != BK_GUI_EVENT_MOUSE_UP) return false;

    if (state->dragging_timeline) {
        state->dragging_timeline = false;
        viewer_seek_from_x(state, l.timeline, event->x);
        if (state->resume_after_seek) {
            state->playing = true;
            viewer_schedule_current(state, bk_sys_uptime_ms());
            viewer_audio_seek_to_video(state);
        }
        state->resume_after_seek = false;
        return true;
    }
    if (state->dragging_volume) {
        state->dragging_volume = false;
        viewer_volume_from_x(state, l.volume, event->x);
        return true;
    }
    if (!state->loaded) return false;
    if (rvui_contains(l.first, event->x, event->y)) {
        viewer_stop(state, false); viewer_set_frame(state, 0U);
    } else if (rvui_contains(l.prev, event->x, event->y)) {
        viewer_stop(state, false); viewer_set_frame(state, state->frame ? state->frame - 1U : 0U);
    } else if (rvui_contains(l.play, event->x, event->y)) {
        viewer_toggle_play(state);
    } else if (rvui_contains(l.stop, event->x, event->y)) {
        viewer_stop(state, true);
    } else if (rvui_contains(l.next, event->x, event->y)) {
        viewer_stop(state, false); viewer_set_frame(state, state->frame + 1U);
    } else if (rvui_contains(l.last, event->x, event->y)) {
        viewer_stop(state, false); viewer_set_frame(state, state->video.frame_count - 1U);
    } else if (rvui_contains(l.loop, event->x, event->y)) {
        state->loop = !state->loop; bk_gui_window_invalidate(state->window);
    } else if (rvui_contains(l.quality, event->x, event->y)) {
        viewer_cycle_quality(state);
    } else if (rvui_contains(l.sound, event->x, event->y) &&
               vv_audio_available(&state->video)) {
        viewer_toggle_mute(state);
    } else return false;
    return true;
}

static void viewer_unload(viewer_state_t *state) {
    if (!state) return;
    viewer_audio_stop(state);
    if (state->loaded) vv_close(&state->video);
    if (state->window) bk_gui_window_set_streaming(state->window, false);
    if (state->file_data) bk_sys_free(state->file_data);
    state->file_data = NULL;
    state->file_size = 0U;
    state->loaded = false;
    state->playing = false;
    state->frame = 0U;
    state->dropped_frames = 0U;
    state->total_duration_ms = 0U;
    state->path[0] = '\0';
    zero_memory(&state->video, sizeof(state->video));
}

static bool viewer_load(viewer_state_t *state, const char *path) {
    void *data = NULL;
    uint32_t size = 0;
    uint8_t magic[12];
    int fd;
    int32_t length;
    bool opened = false;
    vv_video_t video;
    zero_memory(&video, sizeof(video));
    if (!state || !path || !*path) return false;
    fd = bk_file_open(path, BK_FILE_READ_ONLY);
    if (fd < 0) goto read_fail;
    length = bk_file_size(fd);
    (void)bk_file_seek(fd, 0, 0U);
    if (length < 12 || bk_file_read(fd, magic, sizeof(magic)) != 12) {
        (void)bk_file_close(fd);
        goto read_fail;
    }
    (void)bk_file_close(fd);
    size = (uint32_t)length;

    /* Disk-backed MPEG-1/AVI: the complete compressed movie never needs
     * to fit in RAM. pl_mpeg owns independent seekable file cursors. */
    if (magic[0] == 0 && magic[1] == 0 && magic[2] == 1 && magic[3] == 0xBA)
        opened = vv_open_mpeg_stream(&video, path, size);
    else if (magic[0] == 'R' && magic[1] == 'I' &&
             magic[2] == 'F' && magic[3] == 'F' &&
             magic[8] == 'A' && magic[9] == 'V' &&
             magic[10] == 'I' && magic[11] == ' ')
        opened = vv_open_avi_stream(&video, path, size);
    else if (size <= 32U * 1024U * 1024U &&
             bk_file_read_all(path, &data, &size) && data && size)
        opened = vv_open(&video, data, size);

    if (!opened) {
        if (data) bk_sys_free(data);
        text_copy(state->status, sizeof(state->status),
                  "Unsupported codec/container or invalid movie (MPEG-1/AVI/BKV/GIF).");
        bk_gui_window_invalidate(state->window);
        return false;
    }
    if (!vv_prepare_frame(&video, 0U)) {
        vv_close(&video);
        if (data) bk_sys_free(data);
        text_copy(state->status, sizeof(state->status),
                  "Movie opened, but its first frame could not be decoded.");
        bk_gui_window_invalidate(state->window);
        return false;
    }

    viewer_unload(state);
    state->file_data = data;
    state->file_size = size;
    state->video = video;
    state->loaded = true;
    state->playing = false;
    state->loop = video.loop_default;
    state->frame = 0U;
    state->dropped_frames = 0U;
    state->total_duration_ms = vv_total_duration_ms(&state->video);
    state->quality = VR_QUALITY_AUTO;
    state->audio_cursor_valid = false;
    /* Ask the OS compositor for the fast opaque/video presentation path. */
    bk_gui_window_set_streaming(state->window, true);
    viewer_schedule_current(state, bk_sys_uptime_ms());
    text_copy(state->path, sizeof(state->path), path);
    text_copy(state->status, sizeof(state->status),
              (video.format == VV_FORMAT_AVI || video.format == VV_FORMAT_MPEG1)
              ? "Ready (streamed video)" : "Ready");
    bk_gui_window_invalidate(state->window);
    return true;
read_fail:
    text_copy(state->status, sizeof(state->status), "Could not read movie file.");
    bk_gui_window_invalidate(state->window);
    return false;
}

static bool viewer_advance_one(viewer_state_t *state) {
    if (state->frame + 1U < state->video.frame_count) {
        state->frame++;
    } else if (state->loop) {
        state->frame = 0U;
        viewer_audio_stop(state);
        viewer_audio_seek_to_video(state);
    } else {
        state->playing = false;
        viewer_audio_stop(state);
        return false;
    }
    state->video.frame_prepared = false;
    state->video.frame_image_valid = false;
    return true;
}

static void viewer_audio_tick(viewer_state_t *state) {
    uint32_t samples;
    uint16_t rate;
    if (!state || !state->playing || !state->loaded || state->muted ||
        !vv_audio_available(&state->video) || !bk_sound_pcm_available() ||
        !state->audio_buffer) return;
    if (bk_sound_pcm_busy()) return;
    if (!state->audio_cursor_valid) viewer_audio_seek_to_video(state);
    if (!state->audio_cursor_valid) return;
    samples = vv_audio_decode_u8_mono(&state->video,
        &state->audio_chunk, &state->audio_chunk_offset,
        state->audio_buffer, AUDIO_BUFFER_SIZE);
    if (!samples) return;
    rate = vv_audio_sample_rate(&state->video);
    if (!rate) return;
    if (!bk_sound_play_pcm_u8(state->audio_buffer, samples, rate, state->volume))
        state->audio_cursor_valid = false;
}

static void viewer_tick(viewer_state_t *state) {
    uint32_t now, advanced = 0U;
    if (!state || !state->loaded || !state->playing) return;
    viewer_audio_tick(state);
    now = bk_sys_uptime_ms();
    if ((int32_t)(now - state->next_frame_ms) < 0) return;
    do {
        if (!viewer_advance_one(state)) break;
        advanced++;
        state->next_frame_ms += vv_frame_duration_ms(&state->video, state->frame);
    } while (state->playing && advanced < 12U &&
             (int32_t)(now - state->next_frame_ms) >= 0);
    if (advanced > 1U) state->dropped_frames += advanced - 1U;
    if (advanced >= 12U && (int32_t)(now - state->next_frame_ms) >= 0)
        viewer_schedule_current(state, now);
    bk_gui_window_invalidate(state->window);
}

static void viewer_file_selected(const char *path, void *context) {
    viewer_state_t *state = (viewer_state_t *)context;
    if (!state || !path || !path[0]) return;
    (void)viewer_load(state, path);
}

static void viewer_open_dialog(viewer_state_t *state) {
    if (!state) return;
    if (!bk_file_dialog_open(state->desktop, "Open movie", "/", NULL, 0U,
                             viewer_file_selected, state)) {
        text_copy(state->status, sizeof(state->status), "Could not open file selector.");
        bk_gui_window_invalidate(state->window);
    }
}

static void viewer_menu(bk_gui_window_t *window UNUSED,
                        uint32_t item_id, void *context) {
    viewer_state_t *state = (viewer_state_t *)context;
    if (!state) return;
    switch (item_id) {
        case MENU_FILE_OPEN: viewer_open_dialog(state); break;
        case MENU_FILE_CLOSE: viewer_unload(state); bk_gui_window_invalidate(state->window); break;
        case MENU_FILE_EXIT: bk_gui_close_window(state->window); break;
        case MENU_PLAY_TOGGLE: viewer_toggle_play(state); break;
        case MENU_PLAY_STOP: viewer_stop(state, true); break;
        case MENU_PLAY_FIRST: viewer_stop(state, false); viewer_set_frame(state, 0U); break;
        case MENU_PLAY_PREV:
            if (state->loaded) { viewer_stop(state, false); viewer_set_frame(state, state->frame ? state->frame - 1U : 0U); }
            break;
        case MENU_PLAY_NEXT:
            if (state->loaded) { viewer_stop(state, false); viewer_set_frame(state, state->frame + 1U); }
            break;
        case MENU_PLAY_LAST:
            if (state->loaded) { viewer_stop(state, false); viewer_set_frame(state, state->video.frame_count - 1U); }
            break;
        case MENU_PLAY_LOOP:
            state->loop = !state->loop; bk_gui_window_invalidate(state->window); break;
        case MENU_VIDEO_AUTO: viewer_set_quality(state, VR_QUALITY_AUTO); break;
        case MENU_VIDEO_EXACT: viewer_set_quality(state, VR_QUALITY_EXACT); break;
        case MENU_VIDEO_FINE: viewer_set_quality(state, VR_QUALITY_FINE); break;
        case MENU_VIDEO_FAST: viewer_set_quality(state, VR_QUALITY_FAST); break;
        case MENU_VIDEO_TURBO: viewer_set_quality(state, VR_QUALITY_TURBO); break;
        case MENU_VIDEO_ULTRA: viewer_set_quality(state, VR_QUALITY_ULTRA); break;
        case MENU_AUDIO_MUTE: viewer_toggle_mute(state); break;
        case MENU_HELP_ABOUT:
            text_copy(state->status, sizeof(state->status),
                      "Viewer 3.2 - MPEG-1/MP2 + AVI player for BlesKernOS");
            bk_gui_window_invalidate(state->window);
            break;
        default: break;
    }
}

static void viewer_add_menus(viewer_state_t *state) {
    int file, play, video, audio, help;
    if (!state || !state->window) return;
    file = bk_gui_add_menu(state->window, "File");
    play = bk_gui_add_menu(state->window, "Playback");
    video = bk_gui_add_menu(state->window, "Video");
    audio = bk_gui_add_menu(state->window, "Audio");
    help = bk_gui_add_menu(state->window, "Help");
    if (file >= 0) {
        bk_gui_add_menu_item(state->window, file, MENU_FILE_OPEN, "Open...", viewer_menu, state);
        bk_gui_add_menu_item(state->window, file, MENU_FILE_CLOSE, "Close movie", viewer_menu, state);
        bk_gui_add_menu_item(state->window, file, MENU_FILE_EXIT, "Exit", viewer_menu, state);
    }
    if (play >= 0) {
        bk_gui_add_menu_item(state->window, play, MENU_PLAY_TOGGLE, "Play / Pause", viewer_menu, state);
        bk_gui_add_menu_item(state->window, play, MENU_PLAY_STOP, "Stop", viewer_menu, state);
        bk_gui_add_menu_item(state->window, play, MENU_PLAY_FIRST, "First frame", viewer_menu, state);
        bk_gui_add_menu_item(state->window, play, MENU_PLAY_PREV, "Previous frame", viewer_menu, state);
        bk_gui_add_menu_item(state->window, play, MENU_PLAY_NEXT, "Next frame", viewer_menu, state);
        bk_gui_add_menu_item(state->window, play, MENU_PLAY_LAST, "Last frame", viewer_menu, state);
        bk_gui_add_menu_item(state->window, play, MENU_PLAY_LOOP, "Loop", viewer_menu, state);
    }
    if (video >= 0) {
        bk_gui_add_menu_item(state->window, video, MENU_VIDEO_AUTO, "Quality: Auto", viewer_menu, state);
        bk_gui_add_menu_item(state->window, video, MENU_VIDEO_EXACT, "Quality: Exact", viewer_menu, state);
        bk_gui_add_menu_item(state->window, video, MENU_VIDEO_FINE, "Quality: Fine", viewer_menu, state);
        bk_gui_add_menu_item(state->window, video, MENU_VIDEO_FAST, "Quality: Fast", viewer_menu, state);
        bk_gui_add_menu_item(state->window, video, MENU_VIDEO_TURBO, "Quality: Turbo", viewer_menu, state);
        bk_gui_add_menu_item(state->window, video, MENU_VIDEO_ULTRA, "Quality: Ultra", viewer_menu, state);
    }
    if (audio >= 0)
        bk_gui_add_menu_item(state->window, audio, MENU_AUDIO_MUTE, "Mute / Unmute", viewer_menu, state);
    if (help >= 0)
        bk_gui_add_menu_item(state->window, help, MENU_HELP_ABOUT, "About Viewer", viewer_menu, state);
}

static void viewer_load_icons(viewer_state_t *state) {
    if (!state) return;
    state->play_icon_loaded = bk_graphics_icon_load("16/Mplayer10", &state->play_icon);
    state->pause_icon_loaded = bk_graphics_icon_load("16/Mplayer11", &state->pause_icon);
    state->sound_icon_loaded = bk_graphics_icon_load("16/SOUND", &state->sound_icon);
}

static void viewer_free_icons(viewer_state_t *state) {
    if (!state) return;
    if (state->play_icon_loaded) bk_gui_image_free(&state->play_icon);
    if (state->pause_icon_loaded) bk_gui_image_free(&state->pause_icon);
    if (state->sound_icon_loaded) bk_gui_image_free(&state->sound_icon);
}

void bleskernos_program_main(void *unused) {
    viewer_state_t *state;
    bk_gui_desktop_t *desktop;
    char argument[BK_PATH_MAX];
    (void)unused;
    if (bk_sys_api_version() < VIEWER_MIN_API) return;
    desktop = bk_gui_desktop();
    if (!desktop) return;
    state = (viewer_state_t *)bk_sys_alloc(sizeof(*state));
    if (!state) return;
    zero_memory(state, sizeof(*state));
    state->desktop = desktop;
    state->quality = VR_QUALITY_AUTO;
    state->volume = 220U;
    state->audio_buffer = (uint8_t *)bk_sys_alloc(AUDIO_BUFFER_SIZE);
    text_copy(state->status, sizeof(state->status), "Viewer 3.2 - Multimedia Player");

    state->window = bk_gui_create_window(desktop, 44, 28, 720, 540, "Viewer 3.2");
    if (!state->window) {
        if (state->audio_buffer) bk_sys_free(state->audio_buffer);
        bk_sys_free(state);
        return;
    }
    viewer_add_menus(state);
    viewer_load_icons(state);
    bk_gui_set_window_content(state->window, viewer_paint, state);
    bk_gui_set_window_event_handler(state->window, viewer_event, state);
    bk_gui_set_window_min_size(state->window, 560, 410);
    bk_gui_window_set_owner(state->window, bk_sys_getpid());
    bk_proc_bind_window(state->window);

    argument[0] = '\0';
    if (bk_proc_launch_arg_copy(argument, sizeof(argument))) {
        trim_argument(argument);
        if (argument[0]) (void)viewer_load(state, argument);
    }
    bk_gui_window_invalidate(state->window);
    while (bk_gui_window_is_open(state->window) && !bk_proc_exit_requested()) {
        viewer_tick(state);
        bk_sys_sleep_ms(state->playing ? 2U : 8U);
    }

    viewer_audio_stop(state);
    bk_proc_bind_window(NULL);
    viewer_unload(state);
    if (state->window) bk_gui_destroy_window(desktop, state->window);
    state->window = NULL;
    viewer_free_icons(state);
    if (state->audio_buffer) bk_sys_free(state->audio_buffer);
    bk_sys_free(state);
}
