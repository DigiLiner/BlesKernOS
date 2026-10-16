#include "usb_dwc2_hid.h"
#include "mmio.h"
#include "timer.h"
#include "mailbox.h"
#include "cache.h"
#include "uart.h"
#include "../../../kernel/include/keyboard.h"
#include "../../../kernel/include/mouse.h"
#include "../../../kernel/include/memory.h"
#include "../../../kernel/include/vga.h"
#include "../../../kernel/include/network.h"

#define PERIPHERAL_BASE 0x3F000000U
#define DWC_BASE        (PERIPHERAL_BASE + 0x980000U)

#define DWC_GAHBCFG     (DWC_BASE + 0x008U)
#define DWC_GUSBCFG     (DWC_BASE + 0x00CU)
#define DWC_GRSTCTL     (DWC_BASE + 0x010U)
#define DWC_GINTSTS     (DWC_BASE + 0x014U)
#define DWC_GINTMSK     (DWC_BASE + 0x018U)
#define DWC_GRXFSIZ     (DWC_BASE + 0x024U)
#define DWC_GNPTXFSIZ   (DWC_BASE + 0x028U)
#define DWC_HPTXFSIZ    (DWC_BASE + 0x100U)
#define DWC_HCFG        (DWC_BASE + 0x400U)
#define DWC_HFNUM       (DWC_BASE + 0x408U)
#define DWC_HAINT       (DWC_BASE + 0x414U)
#define DWC_HAINTMSK    (DWC_BASE + 0x418U)
#define DWC_HPRT        (DWC_BASE + 0x440U)
#define DWC_HC_BASE(n)  (DWC_BASE + 0x500U + ((uint32_t)(n) * 0x20U))
#define DWC_HCCHAR(n)   (DWC_HC_BASE(n) + 0x00U)
#define DWC_HCSPLT(n)   (DWC_HC_BASE(n) + 0x04U)
#define DWC_HCINT(n)    (DWC_HC_BASE(n) + 0x08U)
#define DWC_HCINTMSK(n) (DWC_HC_BASE(n) + 0x0CU)
#define DWC_HCTSIZ(n)   (DWC_HC_BASE(n) + 0x10U)
#define DWC_HCDMA(n)    (DWC_HC_BASE(n) + 0x14U)
#define DWC_PCGCCTL     (DWC_BASE + 0xE00U)

#define GAHBCFG_GLBL_INTR_EN (1U << 0)
#define GAHBCFG_HBST_INCR4   (3U << 1)
#define GAHBCFG_DMA_EN       (1U << 5)
#define GUSBCFG_FORCEHOST    (1U << 29)
#define GUSBCFG_FORCEDEV     (1U << 30)
#define GRSTCTL_CSFTRST      (1U << 0)
#define GRSTCTL_RXFFLSH      (1U << 4)
#define GRSTCTL_TXFFLSH      (1U << 5)
#define GRSTCTL_TXFNUM_ALL   (0x10U << 6)
#define GRSTCTL_AHBIDLE      (1U << 31)

#define HPRT_CONN            (1U << 0)
#define HPRT_CONNDET         (1U << 1)
#define HPRT_ENA             (1U << 2)
#define HPRT_ENCHNG          (1U << 3)
#define HPRT_OVRCURRCHNG     (1U << 5)
#define HPRT_RST             (1U << 8)
#define HPRT_PWR             (1U << 12)
#define HPRT_SPD_SHIFT       17U
#define HPRT_SPD_MASK        (3U << HPRT_SPD_SHIFT)

#define HCCHAR_MPS_MASK      0x7FFU
#define HCCHAR_EPNUM_SHIFT   11U
#define HCCHAR_EPDIR         (1U << 15)
#define HCCHAR_LSDEV         (1U << 17)
#define HCCHAR_EPTYPE_SHIFT  18U
#define HCCHAR_DEVADDR_SHIFT 22U
#define HCCHAR_ODDFRM        (1U << 29)
#define HCCHAR_CHDIS         (1U << 30)
#define HCCHAR_CHENA         (1U << 31)

#define HCINT_XFERCOMP       (1U << 0)
#define HCINT_CHHLTD         (1U << 1)
#define HCINT_AHBERR         (1U << 2)
#define HCINT_STALL          (1U << 3)
#define HCINT_NAK            (1U << 4)
#define HCINT_ACK            (1U << 5)
#define HCINT_NYET           (1U << 6)
#define HCINT_XACTERR        (1U << 7)
#define HCINT_BBLERR         (1U << 8)
#define HCINT_FRMOVRUN       (1U << 9)
#define HCINT_DATATGLERR     (1U << 10)
#define HCINT_ALL            0x3FFFU
#define HCINT_FATAL          (HCINT_AHBERR | HCINT_STALL | HCINT_XACTERR | \
                              HCINT_BBLERR | HCINT_FRMOVRUN | HCINT_DATATGLERR)

#define HCTSIZ_XFRSIZ_MASK   0x7FFFFU
#define HCTSIZ_PKTCNT_SHIFT  19U
#define HCTSIZ_PID_SHIFT     29U
#define PID_DATA0            0U
#define PID_DATA1            2U
#define PID_SETUP            3U

#define EP_CONTROL           0U
#define EP_INTERRUPT         3U
#define EP_BULK              2U
#define USB_DIR_IN           0x80U

