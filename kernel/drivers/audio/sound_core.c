#include "../../include/sound.h"

static const sound_driver_ops_t *g_sound_driver;
static uint8_t g_sound_master_volume = SOUND_DEFAULT_MASTER_VOLUME;

bool sound_register_driver(const sound_driver_ops_t *ops) {
    if (!ops || !ops->poll || !ops->play || !ops->stop || !ops->beep ||
        !ops->start_tone || !ops->play_pcm_u8 || !ops->pcm_available ||
        !ops->pcm_is_busy || !ops->has_sb16 || !ops->sb16_play_tone ||
        !ops->sb16_is_busy || !ops->pcm_name) return false;
    if (!g_sound_driver || ops->priority > g_sound_driver->priority)
        g_sound_driver = ops;
    return true;
}

void sound_init(void) {}
void sound_poll(void) { if (g_sound_driver) g_sound_driver->poll(); }
void sound_play(uint32_t hz) { if (g_sound_driver) g_sound_driver->play(hz); }
void sound_stop(void) { if (g_sound_driver) g_sound_driver->stop(); }
void sound_beep(uint32_t hz, uint32_t ms) {
    if (g_sound_driver) g_sound_driver->beep(hz, ms);
}
bool sound_start_tone(uint32_t hz, uint32_t ms) {
    return g_sound_driver && g_sound_driver->start_tone(hz, ms);
}
bool sound_play_pcm_u8(const uint8_t *samples, uint32_t length,
                       uint16_t rate, uint8_t volume) {
    uint32_t scaled;
    if (!g_sound_driver) return false;
    scaled = ((uint32_t)volume * (uint32_t)g_sound_master_volume + 50U) / 100U;
    if (scaled > 255U) scaled = 255U;
    return g_sound_driver->play_pcm_u8(samples, length, rate, (uint8_t)scaled);
}

void sound_set_master_volume(uint8_t percent) {
    g_sound_master_volume = percent > 100U ? 100U : percent;
}

uint8_t sound_get_master_volume(void) {
    return g_sound_master_volume;
}
bool sound_pcm_available(void) {
    return g_sound_driver && g_sound_driver->pcm_available();
}
bool sound_pcm_is_busy(void) {
    return g_sound_driver && g_sound_driver->pcm_is_busy();
}
bool sound_has_sb16(void) {
    return g_sound_driver && g_sound_driver->has_sb16();
}
bool sound_sb16_play_tone(uint32_t hz, uint32_t ms) {
    return g_sound_driver && g_sound_driver->sb16_play_tone(hz, ms);
}
bool sound_sb16_is_busy(void) {
    return g_sound_driver && g_sound_driver->sb16_is_busy();
}
const char *sound_pcm_name(void) {
    return g_sound_driver ? g_sound_driver->pcm_name() : "driver-not-loaded";
}
