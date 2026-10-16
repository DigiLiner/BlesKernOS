/* SPDX-License-Identifier: MIT */
/* DEC Tulip: PCI IDs, registers, descriptors and link/reset logic. */
#include "legacy_net.h"
#define LEGACY_NET_NAME "DEC Tulip"
#define LEGACY_NET_BUSMASTER PCI_COMMAND_BUSMASTER

static bool supported(const pci_device_t *d) {
    if (d->class_code != 2 || d->subclass != 0)
        return false;
    return d->vendor_id == 0x1011 && (d->device_id == 0x0009 || d->device_id == 0x0019);
}

static bool get_mac(void) {
    for (unsigned i = 0; i < 3; i++) {
        uint16_t v = eeprom(10 + i, 0x48, 0x4800, 1, 2, 4, 8);
        nic.mac[2 * i] = (uint8_t)v;
        nic.mac[2 * i + 1] = (uint8_t)(v >> 8);
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
        r->word[0] = OWN;
        r->word[1] = 0x01000000U | (BUFFER - 1);
        r->word[2] = physical(rx_buffer(i));
        r->word[3] = next;
        t->word[3] = tnext;
        t->word[2] = physical(tx_buffer(i));
    }

    legacy_barrier();
}

static bool hardware_start(void) {
    if (!get_mac())
        return false;
    put32(0, 1);
    if (!wait32(0, 1, 0))
        return false;
    put32(0, 0x00008000);
    put32(0x38, 0);
    put32(0x18, physical(nic.rx));
    put32(0x20, physical(nic.tx));
    put32(0x30, 0x020e2042);
    put32(0x10, 1);
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
        d->word[1] = 0xe1000000U | bytes;
        legacy_barrier();
        d->word[0] = OWN;
        legacy_barrier();
        nic.tx_index = (n + 1) & (RING - 1);
        ok = true;
        put32(8, 1);
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
        length = (status >> 16) & 0x3fff;
        valid = (status & 0x0300) == 0x0300 && !(status & 0x8000);
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
        d->word[0] = OWN;
        nic.rx_index = (n + 1) & (RING - 1);
    }

    put32(0x28, reg32(0x28));
    put32(0x10, 1);
}

static bool link_up(void) {
    if (!nic.running)
        return false;
    return true;
}

static bool hardware_stop(void) {
    bool stopped = true;
    put32(0x30, 0);
    put32(0, 1);
    stopped = wait32(0, 1, 0);
    return stopped;
}

static bool map_bars(void) {
    bool bars = false;
    for (unsigned i = 0; i < 6 && !bars; i++)
        if (nic.pci->bars[i] & 1)
            bars = legacy_io_bar(nic.pci, (uint8_t)i, 0x80, &nic.io);

    for (unsigned i = 0; i < 6 && !bars; i++)
        if (!(nic.pci->bars[i] & 1))
            bars = legacy_mmio_bar(nic.pci, (uint8_t)i, 0x80, &nic.mmio);

    return bars;
}

#include "legacy_net_runtime.inc"
