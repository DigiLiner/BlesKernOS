#ifndef BLES_ARCH_ARM32_RPI3_TIMER_H
#define BLES_ARCH_ARM32_RPI3_TIMER_H

#include "../../../kernel/include/types.h"

#define RPI3_TIMER_HZ 100U

void rpi3_timer_init(void);
void rpi3_timer_handle_irq(void);
uint32_t rpi3_timer_ticks(void);
uint32_t rpi3_timer_now_us(void);
void rpi3_timer_delay_us(uint32_t usec);

#endif