#define USB_REQ_GET_STATUS        0x00U
#define USB_REQ_CLEAR_FEATURE     0x01U
#define USB_REQ_SET_FEATURE       0x03U
#define USB_REQ_SET_ADDRESS       0x05U
#define USB_REQ_GET_DESCRIPTOR    0x06U
#define USB_REQ_SET_CONFIGURATION 0x09U
#define HID_REQ_GET_REPORT        0x01U
#define HID_REQ_SET_IDLE          0x0AU
#define HID_REQ_SET_PROTOCOL      0x0BU
#define DESC_DEVICE               1U
#define DESC_CONFIG               2U
#define DESC_INTERFACE            4U
#define DESC_ENDPOINT             5U
#define DESC_HUB                  0x29U
#define CLASS_HID                 3U
#define CLASS_HUB                 9U
#define HUB_PORT_POWER            8U
#define HUB_PORT_RESET            4U
#define HUB_C_PORT_CONNECTION     16U
#define HUB_C_PORT_ENABLE         17U
#define HUB_C_PORT_RESET          20U
#define HUB_STATUS_CONNECTION     (1U << 0)
#define HUB_STATUS_ENABLE         (1U << 1)
#define HUB_STATUS_LOW_SPEED      (1U << 9)
#define HUB_STATUS_HIGH_SPEED     (1U << 10)

#define USB_SPEED_LOW  1U
#define USB_SPEED_FULL 2U
#define USB_SPEED_HIGH 3U

#define DWC_MAX_DEVICES 8U
#define DWC_DMA_BYTES   2048U
#define DWC_TIMEOUT_US  250000U
#define DWC_POLL_US     10000U

typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_t;

typedef enum {
    DWC_DEV_NONE = 0,
    DWC_DEV_HUB,
    DWC_DEV_KEYBOARD,
    DWC_DEV_MOUSE,
    DWC_DEV_TABLET,
    DWC_DEV_ETHERNET
} dwc_kind_t;

typedef struct {
    bool active;
    uint8_t address;
    uint8_t speed;
    uint8_t max_packet0;
    uint8_t configuration;
    dwc_kind_t kind;
    uint8_t interface_number;
    uint8_t endpoint;
    uint16_t max_packet;
    uint8_t bulk_out_endpoint;
    uint16_t bulk_out_max_packet;
    uint8_t bulk_in_toggle;
    uint8_t bulk_out_toggle;
    uint8_t interval;
    uint8_t toggle;
    uint8_t hub_ports;
    uint8_t previous_modifiers;
    uint8_t previous_keys[6];
} dwc_device_t;

static uint8_t g_dma[DWC_DMA_BYTES] __attribute__((aligned(64)));
static usb_setup_t g_setup __attribute__((aligned(64)));
static dwc_device_t g_devices[DWC_MAX_DEVICES];
static uint8_t g_next_address = 1U;
static bool g_online;
static uint32_t g_device_count;
static uint32_t g_last_poll_us;
static uint32_t g_diag_budget = 24U;
static bool g_mouse_report_seen;
static bool g_tablet_move_seen;
static uint32_t g_tablet_last_x;
static uint32_t g_tablet_last_y;
static dwc_device_t *g_ethernet;
static uint8_t g_ethernet_mac[6];

#define SMSC_HW_CFG       0x014U
#define SMSC_PM_CTRL      0x020U
#define SMSC_AFC_CFG      0x02CU
#define SMSC_MAC_CR       0x100U
#define SMSC_ADDRH        0x104U
#define SMSC_ADDRL        0x108U
#define SMSC_MII_ADDR     0x114U
#define SMSC_MII_DATA     0x118U
#define SMSC_HW_CFG_LRST  (1U << 3)
#define SMSC_PM_PHY_RST   (1U << 4)
#define SMSC_MAC_RXEN     (1U << 2)
#define SMSC_MAC_TXEN     (1U << 3)
#define SMSC_MII_BUSY     (1U << 0)
#define SMSC_MII_WRITE    (1U << 1)
#define SMSC_TX_FIRST     0x00002000U
#define SMSC_TX_LAST      0x00001000U
#define SMSC_RX_ERROR     0x00008000U

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t dma_bus(const void *p) {
    /* BCM2836/2837 GPU-bus mastering peripherals see SDRAM through the
       0xC0000000 alias. QEMU's raspi2b model implements the same address
       translation, so the hardware and emulator use one code path. */
    return 0xC0000000U | ((uint32_t)(uintptr_t)p & 0x3FFFFFFFU);
}

static bool wait_mask(uint32_t reg, uint32_t mask, bool set, uint32_t timeout) {
    uint32_t start = rpi3_timer_now_us();
    while ((((mmio_read(reg) & mask) != 0U) ? true : false) != set) {
        if ((uint32_t)(rpi3_timer_now_us() - start) >= timeout) return false;
    }
    return true;
}

static void halt_channel(uint32_t ch) {
    uint32_t hcchar = mmio_read(DWC_HCCHAR(ch));
    if ((hcchar & HCCHAR_CHENA) == 0U) return;
    hcchar |= HCCHAR_CHDIS | HCCHAR_CHENA;
    mmio_write(DWC_HCCHAR(ch), hcchar);
    (void)wait_mask(DWC_HCCHAR(ch), HCCHAR_CHENA, false, 5000U);
}

