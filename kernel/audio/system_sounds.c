#include "../include/system_sounds.h"
#include "../include/sound.h"
#include "../include/startup_sound.h"
#include "../include/user_config.h"
#include "../include/vfs.h"
#include "../include/memory.h"
#include "../include/task.h"
#include "../include/pit.h"
#include "../include/vga.h"
#include "../libc/include/string.h"
#include "../libc/include/stdio.h"

typedef struct {
    const char *path;
    uint8_t *file;
    const uint8_t *samples;
    uint32_t sample_bytes;
    uint16_t sample_rate;
    bool available;
} system_sound_clip_t;

static system_sound_clip_t g_clips[SYSTEM_SOUND_EVENT_COUNT] = {
    {BK_SYSTEM_SOUND_OPEN_PATH, NULL, NULL, 0U, 0U, false},
    {BK_SYSTEM_SOUND_CLOSE_PATH, NULL, NULL, 0U, 0U, false},
    {BK_SYSTEM_SOUND_MINIMIZE_PATH, NULL, NULL, 0U, 0U, false},
    {BK_SYSTEM_SOUND_RESTORE_PATH, NULL, NULL, 0U, 0U, false},
    {BK_SYSTEM_SOUND_RESIZE_PATH, NULL, NULL, 0U, 0U, false},
    {BK_SYSTEM_SOUND_BUTTON_PATH, NULL, NULL, 0U, 0U, false},
    {BK_SYSTEM_SOUND_SCROLL_PATH, NULL, NULL, 0U, 0U, false},
};

static bool g_initialized;
static bool g_master_enabled = true;
static uint8_t g_master_volume = SOUND_DEFAULT_MASTER_VOLUME;
static bool g_startup_enabled = true;
static bool g_event_enabled[SYSTEM_SOUND_EVENT_COUNT] = {
    true, true, true, true, true, true, true
};
static volatile bool g_resize_active;
static uint32_t g_last_play_tick[SYSTEM_SOUND_EVENT_COUNT];

static uint16_t ss_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t ss_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool ss_id(const uint8_t *p, const char *id) {
    return p && id && kmemcmp(p, id, 4) == 0;
}

static bool ss_parse_bool(const char *text, const char *key, bool fallback) {
    size_t key_length;
    if (!text || !key) return fallback;
    key_length = kstrlen(key);
    while (*text) {
        const char *line = text;
        const char *end = line;
        while (*end && *end != '\r' && *end != '\n') end++;
        if ((size_t)(end - line) > key_length &&
            kstrncmp(line, key, key_length) == 0 && line[key_length] == '=')
            return line[key_length + 1U] != '0';
        text = end;
        while (*text == '\r' || *text == '\n') text++;
    }
    return fallback;
}

static uint32_t ss_parse_u32(const char *text, const char *key, uint32_t fallback) {
    size_t key_length;
    if (!text || !key) return fallback;
    key_length = kstrlen(key);
    while (*text) {
        const char *line = text;
        const char *end = line;
        uint32_t value = 0U;
        bool any = false;
        while (*end && *end != '\r' && *end != '\n') end++;
        if ((size_t)(end - line) > key_length &&
            kstrncmp(line, key, key_length) == 0 && line[key_length] == '=') {
            const char *p = line + key_length + 1U;
            while (p < end && *p >= '0' && *p <= '9') {
                any = true;
                value = value * 10U + (uint32_t)(*p - '0');
                if (value > 1000U) break;
                p++;
            }
            return any ? value : fallback;
        }
        text = end;
        while (*text == '\r' || *text == '\n') text++;
    }
    return fallback;
}

