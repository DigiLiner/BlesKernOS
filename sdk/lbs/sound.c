#include <bleskernos_sound_lbs.h>
int bk_sound_mix_u8(uint8_t *out, uint32_t n, const uint8_t *const *in, const uint16_t *gain,
                    uint32_t count) {
    uint32_t i, c;
    int sum;
    if ((!out && n) || (!in && count) || (!gain && count) || count > 32)
        return BK_LBS_INVALID;
    for (c = 0; c < count; c++)
        if (!in[c] || gain[c] > 256)
            return BK_LBS_INVALID;
    for (i = 0; i < n; i++) {
        sum = 0;
        for (c = 0; c < count; c++)
            sum += ((int)in[c][i] - 128) * (int)gain[c] / 256;
        if (sum > 127)
            sum = 127;
        if (sum < -128)
            sum = -128;
        out[i] = sum + 128;
    }
    return 0;
}

extern uint8_t bk_system_sound_master_volume(void);
extern void bk_system_sound_set_master_volume(uint8_t percent);
uint32_t bk_sound_volume(void) {
    return bk_system_sound_master_volume();
}
int bk_sound_set_volume(uint32_t percent) {
    if (percent > 100)
        return BK_LBS_INVALID;
    bk_system_sound_set_master_volume((uint8_t)percent);
    return 0;
}
