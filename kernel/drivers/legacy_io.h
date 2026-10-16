#ifndef BK_LEGACY_IO_H
#define BK_LEGACY_IO_H
#include "../include/memory.h"
#include "../include/paging.h"
#include "../include/pci.h"
#include "../include/pit.h"
#include "../include/types.h"

#ifndef BK_DRIVER_HOST_TEST
static inline uint8_t legacy_in8(uint16_t p) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint16_t legacy_in16(uint16_t p) {
    uint16_t v;
    __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint32_t legacy_in32(uint16_t p) {
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void legacy_out8(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p) : "memory");
}
static inline void legacy_out16(uint16_t p, uint16_t v) {
    __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p) : "memory");
}
static inline void legacy_out32(uint16_t p, uint32_t v) {
    __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p) : "memory");
}
#endif
static inline void legacy_barrier(void) { __sync_synchronize(); }
static inline bool legacy_mac_valid(const uint8_t *m) {
    uint8_t a = 0, b = 0xff;
    for (unsigned i = 0; i < 6; i++) {
        a |= m[i];
        b &= m[i];
    }
    return a && b != 0xff && !(m[0] & 1);
}
static inline bool legacy_io_bar(const pci_device_t *d, uint8_t index, uint32_t span,
                                 uint16_t *port) {
    pci_bar_info_t b;
    if (!d || !port || !span || span > 0x10000U)
        return false;
    if (!pci_get_bar_info(d, index, &b) || !b.is_io || !b.base || b.base > 0x10000U - span ||
        b.size < span)
        return false;
    *port = (uint16_t)b.base;
    return true;
}
static inline bool legacy_mmio_bar(const pci_device_t *d, uint8_t index, uint32_t span,
                                   volatile uint8_t **base) {
    pci_bar_info_t b;
    if (!d || !base || !span)
        return false;
    if (!pci_get_bar_info(d, index, &b) || b.is_io || !b.base || b.size < span ||
        b.base > 0xffffffffU - span)
        return false;
    if (b.is_64 && (index >= 5 || d->bars[index + 1]))
        return false;
    if (!paging_kernel_mmio_uncached(b.base, span))
        return false;
    *base = (volatile uint8_t *)(uintptr_t)b.base;
    return true;
}
typedef struct {
    void *allocation;
    uint8_t *data;
    uint32_t size;
} legacy_dma_t;
static inline bool legacy_dma_alloc(legacy_dma_t *b, uint32_t size) {
    b->allocation = NULL;
    b->data = mm_alloc_guarded_stack(size, false, &b->allocation, &b->size);
    return b->data != NULL;
}
static inline void legacy_dma_free(legacy_dma_t *b) {
    if (b->allocation)
        mm_free_guarded_stack(b->allocation, b->data, b->size);
    b->allocation = NULL;
    b->data = NULL;
    b->size = 0;
}
#endif