/* Return 0=success, 1=NAK/no data yet, negative=fatal/stall/timeout. */
static int channel_transfer(uint8_t addr, uint8_t speed, uint8_t ep,
                            uint8_t ep_type, bool in, uint16_t max_packet,
                            uint8_t pid, void *buffer, uint32_t length,
                            uint32_t *actual, uint32_t timeout_us) {
    const uint32_t ch = 0U;
    uint32_t packets;
    uint32_t hcchar;
    uint32_t hcint;
    uint32_t start;
    uint32_t remain;

    if (!max_packet) max_packet = 8U;
    if (length > DWC_DMA_BYTES) return -10;
    packets = length ? ((length + max_packet - 1U) / max_packet) : 1U;
    if (packets > 0x3FFU) return -11;
    if (actual) *actual = 0U;

    halt_channel(ch);
    mmio_write(DWC_HCINT(ch), HCINT_ALL);
    mmio_write(DWC_HCINTMSK(ch), HCINT_ALL);
    mmio_write(DWC_HCSPLT(ch), 0U);

    if (length && !in && buffer) kmemcpy(g_dma, buffer, length);
    if (in) rpi3_cache_clean_invalidate_range(g_dma, length ? length : 64U);
    else rpi3_cache_clean_range(g_dma, length ? length : 64U);
    arm_dmb();

    mmio_write(DWC_HCDMA(ch), dma_bus(g_dma));
    mmio_write(DWC_HCTSIZ(ch),
        (length & HCTSIZ_XFRSIZ_MASK) |
        ((packets & 0x3FFU) << HCTSIZ_PKTCNT_SHIFT) |
        (((uint32_t)pid & 3U) << HCTSIZ_PID_SHIFT));

    hcchar = ((uint32_t)max_packet & HCCHAR_MPS_MASK) |
             (((uint32_t)ep & 0xFU) << HCCHAR_EPNUM_SHIFT) |
             (((uint32_t)ep_type & 3U) << HCCHAR_EPTYPE_SHIFT) |
             (((uint32_t)addr & 0x7FU) << HCCHAR_DEVADDR_SHIFT);
    if (in) hcchar |= HCCHAR_EPDIR;
    if (speed == USB_SPEED_LOW) hcchar |= HCCHAR_LSDEV;
    if (ep_type == EP_INTERRUPT && (mmio_read(DWC_HFNUM) & 1U) == 0U)
        hcchar |= HCCHAR_ODDFRM;
    mmio_write(DWC_HCCHAR(ch), hcchar | HCCHAR_CHENA);
    arm_dmb();

    start = rpi3_timer_now_us();
    for (;;) {
        hcint = mmio_read(DWC_HCINT(ch));
        if (hcint & HCINT_XFERCOMP) {
            remain = mmio_read(DWC_HCTSIZ(ch)) & HCTSIZ_XFRSIZ_MASK;
            if (remain > length) remain = length;
            if (actual) *actual = length - remain;
            if (in && length && buffer) {
                rpi3_cache_invalidate_range(g_dma, length);
                kmemcpy(buffer, g_dma, length - remain);
            }
            mmio_write(DWC_HCINT(ch), HCINT_ALL);
            return 0;
        }
        if (hcint & HCINT_NAK) {
            halt_channel(ch);
            mmio_write(DWC_HCINT(ch), HCINT_ALL);
            return 1;
        }
        if (hcint & HCINT_FATAL) {
            halt_channel(ch);
            if (g_diag_budget) {
                kprintf("[DWC2] transfer error addr=%u ep=%u int=%x\n",
                        addr, ep, hcint);
                g_diag_budget--;
            }
            mmio_write(DWC_HCINT(ch), HCINT_ALL);
            return (hcint & HCINT_STALL) ? -2 : -3;
        }
        if ((uint32_t)(rpi3_timer_now_us() - start) >= timeout_us) {
            halt_channel(ch);
            mmio_write(DWC_HCINT(ch), HCINT_ALL);
            return -4;
        }
    }
}

static int control_transfer(uint8_t addr, uint8_t speed, uint8_t maxp0,
                            uint8_t request_type, uint8_t request,
                            uint16_t value, uint16_t index,
                            void *data, uint16_t length) {
    uint32_t actual = 0U;
    int rc;
    bool in = (request_type & USB_DIR_IN) != 0U;

    g_setup.bmRequestType = request_type;
    g_setup.bRequest = request;
    g_setup.wValue = value;
    g_setup.wIndex = index;
    g_setup.wLength = length;
    rc = channel_transfer(addr, speed, 0U, EP_CONTROL, false, maxp0,
                          PID_SETUP, &g_setup, sizeof(g_setup), &actual,
                          DWC_TIMEOUT_US);
    if (rc != 0 || actual != sizeof(g_setup)) return rc ? rc : -20;

    if (length) {
        rc = channel_transfer(addr, speed, 0U, EP_CONTROL, in, maxp0,
                              PID_DATA1, data, length, &actual,
                              DWC_TIMEOUT_US);
        if (rc != 0) return rc;
    }
    rc = channel_transfer(addr, speed, 0U, EP_CONTROL, !in, maxp0,
                          PID_DATA1, NULL, 0U, &actual, DWC_TIMEOUT_US);
    return rc;
}

static bool smsc_write(dwc_device_t *dev, uint16_t reg, uint32_t value) {
    return dev && control_transfer(dev->address, dev->speed, dev->max_packet0,
        0x40U, 0xA0U, 0U, reg, &value, sizeof(value)) == 0;
}

static bool smsc_read(dwc_device_t *dev, uint16_t reg, uint32_t *value) {
    uint32_t result = 0U;
    if (!dev || !value || control_transfer(dev->address, dev->speed,
        dev->max_packet0, 0xC0U, 0xA1U, 0U, reg,
        &result, sizeof(result)) != 0) return false;
    *value = result;
    return true;
}

static bool smsc_wait_clear(dwc_device_t *dev, uint16_t reg, uint32_t mask) {
    uint32_t start = rpi3_timer_now_us(), value;
    do {
        if (!smsc_read(dev, reg, &value)) return false;
        if ((value & mask) == 0U) return true;
        rpi3_timer_delay_us(1000U);
    } while ((uint32_t)(rpi3_timer_now_us() - start) < 1000000U);
    return false;
}

static bool smsc_mii_read(dwc_device_t *dev, uint8_t reg, uint16_t *value) {
    uint32_t command, data;
    if (!dev || !value || !smsc_wait_clear(dev, SMSC_MII_ADDR, SMSC_MII_BUSY))
        return false;
    command = (1U << 11) | ((uint32_t)(reg & 31U) << 6) | SMSC_MII_BUSY;
    if (!smsc_write(dev, SMSC_MII_ADDR, command) ||
        !smsc_wait_clear(dev, SMSC_MII_ADDR, SMSC_MII_BUSY) ||
        !smsc_read(dev, SMSC_MII_DATA, &data)) return false;
    *value = (uint16_t)data;
    return true;
}

