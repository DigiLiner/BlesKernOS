#ifndef BK_LEGACY_NET_H
#define BK_LEGACY_NET_H
/* SPDX-License-Identifier: MIT
 * Polled legacy Ethernet engines. Register/descriptor layouts were studied in
 * Linux and iPXE; this implementation uses BlesKernOS's own PCI/DMA/net ABI.
 * No interrupts, jumbo frames, VLAN offload or checksum offload are enabled.
 */
#include "../../include/driver.h"
#include "../../include/network.h"
#include "../../include/task.h"
#include "../../include/vga.h"
#include "../legacy_io.h"

#define RING 16U
#define BUFFER 2048U
#define DESC_SIZE 16U
#define RX_OFFSET 4096U
#define TX_OFFSET (RX_OFFSET + RING * BUFFER)
#define DMA_SIZE (TX_OFFSET + RING * BUFFER)
#define OWN 0x80000000U
typedef struct {
    volatile uint32_t word[4];
} nic_desc_t;
static struct {
    const pci_device_t *pci;
    volatile uint8_t *mmio;
    uint16_t io, saved_command;
    uint8_t mac[6], phy;
    legacy_dma_t dma;
    nic_desc_t *rx, *tx;
    uint32_t rx_index, tx_index;
    volatile uint32_t tx_lock;
    bool running, registered, dma_started;
} nic;
static inline uint32_t reg32(uint32_t off) {
    return nic.mmio ? *(volatile uint32_t *)(nic.mmio + off) : legacy_in32(nic.io + (uint16_t)off);
}
static inline uint16_t reg16(uint32_t off) {
    return nic.mmio ? *(volatile uint16_t *)(nic.mmio + off) : legacy_in16(nic.io + (uint16_t)off);
}
static inline uint8_t reg8(uint32_t off) {
    return nic.mmio ? nic.mmio[off] : legacy_in8(nic.io + (uint16_t)off);
}
static inline void put32(uint32_t off, uint32_t value) {
    if (nic.mmio)
        *(volatile uint32_t *)(nic.mmio + off) = value;
    else
        legacy_out32(nic.io + (uint16_t)off, value);
}
static inline void put16(uint32_t off, uint16_t value) {
    if (nic.mmio)
        *(volatile uint16_t *)(nic.mmio + off) = value;
    else
        legacy_out16(nic.io + (uint16_t)off, value);
}
static inline void put8(uint32_t off, uint8_t value) {
    if (nic.mmio)
        nic.mmio[off] = value;
    else
        legacy_out8(nic.io + (uint16_t)off, value);
}
static inline bool wait32(uint32_t off, uint32_t mask, uint32_t wanted) {
    for (unsigned i = 0; i < 200000U; i++) {
        if ((reg32(off) & mask) == wanted)
            return true;
        __asm__ volatile("pause");
    }
    return false;
}
static inline bool wait8(uint32_t off, uint8_t mask, uint8_t wanted) {
    for (unsigned i = 0; i < 200000U; i++) {
        if ((reg8(off) & mask) == wanted)
            return true;
        __asm__ volatile("pause");
    }
    return false;
}
static uint32_t physical(const void *p) { return (uint32_t)(uintptr_t)p; }
static uint8_t *rx_buffer(unsigned n) { return nic.dma.data + RX_OFFSET + n * BUFFER; }
static uint8_t *tx_buffer(unsigned n) { return nic.dma.data + TX_OFFSET + n * BUFFER; }
static inline uint16_t eeprom(unsigned address, uint32_t off, uint32_t enable, uint32_t cs,
                              uint32_t clk, uint32_t input, uint32_t output) {
    uint32_t command = 0x180U | (address & 63U);
    put32(off, enable | cs);
    for (int bit = 8; bit >= 0; bit--) {
        uint32_t value = enable | cs | ((command & (1U << bit)) ? input : 0);
        put32(off, value);
        (void)reg32(off);
        put32(off, value | clk);
        (void)reg32(off);
    }
    uint16_t value = 0;
    for (unsigned bit = 0; bit < 16; bit++) {
        put32(off, enable | cs);
        (void)reg32(off);
        put32(off, enable | cs | clk);
        value = (uint16_t)((value << 1) | ((reg32(off) & output) ? 1 : 0));
    }
    put32(off, enable);
    return value;
}

static net_device_ops_t ops;
static bool send_frame(const void *frame, uint16_t length);
/* A private context and DMA arena are instantiated for each DVR module. */
static uint8_t *copy_tx(unsigned n, const void *frame, uint16_t length) {
    uint8_t *copy = tx_buffer(n);
    kmemcpy(copy, frame, length);
    if (length < 60U)
        kmemset(copy + length, 0, 60U - length);
    return copy;
}

#endif
