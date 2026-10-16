#include "irq.h"
#include "mmio.h"
#include "task_arm.h"
#include "syscall_arm.h"

#define PERIPHERAL_BASE   0x3F000000U
#define IRQ_BASE          (PERIPHERAL_BASE + 0xB200U)
#define IRQ_PENDING_1     (IRQ_BASE + 0x04U)
#define IRQ_PENDING_2     (IRQ_BASE + 0x08U)
#define ENABLE_IRQS_1     (IRQ_BASE + 0x10U)
#define ENABLE_IRQS_2     (IRQ_BASE + 0x14U)
#define DISABLE_IRQS_1    (IRQ_BASE + 0x1CU)
#define DISABLE_IRQS_2    (IRQ_BASE + 0x20U)

typedef struct {
    rpi3_irq_handler_t handler;
    void *opaque;
} irq_slot_t;

static irq_slot_t g_irq_slots[RPI3_IRQ_GPU_COUNT];

void rpi3_irq_init(void) {
    uint32_t i;

    /* Do not inherit enabled GPU interrupt sources from firmware. */
    mmio_write(DISABLE_IRQS_1, 0xFFFFFFFFU);
    mmio_write(DISABLE_IRQS_2, 0xFFFFFFFFU);
    for (i = 0U; i < RPI3_IRQ_GPU_COUNT; ++i) {
        g_irq_slots[i].handler = (rpi3_irq_handler_t)0;
        g_irq_slots[i].opaque = (void *)0;
    }
    arm_dmb();
}

int rpi3_irq_register(uint32_t irq, rpi3_irq_handler_t handler, void *opaque) {
    if (irq >= RPI3_IRQ_GPU_COUNT || handler == (rpi3_irq_handler_t)0)
        return -1;

    rpi3_irq_disable_source(irq);
    g_irq_slots[irq].opaque = opaque;
    arm_dmb();
    g_irq_slots[irq].handler = handler;
    arm_dmb();
    return 0;
}

void rpi3_irq_enable_source(uint32_t irq) {
    if (irq < 32U)
        mmio_write(ENABLE_IRQS_1, 1U << irq);
    else if (irq < 64U)
        mmio_write(ENABLE_IRQS_2, 1U << (irq - 32U));
    arm_dmb();
}

void rpi3_irq_disable_source(uint32_t irq) {
    if (irq < 32U)
        mmio_write(DISABLE_IRQS_1, 1U << irq);
    else if (irq < 64U)
        mmio_write(DISABLE_IRQS_2, 1U << (irq - 32U));
    arm_dmb();
}

void rpi3_irq_enable(void) {
    __asm__ volatile("cpsie i" ::: "memory");
}

void rpi3_irq_disable(void) {
    __asm__ volatile("cpsid i" ::: "memory");
}

uint32_t rpi3_irq_pending1(void) {
    return mmio_read(IRQ_PENDING_1);
}

uint32_t rpi3_irq_pending2(void) {
    return mmio_read(IRQ_PENDING_2);
}

/* Called by the assembly IRQ vector. The legacy BCM283x controller exposes
   GPU interrupts in two 32-bit pending banks. Stage 2 keeps dispatch generic
   so drivers added later do not need to edit vectors.S. */
arm_task_frame_t *arm_irq_dispatch(arm_task_frame_t *frame) {
    uint32_t bank;
    uint32_t bit;
    uint32_t pending;
    pending = rpi3_irq_pending1();
    for (bit = 0U; bit < 32U; ++bit) {
        if ((pending & (1U << bit)) != 0U) {
            irq_slot_t *slot = &g_irq_slots[bit];
            if (slot->handler != (rpi3_irq_handler_t)0)
                slot->handler(bit, slot->opaque);
            else
                rpi3_irq_disable_source(bit);
        }
    }

    bank = rpi3_irq_pending2();
    for (bit = 0U; bit < 32U; ++bit) {
        if ((bank & (1U << bit)) != 0U) {
            uint32_t irq = bit + 32U;
            irq_slot_t *slot = &g_irq_slots[irq];
            if (slot->handler != (rpi3_irq_handler_t)0)
                slot->handler(irq, slot->opaque);
            else
                rpi3_irq_disable_source(irq);
        }
    }
    return rpi3_task_irq_schedule(frame);
}

arm_task_frame_t *arm_svc_dispatch(arm_task_frame_t *frame) {
    if (frame && (frame->cpsr & 0x1FU) == 0x10U)
        return rpi3_syscall_dispatch(frame);
    return rpi3_task_svc_schedule(frame);
}