static bool smsc_link_up(void) {
    uint16_t status = 0U;
    /* Read twice: BMSR link is latch-low. */
    if (!g_ethernet || !smsc_mii_read(g_ethernet, 1U, &status) ||
        !smsc_mii_read(g_ethernet, 1U, &status)) return false;
    return (status & (1U << 2)) != 0U;
}

static bool smsc_send(const void *frame, uint16_t length) {
    uint8_t packet[NET_FRAME_MAX + 8U] __attribute__((aligned(4)));
    uint32_t actual = 0U;
    int rc;
    if (!g_ethernet || !frame || length < 14U || length > NET_FRAME_MAX) return false;
    *(uint32_t *)(void *)packet = (uint32_t)length | SMSC_TX_FIRST | SMSC_TX_LAST;
    *(uint32_t *)(void *)(packet + 4U) = (uint32_t)length;
    kmemcpy(packet + 8U, frame, length);
    rc = channel_transfer(g_ethernet->address, g_ethernet->speed,
        g_ethernet->bulk_out_endpoint, EP_BULK, false,
        g_ethernet->bulk_out_max_packet,
        g_ethernet->bulk_out_toggle ? PID_DATA1 : PID_DATA0,
        packet, (uint32_t)length + 8U, &actual, DWC_TIMEOUT_US);
    if (rc != 0) return false;
    g_ethernet->bulk_out_toggle ^= 1U;
    return actual == (uint32_t)length + 8U;
}

static void smsc_poll(void) {
    uint8_t packet[NET_FRAME_MAX + 8U] __attribute__((aligned(4)));
    uint32_t actual = 0U, status, length;
    int rc;
    if (!g_ethernet) return;
    rc = channel_transfer(g_ethernet->address, g_ethernet->speed,
        g_ethernet->endpoint, EP_BULK, true, g_ethernet->max_packet,
        g_ethernet->bulk_in_toggle ? PID_DATA1 : PID_DATA0,
        packet, sizeof(packet), &actual, 1500U);
    if (rc != 0) return;
    g_ethernet->bulk_in_toggle ^= 1U;
    if (actual < 8U) return;
    status = *(const uint32_t *)(const void *)packet;
    length = (status >> 16) & 0x3FFFU;
    if ((status & SMSC_RX_ERROR) || length < 4U || length + 4U > actual) return;
    length -= 4U; /* LAN95xx includes Ethernet FCS in the reported length. */
    if (length <= NET_FRAME_MAX) netdev_receive(packet + 4U, (uint16_t)length);
}

static const net_device_ops_t g_smsc_ops = {
    "smsc95xx", g_ethernet_mac, 1500U, smsc_send, smsc_link_up, smsc_poll
};

static bool smsc_init(dwc_device_t *dev) {
    uint32_t value, low, high;
    if (!dev || !dev->endpoint || !dev->bulk_out_endpoint) return false;
    if (!smsc_read(dev, SMSC_HW_CFG, &value) ||
        !smsc_write(dev, SMSC_HW_CFG, value | SMSC_HW_CFG_LRST) ||
        !smsc_wait_clear(dev, SMSC_HW_CFG, SMSC_HW_CFG_LRST)) return false;
    if (!smsc_read(dev, SMSC_PM_CTRL, &value) ||
        !smsc_write(dev, SMSC_PM_CTRL, value | SMSC_PM_PHY_RST) ||
        !smsc_wait_clear(dev, SMSC_PM_CTRL, SMSC_PM_PHY_RST)) return false;
    if (!smsc_read(dev, SMSC_ADDRL, &low) || !smsc_read(dev, SMSC_ADDRH, &high))
        return false;
    g_ethernet_mac[0] = (uint8_t)low; g_ethernet_mac[1] = (uint8_t)(low >> 8);
    g_ethernet_mac[2] = (uint8_t)(low >> 16); g_ethernet_mac[3] = (uint8_t)(low >> 24);
    g_ethernet_mac[4] = (uint8_t)high; g_ethernet_mac[5] = (uint8_t)(high >> 8);
    if (!(low | (high & 0xFFFFU))) {
        g_ethernet_mac[0] = 0x02U; g_ethernet_mac[1] = 0x42U;
        g_ethernet_mac[2] = 0x4BU; g_ethernet_mac[3] = 0x4FU;
        g_ethernet_mac[4] = (uint8_t)dev->address; g_ethernet_mac[5] = 1U;
        low = (uint32_t)g_ethernet_mac[0] | ((uint32_t)g_ethernet_mac[1] << 8) |
              ((uint32_t)g_ethernet_mac[2] << 16) | ((uint32_t)g_ethernet_mac[3] << 24);
        high = (uint32_t)g_ethernet_mac[4] | ((uint32_t)g_ethernet_mac[5] << 8);
        (void)smsc_write(dev, SMSC_ADDRL, low); (void)smsc_write(dev, SMSC_ADDRH, high);
    }
    (void)smsc_write(dev, SMSC_AFC_CFG, 0x00F830A1U);
    if (!smsc_read(dev, SMSC_MAC_CR, &value)) return false;
    if (!smsc_write(dev, SMSC_MAC_CR, value | SMSC_MAC_RXEN | SMSC_MAC_TXEN))
        return false;
    g_ethernet = dev;
    if (!netdev_register(&g_smsc_ops)) { g_ethernet = NULL; return false; }
    kprintf("[SMSC95XX] Ethernet online mac=%x:%x:%x:%x:%x:%x\n",
            g_ethernet_mac[0], g_ethernet_mac[1], g_ethernet_mac[2],
            g_ethernet_mac[3], g_ethernet_mac[4], g_ethernet_mac[5]);
    return true;
}

