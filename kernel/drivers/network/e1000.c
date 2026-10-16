/* SPDX-License-Identifier: MIT */
/* Intel 8254x: PCI IDs, registers, descriptors and link/reset logic. */
#include "legacy_net.h"
#define LEGACY_NET_NAME "Intel 8254x"
#define LEGACY_NET_BUSMASTER PCI_COMMAND_BUSMASTER

static bool supported(const pci_device_t *d) {
    if (d->class_code != 2 || d->subclass != 0)
        return false;
    return d->vendor_id == 0x8086 &&
           (d->device_id == 0x1000 || d->device_id == 0x1001 || d->device_id == 0x1004 ||
            d->device_id == 0x1008 || d->device_id == 0x1009 || d->device_id == 0x100c ||
            d->device_id == 0x100d || d->device_id == 0x100e || d->device_id == 0x1010 ||
            d->device_id == 0x1011 || d->device_id == 0x1079 || d->device_id == 0x107a ||
            d->device_id == 0x107b);
}

static bool get_mac(void) {
    uint32_t low = reg32(0x5400);
    uint32_t high = reg32(0x5404);
    for (unsigned i = 0; i < 4; i++)
        nic.mac[i] = (uint8_t)(low >> (i * 8));

    nic.mac[4] = (uint8_t)high;
    nic.mac[5] = (uint8_t)(high >> 8);
    if (!legacy_mac_valid(nic.mac))
        for (unsigned i = 0; i < 3; i++) {
            put32(0x14, 1U | (i << 8));
            if (!wait32(0x14, 0x10, 0x10))
                return false;
            uint16_t value = (uint16_t)(reg32(0x14) >> 16);
            nic.mac[2 * i] = (uint8_t)value;
            nic.mac[2 * i + 1] = (uint8_t)(value >> 8);
        }

    return legacy_mac_valid(nic.mac);
}

static void rings_prepare(void) {
    nic.rx = (nic_desc_t *)nic.dma.data;
    nic.tx = (nic_desc_t *)(nic.dma.data + RING * DESC_SIZE);
    for (unsigned i = 0; i < RING; i++) {
        nic_desc_t *r = &nic.rx[i];
        nic_desc_t *t = &nic.tx[i];
        r->word[0] = physical(rx_buffer(i));
        t->word[0] = physical(tx_buffer(i));
        t->word[3] = 1;
    }

    legacy_barrier();
}

static bool hardware_start(void) {
    if (!get_mac())
        return false;
    put32(0xd8, 0xffffffffU);
    put32(0, reg32(0) | (1U << 26));
    if (!wait32(0, 1U << 26, 0))
        return false;
    put32(0xd8, 0xffffffffU);
    put32(0, reg32(0) | 0x40U);
    put32(0x5400, (uint32_t)nic.mac[0] | ((uint32_t)nic.mac[1] << 8) |
                      ((uint32_t)nic.mac[2] << 16) | ((uint32_t)nic.mac[3] << 24));
    put32(0x5404, 0x80000000U | nic.mac[4] | ((uint32_t)nic.mac[5] << 8));
    for (unsigned i = 0; i < 128; i++)
        put32(0x5200 + i * 4, 0);

    put32(0x2800, physical(nic.rx));
    put32(0x2804, 0);
    put32(0x2808, RING * 16);
    put32(0x2810, 0);
    put32(0x2818, RING - 1);
    put32(0x3800, physical(nic.tx));
    put32(0x3804, 0);
    put32(0x3808, RING * 16);
    put32(0x3810, 0);
    put32(0x3818, 0);
    put32(0x410, 0x0060200a);
    put32(0x400, 0x0004010a);
    put32(0x100, 0x04008002);
    nic.dma_started = true;
    return true;
}

static bool submit_frame(const void *frame, uint16_t length) {
    bool ok = false;
    unsigned n = nic.tx_index;
    nic_desc_t *d = &nic.tx[n];
    uint16_t bytes = (length < 60) ? (60) : (length);
    bool available = (d->word[3] & 1) != 0;
    if (available) {
        copy_tx(n, frame, length);
        d->word[2] = 0x0b000000U | bytes;
        d->word[3] = 0;
        legacy_barrier();
        nic.tx_index = (n + 1) & (RING - 1);
        ok = true;
        put32(0x3818, nic.tx_index);
    }
    return ok;
}

static void poll_device(void) {
    if (!nic.running)
        return;
    for (unsigned budget = 0; budget < RING; budget++) {
        unsigned n = nic.rx_index;
        nic_desc_t *d = &nic.rx[n];
        uint32_t status = d->word[0];
        uint32_t length = 0;
        bool valid = false;
        bool done = false;
        status = d->word[3];
        done = (status & 1) != 0;
        length = d->word[2] & 0xffff;
        valid = (status & 3) == 3 && !(status & 0xff00);
        if (!done)
            break;
        legacy_barrier();
        if (valid && length >= 14 && length <= NET_FRAME_MAX)
            netdev_receive(rx_buffer(n), (uint16_t)length);
        d->word[2] = 0;
        d->word[3] = 0;
        legacy_barrier();
        put32(0x2818, n);
        nic.rx_index = (n + 1) & (RING - 1);
    }

    (void)reg32(0xc0);
}

static bool link_up(void) {
    if (!nic.running)
        return false;
    return (reg32(8) & 2) != 0;
}

static bool hardware_stop(void) {
    bool stopped = true;
    put32(0x100, 0);
    put32(0x400, 0);
    put32(0, reg32(0) | (1U << 26));
    stopped = wait32(0, 1U << 26, 0);
    return stopped;
}

static bool map_bars(void) {
    bool bars = false;
    bars = legacy_mmio_bar(nic.pci, 0, 0x6000, &nic.mmio);
    return bars;
}

#include "legacy_net_runtime.inc"