static bool ss_load_clip(system_sound_clip_t *clip) {
    uint32_t size = 0U;
    uint16_t format = 0U, channels = 0U, bits = 0U;
    uint32_t rate = 0U;
    const uint8_t *samples = NULL;
    uint32_t sample_bytes = 0U;
    bool have_format = false;

    if (!clip || !clip->path) return false;
    if (!vfs_read_all(clip->path, (void **)&clip->file, &size) || !clip->file)
        return false;
    (void)mm_set_allocation_subsystem(clip->file,MM_SUBSYSTEM_AUDIO);
    if (size < 12U || !ss_id(clip->file, "RIFF") ||
        !ss_id(clip->file + 8U, "WAVE")) goto invalid;

    for (uint32_t offset = 12U; offset + 8U <= size;) {
        const uint8_t *chunk = clip->file + offset;
        uint32_t chunk_size = ss_le32(chunk + 4U);
        uint32_t data_offset = offset + 8U;
        uint32_t padded;
        if (chunk_size > size - data_offset) goto invalid;
        if (ss_id(chunk, "fmt ")) {
            if (chunk_size < 16U) goto invalid;
            format = ss_le16(clip->file + data_offset);
            channels = ss_le16(clip->file + data_offset + 2U);
            rate = ss_le32(clip->file + data_offset + 4U);
            bits = ss_le16(clip->file + data_offset + 14U);
            have_format = true;
        } else if (ss_id(chunk, "data") && !samples) {
            samples = clip->file + data_offset;
            sample_bytes = chunk_size;
        }
        padded = chunk_size + (chunk_size & 1U);
        if (padded > size - data_offset) break;
        offset = data_offset + padded;
    }

    if (!have_format || !samples || !sample_bytes || format != 1U ||
        channels != 1U || bits != 8U || rate < 4000U || rate > 44100U)
        goto invalid;

    clip->samples = samples;
    clip->sample_bytes = sample_bytes;
    clip->sample_rate = (uint16_t)rate;
    clip->available = true;
    return true;

invalid:
    kfree(clip->file);
    clip->file = NULL;
    clip->samples = NULL;
    clip->sample_bytes = 0U;
    clip->sample_rate = 0U;
    clip->available = false;
    return false;
}

static bool ss_save_config(void) {
    char text[640];
    int length = snprintf(text, sizeof(text),
        "[SOUND]\r\n"
        "StartupEnabled=%u\r\n"
        "StartupSound=/SYSTEM/SOUNDS/START.WAV\r\n"
        "SystemEventsEnabled=%u\r\n"
        "MasterVolume=%u\r\n"
        "WindowOpen=%u\r\n"
        "WindowClose=%u\r\n"
        "WindowMinimize=%u\r\n"
        "WindowRestore=%u\r\n"
        "WindowResize=%u\r\n"
        "Button=%u\r\n"
        "Scroll=%u\r\n",
        g_startup_enabled ? 1U : 0U,
        g_master_enabled ? 1U : 0U,
        (uint32_t)g_master_volume,
        g_event_enabled[SYSTEM_SOUND_WINDOW_OPEN] ? 1U : 0U,
        g_event_enabled[SYSTEM_SOUND_WINDOW_CLOSE] ? 1U : 0U,
        g_event_enabled[SYSTEM_SOUND_WINDOW_MINIMIZE] ? 1U : 0U,
        g_event_enabled[SYSTEM_SOUND_WINDOW_RESTORE] ? 1U : 0U,
        g_event_enabled[SYSTEM_SOUND_WINDOW_RESIZE] ? 1U : 0U,
        g_event_enabled[SYSTEM_SOUND_BUTTON] ? 1U : 0U,
        g_event_enabled[SYSTEM_SOUND_SCROLL] ? 1U : 0U);
    if (length <= 0 || (uint32_t)length >= sizeof(text)) return false;
    return bk_user_config_write_all(BK_SOUND_CONFIG_PATH, text,
                                    (uint32_t)length);
}

void system_sounds_init(void) {
    void *buffer = NULL;
    uint32_t size = 0U;
    const char *text = NULL;
    if (g_initialized) return;
    g_initialized = true;

    if (bk_user_config_read_all(BK_SOUND_CONFIG_PATH, "/SOUND.INI",
                                &buffer, &size) && buffer) {
        text = (const char *)buffer;
        g_startup_enabled = ss_parse_bool(text, "StartupEnabled", true);
        g_master_enabled = ss_parse_bool(text, "SystemEventsEnabled", true);
        {
            uint32_t volume = ss_parse_u32(text, "MasterVolume", SOUND_DEFAULT_MASTER_VOLUME);
            if (volume > 100U) volume = 100U;
            g_master_volume = (uint8_t)volume;
        }
        g_event_enabled[SYSTEM_SOUND_WINDOW_OPEN] =
            ss_parse_bool(text, "WindowOpen", true);
        g_event_enabled[SYSTEM_SOUND_WINDOW_CLOSE] =
            ss_parse_bool(text, "WindowClose", true);
        g_event_enabled[SYSTEM_SOUND_WINDOW_MINIMIZE] =
            ss_parse_bool(text, "WindowMinimize", true);
        g_event_enabled[SYSTEM_SOUND_WINDOW_RESTORE] =
            ss_parse_bool(text, "WindowRestore", true);
        g_event_enabled[SYSTEM_SOUND_WINDOW_RESIZE] =
            ss_parse_bool(text, "WindowResize", true);
        g_event_enabled[SYSTEM_SOUND_BUTTON] =
            ss_parse_bool(text, "Button", true);
        g_event_enabled[SYSTEM_SOUND_SCROLL] =
            ss_parse_bool(text, "Scroll", true);
        kfree(buffer);
    }

    sound_set_master_volume(g_master_volume);

    if (!sound_pcm_available()) {
        SOUND_LOG("[SOUND:EVENT] sin backend PCM; sonidos GUI desactivados en esta sesion\n");
        return;
    }

    for (uint32_t i = 0U; i < SYSTEM_SOUND_EVENT_COUNT; i++) {
        if (ss_load_clip(&g_clips[i]))
            SOUND_LOG("[SOUND:EVENT] precargado %s (%u bytes @ %u Hz)\n",
                    g_clips[i].path, g_clips[i].sample_bytes,
                    g_clips[i].sample_rate);
        else
            SOUND_LOG("[SOUND:EVENT] opcional ausente/invalido: %s\n",
                    g_clips[i].path);
    }
}