static bool usage_in_report(const uint8_t keys[6], uint8_t usage) {
    uint32_t i;
    for (i = 0U; i < 6U; ++i) if (keys[i] == usage) return true;
    return false;
}

static void keyboard_report(dwc_device_t *dev, const uint8_t *report,
                            uint32_t bytes) {
    uint8_t mods;
    uint8_t keys[6] = {0,0,0,0,0,0};
    uint32_t i;
    if (!dev || !report || bytes < 8U) return;
    mods = report[0];
    for (i = 0U; i < 8U; ++i) {
        uint8_t mask = (uint8_t)(1U << i);
        if ((mods & mask) != (dev->previous_modifiers & mask))
            kbd_inject_hid_usage((uint16_t)(0xE0U + i), (mods & mask) != 0U);
    }
    for (i = 0U; i < 6U; ++i) keys[i] = report[2U + i];
    for (i = 0U; i < 6U; ++i) {
        uint8_t old = dev->previous_keys[i];
        if (old >= 4U && !usage_in_report(keys, old))
            kbd_inject_hid_usage(old, false);
    }
    for (i = 0U; i < 6U; ++i) {
        uint8_t now = keys[i];
        if (now >= 4U && !usage_in_report(dev->previous_keys, now))
            kbd_inject_hid_usage(now, true);
    }
    dev->previous_modifiers = mods;
    kmemcpy(dev->previous_keys, keys, sizeof(keys));
}

static void mouse_report(const uint8_t *report, uint32_t bytes) {
    int32_t dx, dy, wheel = 0;
    if (!report || bytes < 3U) return;
    dx = (int8_t)report[1];
    dy = (int8_t)report[2];
    if (bytes >= 4U) wheel = (int8_t)report[3];
    mouse_inject_relative(dx, dy, wheel, report[0] & 0x07U);
    if (!g_mouse_report_seen && (dx || dy || wheel || (report[0] & 0x07U))) {
        g_mouse_report_seen = true;
        kprintf("[INPUT] first USB mouse report dx=%d dy=%d buttons=%u\n",
                dx, dy, (uint32_t)(report[0] & 0x07U));
    }
}

static void tablet_report(const uint8_t *report, uint32_t bytes) {
    uint32_t x;
    uint32_t y;
    int32_t wheel = 0;
    if (!report || bytes < 5U) return;
    x = (uint32_t)rd16(report + 1U);
    y = (uint32_t)rd16(report + 3U);
    if (bytes >= 6U) wheel = (int8_t)report[5];
    mouse_inject_absolute((int32_t)x, (int32_t)y, 0x7FFF, 0x7FFF,
                          wheel, report[0] & 0x07U);
    if (!g_mouse_report_seen) {
        g_mouse_report_seen = true;
        g_tablet_last_x = x;
        g_tablet_last_y = y;
        kprintf("[INPUT] first USB tablet report x=%u y=%u buttons=%u\n",
                x, y, (uint32_t)(report[0] & 0x07U));
    } else if (!g_tablet_move_seen &&
               (x != g_tablet_last_x || y != g_tablet_last_y)) {
        g_tablet_move_seen = true;
        kprintf("[INPUT] USB tablet movement confirmed x=%u y=%u\n", x, y);
    }
    g_tablet_last_x = x;
    g_tablet_last_y = y;
}

static dwc_device_t *alloc_device(void) {
    uint32_t i;
    for (i = 0U; i < DWC_MAX_DEVICES; ++i) {
        if (!g_devices[i].active) {
            kmemset(&g_devices[i], 0, sizeof(g_devices[i]));
            return &g_devices[i];
        }
    }
    return NULL;
}

static bool hub_port_status(dwc_device_t *hub, uint8_t port,
                            uint16_t *status, uint16_t *change) {
    uint8_t raw[4];
    int rc;
    kmemset(raw, 0, sizeof(raw));
    rc = control_transfer(hub->address, hub->speed, hub->max_packet0,
                          0xA3U, USB_REQ_GET_STATUS, 0U, port,
                          raw, sizeof(raw));
    if (rc != 0) return false;
    if (status) *status = rd16(raw);
    if (change) *change = rd16(raw + 2U);
    return true;
}

static bool hub_port_feature(dwc_device_t *hub, uint8_t port,
                             uint8_t request, uint16_t feature) {
    return control_transfer(hub->address, hub->speed, hub->max_packet0,
                            0x23U, request, feature, port, NULL, 0U) == 0;
}

static int enumerate_device(uint8_t speed, uint8_t depth);

