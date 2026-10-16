/* SPDX-License-Identifier: MIT
 * Shared packet boundary checks, DMA ownership and module lifecycle.
 * Included after each chip's implementation; all state remains DVR-private.
 */
static bool send_frame(const void *frame, uint16_t length) {
    if (!nic.running || !frame || length < 14U || length > NET_FRAME_MAX ||
        !__sync_bool_compare_and_swap(&nic.tx_lock, 0, 1))
        return false;
    bool ok = submit_frame(frame, length);
    legacy_barrier();
    __sync_lock_release(&nic.tx_lock);
    return ok;
}
static void shutdown_device(void) {
    nic.running = false;
    if (nic.registered) {
        netdev_unregister(&ops);
        nic.registered = false;
    }
    if (!nic.pci)
        return;
    bool stopped = hardware_stop();
    pci_config_write16(nic.pci->bus, nic.pci->slot, nic.pci->function, 4, nic.saved_command);
    /* Keep live DMA backing quarantined when reset cannot be confirmed. */
    if (stopped)
        legacy_dma_free(&nic.dma);
}
static void rx_worker(void *unused) {
    (void)unused;
    while (nic.running) {
        netdev_poll();
        task_sleep(1);
    }
    task_exit();
}
static bool driver_init(void) {
    if (netdev_present())
        return false;
    kmemset(&nic, 0, sizeof(nic));
    for (unsigned i = 0; i < pci_device_count(); i++) {
        const pci_device_t *d = pci_device_at(i);
        if (d && supported(d)) {
            nic.pci = d;
            break;
        }
    }
    if (!nic.pci)
        return false;
    nic.saved_command = pci_config_read16(nic.pci->bus, nic.pci->slot, nic.pci->function, 4);
    if (!map_bars())
        return false;
    uint16_t command = (nic.mmio ? PCI_COMMAND_MEMORY : PCI_COMMAND_IO) | LEGACY_NET_BUSMASTER;
    if (!pci_enable_command(nic.pci, command))
        return false;
    if (!legacy_dma_alloc(&nic.dma, DMA_SIZE)) {
        pci_config_write16(nic.pci->bus, nic.pci->slot, nic.pci->function, 4, nic.saved_command);
        return false;
    }
    rings_prepare();
    if (!hardware_start()) {
        shutdown_device();
        return false;
    }
    ops = (net_device_ops_t){LEGACY_NET_NAME, nic.mac, 1500, send_frame, link_up, poll_device};
    nic.running = true;
    if (!netdev_register(&ops)) {
        shutdown_device();
        return false;
    }
    nic.registered = true;
    if (task_create("legacy-net-rx", rx_worker, NULL) < 0) {
        shutdown_device();
        return false;
    }
    kprintf("[LEGACYNET] %s PCI %x:%x MAC %02x:%02x:%02x:%02x:%02x:%02x (poll/DMA32)\n",
            LEGACY_NET_NAME, nic.pci->vendor_id, nic.pci->device_id, nic.mac[0], nic.mac[1],
            nic.mac[2], nic.mac[3], nic.mac[4], nic.mac[5]);
    return true;
}
const bk_driver_module_t *bleskernos_driver_query(void) {
    static bk_driver_module_t module = {
        BK_DRIVER_ABI_VERSION,
        sizeof(bk_driver_module_t),
        1,
        0,
        NULL,
        "1990/2000 Ethernet, bounded descriptor rings, polled RX/TX",
        driver_init,
        shutdown_device};
    module.name = LEGACY_NET_NAME;
    return &module;
}
