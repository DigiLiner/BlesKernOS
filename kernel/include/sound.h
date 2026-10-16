#ifndef SOUND_H
#define SOUND_H

#include "types.h"

#define SOUND_DEFAULT_MASTER_VOLUME 30U

/* Serial logging must not delay PCM refill or short GUI sounds. Enable only
 * for an audio diagnostic build with -DBLESK_AUDIO_DEBUG=1. Disabled calls
 * also skip argument evaluation (including controller register reads). */
#if defined(BLESK_AUDIO_DEBUG) && BLESK_AUDIO_DEBUG
#define SOUND_LOG(...) kprintf(__VA_ARGS__)
#else
#define SOUND_LOG(...) do { if (0) kprintf(__VA_ARGS__); } while (0)
#endif


typedef struct {
    void (*poll)(void);
    void (*play)(uint32_t frequency_hz);
    void (*stop)(void);
    void (*beep)(uint32_t frequency_hz, uint32_t duration_ms);
    bool (*start_tone)(uint32_t frequency_hz, uint32_t duration_ms);
    bool (*play_pcm_u8)(const uint8_t *samples, uint32_t length,
                        uint16_t sample_rate_hz, uint8_t volume);
    bool (*pcm_available)(void);
    bool (*pcm_is_busy)(void);
    bool (*has_sb16)(void);
    bool (*sb16_play_tone)(uint32_t frequency_hz, uint32_t duration_ms);
    bool (*sb16_is_busy)(void);
    const char *(*pcm_name)(void);
    /* Gana el controlador disponible con mayor prioridad. */
    uint32_t priority;
} sound_driver_ops_t;

bool sound_register_driver(const sound_driver_ops_t *ops);

void sound_init(void);
void sound_play(uint32_t frequency_hz);
void sound_stop(void);
void sound_beep(uint32_t frequency_hz, uint32_t duration_ms);
bool sound_start_tone(uint32_t frequency_hz, uint32_t duration_ms);
bool sound_play_pcm_u8(const uint8_t *samples, uint32_t length,
                       uint16_t sample_rate_hz, uint8_t volume);
/* Global PCM master gain, applied after each source/voice volume. */
void sound_set_master_volume(uint8_t percent);
uint8_t sound_get_master_volume(void);
/* Reproduce WAV PCM 8-bit mono en una tarea asincrona. */
bool sound_play_file(const char *path);
void sound_poll(void);
bool sound_pcm_available(void);
bool sound_pcm_is_busy(void);
bool sound_has_sb16(void);
bool sound_sb16_play_tone(uint32_t frequency_hz, uint32_t duration_ms);
bool sound_sb16_is_busy(void);
const char *sound_pcm_name(void);

#endif
