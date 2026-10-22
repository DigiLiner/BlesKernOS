#ifndef BLESKERNOS_SOUND_LBS_H
#define BLESKERNOS_SOUND_LBS_H
#include <bleskernos_lbs.h>
/* Saturating u8 mono mixer; channels must have matching sample rates.
   gain is 0..256 per channel; caller owns input/output and playback lifetime. */
int bk_sound_mix_u8(uint8_t *out, uint32_t samples, const uint8_t *const *channels,
                    const uint16_t *gain, uint32_t count);
uint32_t bk_sound_volume(void);
int bk_sound_set_volume(uint32_t percent);
#endif
