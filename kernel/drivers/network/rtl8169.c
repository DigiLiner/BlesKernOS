/* SPDX-License-Identifier: MIT */
/* Realtek RTL8169: PCI IDs, registers, descriptors and link/reset logic. */
#include "legacy_net.h"
#define LEGACY_NET_NAME "Realtek RTL8169"
#define LEGACY_NET_BUSMASTER PCI_COMMAND_BUSMASTER

static bool supported(const pci_device_t *d) {
    if (d->class_code != 2 || d->subclass != 0)
        return false;
    return (d->vendor_id == 0x10ec && (d->device_id == 0x8169 || d->device_id == 0x8167)) ||
           (d->vendor_id == 0x1186 && d->device_id == 0x4300);
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
        r->word[0] = OWN | BUFFER | ((i == RING - 1) ? (0x40000000) : (0));
        r->word[2] = physical(rx_buffer(i));
        t->word[0] = (i == RING - 1) ? (0x40000000) : (0);
        t->word[2] = physical(tx_buffer(i));
    }

    legacy_barrier();
}

static bool hardware_start(void) {
    if (!get_mac())
        return false;
    put8(0x37, 0x10);
    if (!wait8(0x37, 0x10, 0))
        return false;
    put16(0x3c, 0);
    put16(0x3e, 0xffff);
    put8(0x50, 0xc0);
    put16(0xe0, 0);
    put16(0xda, BUFFER);
    put32(0x20, physical(nic.tx));
    put32(0x24, 0);
    put32(0xe4, physical(nic.rx));
    put32(0xe8, 0);
    put8(0xec, 0x3f);
    put32(0x40, 0x03000700);
    put32(0x44, 0x0000e70a);
    put8(0x50, 0);
    put8(0x37, 0x0c);
    nic.dma_started = true;
    return true;
}

static bool submit_frame(const void *frame, uint16_t length) {
    bool ok = false;
    unsigned n = nic.tx_index;
    nic_desc_t *d = &nic.tx[n];
    uint16_t bytes = (length < 60) ? (60) : (length);
    bool available = (d->word[0] & OWN) == 0;
    if (available) {
        copy_tx(n, frame, length);
        d->word[1] = 0;
        legacy_barrier();
        d->word[0] = OWN | 0x30000000U | bytes | ((n == RING - 1) ? (0x40000000U) : (0));
        legacy_barrier();
        nic.tx_index = (n + 1) & (RING - 1);
        ok = true;
        put8(0x38, 0x40);
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
        done = !(status & OWN);
        length = status & 0x1fff;
        valid = (status & 0x30000000) == 0x30000000 && !(status & 0x00200000);
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
        d->word[0] = OWN | BUFFER | ((n == RING - 1) ? (0x40000000U) : (0));
        nic.rx_index = (n + 1) & (RING - 1);
    }

    put16(0x3e, reg16(0x3e));
}

static bool link_up(void) {
    if (!nic.running)
        return false;
    return (reg8(0x6c) & 2) != 0;
}

static bool hardware_stop(void) {
    bool stopped = true;
    put8(0x37, 0x10);
    stopped = wait8(0x37, 0x10, 0);
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
