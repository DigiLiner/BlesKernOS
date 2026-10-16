#include "timer.h"
#include "irq.h"
#include "mmio.h"

#define PERIPHERAL_BASE     0x3F000000U
#define SYSTEM_TIMER_BASE   (PERIPHERAL_BASE + 0x3000U)
#define SYSTEM_TIMER_CS     (SYSTEM_TIMER_BASE + 0x00U)
#define SYSTEM_TIMER_CLO    (SYSTEM_TIMER_BASE + 0x04U)
#define SYSTEM_TIMER_C1     (SYSTEM_TIMER_BASE + 0x10U)
#define SYSTEM_TIMER_IRQ_1  1U
#define SYSTEM_TIMER_MATCH_1 (1U << 1)
#define TICK_INTERVAL_US    (1000000U / RPI3_TIMER_HZ)
/* A phase-preserving catch-up may leave C1 only a handful of microseconds
   ahead of CLO.  QEMU and a busy real Pi can cross that value between the
   CLO read and the C1 MMIO write, after which no further equality match is
   generated and every sleeping task remains asleep forever. */

static volatile uint32_t g_ticks;
static uint32_t g_next_compare;

static int time_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static void timer_irq(uint32_t irq, void *opaque) {
    uint32_t now;
    uint32_t elapsed;
    (void)irq;
    (void)opaque;

    mmio_write(SYSTEM_TIMER_CS, SYSTEM_TIMER_MATCH_1);
    now = mmio_read(SYSTEM_TIMER_CLO);

    /* Advance from the previous deadline, rather than from 'now'. This keeps
       the 100 Hz clock stable. If UART/debugging delayed us, account for the
       missed periods and always arm a compare in the future. */
    elapsed = 1U;
    if (!time_after(g_next_compare + TICK_INTERVAL_US, now))
        elapsed += (uint32_t)(now - (g_next_compare + TICK_INTERVAL_US)) /
                   TICK_INTERVAL_US + 1U;
    g_ticks += elapsed;

    /* Always provide a complete interval of lead time.  Preserving the old
       phase can produce a mathematically-future deadline only a few CPU
       instructions away; the MMIO write then lands after that value and the
       BCM/QEMU equality comparator never interrupts again.  Missed periods
       are already reflected in elapsed/g_ticks, so restarting the hardware
       deadline from the observed CLO loses no scheduler accounting. */
    g_next_compare = now + TICK_INTERVAL_US;

    mmio_write(SYSTEM_TIMER_C1, g_next_compare);
}

void rpi3_timer_init(void) {
    uint32_t now;

    g_ticks = 0U;
    now = mmio_read(SYSTEM_TIMER_CLO);
    g_next_compare = now + TICK_INTERVAL_US;
    mmio_write(SYSTEM_TIMER_CS, SYSTEM_TIMER_MATCH_1);
    mmio_write(SYSTEM_TIMER_C1, g_next_compare);

    if (rpi3_irq_register(SYSTEM_TIMER_IRQ_1, timer_irq, (void *)0) == 0)
        rpi3_irq_enable_source(SYSTEM_TIMER_IRQ_1);
    arm_dmb();
}

void rpi3_timer_handle_irq(void) {
    /* Compatibility entry point retained for early Stage-1 callers. */
    timer_irq(SYSTEM_TIMER_IRQ_1, (void *)0);
}

uint32_t rpi3_timer_ticks(void) {
    return g_ticks;
}

uint32_t rpi3_timer_now_us(void) {
    return mmio_read(SYSTEM_TIMER_CLO);
}

void rpi3_timer_delay_us(uint32_t usec) {
    uint32_t start = rpi3_timer_now_us();
    while ((uint32_t)(rpi3_timer_now_us() - start) < usec)
        __asm__ volatile("nop");
}
