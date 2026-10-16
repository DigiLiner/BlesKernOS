/* SPDX-License-Identifier: MIT */
/* AMD PCnet PCI: PCI IDs, registers, descriptors and link/reset logic. */
#include "legacy_net.h"
#define LEGACY_NET_NAME "AMD PCnet PCI"
#define LEGACY_NET_BUSMASTER PCI_COMMAND_BUSMASTER

static bool supported(const pci_device_t *d) {
    if (d->class_code != 2 || d->subclass != 0)
        return false;
    return d->vendor_id == 0x1022 && d->device_id == 0x2000;
}

static uint16_t csr_read(uint16_t index) {
    put16(0x12, index);
    return reg16(0x10);
}

static void csr_write(uint16_t index, uint16_t value) {
    put16(0x12, index);
    put16(0x10, value);
}

static void bcr_write(uint16_t index, uint16_t value) {
    put16(0x12, index);
    put16(0x16, value);
}

static bool get_mac(void) {
    for (unsigned i = 0; i < 6; i++)
        nic.mac[i] = reg8(i);

    return legacy_mac_valid(nic.mac);
}

static void rings_prepare(void) {
    nic.rx = (nic_desc_t *)nic.dma.data;
    nic.tx = (nic_desc_t *)(nic.dma.data + RING * DESC_SIZE);
    for (unsigned i = 0; i < RING; i++) {
        nic_desc_t *r = &nic.rx[i];
        nic_desc_t *t = &nic.tx[i];
        r->word[0] = physical(rx_buffer(i));
        r->word[1] = 0x8000f800U;
        t->word[0] = physical(tx_buffer(i));
    }

    legacy_barrier();
}

static bool hardware_start(void) {
    if (!get_mac())
        return false;
    (void)reg16(0x14);
    csr_write(0, 4);
    bcr_write(20, 2);
    bcr_write(2, 2);
    uint8_t *init = nic.dma.data + 512;
    init[2] = 0x40;
    init[3] = 0x40;
    kmemcpy(init + 4, nic.mac, 6);
    *((uint32_t *)(init + 20)) = physical(nic.rx);
    *((uint32_t *)(init + 24)) = physical(nic.tx);
    csr_write(1, (uint16_t)physical(init));
    csr_write(2, (uint16_t)(physical(init) >> 16));
    csr_write(0, 1);
    bool ready = false;
    for (unsigned i = 0; i < 200000; i++)
        if (csr_read(0) & 0x100) {
            ready = true;
            break;
        }

    if (!ready)
        return false;
    csr_write(0, 0x0102);
    nic.dma_started = true;
    return true;
}

static bool submit_frame(const void *frame, uint16_t length) {
    bool ok = false;
    unsigned n = nic.tx_index;
    nic_desc_t *d = &nic.tx[n];
    uint16_t bytes = (length < 60) ? (60) : (length);
    bool available = (d->word[1] & OWN) == 0;
    if (available) {
        copy_tx(n, frame, length);
        d->word[2] = 0;
        d->word[1] = 0x0300f000U | (uint16_t)(-((int)bytes));
        legacy_barrier();
        d->word[1] |= OWN;
        legacy_barrier();
        nic.tx_index = (n + 1) & (RING - 1);
        ok = true;
        csr_write(0, 8);
    }
    return ok;
}

static void poll_device(void) {
    if (!nic.running)
        return;
    for (unsigned budget = 0; budget < RING; budget++) {
        unsigned n = nic.rx_index;
        nic_desc_t *d = &nic.rx[n];
        uint32_t status = d->word[1];
        uint32_t length = 0;
        bool valid = false;
        bool done = false;
        done = !(status & OWN);
        length = d->word[2] & 0xfff;
        valid = (status & 0x43000000U) == 0x03000000U;
        if (!done)
            break;
        legacy_barrier();
        if (length >= 4)
            length -= 4;
        else
            valid = false;
        if (valid && length >= 14 && length <= NET_FRAME_MAX)
            netdev_receive(rx_buffer(n), (uint16_t)length);
        d->word[2] = 0;
        legacy_barrier();
        d->word[1] = 0x8000f800U;
        nic.rx_index = (n + 1) & (RING - 1);
    }

    if (__sync_bool_compare_and_swap(&nic.tx_lock, 0, 1)) {
        csr_write(0, csr_read(0) & 0x7f00);
        __sync_lock_release(&nic.tx_lock);
    }
}

static bool link_up(void) {
    if (!nic.running)
        return false;
    return true;
}

static bool hardware_stop(void) {
    bool stopped = true;
    csr_write(0, 4);
    stopped = (csr_read(0) & 4) != 0;
    return stopped;
}

static bool map_bars(void) {
    bool bars = false;
    bars = legacy_io_bar(nic.pci, 0, 0x20, &nic.io);
    return bars;
}

#include "legacy_net_runtime.inc"
