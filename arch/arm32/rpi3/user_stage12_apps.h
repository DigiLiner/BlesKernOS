#ifndef BLES_ARCH_ARM32_RPI3_USER_STAGE12_APPS_H
#define BLES_ARCH_ARM32_RPI3_USER_STAGE12_APPS_H

#include "../../../kernel/include/types.h"

int rpi3_stage12_spawn_command(const char *command, const char *launch_line);
uint32_t rpi3_stage12_command_count(void);
const char *rpi3_stage12_command_name(uint32_t index);

#endif
