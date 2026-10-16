#include "physmem.h"

/* Until Stage 4 parses the firmware DTB/mailbox memory information, keep the
   bootstrap allocator deliberately inside the first 64 MiB. This avoids
   pretending that the whole 1 GiB belongs to ARM when firmware/GPU reservations
   may reduce it. */
#define BOOTSTRAP_LIMIT 0x04000000U

extern unsigned char __kernel_end[];
static uint32_t g_next;
static uint32_t g_base;
static uint32_t g_limit;

static uint32_t align_up(uint32_t value, uint32_t align) {
    return (value + align - 1U) & ~(align - 1U);
}

void rpi3_physmem_init(void) {
    g_base = align_up((uint32_t)(uintptr_t)__kernel_end, RPI3_PAGE_SIZE);
    g_next = g_base;
    g_limit = BOOTSTRAP_LIMIT;
    if (g_base >= g_limit)
        g_limit = g_base;
}

void *rpi3_phys_alloc_pages(uint32_t count) {
    uint32_t bytes;
    uint32_t addr;

    if (count == 0U || count > (0xFFFFFFFFU / RPI3_PAGE_SIZE))
        return (void *)0;
    bytes = count * RPI3_PAGE_SIZE;
    if (g_next > g_limit || bytes > (g_limit - g_next))
        return (void *)0;

    addr = g_next;
    g_next += bytes;
    return (void *)(uintptr_t)addr;
}

void *rpi3_phys_alloc_page(void) {
    return rpi3_phys_alloc_pages(1U);
}

uint32_t rpi3_phys_free_bytes(void) {
    return (g_next <= g_limit) ? (g_limit - g_next) : 0U;
}

uint32_t rpi3_phys_base(void) { return g_base; }
uint32_t rpi3_phys_limit(void) { return g_limit; }
