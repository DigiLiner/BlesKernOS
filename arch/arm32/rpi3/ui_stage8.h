#ifndef BLES_ARCH_ARM32_RPI3_UI_STAGE8_H
#define BLES_ARCH_ARM32_RPI3_UI_STAGE8_H

#include "../../../kernel/include/types.h"

int rpi3_stage8_video_bootstrap(void);
void rpi3_stage8_boot_status(const char *text, uint8_t progress);
int rpi3_stage8_shell_init(void);
void rpi3_stage8_service(void);

#endif
