/* SPDX-License-Identifier: MIT */
/* NatSemi DP83815: PCI IDs, registers, descriptors and link/reset logic. */
#include "legacy_net.h"
#define LEGACY_NET_NAME "NatSemi DP83815"
#define LEGACY_NET_BUSMASTER PCI_COMMAND_BUSMASTER

static bool supported(const pci_device_t *d) {
    if (d->class_code != 2 || d->subclass != 0)
        return false;
    return d->vendor_id == 0x100b && d->device_id == 0x0020;
}

static bool get_mac(void) {
    uint16_t previous = eeprom(6, 8, 0, 8, 4, 1, 2);
    for (unsigned i = 0; i < 3; i++) {
        uint16_t v = eeprom(7 + i, 8, 0, 8, 4, 1, 2);
        nic.mac[2 * i] = (uint8_t)((v << 1) | (previous >> 15));
        nic.mac[2 * i + 1] = (uint8_t)(v >> 7);
        previous = v;
    }

    return legacy_mac_valid(nic.mac);
}

static void rings_prepare(void) {
    nic.rx = (nic_desc_t *)nic.dma.data;
    nic.tx = (nic_desc_t *)(nic.dma.data + RING * DESC_SIZE);
    for (unsigned i = 0; i < RING; i++) {
        nic_desc_t *r = &nic.rx[i];
        nic_desc_t *t = &nic.tx[i];
        uint32_t next = physical(&nic.rx[(i + 1) & (RING - 1)]);
        uint32_t tnext = physical(&nic.tx[(i + 1) & (RING - 1)]);
        r->word[0] = next;
        r->word[1] = BUFFER;
        r->word[2] = physical(rx_buffer(i));
        t->word[0] = tnext;
        t->word[2] = physical(tx_buffer(i));
    }

    legacy_barrier();
}

static bool hardware_start(void) {
    if (!get_mac())
        return false;
    put32(0, 0x100);
    if (!wait32(0, 0x100, 0))
        return false;
    put32(0x18, 0);
    put32(0x14, 0);
    put32(0x20, physical(nic.tx));
    put32(0x30, physical(nic.rx));
    for (unsigned i = 0; i < 3; i++) {
        put32(0x48, i * 2);
        put16(0x4c, (uint16_t)(nic.mac[i * 2] | ((uint16_t)nic.mac[i * 2 + 1] << 8)));
    }

    put32(0x48, 0xc8000000U);
    put32(0x24, 0x10000000U | 0x00000800U | 0x30U);
    put32(0x34, 0x10000000U | 0x00000020U);
    put32(0, 5);
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
        legacy_barrier();
        d->word[1] = OWN | bytes;
        legacy_barrier();
        nic.tx_index = (n + 1) & (RING - 1);
        ok = true;
        put32(0, 1);
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
        done = (status & OWN) != 0;
        length = status & 0xfff;
        valid = (status & 0x08000000) != 0 && !(status & 0x40000000);
        if (!done)
            break;
        legacy_barrier();
        if (length >= 4)
            length -= 4;
        else
            valid = false;
        if (valid && length >= 14 && length <= NET_FRAME_MAX)
            netdev_receive(rx_buffer(n), (uint16_t)length);
        legacy_barrier();
        d->word[1] = BUFFER;
        nic.rx_index = (n + 1) & (RING - 1);
    }

    (void)reg32(0x10);
    put32(0, 4);
}

static bool link_up(void) {
    if (!nic.running)
        return false;
    return (reg32(4) & 0x80000000U) != 0;
}

static bool hardware_stop(void) {
    bool stopped = true;
    put32(0, 0x100);
    stopped = wait32(0, 0x100, 0);
    return stopped;
}

static bool map_bars(void) {
    bool bars = false;
    for (unsigned i = 0; i < 6 && !bars; i++)
        if (nic.pci->bars[i] & 1)
            bars = legacy_io_bar(nic.pci, (uint8_t)i, 0x100, &nic.io);

    for (unsigned i = 0; i < 6 && !bars; i++)
        if (!(nic.pci->bars[i] & 1))
            bars = legacy_mmio_bar(nic.pci, (uint8_t)i, 0x100, &nic.mmio);

    return bars;
}

#include "legacy_net_runtime.inc"