bool system_sounds_enabled(void) {
    system_sounds_init();
    return g_master_enabled;
}

bool system_sounds_set_enabled(bool enabled) {
    bool old;
    system_sounds_init();
    old = g_master_enabled;
    g_master_enabled = enabled;
    if (!enabled) g_resize_active = false;
    if (ss_save_config()) return true;
    g_master_enabled = old;
    return false;
}

uint8_t system_sounds_master_volume(void) {
    system_sounds_init();
    return g_master_volume;
}

bool system_sounds_set_master_volume(uint8_t percent) {
    uint8_t old;
    system_sounds_init();
    if (percent > 100U) percent = 100U;
    old = g_master_volume;
    g_master_volume = percent;
    sound_set_master_volume(percent);
    if (ss_save_config()) return true;
    g_master_volume = old;
    sound_set_master_volume(old);
    return false;
}

bool system_sounds_startup_enabled(void) {
    system_sounds_init();
    return g_startup_enabled;
}

bool system_sounds_set_startup_enabled(bool enabled) {
    bool old;
    system_sounds_init();
    old = g_startup_enabled;
    g_startup_enabled = enabled;
    if (ss_save_config()) return true;
    g_startup_enabled = old;
    return false;
}

bool system_sound_event_enabled(system_sound_event_t event) {
    system_sounds_init();
    if ((uint32_t)event >= SYSTEM_SOUND_EVENT_COUNT) return false;
    return g_event_enabled[event];
}

bool system_sound_event_set_enabled(system_sound_event_t event, bool enabled) {
    bool old;
    system_sounds_init();
    if ((uint32_t)event >= SYSTEM_SOUND_EVENT_COUNT) return false;
    old = g_event_enabled[event];
    g_event_enabled[event] = enabled;
    if (event == SYSTEM_SOUND_WINDOW_RESIZE && !enabled)
        g_resize_active = false;
    if (ss_save_config()) return true;
    g_event_enabled[event] = old;
    return false;
}

bool system_sound_event_available(system_sound_event_t event) {
    system_sounds_init();
    if ((uint32_t)event >= SYSTEM_SOUND_EVENT_COUNT) return false;
    return g_clips[event].available;
}

bool system_sound_event_play(system_sound_event_t event) {
    system_sound_clip_t *clip;
    uint32_t now;
    uint32_t min_ms = 0U;
    uint32_t min_ticks;
    system_sounds_init();
    if ((uint32_t)event >= SYSTEM_SOUND_EVENT_COUNT || !g_master_enabled ||
        !g_event_enabled[event]) return false;
    clip = &g_clips[event];
    if (!clip->available || !sound_pcm_available()) return false;

    /* Pointer-driven events may arrive hundreds of times per second.  Limit
       only those events so the mixer cannot fill all voices while dragging. */
    if (event == SYSTEM_SOUND_WINDOW_RESIZE) min_ms = 48U;
    else if (event == SYSTEM_SOUND_SCROLL) min_ms = 36U;
    if (min_ms) {
        uint32_t hz = pit_get_frequency_hz();
        if (!hz) hz = 100U;
        min_ticks = (uint32_t)(((uint64_t)hz * min_ms + 999ULL) / 1000ULL);
        if (!min_ticks) min_ticks = 1U;
        now = pit_get_ticks();
        if (g_last_play_tick[event] &&
            (uint32_t)(now - g_last_play_tick[event]) < min_ticks)
            return false;
        g_last_play_tick[event] = now;
    }

    /* Per-event WAVs are authored at their intended relative loudness.  Feed
       them at full source gain; sound_core applies the user's master volume. */
    return sound_play_pcm_u8(clip->samples, clip->sample_bytes,
                             clip->sample_rate, 255U);
}

void system_sound_resize_begin(void) {
    system_sounds_init();
    g_resize_active = true;
    (void)system_sound_event_play(SYSTEM_SOUND_WINDOW_RESIZE);
}

void system_sound_resize_end(void) {
    g_resize_active = false;
}

