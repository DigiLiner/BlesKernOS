#include "../../include/types.h"
#include "../../include/memory.h"
#include "../../include/pci.h"
#include "../../include/pic.h"
#include "../../include/pit.h"
#include "../../include/task.h"
#include "../../include/network.h"
#include "../../include/driver.h"
#include "../../include/vga.h"

#define RTL_VENDOR_REALTEK 0x10ECU
#define RTL_DEVICE_8139    0x8139U
#define RTL_DEVICE_8138    0x8138U
#define RTL_DEVICE_8129    0x8129U

#define RTL_RX_RING_SIZE 32768U
#define RTL_RX_CONFIG ((4U<<13)|(2U<<11)|(4U<<8)|(1U<<7)|0x0EU)
#define RTL_RX_ALLOC_SIZE (RTL_RX_RING_SIZE + 16U + 1536U)
#define RTL_TX_SLOTS 4U
#define RTL_TX_BUFFER_SIZE 1536U

#define RTL_MAC0       0x00U
#define RTL_TX_STATUS  0x10U
#define RTL_TX_ADDRESS 0x20U
#define RTL_RX_BUFFER  0x30U
#define RTL_COMMAND    0x37U
#define RTL_CAPR       0x38U
#define RTL_IMR        0x3CU
#define RTL_ISR        0x3EU
#define RTL_TCR        0x40U
#define RTL_RCR        0x44U
#define RTL_TX_CONFIG  ((3U<<24)|(6U<<8))
#define RTL_CONFIG1    0x52U
#define RTL_BMSR       0x64U

#define RTL_CMD_RX_EMPTY 0x01U
#define RTL_CMD_TX_ENABLE 0x04U
#define RTL_CMD_RX_ENABLE 0x08U
#define RTL_CMD_RESET 0x10U
#define RTL_RX_OK 0x0001U
#define RTL_TX_OWN 0x00002000U
#define RTL_TX_OK  0x00008000U

typedef struct {
    uint16_t io;
    uint8_t mac[6];
    uint8_t *rx_ring;
    void *rx_raw;
    uint32_t rx_offset;
    uint32_t rx_pending_tick;
    bool rx_pending;
    uint8_t *tx_buffer[RTL_TX_SLOTS];
    uint8_t tx_slot;
    uint8_t tx_threshold;
    volatile uint32_t tx_lock;
    volatile bool running;
    const pci_device_t *pci;
} rtl8139_state_t;

static rtl8139_state_t g_rtl;

