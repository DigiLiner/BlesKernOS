#ifndef BLES_ARCH_ARM32_RPI3_IRQ_H
#define BLES_ARCH_ARM32_RPI3_IRQ_H

#include "../../../kernel/include/types.h"

#define RPI3_IRQ_GPU_COUNT 64U

typedef void (*rpi3_irq_handler_t)(uint32_t irq, void *opaque);

void rpi3_irq_init(void);
void rpi3_irq_enable(void);
void rpi3_irq_disable(void);
int rpi3_irq_register(uint32_t irq, rpi3_irq_handler_t handler, void *opaque);
void rpi3_irq_enable_source(uint32_t irq);
void rpi3_irq_disable_source(uint32_t irq);
uint32_t rpi3_irq_pending1(void);
uint32_t rpi3_irq_pending2(void);

#endif
