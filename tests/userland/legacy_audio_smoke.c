#include "../../kernel/include/api.h"
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;
    bool available = bk_sound_pcm_available();
    kprintf("[DRIVERTEST] audio=%s available=%u\n", bk_sound_pcm_name(), available);
    bool played = available && bk_sound_tone(440, 800);
    bk_sys_sleep_ms(1000);
    bk_sound_stop();
    uint8_t samples[4096];
    for (unsigned i = 0; i < sizeof(samples); i++)
        samples[i] = (i % 40 < 20) ? 80 : 176;
    bool pcm = available && bk_sound_play_pcm_u8(samples, sizeof(samples), 22050, 200);
    bk_sys_sleep_ms(250);
    bk_sound_stop();
    kprintf("[DRIVERTEST] DONE result=%s\n", played && pcm ? "PASS" : "FAIL");
}
