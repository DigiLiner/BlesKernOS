/* SPDX-License-Identifier: MIT
 * EHCI bulk-class transport. Studied Linux v6.12 ehci-q.c; implementation
 * uses BlesKernOS-owned descriptors/bounce buffers, not Linux URBs.
 * Included by the established EHCI/BOT engine to share one schedule safely.
 */
static bool ehci_class_lock(ehci_controller_t *hc) {
    if (!hc || hc->faulted) return false;
    uint32_t start = pit_get_ticks(), hz = pit_get_frequency_hz();
    if (!hz) hz = 100U;
    while (!__sync_bool_compare_and_swap(&hc->transfer_lock, 0U, 1U)) {
        if (hc->faulted || pit_get_ticks() - start > hz * 5U) return false;
        task_yield();
    }
    return true;
}
static bool ehci_class_control(usb_device_t *device, uint8_t type, uint8_t request,
                                uint16_t value, uint16_t index, void *data, uint16_t length) {
    if (!device || device->speed != USB_SPEED_HIGH) return false;
    return usb_request(device->hcd_private, device->address, device->max_packet0,
                       type, request, value, index, length, data);
}
static bool ehci_class_endpoint(usb_device_t *device, usb_endpoint_t *endpoint,
                                 void *data, uint32_t length, uint32_t *actual_length,
                                 uint32_t timeout_ms) {
    ehci_controller_t *hc = device->hcd_private;
    if (actual_length) *actual_length = 0;
    /* Interrupt endpoints require the periodic schedule; never pretend that
     * async bulk scheduling implements high-speed HID polling or hub status. */
    if (device->speed != USB_SPEED_HIGH || (endpoint->attributes & 3U) != USB_EP_BULK ||
        !endpoint->max_packet || endpoint->max_packet > 512U ||
        !USB_ENDPOINT_NUMBER(endpoint->address) || !ehci_class_lock(hc)) return false;
    bool in = USB_ENDPOINT_IN(endpoint->address), ok = true;
    uint32_t total = 0;
    bool first = true;
    while (total < length || first) {
        first = false;
        uint32_t chunk = length - total, result = TD_ACTIVE;
        if (chunk > 4096U) chunk = 4096U;
        if (!chunk) { /* Explicit terminating ZLP for ECM. */ }
        else if (in) kmemset(hc->class_dma, 0, chunk);
        else kmemcpy(hc->class_dma, (uint8_t *)data + total, chunk);
        ehci_td_t *td = ehci_alloc_td(hc);
        if (!td) { ok = false; break; }
        ehci_td_init(td, NULL, in ? TD_PID_IN : TD_PID_OUT, endpoint->toggle,
                     hc->class_dma, chunk);
        ehci_qh_t *qh = ehci_alloc_qh(hc, device->address,
            USB_ENDPOINT_NUMBER(endpoint->address), endpoint->max_packet);
        if (!qh) { ehci_free_chain(td); ok = false; break; }
        bool completed = ehci_run_qh(hc, qh, td, false, &result, timeout_ms);
        uint32_t remaining = (result >> 16) & 0x7fffU;
        if (!completed || remaining > chunk) { ok = false; break; }
        uint32_t actual = chunk - remaining;
        endpoint->toggle = (uint8_t)(result >> 31);
        __sync_synchronize();
        if (in && actual) kmemcpy((uint8_t *)data + total, hc->class_dma, actual);
        total += actual;
        if (in && actual < chunk) break;
        if (!in && actual != chunk) { ok = false; break; }
    }
    if (actual_length) *actual_length = total;
    __sync_lock_release(&hc->transfer_lock);
    return ok;
}
static const usb_host_ops_t ehci_class_ops = {ehci_class_control, ehci_class_endpoint};
