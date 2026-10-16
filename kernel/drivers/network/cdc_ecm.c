/* SPDX-License-Identifier: MIT
 * USB CDC Ethernet Control Model (USB-IF CDC 1.2). Studied Linux cdc_ether.c
 * v6.12. Original implementation for BlesKernOS USB/net ABI; no Linux code.
 * No RNDIS, CDC-NCM, vendor-specific USB NICs or checksum offloads.
 */
#include "../../include/driver.h"
#include "../../include/usb_core.h"
#include "../../include/network.h"
#include "../../include/memory.h"
#include "../../include/task.h"
#include "../../include/vga.h"

static struct {
    usb_device_t *device;
    usb_interface_t *control, *data;
    usb_endpoint_t *input, *output;
    uint8_t mac[6];
    uint16_t segment_size;
    uint8_t rx[2048], tx[2048];
    volatile uint32_t tx_lock;
    bool registered, running;
} ecm;
static net_device_ops_t net_ops;
static const usb_class_driver_t class_ops;

static int hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static bool ecm_mac(const uint8_t *string, uint32_t length, uint8_t mac[6]) {
    if (!string || length < 26U || string[0] != 26U || string[1] != 3U)
        return false;
    uint8_t any = 0;
    for (unsigned i = 0; i < 6; i++) {
        int hi = hex_digit(string[2U + i * 4U]), lo = hex_digit(string[4U + i * 4U]);
        if (hi < 0 || lo < 0 || string[3U + i * 4U] || string[5U + i * 4U])
            return false;
        mac[i] = (uint8_t)((hi << 4) | lo);
        any |= mac[i];
    }
    return any && !(mac[0] & 1U);
}
static bool ecm_send(const void *frame, uint16_t length) {
    if (!ecm.registered || !ecm.device->connected || !frame || length < 14U ||
        length > NET_FRAME_MAX || length > ecm.segment_size || !__sync_bool_compare_and_swap(&ecm.tx_lock, 0U, 1U))
        return false;
    uint32_t actual = 0, bytes = length < 60U ? 60U : length;
    kmemset(ecm.tx, 0, bytes);
    kmemcpy(ecm.tx, frame, length);
    bool ok = usb_endpoint_transfer(ecm.device, ecm.output, ecm.tx, bytes, &actual, 100U) &&
              actual == bytes;
    if (ok && bytes % ecm.output->max_packet == 0U)
        ok = usb_endpoint_transfer(ecm.device, ecm.output, NULL, 0U, &actual, 100U);
    __sync_lock_release(&ecm.tx_lock);
    return ok;
}
static bool ecm_link(void) { return ecm.registered && ecm.device && ecm.device->connected; }
static void ecm_poll(void) {
    if (!ecm_link())
        return;
    uint32_t actual = 0;
    bool ok = usb_endpoint_transfer(ecm.device, ecm.input, ecm.rx, sizeof(ecm.rx), &actual, 2U);
    /* netdev_poll serializes RX; TX has a separate buffer/lock so replies
     * and another CPU can transmit while an IN poll waits for USB frames. */
    if (ok && actual >= 14U && actual <= NET_FRAME_MAX && actual <= ecm.segment_size)
        netdev_receive(ecm.rx, (uint16_t)actual);
}
static void ecm_disconnect(usb_device_t *device, usb_interface_t *interface) {
    (void)interface;
    if (ecm.device != device)
        return;
    ecm.registered = false;
    netdev_unregister(&net_ops);
}
static bool ecm_probe(usb_device_t *device, usb_interface_t *control) {
    if (!device || !control || ecm.registered || netdev_present() || device->speed == USB_SPEED_LOW || control->class_code != 2U ||
        control->subclass != 6U || control->protocol != 0U)
        return false;
    uint8_t data_number = 255U, mac_index = 0;
    uint16_t segment_size = 0;
    for (uint32_t pos = 0; pos + 2U <= control->extra_length;) {
        const uint8_t *d = control->extra + pos;
        if (d[0] < 2U || d[0] > control->extra_length - pos)
            return false;
        if (d[1] == 0x24U && d[0] >= 5U && d[2] == 6U && d[3] == control->number)
            data_number = d[4];
        if (d[1] == 0x24U && d[0] >= 13U && d[2] == 0x0fU) {
            mac_index = d[3];
            segment_size = (uint16_t)d[8] | ((uint16_t)d[9] << 8);
        }
        pos += d[0];
    }
    if (!mac_index || data_number == 255U || segment_size < 1514U)
        return false;
    usb_interface_t *data = NULL;
    usb_endpoint_t *in = NULL, *out = NULL;
    for (unsigned i = 0; i < device->interface_count; i++) {
        usb_interface_t *candidate = &device->interfaces[i];
        if (candidate->number != data_number || candidate->class_code != 0x0aU ||
            candidate->claimed)
            continue;
        in = usb_interface_endpoint(candidate, USB_EP_BULK, true, 0);
        out = usb_interface_endpoint(candidate, USB_EP_BULK, false, 0);
        if (in && out && in->max_packet && out->max_packet) {
            data = candidate;
            break;
        }
    }
    if (!data)
        return false;
    uint8_t string[26] = {0}, languages[4] = {0}, mac[6];
    if (!usb_control_transfer(device, 0x80U, 6U, 0x0300U, 0, languages, sizeof(languages)) ||
        languages[0] < 4U || languages[1] != 3U)
        return false;
    uint16_t language = (uint16_t)languages[2] | ((uint16_t)languages[3] << 8);
    if (!usb_control_transfer(device, 0x80U, 6U, (uint16_t)(0x0300U | mac_index), language, string,
                              sizeof(string)) ||
        !ecm_mac(string, sizeof(string), mac))
        return false;
    if (!usb_set_interface(device, data_number, data->alternate))
        return false;
    /* Directed, broadcast and all-multicast; hardware does not need promisc. */
    if (!usb_control_transfer(device, 0x21U, 0x43U, 0x0eU, control->number, NULL, 0))
        return false;
    ecm.segment_size = segment_size < NET_FRAME_MAX ? segment_size : NET_FRAME_MAX;
    ecm.device = device;
    ecm.control = control;
    ecm.data = data;
    ecm.input = in;
    ecm.output = out;
    kmemcpy(ecm.mac, mac, sizeof(mac));
    net_ops = (net_device_ops_t){"USB CDC-ECM", ecm.mac, 1500U, ecm_send, ecm_link, ecm_poll};
    if (!netdev_register(&net_ops))
        return false;
    ecm.registered = true;
    /* The control interface is assigned by the core after probe returns. */
    for (unsigned i = 0; i < device->interface_count; i++)
        if (device->interfaces[i].number == data_number) {
            device->interfaces[i].claimed = true;
            device->interfaces[i].owner = &class_ops;
        }
    kprintf("[USBECM] dev=%u control=%u data=%u alt=%u MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
            device->id, control->number, data_number, data->alternate, mac[0], mac[1], mac[2],
            mac[3], mac[4], mac[5]);
    return true;
}
static const usb_class_driver_t class_ops = {BK_USB_CORE_ABI_VERSION, sizeof(usb_class_driver_t),
                                             "CDC-ECM", ecm_probe, ecm_disconnect};
static void ecm_worker(void *unused) {
    (void)unused;
    while (ecm.running) {
        netdev_poll();
        task_sleep(1U);
    }
    task_exit();
}
static bool ecm_init(void) {
    kmemset(&ecm, 0, sizeof(ecm));
    if (!usb_class_register(&class_ops))
        return false;
    ecm.running = true;
    if (task_create("usb-ecm-rx", ecm_worker, NULL) < 0) {
        ecm.running = false;
        usb_class_unregister(&class_ops);
        return false;
    }
    return true;
}
static void ecm_shutdown(void) {
    ecm.running = false;
    usb_class_unregister(&class_ops);
}
const bk_driver_module_t *bleskernos_driver_query(void) {
    static const bk_driver_module_t module = {BK_DRIVER_ABI_VERSION,
                                              sizeof(bk_driver_module_t),
                                              1U,
                                              0U,
                                              "USB CDC-ECM",
                                              "Standard USB Ethernet family, UHCI/EHCI bulk",
                                              ecm_init,
                                              ecm_shutdown};
    return &module;
}
