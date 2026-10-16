#ifndef BLES_ARCH_ARM32_RPI3_MMIO_H
#define BLES_ARCH_ARM32_RPI3_MMIO_H

#include "../../../kernel/include/types.h"

static inline void mmio_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)reg = value;
}

static inline uint32_t mmio_read(uint32_t reg) {
    return *(volatile uint32_t *)(uintptr_t)reg;
}

static inline void arm_dmb(void) {
    __asm__ volatile("dmb sy" ::: "memory");
}

#endif