static void enumerate_hub_ports(dwc_device_t *hub, uint8_t depth) {
    uint8_t descriptor[16];
    uint8_t port;
    uint16_t status, change;
    uint8_t child_speed;
    if (!hub || depth > 1U) return;
    kmemset(descriptor, 0, sizeof(descriptor));
    if (control_transfer(hub->address, hub->speed, hub->max_packet0,
                         0xA0U, USB_REQ_GET_DESCRIPTOR,
                         (uint16_t)(DESC_HUB << 8), 0U,
                         descriptor, 9U) != 0) return;
    hub->hub_ports = descriptor[2];
    if (hub->hub_ports > 8U) hub->hub_ports = 8U;
    kprintf("[DWC2] hub addr=%u ports=%u speed=%u\n",
            hub->address, hub->hub_ports, hub->speed);
    for (port = 1U; port <= hub->hub_ports; ++port)
        (void)hub_port_feature(hub, port, USB_REQ_SET_FEATURE, HUB_PORT_POWER);
    rpi3_timer_delay_us(50000U);
    for (port = 1U; port <= hub->hub_ports; ++port) {
        if (!hub_port_status(hub, port, &status, &change)) continue;
        if ((status & HUB_STATUS_CONNECTION) == 0U) continue;
        (void)hub_port_feature(hub, port, USB_REQ_SET_FEATURE, HUB_PORT_RESET);
        rpi3_timer_delay_us(60000U);
        if (!hub_port_status(hub, port, &status, &change)) continue;
        (void)hub_port_feature(hub, port, USB_REQ_CLEAR_FEATURE,
                               HUB_C_PORT_CONNECTION);
        (void)hub_port_feature(hub, port, USB_REQ_CLEAR_FEATURE,
                               HUB_C_PORT_ENABLE);
        (void)hub_port_feature(hub, port, USB_REQ_CLEAR_FEATURE,
                               HUB_C_PORT_RESET);
        if ((status & HUB_STATUS_CONNECTION) == 0U) continue;
        if ((status & HUB_STATUS_ENABLE) == 0U) {
            kprintf("[DWC2] hub port %u connected but not enabled status=%x\n",
                    port, status);
            continue;
        }
        /* Split transactions are only required for FS/LS children behind a
           high-speed hub. Stage 9 intentionally handles the ordinary USB 1.1
           hub topology QEMU creates; real Pi 3 LAN9514 split support is the
           next DWC2 refinement if the firmware exposes it as high speed. */
        if (hub->speed == USB_SPEED_HIGH &&
            (status & HUB_STATUS_HIGH_SPEED) == 0U) {
            kprintf("[DWC2] port %u needs HS split; deferred\n", port);
            continue;
        }
        if (status & HUB_STATUS_LOW_SPEED) child_speed = USB_SPEED_LOW;
        else if (status & HUB_STATUS_HIGH_SPEED) child_speed = USB_SPEED_HIGH;
        else child_speed = USB_SPEED_FULL;
        (void)enumerate_device(child_speed, (uint8_t)(depth + 1U));
    }
}

static int enumerate_device(uint8_t speed, uint8_t depth) {
    uint8_t first8[8];
    uint8_t devdesc[18];
    uint8_t config[512];
    uint16_t total;
    uint32_t pos;
    uint8_t address;
    uint8_t maxp0 = 8U;
    dwc_device_t *dev;
    int rc;

    if (g_next_address > 120U || depth > 2U) return -30;
    kmemset(first8, 0, sizeof(first8));
    rc = control_transfer(0U, speed, 8U, 0x80U, USB_REQ_GET_DESCRIPTOR,
                          (uint16_t)(DESC_DEVICE << 8), 0U,
                          first8, sizeof(first8));
    if (rc != 0) return -31;
    if (first8[0] >= 8U && first8[7] != 0U) maxp0 = first8[7];
    address = g_next_address++;
    rc = control_transfer(0U, speed, maxp0, 0x00U, USB_REQ_SET_ADDRESS,
                          address, 0U, NULL, 0U);
    if (rc != 0) return -32;
    rpi3_timer_delay_us(5000U);

    kmemset(devdesc, 0, sizeof(devdesc));
    rc = control_transfer(address, speed, maxp0, 0x80U,
                          USB_REQ_GET_DESCRIPTOR,
                          (uint16_t)(DESC_DEVICE << 8), 0U,
                          devdesc, sizeof(devdesc));
    if (rc != 0) return -33;
    kmemset(config, 0, sizeof(config));
    rc = control_transfer(address, speed, maxp0, 0x80U,
                          USB_REQ_GET_DESCRIPTOR,
                          (uint16_t)(DESC_CONFIG << 8), 0U, config, 9U);
    if (rc != 0) return -34;
    total = rd16(config + 2U);
    if (total < 9U || total > sizeof(config)) return -35;
    rc = control_transfer(address, speed, maxp0, 0x80U,
                          USB_REQ_GET_DESCRIPTOR,
                          (uint16_t)(DESC_CONFIG << 8), 0U, config, total);
    if (rc != 0) return -36;

    dev = alloc_device();
    if (!dev) return -37;
    dev->address = address;
    dev->speed = speed;
    dev->max_packet0 = maxp0;
    dev->configuration = config[5];
    if (rd16(devdesc + 8U) == 0x0424U &&
        (rd16(devdesc + 10U) == 0xEC00U ||
         rd16(devdesc + 10U) == 0x9500U ||
         rd16(devdesc + 10U) == 0x9505U))
        dev->kind = DWC_DEV_ETHERNET;

    pos = 9U;
    while (pos + 2U <= total && config[pos] >= 2U && pos + config[pos] <= total) {
        uint8_t len = config[pos];
        uint8_t type = config[pos + 1U];
        if (type == DESC_INTERFACE && len >= 9U) {
            uint8_t klass = config[pos + 5U];
            uint8_t subclass = config[pos + 6U];
            uint8_t protocol = config[pos + 7U];
            if (dev->kind == DWC_DEV_ETHERNET) {
                dev->interface_number = config[pos + 2U];
            } else if (klass == CLASS_HUB) {
                dev->kind = DWC_DEV_HUB;
                dev->interface_number = config[pos + 2U];
            } else if (klass == CLASS_HID && subclass == 1U &&
                       (protocol == 1U || protocol == 2U)) {
                dev->kind = protocol == 1U ? DWC_DEV_KEYBOARD : DWC_DEV_MOUSE;
                dev->interface_number = config[pos + 2U];
            } else if (klass == CLASS_HID && subclass == 0U && protocol == 0U) {
                /* QEMU usb-tablet is a report-protocol HID pointer rather
                   than a boot mouse.  Accept it as an absolute pointer; this
                   also avoids host pointer-grab dependence during bring-up. */
                dev->kind = DWC_DEV_TABLET;
                dev->interface_number = config[pos + 2U];
            }
        } else if (type == DESC_ENDPOINT && len >= 7U &&
                   dev->kind == DWC_DEV_ETHERNET &&
                   (config[pos + 3U] & 3U) == EP_BULK) {
            if (config[pos + 2U] & 0x80U) {
                dev->endpoint = config[pos + 2U] & 0x0FU;
                dev->max_packet = rd16(config + pos + 4U) & 0x7FFU;
            } else {
                dev->bulk_out_endpoint = config[pos + 2U] & 0x0FU;
                dev->bulk_out_max_packet = rd16(config + pos + 4U) & 0x7FFU;
            }
        } else if (type == DESC_ENDPOINT && len >= 7U &&
                   dev->kind != DWC_DEV_NONE && (config[pos + 2U] & 0x80U) &&
                   (config[pos + 3U] & 3U) == EP_INTERRUPT && !dev->endpoint) {
            dev->endpoint = config[pos + 2U] & 0x0FU;
            dev->max_packet = rd16(config + pos + 4U) & 0x7FFU;
            dev->interval = config[pos + 6U];
        }
        pos += len;
    }

    rc = control_transfer(address, speed, maxp0, 0x00U,
                          USB_REQ_SET_CONFIGURATION,
                          dev->configuration, 0U, NULL, 0U);
    if (rc != 0) return -38;
    rpi3_timer_delay_us(5000U);

    if (dev->kind == DWC_DEV_KEYBOARD || dev->kind == DWC_DEV_MOUSE ||
        dev->kind == DWC_DEV_TABLET) {
        const char *kind_name;
        if (!dev->endpoint || !dev->max_packet) return -39;
        /* Boot keyboard/mouse accept SET_PROTOCOL. QEMU's tablet explicitly
           stalls that request, so leave it in report protocol and only set
           idle (which also activates QEMU's pointer input handler). */
        if (dev->kind != DWC_DEV_TABLET) {
            (void)control_transfer(address, speed, maxp0, 0x21U,
                                   HID_REQ_SET_PROTOCOL, 0U,
                                   dev->interface_number, NULL, 0U);
        }
        (void)control_transfer(address, speed, maxp0, 0x21U,
                               HID_REQ_SET_IDLE, 0U,
                               dev->interface_number, NULL, 0U);
        dev->active = true;
        g_device_count++;
        kind_name = dev->kind == DWC_DEV_KEYBOARD ? "keyboard" :
                    (dev->kind == DWC_DEV_MOUSE ? "mouse" : "tablet");
        kprintf("[DWC2:HID] %s addr=%u ep=%u max=%u speed=%u\n",
                kind_name, address, dev->endpoint, dev->max_packet, speed);
        return 0;
    }
    if (dev->kind == DWC_DEV_HUB) {
        dev->active = true;
        g_device_count++;
        enumerate_hub_ports(dev, depth);
        return 0;
    }
    if (dev->kind == DWC_DEV_ETHERNET) {
        if (!smsc_init(dev)) {
            kprintf("[SMSC95XX] init failed addr=%u\n", address);
            kmemset(dev, 0, sizeof(*dev));
            return -41;
        }
        dev->active = true;
        g_device_count++;
        return 0;
    }

    kprintf("[DWC2] unsupported device addr=%u class=%x interfaces=%u\n",
            address, devdesc[4], config[4]);
    kmemset(dev, 0, sizeof(*dev));
    return -40;
}

