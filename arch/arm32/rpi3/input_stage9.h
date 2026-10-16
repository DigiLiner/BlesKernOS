#ifndef BLES_ARCH_ARM32_RPI3_INPUT_STAGE9_H
#define BLES_ARCH_ARM32_RPI3_INPUT_STAGE9_H

#include "../../../kernel/include/types.h"

int rpi3_stage9_input_init(void);
void rpi3_stage9_input_poll(void);
bool rpi3_stage9_usb_ready(void);

#endif
