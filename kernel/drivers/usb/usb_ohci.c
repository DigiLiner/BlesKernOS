/* SPDX-License-Identifier: MIT
 * PCI OHCI 1.0 root-port host, original BlesKernOS implementation.
 * Register/descriptor semantics studied in Linux v6.12 ohci.h/ohci-q.c.
 * No Linux source copied. One serialized ED/TD arena per controller; guarded
 * DMA32, synchronous control/bulk and a real periodic interrupt schedule.
 * Root-connected low/full-speed HID, printers and CDC-ECM; no isochronous,
 * OHCI mass-storage boot path or downstream hubs in this implementation.
 */
#include "../../include/usb_ohci.h"
#include "../../include/usb_core.h"
#include "../../include/task.h"
#include "../../include/vga.h"
#include "../legacy_io.h"
#define OHCI_HOSTS 4U
#define OHCI_PORTS 15U
#define OHCI_CONTROL 4U
#define OHCI_COMMAND 8U
#define OHCI_INTR_STATUS 12U
#define OHCI_INTR_DISABLE 20U
#define OHCI_HCCA 24U
#define OHCI_CONTROL_HEAD 32U
#define OHCI_BULK_HEAD 40U
#define OHCI_FRAME 60U
#define OHCI_RH_A 72U
#define OHCI_RH_STATUS 80U
#define OHCI_PORT_BASE 84U
#define OHCI_PERIODIC 4U
#define OHCI_CONTROL_LIST 16U
#define OHCI_BULK_LIST 32U
#define OHCI_OPERATIONAL 0x80U
#define OHCI_ED_SKIP (1U << 14)
#define OHCI_TD_ACTIVE (0xfU << 28)
#define OHCI_TD_ROUND (1U << 18)
#define OHCI_TD_IN (2U << 19)
#define OHCI_TD_OUT (1U << 19)