/* Return an HPRT value that is safe to write back as a control value.
 *
 * HPRT mixes ordinary R/W bits with W1C/change bits and with PRTENA, which
 * is set by the controller after a successful port reset.  Most importantly,
 * PRTRESET is an ordinary control bit: callers that want to *deassert* reset
 * must write it as zero.  Stage 9 accidentally preserved the currently-read
 * RST bit here, so the second write of the reset sequence kept RST asserted
 * forever and QEMU could never set PRTENA.
 */
static uint32_t hprt_control_base(void) {
    uint32_t v = mmio_read(DWC_HPRT);
    v &= ~(HPRT_ENA | HPRT_CONNDET | HPRT_ENCHNG | HPRT_OVRCURRCHNG |
           HPRT_RST);
    return v;
}

static int core_init(void) {
    uint32_t v;
    uint32_t speed;
    int rc;

    rc = rpi3_mailbox_set_power_state(3U, true, true);
    if (rc != 0) {
        kprintf("[DWC2] USB power mailbox rc=%d (continuing for QEMU)\n", rc);
    }
    mmio_write(DWC_PCGCCTL, 0U);
    rpi3_timer_delay_us(10000U);
    if (!wait_mask(DWC_GRSTCTL, GRSTCTL_AHBIDLE, true, 100000U)) return -1;
    mmio_write(DWC_GRSTCTL, mmio_read(DWC_GRSTCTL) | GRSTCTL_CSFTRST);
    if (!wait_mask(DWC_GRSTCTL, GRSTCTL_CSFTRST, false, 100000U)) return -2;
    rpi3_timer_delay_us(100000U);

    v = mmio_read(DWC_GUSBCFG);
    v &= ~GUSBCFG_FORCEDEV;
    v |= GUSBCFG_FORCEHOST;
    mmio_write(DWC_GUSBCFG, v);
    rpi3_timer_delay_us(50000U);

    mmio_write(DWC_GINTMSK, 0U);
    mmio_write(DWC_GAHBCFG, GAHBCFG_HBST_INCR4 | GAHBCFG_DMA_EN |
                               GAHBCFG_GLBL_INTR_EN);
    /* BCM2835/6 known-good host FIFO layout (DWORD units). */
    mmio_write(DWC_GRXFSIZ, 774U);
    mmio_write(DWC_GNPTXFSIZ, (256U << 16) | 774U);
    mmio_write(DWC_HPTXFSIZ, (512U << 16) | (774U + 256U));
    mmio_write(DWC_GRSTCTL, GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM_ALL);
    if (!wait_mask(DWC_GRSTCTL, GRSTCTL_TXFFLSH, false, 100000U)) return -3;
    mmio_write(DWC_GRSTCTL, GRSTCTL_RXFFLSH);
    if (!wait_mask(DWC_GRSTCTL, GRSTCTL_RXFFLSH, false, 100000U)) return -4;
    mmio_write(DWC_HAINTMSK, 1U);
    mmio_write(DWC_HCFG, 0U);

    v = hprt_control_base();
    if ((v & HPRT_PWR) == 0U) mmio_write(DWC_HPRT, v | HPRT_PWR);
    rpi3_timer_delay_us(100000U);
    v = mmio_read(DWC_HPRT);
    if ((v & HPRT_CONN) == 0U) {
        kprintf("[DWC2] root no-device hprt=%x\n", v);
        return -5;
    }

    kprintf("[DWC2] root before reset hprt=%x\n", v);
    /* Assert reset for >=50 ms, then explicitly deassert it.  QEMU's DWC2
       model, like the real core, sets PRTENA when it observes this 1->0
       transition while a device is attached. */
    v = hprt_control_base() | HPRT_PWR | HPRT_RST;
    mmio_write(DWC_HPRT, v);
    rpi3_timer_delay_us(60000U);
    v = hprt_control_base() | HPRT_PWR; /* RST deliberately stays clear. */
    mmio_write(DWC_HPRT, v);

    /* The hardware may need a little recovery time after reset. */
    {
        uint32_t start = rpi3_timer_now_us();
        do {
            v = mmio_read(DWC_HPRT);
            if ((v & HPRT_CONN) == 0U) break;
            if (v & HPRT_ENA) break;
        } while ((uint32_t)(rpi3_timer_now_us() - start) < 100000U);
    }
    kprintf("[DWC2] root after reset hprt=%x\n", v);
    if ((v & HPRT_CONN) == 0U || (v & HPRT_ENA) == 0U) return -6;
    speed = (v & HPRT_SPD_MASK) >> HPRT_SPD_SHIFT;
    /* DWC2 PRTSPD: 0=high, 1=full, 2=low. */
    if (speed == 0U) speed = USB_SPEED_HIGH;
    else if (speed == 2U) speed = USB_SPEED_LOW;
    else speed = USB_SPEED_FULL;
    kprintf("[DWC2] root connected hprt=%x speed=%u\n", v, speed);
    return enumerate_device((uint8_t)speed, 0U);
}

