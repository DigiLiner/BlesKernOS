#include "exceptions.h"
#include "uart.h"
#include "task_arm.h"

extern unsigned char arm_vector_table[];

static const char *exception_name(uint32_t kind) {
    switch (kind) {
        case ARM_EXC_RESET:          return "reset";
        case ARM_EXC_UNDEFINED:      return "undefined instruction";
        case ARM_EXC_SVC:            return "supervisor call";
        case ARM_EXC_PREFETCH_ABORT: return "prefetch abort";
        case ARM_EXC_DATA_ABORT:     return "data abort";
        case ARM_EXC_RESERVED:       return "reserved vector";
        case ARM_EXC_IRQ:            return "IRQ";
        case ARM_EXC_FIQ:            return "FIQ";
        default:                     return "unknown";
    }
}

static uint32_t read_dfsr(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(v));
    return v;
}

static uint32_t read_ifsr(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(v));
    return v;
}

static uint32_t read_dfar(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(v));
    return v;
}

static uint32_t read_ifar(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(v));
    return v;
}

void arm_exceptions_init(void) {
    uint32_t vbar = (uint32_t)(uintptr_t)arm_vector_table;
    uint32_t sctlr;

    /* Use the relocatable low-vector base rather than the legacy fixed
       0xffff0000 high vectors, then install our table in VBAR. */
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    sctlr &= ~(1U << 13); /* SCTLR.V = 0 */
    __asm__ volatile(
        "mcr p15, 0, %0, c1, c0, 0\n"
        "mcr p15, 0, %1, c12, c0, 0\n"
        "dsb sy\n"
        "isb\n"
        : : "r"(sctlr), "r"(vbar) : "memory");
}

void arm_exception_panic(uint32_t kind, const arm_exception_frame_t *frame) {
    rpi3_uart_puts("\n\n[PANIC][ARM] exception: ");
    rpi3_uart_puts(exception_name(kind));
    rpi3_uart_puts("\n  LR   = ");
    rpi3_uart_puthex(frame ? frame->lr : 0U);
    rpi3_uart_puts("\n  SPSR = ");
    rpi3_uart_puthex(frame ? frame->spsr : 0U);

    if (kind == ARM_EXC_DATA_ABORT) {
        rpi3_uart_puts("\n  DFSR = ");
        rpi3_uart_puthex(read_dfsr());
        rpi3_uart_puts("\n  DFAR = ");
        rpi3_uart_puthex(read_dfar());
    } else if (kind == ARM_EXC_PREFETCH_ABORT) {
        rpi3_uart_puts("\n  IFSR = ");
        rpi3_uart_puthex(read_ifsr());
        rpi3_uart_puts("\n  IFAR = ");
        rpi3_uart_puthex(read_ifar());
    }

    rpi3_uart_puts("\n[HALT] CPU stopped after exception\n");
    for (;;) __asm__ volatile("wfe");
}

arm_task_frame_t *arm_user_fault_dispatch(uint32_t kind, arm_task_frame_t *frame) {
    if (kind == ARM_EXC_DATA_ABORT) {
        rpi3_uart_puts("[USER:FAULT] data abort DFAR=");
        rpi3_uart_puthex(read_dfar());
        rpi3_uart_puts(" DFSR=");
        rpi3_uart_puthex(read_dfsr());
        rpi3_uart_puts("\n");
    } else if (kind == ARM_EXC_PREFETCH_ABORT) {
        rpi3_uart_puts("[USER:FAULT] prefetch abort IFAR=");
        rpi3_uart_puthex(read_ifar());
        rpi3_uart_puts(" IFSR=");
        rpi3_uart_puthex(read_ifsr());
        rpi3_uart_puts("\n");
    }
    return rpi3_task_user_fault(kind, frame);
}
