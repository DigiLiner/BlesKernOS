/* SPDX-License-Identifier: MIT */
/* NE2000 PCI: PCI IDs, registers, descriptors and link/reset logic. */
#include "legacy_net.h"
#define LEGACY_NET_NAME "NE2000 PCI"
#define LEGACY_NET_BUSMASTER 0U

static bool supported(const pci_device_t *d) {
    if (d->class_code != 2 || d->subclass != 0)
        return false;
    return (d->vendor_id == 0x10ec && d->device_id == 0x8029) ||
           (d->vendor_id == 0x1050 && d->device_id == 0x0940) ||
           (d->vendor_id == 0x11f6 && d->device_id == 0x1401) ||
           (d->vendor_id == 0x8e2e && d->device_id == 0x3000);
}

static bool ne_dma(uint16_t address, void *buffer, uint16_t length, bool write) {
    uint16_t count = (uint16_t)((length + 1U) & ~1U);
    uint8_t *data = buffer;
    put8(0, 0x22);
    put8(7, 0x40);
    put8(0x0a, (uint8_t)count);
    put8(0x0b, (uint8_t)(count >> 8));
    put8(8, (uint8_t)address);
    put8(9, (uint8_t)(address >> 8));
    put8(0, write ? 0x12 : 0x0a);
    for (unsigned i = 0; i < count; i += 2) {
        if (write) {
            uint16_t v = data[i];
            if (i + 1 < length)
                v |= (uint16_t)data[i + 1] << 8;
            legacy_out16(nic.io + 0x10, v);
        } else {
            uint16_t v = legacy_in16(nic.io + 0x10);
            data[i] = (uint8_t)v;
            if (i + 1 < length)
                data[i + 1] = (uint8_t)(v >> 8);
        }
    }
    for (unsigned i = 0; i < 200000; i++)
        if (reg8(7) & 0x40) {
            put8(7, 0x40);
            put8(0, 0x22);
            return true;
        }
    put8(0, 0x22);
    return false;
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
        r->word[1] = 0x8000U | (BUFFER - 1);
        r->word[2] = physical(rx_buffer(i));
        r->word[3] = next;
        t->word[3] = tnext;
        t->word[2] = physical(tx_buffer(i));
    }

    legacy_barrier();
}

static bool hardware_start(void) {
    uint8_t reset = reg8(0x1f);
    put8(0x1f, reset);
    if (!wait8(7, 0x80, 0x80))
        return false;
    put8(0, 0x21);
    put8(0x0e, 0x49);
    put8(0x0c, 0x20);
    put8(0x0d, 2);
    put8(0x0f, 0);
    put8(7, 0xff);
    uint8_t prom[32];
    if (!ne_dma(0, prom, 32, false))
        return false;
    for (unsigned i = 0; i < 6; i++)
        nic.mac[i] = prom[i * 2];

    if (!legacy_mac_valid(nic.mac))
        return false;
    put8(0, 0x21);
    put8(1, 0x46);
    put8(2, 0x80);
    put8(3, 0x46);
    put8(4, 0x40);
    put8(0, 0x61);
    for (unsigned i = 0; i < 6; i++)
        put8(1 + i, nic.mac[i]);

    put8(7, 0x47);
    for (unsigned i = 0; i < 8; i++)
        put8(8 + i, 0);

    put8(0, 0x22);
    put8(0x0c, 4);
    put8(0x0d, 0);
    nic.rx_index = 0x47;
    return true;
}

static bool submit_frame(const void *frame, uint16_t length) {
    bool ok = false;
    uint16_t bytes = (length < 60) ? (60) : (length);
    if (!(reg8(0) & 4)) {
        uint8_t *copy = copy_tx(0, frame, length);
        ok = ne_dma(0x4000, copy, bytes, true);
        if (ok) {
            put8(4, 0x40);
            put8(5, (uint8_t)bytes);
            put8(6, (uint8_t)(bytes >> 8));
            put8(0, 0x26);
        }
    }
    return ok;
}

static void ne_poll(void) {
    /* The 8390 page selector and remote-DMA engine are shared by RX and TX.
     * Release the lock before invoking the stack, which may send an ARP reply. */
    if (!__sync_bool_compare_and_swap(&nic.tx_lock, 0, 1))
        return;
    uint16_t lengths[RING];
    unsigned received = 0;
    for (unsigned budget = 0; budget < RING; budget++) {
        put8(0, 0x62);
        uint8_t current = reg8(7);
        put8(0, 0x22);
        if (current < 0x46 || current >= 0x80 || current == nic.rx_index)
            break;
        uint8_t header[4];
        uint16_t address = (uint16_t)(nic.rx_index << 8);
        if (!ne_dma(address, header, 4, false))
            break;
        uint32_t length = header[2] | ((uint32_t)header[3] << 8);
        uint8_t next = header[1];
        if (next < 0x46 || next >= 0x80 || next == nic.rx_index || length < 4 ||
            length > NET_FRAME_MAX + 4) {
            nic.rx_index = current;
            put8(3, current == 0x46 ? 0x7f : current - 1);
            break;
        }
        length -= 4;
        uint8_t *buffer = rx_buffer(received);
        uint32_t first = 0x8000U - address - 4U;
        if (first > length)
            first = length;
        bool ok = ne_dma(address + 4, buffer, (uint16_t)first, false);
        if (ok && first < length)
            ok = ne_dma(0x4600, buffer + first, (uint16_t)(length - first), false);
        if (ok && (header[0] & 1) && length >= 14)
            lengths[received++] = (uint16_t)length;
        nic.rx_index = next;
        put8(3, next == 0x46 ? 0x7f : next - 1);
    }
    put8(7, 0xff);
    __sync_lock_release(&nic.tx_lock);
    for (unsigned i = 0; i < received; i++)
        netdev_receive(rx_buffer(i), lengths[i]);
}

static void poll_device(void) {
    if (!nic.running)
        return;
    ne_poll();
    return;
}

static bool link_up(void) {
    if (!nic.running)
        return false;
    return true;
}

static bool hardware_stop(void) {
    bool stopped = true;
    put8(0, 0x21);
    return stopped;
}

static bool map_bars(void) {
    bool bars = false;
    bars = legacy_io_bar(nic.pci, 0, 0x20, &nic.io);
    return bars;
}

#include "legacy_net_runtime.inc"