int rpi3_dwc2_hid_init(void) {
    int rc;
    kmemset(g_devices, 0, sizeof(g_devices));
    g_next_address = 1U;
    g_device_count = 0U;
    g_last_poll_us = rpi3_timer_now_us();
    g_diag_budget = 24U;
    g_mouse_report_seen = false;
    g_tablet_move_seen = false;
    g_tablet_last_x = 0U;
    g_tablet_last_y = 0U;
    g_ethernet = NULL;
    kmemset(g_ethernet_mac, 0, sizeof(g_ethernet_mac));
    rc = core_init();
    if (rc != 0) {
        kprintf("[DWC2] HID bootstrap unavailable rc=%d\n", rc);
        g_online = false;
        return rc;
    }
    g_online = true;
    kprintf("[DWC2] Stage 9 polling HID online devices=%u\n", g_device_count);
    return 0;
}

void rpi3_dwc2_hid_poll(void) {
    uint32_t now;
    uint32_t i;
    if (!g_online) return;
    now = rpi3_timer_now_us();
    if ((uint32_t)(now - g_last_poll_us) < DWC_POLL_US) return;
    g_last_poll_us = now;
    for (i = 0U; i < DWC_MAX_DEVICES; ++i) {
        dwc_device_t *dev = &g_devices[i];
        uint8_t report[16];
        uint32_t actual = 0U;
        uint32_t want;
        int rc;
        if (!dev->active || (dev->kind != DWC_DEV_KEYBOARD &&
                            dev->kind != DWC_DEV_MOUSE &&
                            dev->kind != DWC_DEV_TABLET)) continue;

        kmemset(report, 0, sizeof(report));

        /* QEMU's absolute usb-tablet is a report-protocol HID device.  Its
         * interrupt endpoint can legitimately NAK until a host event is
         * queued; on the BCM2836 DWC2 model that path is also sensitive to
         * channel DATA0/DATA1 state.  HID GET_REPORT is explicitly supported
         * by QEMU and returns the most recent absolute position even when the
         * event queue is empty, so use control polling for tablets.  Physical
         * Pi boot-HID keyboards/mice keep the interrupt path below. */
        if (dev->kind == DWC_DEV_TABLET) {
            want = 6U; /* buttons + X16 + Y16 + wheel */
            rc = control_transfer(dev->address, dev->speed, dev->max_packet0,
                                  0xA1U, HID_REQ_GET_REPORT, 0x0100U,
                                  dev->interface_number, report,
                                  (uint16_t)want);
            if (rc == 0) tablet_report(report, want);
            else if (g_diag_budget) {
                kprintf("[DWC2:HID] tablet GET_REPORT rc=%d\n", rc);
                g_diag_budget--;
            }
            continue;
        }

        want = dev->kind == DWC_DEV_KEYBOARD ? 8U : 4U;
        if (want > dev->max_packet) want = dev->max_packet;
        if (want > sizeof(report)) want = sizeof(report);
        rc = channel_transfer(dev->address, dev->speed, dev->endpoint,
                              EP_INTERRUPT, true, dev->max_packet,
                              dev->toggle ? PID_DATA1 : PID_DATA0,
                              report, want, &actual, 3000U);
        if (rc == 1) continue; /* NAK: no new HID report this frame. */
        if (rc != 0) continue;
        dev->toggle ^= 1U;
        if (dev->kind == DWC_DEV_KEYBOARD) keyboard_report(dev, report, actual);
        else mouse_report(report, actual);
    }
    smsc_poll();
}

bool rpi3_dwc2_hid_online(void) { return g_online; }
uint32_t rpi3_dwc2_hid_device_count(void) { return g_device_count; }