static uint16_t in16(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static void out16(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static bool rtl_id_supported(uint16_t vendor, uint16_t device) {
    if (vendor == RTL_VENDOR_REALTEK &&
        (device == RTL_DEVICE_8139 || device == RTL_DEVICE_8138 ||
         device == RTL_DEVICE_8129)) return true;
    /* Clones frecuentes con el núcleo 8139 clásico. */
    return (vendor == 0x1113U && device == 0x1211U) ||
           (vendor == 0x1186U && device == 0x1300U) ||
           (vendor == 0x018AU && device == 0x0106U) ||
           (vendor == 0x021BU && device == 0x8139U);
}

static bool rtl_reset(void) {
    outb((uint16_t)(g_rtl.io + RTL_CONFIG1), 0x00U);
    outb((uint16_t)(g_rtl.io + RTL_COMMAND), RTL_CMD_RESET);
    for (uint32_t i = 0; i < 1000000U; i++) {
        if (!(inb((uint16_t)(g_rtl.io + RTL_COMMAND)) & RTL_CMD_RESET))
            return true;
        io_wait();
    }
    return false;
}

static void rtl_copy_from_ring(uint8_t *destination, uint32_t offset,
                               uint32_t length) {
    /* RCR.RxNoWrap is set: the NIC writes a straddling packet into the
     * trailing allocation, NOT back at offset zero (8139too uses the same
     * linear copy for an 32-KiB ring). Only the next header wraps. */
    kmemcpy(destination, g_rtl.rx_ring + offset, length);
}

static void rtl_receive_restart(void) {
    outb((uint16_t)(g_rtl.io+RTL_COMMAND),RTL_CMD_TX_ENABLE);
    outb((uint16_t)(g_rtl.io+RTL_COMMAND),RTL_CMD_RX_ENABLE|RTL_CMD_TX_ENABLE);
    outl((uint16_t)(g_rtl.io+RTL_RX_BUFFER),(uint32_t)(uintptr_t)g_rtl.rx_ring);
    outl((uint16_t)(g_rtl.io+RTL_RCR),RTL_RX_CONFIG);
    g_rtl.rx_offset=0;g_rtl.rx_pending=false;
    out16((uint16_t)(g_rtl.io+RTL_CAPR),0xFFF0U);
}

static void rtl_receive_poll(void) {
    uint32_t work = 32U;
    while (work-- && !(inb((uint16_t)(g_rtl.io + RTL_COMMAND)) & RTL_CMD_RX_EMPTY)) {
        __asm__ volatile("":::"memory"); /* coherent PCI DMA publication */
        uint32_t offset = g_rtl.rx_offset;
        uint16_t status = (uint16_t)(g_rtl.rx_ring[offset] |
                          ((uint16_t)g_rtl.rx_ring[(offset + 1U) & (RTL_RX_RING_SIZE - 1U)] << 8));
        uint16_t wire_length = (uint16_t)(g_rtl.rx_ring[(offset + 2U) & (RTL_RX_RING_SIZE - 1U)] |
                               ((uint16_t)g_rtl.rx_ring[(offset + 3U) & (RTL_RX_RING_SIZE - 1U)] << 8));
        if (wire_length == 0xFFF0U) { /* DMA still copying this packet. */
            uint32_t now=pit_get_ticks(),limit=pit_get_frequency_hz()/10U;
            if(!limit)limit=1;
            if(!g_rtl.rx_pending){g_rtl.rx_pending=true;g_rtl.rx_pending_tick=now;}
            else if((uint32_t)(now-g_rtl.rx_pending_tick)>=limit)rtl_receive_restart();
            break;
        }
        g_rtl.rx_pending=false;
        if (!(status & RTL_RX_OK) || wire_length < 18U || wire_length > 1522U) {
            /* Un encabezado corrupto desincroniza el anillo: recuperar la NIC. */
            rtl_receive_restart();
            return;
        }
        {
            uint8_t frame[NET_FRAME_MAX];
            uint16_t frame_length = (uint16_t)(wire_length - 4U); /* quitar FCS */
            rtl_copy_from_ring(frame, offset + 4U, frame_length);
            netdev_receive(frame, frame_length);
        }
        g_rtl.rx_offset = (offset + 4U + wire_length + 3U) & ~3U;
        g_rtl.rx_offset &= (RTL_RX_RING_SIZE - 1U);
        out16((uint16_t)(g_rtl.io + RTL_CAPR),
              (uint16_t)((g_rtl.rx_offset - 16U) & 0xFFFFU));
    }
    {
        uint16_t interrupts = in16((uint16_t)(g_rtl.io + RTL_ISR));
        if(interrupts&0x50U)rtl_receive_restart(); /* RX overflow/FIFO overrun */
        if (interrupts) out16((uint16_t)(g_rtl.io + RTL_ISR), interrupts);
    }
}

static void rtl_poll_task(void *argument UNUSED) {
    uint32_t interval = (pit_get_frequency_hz() + 999U) / 1000U;
    if (!interval) interval = 1U;
    while (g_rtl.running) {
        netdev_poll();
        task_sleep(interval);
    }
    task_exit();
}

static bool rtl_send(const void *frame, uint16_t length) {
    uint32_t start, timeout;
    if (!g_rtl.running || !frame || length < 14U || length > 1514U) return false;
    start = pit_get_ticks();
    timeout = pit_get_frequency_hz();
    if (!timeout) timeout = 100U;

    for (;;) {
        if (__sync_bool_compare_and_swap(&g_rtl.tx_lock, 0U, 1U)) {
            uint8_t slot = g_rtl.tx_slot;
            uint32_t status = inl((uint16_t)(g_rtl.io + RTL_TX_STATUS + slot * 4U));
            if (status & RTL_TX_OWN) {
                if((status&0x4000U) && g_rtl.tx_threshold<48U)g_rtl.tx_threshold+=4U;
                if(status&0x40000000U)outl((uint16_t)(g_rtl.io+RTL_TCR),RTL_TX_CONFIG|1U);
                uint16_t transmit_length;
                kmemcpy(g_rtl.tx_buffer[slot], frame, length);
                transmit_length = length < 60U ? 60U : length;
                if (transmit_length > length)
                    kmemset(g_rtl.tx_buffer[slot] + length, 0,
                            transmit_length - length);
                outl((uint16_t)(g_rtl.io + RTL_TX_ADDRESS + slot * 4U),
                     (uint32_t)(uintptr_t)g_rtl.tx_buffer[slot]);
                /* Umbral FIFO 256 bytes (8 unidades de 32) y longitud. */
                outl((uint16_t)(g_rtl.io + RTL_TX_STATUS + slot * 4U),
                     ((uint32_t)g_rtl.tx_threshold << 16) | transmit_length);
                g_rtl.tx_slot = (uint8_t)((slot + 1U) & 3U);
                __sync_lock_release(&g_rtl.tx_lock);
                return true;
            }
            __sync_lock_release(&g_rtl.tx_lock);
        }
        /* Nunca dormir/yield con el lock tomado: el poll RX puede necesitar
           enviar un ACK TCP desde otra tarea o desde otro CPU. */
        if ((uint32_t)(pit_get_ticks() - start) >= timeout) return false;
        task_yield();
    }
}

static bool rtl_link_up(void) {
    /* PHY link status is latch-low; the first read clears old link loss. */
    (void)in16((uint16_t)(g_rtl.io + RTL_BMSR));
    return (in16((uint16_t)(g_rtl.io + RTL_BMSR)) & 0x0004U) != 0U;
}

static const net_device_ops_t g_net_ops = {
    "eth0-rtl8139", g_rtl.mac, 1500U, rtl_send, rtl_link_up,
    rtl_receive_poll
};

static void rtl_release(void) {
    if (g_rtl.rx_raw) kfree(g_rtl.rx_raw);
    for (uint8_t i = 0; i < RTL_TX_SLOTS; i++)
        if (g_rtl.tx_buffer[i]) kfree(g_rtl.tx_buffer[i]);
    g_rtl.rx_ring = NULL; g_rtl.rx_raw = NULL;
}

static bool rtl_init(void) {
    const pci_device_t *device = NULL;
    kmemset(&g_rtl, 0, sizeof(g_rtl));
    for (uint32_t i = 0; i < pci_device_count(); i++) {
        const pci_device_t *candidate = pci_device_at(i);
        if (candidate && rtl_id_supported(candidate->vendor_id,
                                          candidate->device_id)) {
            device = candidate;
            break;
        }
    }
    if (!device || !(device->bars[0] & 1U) || (device->bars[0]&~3U)>0xFFFFU) return false;
    g_rtl.pci = device;
    g_rtl.io = (uint16_t)(device->bars[0] & ~3U);
    if (!pci_enable_command(device, PCI_COMMAND_IO | PCI_COMMAND_BUSMASTER) ||
        !rtl_reset()) return false;
    for (uint8_t i = 0; i < 6; i++)
        g_rtl.mac[i] = inb((uint16_t)(g_rtl.io + RTL_MAC0 + i));
    if ((g_rtl.mac[0] & 1U) || !(g_rtl.mac[0] | g_rtl.mac[1] |
        g_rtl.mac[2] | g_rtl.mac[3] | g_rtl.mac[4] | g_rtl.mac[5])) return false;
    g_rtl.rx_raw = kzalloc(RTL_RX_ALLOC_SIZE+255U);
    if(g_rtl.rx_raw)g_rtl.rx_ring=(void *)(((uintptr_t)g_rtl.rx_raw+255U)&~255U);
    g_rtl.tx_threshold=8U;
    for (uint8_t i = 0; i < RTL_TX_SLOTS; i++)
        g_rtl.tx_buffer[i] = (uint8_t *)kmalloc(RTL_TX_BUFFER_SIZE);
    if (!g_rtl.rx_ring || !g_rtl.tx_buffer[0] || !g_rtl.tx_buffer[1] ||
        !g_rtl.tx_buffer[2] || !g_rtl.tx_buffer[3]) {
        rtl_release();
        return false;
    }
    (void)mm_set_allocation_subsystem(g_rtl.rx_raw,MM_SUBSYSTEM_NETWORK);
    for(uint32_t i=0;i<RTL_TX_SLOTS;i++)
        (void)mm_set_allocation_subsystem(g_rtl.tx_buffer[i],MM_SUBSYSTEM_NETWORK);
    outl((uint16_t)(g_rtl.io + RTL_RX_BUFFER),
         (uint32_t)(uintptr_t)g_rtl.rx_ring);
    out16((uint16_t)(g_rtl.io + RTL_IMR), 0); /* sondeo: sin IRQ compartida */
    out16((uint16_t)(g_rtl.io + RTL_ISR), 0xFFFFU);
    /* Real 8139/8101L parts latch DMA thresholds after RX/TX are enabled. */
    outb((uint16_t)(g_rtl.io+RTL_COMMAND),RTL_CMD_RX_ENABLE|RTL_CMD_TX_ENABLE);
    outl((uint16_t)(g_rtl.io+RTL_RCR),RTL_RX_CONFIG);
    outl((uint16_t)(g_rtl.io+RTL_TCR),RTL_TX_CONFIG);
    out16((uint16_t)(g_rtl.io+RTL_CAPR),0xFFF0U);
    g_rtl.running = true;
    if (!netdev_register(&g_net_ops)) {
        g_rtl.running = false;
        rtl_release();
        return false;
    }
    if (task_create("rtl8139-rx", rtl_poll_task, NULL) < 0) {
        netdev_unregister(&g_net_ops);
        g_rtl.running = false;
        rtl_release();
        return false;
    }
    kprintf("  [RTL8139] PCI %x:%x IO=0x%x MAC=%x:%x:%x:%x:%x:%x polling\n",
            device->vendor_id, device->device_id, g_rtl.io,
            g_rtl.mac[0], g_rtl.mac[1], g_rtl.mac[2],
            g_rtl.mac[3], g_rtl.mac[4], g_rtl.mac[5]);
    return true;
}

static void rtl_shutdown(void) {
    if (!g_rtl.running) return;
    g_rtl.running = false;
    netdev_unregister(&g_net_ops);
    outb((uint16_t)(g_rtl.io + RTL_COMMAND), 0);
    rtl_release();
}

const bk_driver_module_t *bleskernos_driver_query(void) {
    static const bk_driver_module_t module = {
        BK_DRIVER_ABI_VERSION, sizeof(bk_driver_module_t), 1U, 0U, "rtl8139",
        "Realtek RTL8129/RTL8139 Fast Ethernet", rtl_init, rtl_shutdown
    };
    return &module;
}
