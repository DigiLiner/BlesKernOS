#ifndef BLES_ARCH_ARM32_RPI3_SD_H
#define BLES_ARCH_ARM32_RPI3_SD_H

#include "../../../kernel/include/types.h"

typedef struct {
    uint32_t rca;
    uint32_t ocr;
    uint32_t base_clock_hz;
    uint32_t bus_clock_hz;
    uint32_t last_interrupt;
    bool initialized;
    bool high_capacity;
    bool cmd8_supported;
} rpi3_sd_info_t;

int rpi3_sd_init(void);
int rpi3_sd_read_block(uint32_t lba, void *buffer512);
int rpi3_sd_read_blocks(uint32_t lba, uint32_t count, void *buffer);
const rpi3_sd_info_t *rpi3_sd_info(void);

#endif
