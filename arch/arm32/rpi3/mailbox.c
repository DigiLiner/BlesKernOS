#include "mailbox.h"
#include "cache.h"
#include "mmio.h"
#include "timer.h"

#define PERIPHERAL_BASE 0x3F000000U
#define MBOX_BASE       (PERIPHERAL_BASE + 0xB880U)
#define MBOX0_READ      (MBOX_BASE + 0x00U)
#define MBOX0_STATUS    (MBOX_BASE + 0x18U)
#define MBOX1_WRITE     (MBOX_BASE + 0x20U)
#define MBOX1_STATUS    (MBOX_BASE + 0x38U)
#define MBOX_FULL       0x80000000U
#define MBOX_EMPTY      0x40000000U
#define MBOX_RESPONSE_OK 0x80000000U
#define MBOX_TIMEOUT_US 1000000U

#define TAG_GET_ARM_MEMORY 0x00010005U
#define TAG_GET_CLOCK_RATE 0x00030002U
#define TAG_SET_POWER_STATE 0x00028001U

static volatile uint32_t g_property[8] __attribute__((aligned(16)));

static int timeout_expired(uint32_t start) {
    return (uint32_t)(rpi3_timer_now_us() - start) >= MBOX_TIMEOUT_US;
}

int rpi3_mailbox_property_call(volatile uint32_t *message, uint32_t bytes) {
    uint32_t addr;
    uint32_t word;
    uint32_t start;

    if (message == (volatile uint32_t *)0 || bytes < 12U ||
        (((uintptr_t)message & 15U) != 0U)) return -1;

    addr = (uint32_t)(uintptr_t)message;
    if ((addr & 0xF0000000U) != 0U) return -2;

    /* Request is in cacheable ARM RAM. Push it to PoC before VideoCore reads it. */
    rpi3_cache_clean_range((const void *)(uintptr_t)addr, bytes);
    arm_dmb();

    start = rpi3_timer_now_us();
    while ((mmio_read(MBOX1_STATUS) & MBOX_FULL) != 0U)
        if (timeout_expired(start)) return -3;

    word = (addr & ~0xFU) | RPI3_MBOX_PROP_CHANNEL;
    mmio_write(MBOX1_WRITE, word);
    arm_dmb();

    for (;;) {
        start = rpi3_timer_now_us();
        while ((mmio_read(MBOX0_STATUS) & MBOX_EMPTY) != 0U)
            if (timeout_expired(start)) return -4;

        arm_dmb();
        if (mmio_read(MBOX0_READ) == word) break;
        /* Other channels are ignored during this single-core bootstrap. */
    }

    /* VideoCore overwrote the request in RAM. Discard stale CPU cache lines. */
    rpi3_cache_invalidate_range((const void *)(uintptr_t)addr, bytes);
    arm_dmb();
    return message[1] == MBOX_RESPONSE_OK ? 0 : -5;
}

int rpi3_mailbox_get_clock_rate(uint32_t clock_id, uint32_t *rate_hz) {
    int rc;
    if (!rate_hz) return -1;
    g_property[0] = 8U * 4U;
    g_property[1] = 0U;
    g_property[2] = TAG_GET_CLOCK_RATE;
    g_property[3] = 8U;
    g_property[4] = 4U;
    g_property[5] = clock_id;
    g_property[6] = 0U;
    g_property[7] = 0U;
    rc = rpi3_mailbox_property_call(g_property, sizeof(g_property));
    if (rc != 0) return rc;
    if ((g_property[4] & 0x80000000U) == 0U || g_property[6] == 0U) return -6;
    *rate_hz = g_property[6];
    return 0;
}

int rpi3_mailbox_get_arm_memory(uint32_t *base, uint32_t *bytes) {
    int rc;
    if (!base || !bytes) return -1;
    g_property[0] = 8U * 4U;
    g_property[1] = 0U;
    g_property[2] = TAG_GET_ARM_MEMORY;
    g_property[3] = 8U;
    g_property[4] = 0U;
    g_property[5] = 0U;
    g_property[6] = 0U;
    g_property[7] = 0U;
    rc = rpi3_mailbox_property_call(g_property, sizeof(g_property));
    if (rc != 0) return rc;
    if ((g_property[4] & 0x80000000U) == 0U || g_property[6] == 0U) return -6;
    *base = g_property[5];
    *bytes = g_property[6];
    return 0;
}


int rpi3_mailbox_set_power_state(uint32_t device_id, bool on, bool wait) {
    int rc;
    g_property[0] = 8U * 4U;
    g_property[1] = 0U;
    g_property[2] = TAG_SET_POWER_STATE;
    g_property[3] = 8U;
    g_property[4] = 8U;
    g_property[5] = device_id;
    g_property[6] = (on ? 1U : 0U) | (wait ? 2U : 0U);
    g_property[7] = 0U;
    rc = rpi3_mailbox_property_call(g_property, sizeof(g_property));
    if (rc != 0) return rc;
    if ((g_property[4] & 0x80000000U) == 0U) return -6;
    if ((g_property[6] & 2U) != 0U) return -7;
    return ((g_property[6] & 1U) != 0U) == on ? 0 : -8;
}

uint32_t rpi3_vc_bus_to_arm(uint32_t bus_addr) {
    return bus_addr & 0x3FFFFFFFU;
}
