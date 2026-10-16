/* SPDX-License-Identifier: GPL-2.0-only
 * Intel 82557/82558/82559 PRO/100, original BlesKernOS polled engine.
 * The 22-byte configure template below is adapted from Linux v2.6.18
 * drivers/net/eepro100.c, written 1996-1999 by Donald Becker, with updates
 * by Andrey V. Savochkin and other kernel developers. See LICENSES/GPL-2.0.txt
 * and docs/platform/family-driver-sources.json for provenance/source delivery.
 */
#include "legacy_net.h"
#define LEGACY_NET_NAME "Intel PRO/100"
#define LEGACY_NET_BUSMASTER PCI_COMMAND_BUSMASTER

static bool supported(const pci_device_t *d) {
    if (d->class_code != 2U || d->subclass != 0U || d->vendor_id != 0x8086U)
        return false;
    /* 82557/8/9 and integrated 82562: do not match unrelated Intel IDs. */
    static const uint16_t ids[] = {0x1229, 0x1209, 0x1029, 0x1030, 0x1031, 0x1032, 0x1033, 0x1034,
                                   0x1035, 0x1036, 0x1037, 0x1038, 0x1039, 0x103a, 0x103b, 0x103c,
                                   0x103d, 0x103e, 0x1050, 0x1059, 0x2449, 0x2459, 0x245d};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        if (d->device_id == ids[i])
            return true;
    return false;
}
static void ee_delay(void) {
    /* Serial EEPROM requires >= 1us per edge, even on fast hosts. */
    for (unsigned i = 0; i < 8; i++)
        legacy_in8(0x80);
}
static uint16_t ee_read(unsigned address, unsigned width) {
    uint32_t command = (6U << width) | address;
    put16(14, 0x4802U);
    ee_delay();
    for (int bit = (int)width + 2; bit >= 0; bit--) {
        uint16_t value = (uint16_t)(0x4802U | ((command & (1U << bit)) ? 4U : 0U));
        put16(14, value);
        ee_delay();
        put16(14, value | 1U);
        ee_delay();
    }
    uint16_t value = 0;
    for (unsigned bit = 0; bit < 16; bit++) {
        put16(14, 0x4802);
        ee_delay();
        put16(14, 0x4803);
        ee_delay();
        value = (uint16_t)((value << 1) | ((reg16(14) & 8U) ? 1U : 0U));
    }
    put16(14, 0x4800);
    ee_delay();
    return value;
}
static bool get_mac(void) {
    for (unsigned width = 6; width <= 8; width += 2) {
        uint16_t words[256], sum = 0;
        for (unsigned i = 0; i < (1U << width); i++) {
            words[i] = ee_read(i, width);
            sum = (uint16_t)(sum + words[i]);
        }
        /* Prevent a wrong address width from producing a plausible MAC. */
        if (sum != 0xbabaU)
            continue;
        for (unsigned i = 0; i < 3; i++) {
            nic.mac[2 * i] = (uint8_t)words[i];
            nic.mac[2 * i + 1] = (uint8_t)(words[i] >> 8);
        }
        if (legacy_mac_valid(nic.mac)) {
            nic.phy = (uint8_t)(words[6] & 31U);
            return true;
        }
    }
    return false;
}
static bool scb_command(uint8_t command, uint32_t pointer) {
    if (!wait8(2, 0xffU, 0))
        return false;
    put32(4, pointer);
    legacy_barrier();
    put8(2, command);
    return wait8(2, 0xffU, 0);
}
static unsigned rx_tail;
static void rings_prepare(void) {
    rx_tail = RING - 1U;
    for (unsigned i = 0; i < RING; i++) {
        nic_desc_t *r = (nic_desc_t *)rx_buffer(i);
        r->word[0] = i == rx_tail ? 0x80000000U : 0;
        r->word[1] = physical(rx_buffer((i + 1U) & (RING - 1U)));
        r->word[2] = 0xffffffffU;
        r->word[3] = (BUFFER - 16U) << 16;
    }
    legacy_barrier();
}
static bool cu_execute(uint16_t command, const void *payload, uint32_t length) {
    nic_desc_t *cb = (nic_desc_t *)tx_buffer(0);
    /* CU idle is required before reusing this synchronous command buffer. */
    if ((reg16(0) & 0xc0U) != 0U || length > BUFFER - 16U)
        return false;
    kmemset(cb, 0, BUFFER);
    cb->word[0] = ((uint32_t)command << 16) | 0x80000000U;
    cb->word[1] = 0xffffffffU;
    if (command == 4U) {
        cb->word[2] = 0xffffffffU;
        cb->word[3] = 0x00008000U | length; /* simplified inline TX, EOF */
        kmemcpy((uint8_t *)cb + 16U, payload, length);
    } else
        kmemcpy((uint8_t *)cb + 8U, payload, length);
    legacy_barrier();
    if (!scb_command(0x10U, physical(cb)))
        return false;
    for (unsigned i = 0; i < 500000U; i++) {
        if (cb->word[0] & 0x8000U) {
            legacy_barrier();
            return (cb->word[0] & 0x2000U) && !(reg16(0) & 0xc0U);
        }
        __asm__ volatile("pause");
    }
    return false;
}
static bool hardware_start(void) {
    if (!get_mac())
        return false;
    put32(8, 0);
    for (unsigned i = 0; i < 20000; i++)
        legacy_in8(0x80);
    put8(3, 1U); /* mask all device interrupts */
    if (!scb_command(0x60U, 0) || !scb_command(6U, 0))
        return false;
    /* Linux eepro100 recommended 82558 configuration template; flow control
     * disabled, simplified RFD/TX, broadcast enabled, PHY controls duplex. */
    static const uint8_t configuration[22] = {22, 0x08, 0,    1,    0,    0,    0x22, 0x03,
                                              1,  0,    0x2e, 0,    0x60, 0x08, 0x88, 0x68,
                                              0,  0x40, 0xf2, 0x84, 0x31, 0x05};
    nic.dma_started = true;
    if (!cu_execute(2U, configuration, sizeof(configuration)) ||
        !cu_execute(1U, nic.mac, sizeof(nic.mac)))
        return false;
    return scb_command(1U, physical(rx_buffer(0)));
}
static bool submit_frame(const void *frame, uint16_t length) {
    uint8_t *copy = copy_tx(1U, frame, length);
    return cu_execute(4U, copy, length < 60U ? 60U : length);
}
static void poll_device(void) {
    if (!nic.running)
        return;
    for (unsigned budget = 0; budget < RING; budget++) {
        nic_desc_t *r = (nic_desc_t *)rx_buffer(nic.rx_index);
        uint32_t status = r->word[0];
        if (!(status & 0x8000U))
            break;
        legacy_barrier();
        uint32_t count = r->word[3] & 0x3fffU;
        if ((status & 0x2000U) && count >= 14U && count <= NET_FRAME_MAX)
            netdev_receive((uint8_t *)r + 16U, (uint16_t)count);
        r->word[3] = (BUFFER - 16U) << 16;
        /* Keep an explicit end marker: hardware must stop before wrapping
         * into unread packets, including emulators without RFD ownership. */
        legacy_barrier();
        r->word[0] = 0x80000000U;
        legacy_barrier();
        volatile uint16_t *old_command = (volatile uint16_t *)(rx_buffer(rx_tail) + 2U);
        *old_command &= (uint16_t)~0x8000U;
        rx_tail = nic.rx_index;
        nic.rx_index = (nic.rx_index + 1U) & (RING - 1U);
    }
    /* Resource exhaustion stops RU at the next completed RFD. Restart only
     * after software has returned buffers; never overwrite a live descriptor. */
    if ((reg16(0) & 0x3cU) != 0x10U) {
        if (scb_command(4U, 0))
            (void)scb_command(1U, physical(rx_buffer(nic.rx_index)));
    }
    put16(0, reg16(0) & 0xfc00U);
}
static bool link_up(void) {
    if (!nic.running)
        return false;
    put32(16, 0x08000000U | ((uint32_t)nic.phy << 21) | (1U << 16));
    if (!wait32(16, 0x10000000U, 0x10000000U))
        return false;
    return (reg32(16) & 4U) != 0;
}
static bool hardware_stop(void) {
    put8(3, 1U);
    put32(8, 0);
    for (unsigned i = 0; i < 20000; i++)
        legacy_in8(0x80);
    return !(reg16(0) & 0xfcU);
}
static bool map_bars(void) { return legacy_io_bar(nic.pci, 1U, 32U, &nic.io); }
#include "legacy_net_runtime.inc"