typedef struct {
    volatile uint32_t info, tail, head, next;
} ohci_ed_t;
typedef struct {
    volatile uint32_t info, current, next, end;
} ohci_td_t;
typedef struct {
    volatile uint32_t *registers;
    legacy_dma_t dma;
    volatile uint32_t *hcca;
    ohci_ed_t *ed;
    ohci_td_t *td, *dummy;
    uint8_t *setup, *data;
    volatile uint32_t lock;
    uint8_t ports;
    bool ready, faulted;
    usb_device_t *root[OHCI_PORTS];
} ohci_host_t;
static ohci_host_t hosts[OHCI_HOSTS];
static uint32_t host_count;
static uint32_t read_reg(ohci_host_t *h, uint32_t off) { return h->registers[off / 4U]; }
static void write_reg(ohci_host_t *h, uint32_t off, uint32_t value) {
    h->registers[off / 4U] = value;
}
static uint32_t bus_address(const void *p) { return (uint32_t)(uintptr_t)p; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static void pause_host(void) {
    if (task_current_pid() && !task_current_is_user())
        task_sleep(1U);
    else
        __asm__ volatile("pause");
}
static uint32_t milliseconds_ticks(uint32_t ms) {
    uint32_t hz = pit_get_frequency_hz();
    if (!hz)
        hz = 100U;
    uint32_t n = (uint32_t)(((uint64_t)ms * hz + 999U) / 1000U);
    return n ? n : 1U;
}
static void delay_ms(uint32_t ms) {
    uint32_t start = pit_get_ticks(), ticks = milliseconds_ticks(ms);
    while (pit_get_ticks() - start < ticks)
        pause_host();
}
static bool lock_host(ohci_host_t *h) {
    if (!h || !h->ready || h->faulted)
        return false;
    uint32_t start = pit_get_ticks(), limit = milliseconds_ticks(5000U);
    while (!__sync_bool_compare_and_swap(&h->lock, 0U, 1U)) {
        if (h->faulted || pit_get_ticks() - start >= limit)
            return false;
        pause_host();
    }
    return true;
}
/* Linux also defers ED retirement across a frame boundary: a completed TD
 * alone is not evidence that the controller forgot its prefetched ED. */
static bool quiesce(ohci_host_t *h) {
    h->ed->info |= OHCI_ED_SKIP;
    legacy_barrier();
    write_reg(h, OHCI_CONTROL, OHCI_OPERATIONAL | 3U);
    uint16_t frame = (uint16_t)read_reg(h, OHCI_FRAME);
    uint32_t start = pit_get_ticks(), limit = milliseconds_ticks(100U);
    while ((uint16_t)((uint16_t)read_reg(h, OHCI_FRAME) - frame) < 2U) {
        if (pit_get_ticks() - start >= limit) {
            /* Never recycle the arena after a controller stops advancing. */
            h->faulted = true;
            write_reg(h, OHCI_CONTROL, 0U);
            return false;
        }
        pause_host();
    }
    write_reg(h, OHCI_CONTROL_HEAD, 0U);
    write_reg(h, OHCI_BULK_HEAD, 0U);
    for (unsigned i = 0; i < 32U; i++)
        h->hcca[i] = 0;
    h->hcca[33] = 0;
    write_reg(h, OHCI_INTR_STATUS, 0xffffffffU);
    legacy_barrier();
    return true;
}
static bool run_transfer(ohci_host_t *h, uint8_t address, uint8_t endpoint, usb_speed_t speed,
                         uint16_t packet, uint8_t kind, uint32_t direction, uint8_t toggle,
                         uint8_t *data, uint32_t length, uint32_t *actual, uint8_t *next_toggle,
                         uint32_t timeout, uint8_t interval) {
    if (actual)
        *actual = 0;
    if (!h->ready || h->faulted || !packet || packet > 64U || length > 4096U ||
        (speed == USB_SPEED_LOW && (kind == USB_EP_BULK || packet > 8U)))
        return false;
    kmemset(h->ed, 0, sizeof(*h->ed));
    kmemset(h->td, 0, sizeof(*h->td));
    kmemset(h->dummy, 0, sizeof(*h->dummy));
    h->ed->info = (uint32_t)address | ((uint32_t)endpoint << 7) | ((uint32_t)packet << 16) |
                  (speed == USB_SPEED_LOW ? (1U << 13) : 0U);
    h->ed->tail = bus_address(h->dummy);
    h->ed->head = bus_address(h->td);
    h->td->info = OHCI_TD_ACTIVE | ((uint32_t)(2U + (toggle & 1U)) << 24) | direction |
                  (direction == OHCI_TD_IN ? OHCI_TD_ROUND : 0U);
    h->td->current = length ? bus_address(data) : 0U;
    h->td->end = length ? bus_address(data) + length - 1U : 0U;
    h->td->next = bus_address(h->dummy);
    uint32_t list;
    if (kind == USB_EP_CONTROL) {
        list = OHCI_CONTROL_LIST;
        write_reg(h, OHCI_CONTROL_HEAD, bus_address(h->ed));
    } else if (kind == USB_EP_BULK) {
        list = OHCI_BULK_LIST;
        write_reg(h, OHCI_BULK_HEAD, bus_address(h->ed));
    } else {
        list = OHCI_PERIODIC;
        uint32_t period = 1U;
        while (period < 32U && period * 2U <= interval) period *= 2U;
        for (unsigned i = 0; i < 32U; i++)
            h->hcca[i] = i % period == 0U ? bus_address(h->ed) : 0U;
        if (timeout < period + 2U) timeout = period + 2U;
    }
    legacy_barrier();
    write_reg(h, OHCI_CONTROL, OHCI_OPERATIONAL | 3U | list);
    if (kind == USB_EP_CONTROL)
        write_reg(h, OHCI_COMMAND, 2U);
    else if (kind == USB_EP_BULK)
        write_reg(h, OHCI_COMMAND, 4U);
    uint32_t start = pit_get_ticks(), limit = milliseconds_ticks(timeout ? timeout : 5000U);
    while ((h->td->info >> 28) >= 14U && !(h->ed->head & 1U)) {
        if (pit_get_ticks() - start >= limit || (read_reg(h, OHCI_INTR_STATUS) & (1U << 4)))
            break;
        pause_host();
    }
    if (!quiesce(h))
        return false;
    uint32_t condition = h->td->info >> 28, cbp = h->td->current;
    bool completed = condition == 0U && !(h->ed->head & 1U);
    if (completed) {
        if (cbp && (cbp < bus_address(data) || cbp > bus_address(data) + length))
            return false;
        uint32_t transferred = cbp ? cbp - bus_address(data) : length;
        if (actual)
            *actual = transferred;
        if (next_toggle)
            *next_toggle = (uint8_t)((h->ed->head >> 1) & 1U);
        return direction == OHCI_TD_IN || transferred == length;
    }
    /* NAK without a completed packet is normal for periodic HID polling. */
    return kind == USB_EP_INTERRUPT && condition >= 14U && !(h->ed->head & 1U);
}
static bool request(ohci_host_t *h, uint8_t address, uint8_t packet, usb_speed_t speed,
                    uint8_t type, uint8_t request_code, uint16_t value, uint16_t index, void *data,
                    uint16_t length) {
    if (length > 2048U || (length && !data) || !lock_host(h))
        return false;
    uint8_t *setup = h->setup;
    setup[0] = type;
    setup[1] = request_code;
    setup[2] = (uint8_t)value;
    setup[3] = (uint8_t)(value >> 8);
    setup[4] = (uint8_t)index;
    setup[5] = (uint8_t)(index >> 8);
    setup[6] = (uint8_t)length;
    setup[7] = (uint8_t)(length >> 8);
    uint32_t actual = 0;
    bool in = (type & 0x80U) != 0U;
    bool ok = run_transfer(h, address, 0, speed, packet, USB_EP_CONTROL, 0U, 0U, setup, 8U, &actual,
                           NULL, 1000U, 0U);
    if (ok && length) {
        if (in)
            kmemset(h->data, 0, length);
        else
            kmemcpy(h->data, data, length);
        ok = run_transfer(h, address, 0, speed, packet, USB_EP_CONTROL,
                          in ? OHCI_TD_IN : OHCI_TD_OUT, 1U, h->data, length, &actual, NULL, 1000U, 0U);
        if (ok && in)
            kmemcpy(data, h->data, length);
    }
    if (ok)
        ok = run_transfer(h, address, 0, speed, packet, USB_EP_CONTROL,
                          in ? OHCI_TD_OUT : OHCI_TD_IN, 1U, h->data, 0U, &actual, NULL, 1000U, 0U);
    __sync_lock_release(&h->lock);
    return ok;
}
static bool control(usb_device_t *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                    void *data, uint16_t length) {
    return request(d->hcd_private, d->address, d->max_packet0, d->speed, type, req, value, index,
                   data, length);
}
static bool endpoint(usb_device_t *d, usb_endpoint_t *ep, void *data, uint32_t length,
                     uint32_t *actual, uint32_t timeout) {
    ohci_host_t *h = d->hcd_private;
    if (actual)
        *actual = 0;
    if (!USB_ENDPOINT_NUMBER(ep->address) || !lock_host(h))
        return false;
    bool in = USB_ENDPOINT_IN(ep->address), ok = true, first = true;
    uint32_t total = 0;
    while (total < length || first) {
        first = false;
        uint32_t n = length - total, got = 0;
        if (n > 4096U)
            n = 4096U;
        if (n) {
            if (in)
                kmemset(h->data, 0, n);
            else
                kmemcpy(h->data, (uint8_t *)data + total, n);
        }
        ok = run_transfer(h, d->address, USB_ENDPOINT_NUMBER(ep->address), d->speed, ep->max_packet,
                          ep->attributes & 3U, in ? OHCI_TD_IN : OHCI_TD_OUT, ep->toggle, h->data,
                          n, &got, &ep->toggle, timeout, ep->interval);
        if (!ok)
            break;
        if (in && got)
            kmemcpy((uint8_t *)data + total, h->data, got);
        total += got;
        if (in && got < n)
            break;
    }
    if (actual)
        *actual = total;
    __sync_lock_release(&h->lock);
    return ok;
}
static const usb_host_ops_t host_ops = {control, endpoint};
static bool reset_port(ohci_host_t *h, uint8_t port, usb_speed_t *speed) {
    uint32_t off = OHCI_PORT_BASE + port * 4U;
    if (!(read_reg(h, off) & 1U))
        return false;
    write_reg(h, off, 1U << 8);
    delay_ms(100U);
    write_reg(h, off, 1U << 4);
    uint32_t start = pit_get_ticks(), limit = milliseconds_ticks(500U);
    while (read_reg(h, off) & (1U << 4)) {
        if (pit_get_ticks() - start >= limit)
            return false;
        pause_host();
    }
    uint32_t status = read_reg(h, off);
    write_reg(h, off, status & 0x1f0000U);
    if ((status & 3U) != 3U)
        return false;
    *speed = (status & (1U << 9)) ? USB_SPEED_LOW : USB_SPEED_FULL;
    delay_ms(20U);
    return true;
}
static bool config_has_ecm(const uint8_t *cfg, uint32_t size) {
    if (size < 9U || cfg[0] < 9U || cfg[0] > size)
        return false;
    for (uint32_t pos = cfg[0]; pos + 2U <= size;) {
        uint8_t n = cfg[pos];
        if (n < 2U || n > size - pos)
            return false;
        if (cfg[pos + 1U] == 4U && n >= 9U && cfg[pos + 5U] == 2U && cfg[pos + 6U] == 6U)
            return true;
        pos += n;
    }
    return false;
}
static usb_device_t *enumerate(ohci_host_t *h, uint8_t port, usb_speed_t speed) {
    uint8_t dd[18] = {0}, cfg[2048] = {0}, alternative[2048];
    uint8_t address = (uint8_t)(port + 1U);
    if (!request(h, 0, 8, speed, 0x80U, 6U, 0x0100U, 0, dd, 8U))
        return NULL;
    uint8_t packet = dd[7];
    if (dd[1] != 1U || (packet != 8U && packet != 16U && packet != 32U && packet != 64U))
        return NULL;
    if (!request(h, 0, packet, speed, 0U, 5U, address, 0, NULL, 0U))
        return NULL;
    delay_ms(10U);
    if (!request(h, address, packet, speed, 0x80U, 6U, 0x0100U, 0, dd, sizeof(dd)) || dd[0] != 18U)
        return NULL;
    if (!request(h, address, packet, speed, 0x80U, 6U, 0x0200U, 0, cfg, 9U))
        return NULL;
    uint16_t size = le16(cfg + 2U);
    if (size < 9U || size > sizeof(cfg))
        return NULL;
    if (!request(h, address, packet, speed, 0x80U, 6U, 0x0200U, 0, cfg, size))
        return NULL;
    if (!config_has_ecm(cfg, size)) {
        for (uint8_t n = 1U; n < dd[17] && n < 8U; n++) {
            kmemset(alternative, 0, sizeof(alternative));
            if (!request(h, address, packet, speed, 0x80U, 6U, (uint16_t)(0x0200U | n), 0,
                         alternative, 9U))
                continue;
            uint16_t length = le16(alternative + 2U);
            if (length < 9U || length > sizeof(alternative))
                continue;
            if (request(h, address, packet, speed, 0x80U, 6U, (uint16_t)(0x0200U | n), 0,
                        alternative, length) &&
                config_has_ecm(alternative, length)) {
                kmemcpy(cfg, alternative, length);
                size = length;
                break;
            }
        }
    }
    if (!cfg[5] || !request(h, address, packet, speed, 0U, 9U, cfg[5], 0, NULL, 0U))
        return NULL;
    return usb_host_publish(h, &host_ops, address, port, speed, dd, cfg, size);
}
static void scan_ports(void) {
    for (uint32_t i = 0; i < host_count; i++) {
        ohci_host_t *h = &hosts[i];
        if (!h->ready || h->faulted)
            continue;
        for (uint8_t port = 0; port < h->ports; port++) {
            uint32_t status = read_reg(h, OHCI_PORT_BASE + 4U * port);
            if (h->root[port] && (!(status & 1U) || (status & (1U << 16)))) {
                if (!lock_host(h))
                    continue;
                usb_host_disconnect(h->root[port]);
                h->root[port] = NULL;
                __sync_lock_release(&h->lock);
            }
            if (h->root[port] || !(status & 1U))
                continue;
            usb_speed_t speed;
            if (reset_port(h, port, &speed))
                h->root[port] = enumerate(h, port, speed);
        }
    }
}
void usb_ohci_init(void) {
    for (uint32_t i = 0; i < pci_device_count() && host_count < OHCI_HOSTS; i++) {
        const pci_device_t *pci = pci_device_at(i);
        if (!pci || pci->class_code != 0x0cU || pci->subclass != 3U || pci->prog_if != 0x10U)
            continue;
        ohci_host_t *h = &hosts[host_count];
        volatile uint8_t *mmio = NULL;
        if (!legacy_mmio_bar(pci, 0U, 0x100U, &mmio))
            continue;
        h->registers = (volatile uint32_t *)mmio;
        if ((read_reg(h, 0) & 0xffU) != 0x10U)
            continue;
        if (!pci_enable_command(pci, PCI_COMMAND_MEMORY | PCI_COMMAND_BUSMASTER))
            continue;
        if (read_reg(h, OHCI_CONTROL) & (1U << 8)) {
            write_reg(h, OHCI_COMMAND, 8U);
            uint32_t start = pit_get_ticks();
            while (read_reg(h, OHCI_CONTROL) & (1U << 8)) {
                if (pit_get_ticks() - start > milliseconds_ticks(1000U))
                    break;
                pause_host();
            }
            if (read_reg(h, OHCI_CONTROL) & (1U << 8))
                continue;
        }
        write_reg(h, OHCI_INTR_DISABLE, 0xffffffffU);
        write_reg(h, OHCI_CONTROL, 0U);
        write_reg(h, OHCI_COMMAND, 1U);
        uint32_t start = pit_get_ticks();
        while (read_reg(h, OHCI_COMMAND) & 1U) {
            if (pit_get_ticks() - start > milliseconds_ticks(100U))
                break;
            pause_host();
        }
        if (read_reg(h, OHCI_COMMAND) & 1U)
            continue;
        h->ports = (uint8_t)(read_reg(h, OHCI_RH_A) & 0xffU);
        if (!h->ports || h->ports > OHCI_PORTS || !legacy_dma_alloc(&h->dma, 8192U))
            continue;
        kmemset(h->dma.data, 0, 8192U);
        h->hcca = (volatile uint32_t *)h->dma.data;
        h->ed = (ohci_ed_t *)(h->dma.data + 256U);
        h->td = (ohci_td_t *)(h->dma.data + 288U);
        h->dummy = (ohci_td_t *)(h->dma.data + 304U);
        h->setup = h->dma.data + 320U;
        h->data = h->dma.data + 4096U;
        write_reg(h, OHCI_HCCA, bus_address(h->dma.data));
        uint32_t fi = 11999U, fit = (read_reg(h, 52U) ^ 0x80000000U) & 0x80000000U;
        write_reg(h, 52U, fit | (((fi - 210U) * 6U / 7U) << 16) | fi);
        write_reg(h, 64U, fi * 9U / 10U);
        write_reg(h, 68U, 0x628U);
        write_reg(h, OHCI_INTR_STATUS, 0xffffffffU);
        write_reg(h, OHCI_RH_STATUS, 1U << 16);
        write_reg(h, OHCI_CONTROL, OHCI_OPERATIONAL | 3U);
        h->ready = true;
        host_count++;
        kprintf("[OHCI] PCI %x:%x ports=%u DMA32 guarded\n", pci->vendor_id, pci->device_id,
                h->ports);
    }
    scan_ports();
}
static void hotplug(void *unused) {
    (void)unused;
    for (;;) {
        scan_ports();
        task_sleep(milliseconds_ticks(1000U));
    }
}
void usb_ohci_start_hotplug_task(void) {
    if (host_count)
        task_create("ohci-hotplug", hotplug, NULL);
}
