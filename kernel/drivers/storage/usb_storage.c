#include "../../include/usb_storage.h"
#include "../../include/usb_uhci.h"
#include "../../include/usb_ohci.h"
#include "../../include/usb_core.h"
#include "../../include/pit.h"
#include "../../include/paging.h"
#include "../../include/types.h"
#include "../../include/memory.h"
#include "../../include/pci.h"
#include "../../include/block.h"
#include "../../include/vfs.h"
#include "../../include/vga.h"
#include "../../include/task.h"

#define EHCI_MAX_CONTROLLERS 2
#define EHCI_MAX_TD 64
#define EHCI_DIAG_MAX_PORTS 8U
#define EHCI_QH_ALIGN 128U
#define EHCI_TD_ALIGN 32U

#define USB_SUBCLASS_SCSI      0x06
#define USB_PROTOCOL_BULK_ONLY 0x50

#define USB_EP_ATTR_BULK 0x02

#define REQ_GET_DESC  0x06
#define REQ_SET_ADDR  0x05
#define REQ_SET_CONF  0x09
#define REQ_CLEAR_FEATURE 0x01

#define DESC_DEVICE 1
#define DESC_CONFIG 2
#define DESC_INTERFACE 4
#define DESC_ENDPOINT 5

#define RT_DEV_TO_HOST 0x80
#define RT_HOST_TO_DEV 0x00
#define RT_STANDARD    0x00
#define RT_DEVICE      0x00
#define RT_CLASS       0x20
#define RT_INTERFACE   0x01
#define RT_ENDPOINT    0x02

#define USB_FEATURE_ENDPOINT_HALT 0
#define MSC_REQ_RESET 0xFF

#define EHCI_USB_CMD          0x00
#define EHCI_USB_STS          0x04
#define EHCI_USB_INTR         0x08
#define EHCI_CTRLDSSEGMENT    0x10
#define EHCI_ASYNCLISTADDR    0x18
#define EHCI_CONFIGFLAG       0x40
#define EHCI_PORTSC_BASE      0x44

#define HCCPARAMS_EECP_MASK   0x0000FF00U
#define HCCPARAMS_EECP_SHIFT  8
#define USBLEGSUP_CAPID       0x01
#define USBLEGSUP_HC_BIOS     0x00010000U
#define USBLEGSUP_HC_OS       0x01000000U
#define USBLEGSUP_NEXT_MASK   0x0000FF00U
#define USBLEGSUP_NEXT_SHIFT  8

#define CMD_RS       (1U << 0)
#define CMD_HCRESET  (1U << 1)
#define CMD_ASE      (1U << 5)
#define CMD_IAAD     (1U << 6)  /* Interrupt on Async Advance Doorbell */
#define STS_IAA      (1U << 5)  /* Interrupt on Async Advance acknowledgement */
#define STS_HCHALTED (1U << 12)
#define STS_ASS      (1U << 15) /* Asynchronous schedule status */

#define PORT_CONNECT (1U << 0)
#define PORT_ENABLE  (1U << 2)
#define PORT_RESET   (1U << 8)
#define PORT_POWER   (1U << 12)
#define PORT_OWNER   (1U << 13)
#define PORT_RWC     ((1U << 1) | (1U << 3) | (1U << 5))

#define PTR_TERM 1U
#define PTR_QH   (1U << 1)

#define TD_ACTIVE      (1U << 7)
#define TD_HALTED      (1U << 6)
#define TD_DATABUFFER  (1U << 5)
#define TD_BABBLE      (1U << 4)
#define TD_XACT        (1U << 3)
#define TD_PID_OUT     0U
#define TD_PID_IN      1U
#define TD_PID_SETUP   2U

#define QH_CH_DTC      (1U << 14)
#define QH_CH_H        (1U << 15)
#define QH_CH_EPS_HIGH (2U << 12)

#define CBW_SIGNATURE 0x43425355U
#define CSW_SIGNATURE 0x53425355U

typedef struct {
    uint8_t len;
    uint8_t type;
    uint16_t usb;
    uint8_t dev_class;
    uint8_t dev_subclass;
    uint8_t dev_protocol;
    uint8_t max_packet0;
    uint16_t vendor;
    uint16_t product;
    uint16_t device;
    uint8_t manufacturer;
    uint8_t product_str;
    uint8_t serial;
    uint8_t configs;
} PACKED usb_device_desc_t;

typedef struct {
    uint8_t len;
    uint8_t type;
    uint16_t total_len;
    uint8_t interfaces;
    uint8_t config_value;
    uint8_t config_str;
    uint8_t attributes;
    uint8_t max_power;
} PACKED usb_config_desc_t;

typedef struct {
    uint8_t len;
    uint8_t type;
    uint8_t number;
    uint8_t alternate;
    uint8_t endpoints;
    uint8_t intf_class;
    uint8_t intf_subclass;
    uint8_t intf_protocol;
    uint8_t intf_str;
} PACKED usb_interface_desc_t;

typedef struct {
    uint8_t len;
    uint8_t type;
    uint8_t addr;
    uint8_t attributes;
    uint16_t max_packet;
    uint8_t interval;
} PACKED usb_endpoint_desc_t;

typedef struct {
    uint8_t type;
    uint8_t req;
    uint16_t value;
    uint16_t index;
    uint16_t len;
} PACKED usb_setup_t;

typedef struct {
    volatile uint32_t link;
    volatile uint32_t alt_link;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t ext_buffer[5];
    uint32_t next;
    uint32_t used;
    uint8_t pad[4];
} PACKED ehci_td_t;

typedef struct {
    volatile uint32_t qhlp;
    volatile uint32_t ch;
    volatile uint32_t caps;
    volatile uint32_t cur_link;
    volatile uint32_t next_link;
    volatile uint32_t alt_link;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t ext_buffer[5];
    uint32_t td_head;
    uint32_t used;
    uint8_t pad[52];
} PACKED ehci_qh_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_len;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_len;
    uint8_t cb[16];
} PACKED usb_msc_cbw_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} PACKED usb_msc_csw_t;

typedef struct {
    volatile uint8_t *cap;
    volatile uint32_t *op;
    uint32_t cap_len;
    uint32_t port_count;
    ehci_qh_t *async_head;
    ehci_qh_t *qh_pool;
    ehci_td_t *td_pool;
    volatile uint32_t transfer_lock;
    uint8_t *class_dma;
    usb_setup_t *setup_dma;
    usb_device_t *root_devices[16];
    bool faulted; /* Never recycle DMA structures after an unsafe unlink. */
} ehci_controller_t;

typedef struct {
    ehci_controller_t *hc;
    uint8_t addr;
    uint8_t max_packet0;
    uint8_t bulk_in;
    uint8_t bulk_out;
    uint8_t interface_number;
    uint8_t root_port;
    uint16_t bulk_in_packet;
    uint16_t bulk_out_packet;
    uint8_t toggle_in;
    uint8_t toggle_out;
    uint32_t sectors;
    uint32_t tag;
    /* Dedicated aligned buffers: do not DMA BOT wrappers on the C stack. */
    usb_msc_cbw_t *cbw_dma;
    usb_msc_csw_t *csw_dma;
    char name[8];
} usb_mass_device_t;

static ehci_controller_t g_hc[EHCI_MAX_CONTROLLERS];
static usb_mass_device_t g_mass;
static uint32_t g_hc_count = 0;
static uint32_t g_usb_disk_count = 0;
static uint32_t g_usb_msc_fail_log_budget = 8;
/* Recovery reads these even if enumeration failed before a block device exists. */
static uint32_t g_ehci_enum_stage = 0;
static uint32_t g_ehci_enum_port = 0;
static uint32_t g_ehci_enum_attempts = 0;
static uint32_t g_ehci_enum_failures = 0;
/* 1=CBW, 2=data, 3=CSW. Kept for recovery after the failure. */
static uint32_t g_bot_phase = 0;
static uint32_t g_bot_last_failed_phase = 0;
static uint32_t g_bot_last_opcode = 0;
static uint32_t g_bot_last_qh_token = 0;
static uint32_t g_bot_last_td_token = 0;
static uint32_t g_bot_last_usb_sts = 0;
static uint32_t g_bot_failures = 0;
static uint32_t g_ehci_unlink_failures = 0;
/* The EHCI scan retries failed devices. Keep these buffers between tries,
 * otherwise each retry leaks another pair of DMA allocations. */
static usb_msc_cbw_t *g_mass_cbw_dma = NULL;
static usb_msc_csw_t *g_mass_csw_dma = NULL;
static uint8_t *g_mass_capacity_dma = NULL;
/* DMA must not target an arbitrary VFS/ELF caller buffer: the caller may be
 * virtually mapped/noncontiguous. 16 KiB is four 4-KiB pages; allocation
 * uses the same identity-mapped heap contract as EHCI QHs and qTDs. */
#define EHCI_BOT_BOUNCE_BYTES 16384U
static uint8_t *g_mass_data_dma = NULL;
static uint32_t g_ehci_read_ok = 0, g_ehci_read_fail = 0;
static uint32_t g_ehci_write_fail = 0;
static uint32_t g_ehci_io_log_budget = 20;
#define ENUM_STAGE(x) do { g_ehci_enum_stage = (x); } while (0)

static inline void usb_io_delay(void) {
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

static void usb_delay_ms(uint32_t ms) {
    for (uint32_t m = 0; m < ms; m++) {
        for (uint32_t i = 0; i < 12000; i++) {
            usb_io_delay();
        }
    }
}

static uint16_t rd16(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t mmio_read(ehci_controller_t *hc, uint32_t off) {
    return hc->op[off / 4U];
}

static void mmio_write(ehci_controller_t *hc, uint32_t off, uint32_t value) {
    hc->op[off / 4U] = value;
}

static void *usb_alloc_aligned(uint32_t size, uint32_t align) {
    uintptr_t raw = (uintptr_t)kzalloc(size + align + sizeof(uintptr_t));
    uintptr_t aligned;
    if (!raw) return NULL;
    aligned = (raw + sizeof(uintptr_t) + align - 1U) & ~(uintptr_t)(align - 1U);
    ((uintptr_t *)aligned)[-1] = raw;
    return (void *)aligned;
}

static ehci_td_t *ehci_alloc_td(ehci_controller_t *hc) {
    for (uint32_t i = 0; i < EHCI_MAX_TD; i++) {
        if (!hc->td_pool[i].used) {
            kmemset(&hc->td_pool[i], 0, sizeof(hc->td_pool[i]));
            hc->td_pool[i].used = true;
            return &hc->td_pool[i];
        }
    }
    return NULL;
}

static void ehci_free_chain(ehci_td_t *td) {
    while (td) {
        ehci_td_t *next = (ehci_td_t *)(uintptr_t)td->next;
        td->used = false;
        td = next;
    }
}

static void ehci_td_init(ehci_td_t *td, ehci_td_t *prev, uint32_t pid,
                         uint32_t toggle, const void *data, uint32_t len) {
    uintptr_t p = (uintptr_t)data;
    if (prev) {
        prev->link = (uint32_t)(uintptr_t)td;
        prev->next = (uint32_t)(uintptr_t)td;
    }
    td->link = PTR_TERM;
    td->alt_link = PTR_TERM;
    td->next = 0;
    td->token = TD_ACTIVE | (3U << 10) | (pid << 8) |
                (len << 16) | (toggle << 31);
    for (uint32_t i = 0; i < 5; i++) {
        td->buffer[i] = (uint32_t)((p & ~0xFFFU) + (i * 0x1000U));
        td->ext_buffer[i] = 0;
    }
    td->buffer[0] = (uint32_t)p;
}

/* EHCI 1.0 4.8.2: software MUST NOT reuse an asynchronous QH or its
 * qTDs merely because a transfer has completed. The controller can still
 * have prefetched the QH. Ring IAAD and wait for USBSTS.IAA before freeing.
 * The previous code immediately recycled the descriptors after rewriting
 * the asynchronous head, which races the real ICH4 controller. */
static bool ehci_unlink_synchronized(ehci_controller_t *hc) {
    uint32_t command;
    uint32_t spins;

    __sync_synchronize();
    mmio_write(hc, EHCI_USB_STS, STS_IAA);  /* clear any stale ack */
    command = mmio_read(hc, EHCI_USB_CMD);
    mmio_write(hc, EHCI_USB_CMD, command | CMD_IAAD);
    for (spins = 0; spins < 2000000U; ++spins) {
        if (mmio_read(hc, EHCI_USB_STS) & STS_IAA) {
            mmio_write(hc, EHCI_USB_STS, STS_IAA);
            __sync_synchronize();
            return true;
        }
        __asm__ volatile ("pause");
    }

    /* Fail-safe fallback: stop the asynchronous schedule and confirm ASS=0.
     * Neither a timeout nor a broken doorbell permits descriptor reuse. */
    command = mmio_read(hc, EHCI_USB_CMD);
    mmio_write(hc, EHCI_USB_CMD, command & ~(CMD_ASE | CMD_IAAD));
    for (spins = 0; spins < 2000000U; ++spins) {
        if (!(mmio_read(hc, EHCI_USB_STS) & STS_ASS)) {
            __sync_synchronize();
            mmio_write(hc, EHCI_USB_CMD,
                       (mmio_read(hc, EHCI_USB_CMD) | CMD_ASE) & ~CMD_IAAD);
            kprintf("  [USB:EHCI] IAAD timeout; async schedule stopped/restarted safely\n");
            g_ehci_unlink_failures++;
            return true;
        }
        __asm__ volatile ("pause");
    }
    g_ehci_unlink_failures++;
    hc->faulted = true;
    kprintf("  [USB:EHCI] FATAL: async unlink never quiesced; DMA descriptors retained\n");
    return false;
}

static bool ehci_run_qh(ehci_controller_t *hc, ehci_qh_t *qh,
                        ehci_td_t *head, bool log_errors, uint32_t *result_token,
                        uint32_t timeout_ms) {
    ehci_qh_t *async = hc->async_head;
    uint32_t spins;
    uint32_t token, next, sts;
    bool completed, ok, safe;

    if (hc->faulted) return false;
    ehci_td_t *last = head;
    while (last->next) last = (ehci_td_t *)(uintptr_t)last->next;
    qh->qhlp = async->qhlp;
    qh->td_head = (uint32_t)(uintptr_t)head;
    qh->cur_link = 0;
    qh->next_link = (uint32_t)(uintptr_t)head;
    qh->alt_link = PTR_TERM;
    qh->token = 0;
    __sync_synchronize();
    async->qhlp = (uint32_t)(uintptr_t)qh | PTR_QH;
    __sync_synchronize();

    uint32_t start = pit_get_ticks();
    uint32_t hz = pit_get_frequency_hz();
    if (!hz) hz = 100U;
    uint32_t ticks = (uint32_t)(((uint64_t)(timeout_ms ? timeout_ms : 5000U) * hz + 999U) / 1000U);
    for (spins = 0; spins < 20000000U; ++spins) {
        if (pit_get_ticks() - start >= ticks) break;
        token = qh->token;
        next = qh->next_link;
        sts = mmio_read(hc, EHCI_USB_STS);
        if (((next & PTR_TERM) && !(token & TD_ACTIVE) && !(last->token & TD_ACTIVE)) ||
            (token & (TD_HALTED | TD_DATABUFFER | TD_BABBLE | TD_XACT)) ||
            (sts & (1U << 4))) break;
        __asm__ volatile ("pause");
    }
    token = qh->token;
    next = qh->next_link;
    sts = mmio_read(hc, EHCI_USB_STS);
    /* A terminated next pointer may coexist with an ACTIVE overlay while
     * the last packet is still executing (notably asynchronous disk I/O). */
    completed = (next & PTR_TERM) != 0 && !(token & TD_ACTIVE) && !(last->token & TD_ACTIVE);
    ok = completed && !(token & (TD_ACTIVE | TD_HALTED |
                                  TD_DATABUFFER | TD_BABBLE | TD_XACT));
    /* BOT requires exactly the requested data length. QH completion alone
     * is insufficient: a short data packet may leave bytes outstanding in
     * a qTD even when the overlay points to a terminated link. The descriptor
     * chain is inspected BEFORE returning it to the free pool. USB control
     * requests are excluded because a short descriptor read is legal. */
    if (result_token) *result_token = token;
    if (ok && !log_errors && !result_token) {
        ehci_td_t *td = head;
        while (td) {
            uint32_t t = td->token;
            if ((t & (TD_ACTIVE | TD_HALTED | TD_DATABUFFER |
                      TD_BABBLE | TD_XACT)) ||
                ((t >> 16) & 0x7FFFU) != 0U) {
                g_bot_last_td_token = t;
                ok = false;
                kprintf("  [USB:EHCI] BOT short/failed qTD token=%x remaining=%u\n",
                        t, (t >> 16) & 0x7FFFU);
                break;
            }
            td = (ehci_td_t *)(uintptr_t)td->next;
        }
    }
    if (!ok) {
        g_bot_last_qh_token = token;
        g_bot_last_td_token = head->token;
        g_bot_last_usb_sts = sts;
        if (log_errors || g_bot_phase != 0) {
            kprintf("  [USB:EHCI] transfer failed phase=%u sts=%x qh_token=%x td0=%x next=%x done=%u spins=%u\n",
                    g_bot_phase, sts, token, head->token, next,
                    completed, spins);
        }
    }

    /* Disconnect from the circular async list first; the doorbell ensures
     * that no hardware traversal can still reference this QH/qTD chain. */
    async->qhlp = (uint32_t)(uintptr_t)async | PTR_QH;
    safe = ehci_unlink_synchronized(hc);
    if (safe) {
        ehci_free_chain(head);
        qh->used = false;
    }
    /* Do not indiscriminately clear IAA, which belongs to the doorbell. */
    mmio_write(hc, EHCI_USB_STS, 0x1FU);
    return ok && safe;
}

static ehci_qh_t *ehci_alloc_qh(ehci_controller_t *hc, uint8_t addr,
                                uint8_t ep, uint16_t max_packet) {
    for (uint32_t i = 1; i < 8; i++) {
        ehci_qh_t *qh = &hc->qh_pool[i];
        if (!qh->used) {
            kmemset(qh, 0, sizeof(*qh));
            qh->used = true;
            qh->ch = (uint32_t)addr |
                     ((uint32_t)ep << 8) |
                     QH_CH_EPS_HIGH |
                     QH_CH_DTC |
                     ((uint32_t)max_packet << 16) |
                     (5U << 28);
            qh->caps = 1U << 30;
            qh->next_link = PTR_TERM;
            qh->alt_link = PTR_TERM;
            return qh;
        }
    }
    return NULL;
}

static bool ehci_control(ehci_controller_t *hc, uint8_t addr, uint8_t max_packet,
                         usb_setup_t *setup, void *data, uint32_t len) {
    ehci_td_t *head;
    ehci_td_t *td;
    ehci_td_t *prev;
    ehci_qh_t *qh;
    uint32_t pid;
    uint32_t toggle = 1;
    uint8_t *it = (uint8_t *)data;

    head = ehci_alloc_td(hc);
    if (!head) return false;
    prev = head;
    ehci_td_init(head, NULL, TD_PID_SETUP, 0, setup, sizeof(*setup));

    pid = (setup->type & USB_DIR_IN) ? TD_PID_IN : TD_PID_OUT;
    while (len) {
        uint32_t chunk = len > max_packet ? max_packet : len;
        td = ehci_alloc_td(hc);
        if (!td) { ehci_free_chain(head); return false; }
        ehci_td_init(td, prev, pid, toggle, it, chunk);
        prev = td;
        toggle ^= 1;
        it += chunk;
        len -= chunk;
    }

    td = ehci_alloc_td(hc);
    if (!td) { ehci_free_chain(head); return false; }
    ehci_td_init(td, prev, (setup->type & USB_DIR_IN) ? TD_PID_OUT : TD_PID_IN,
                 1, NULL, 0);

    if (setup->type & USB_DIR_IN) {
        for (ehci_td_t *data_td = (ehci_td_t *)(uintptr_t)head->next;
             data_td && data_td != td; data_td = (ehci_td_t *)(uintptr_t)data_td->next)
            data_td->alt_link = (uint32_t)(uintptr_t)td;
    }
    qh = ehci_alloc_qh(hc, addr, 0, max_packet);
    if (!qh) { ehci_free_chain(head); return false; }
    return ehci_run_qh(hc, qh, head, true, NULL, 5000U);
}

static bool ehci_class_lock(ehci_controller_t *hc);

static bool usb_request(ehci_controller_t *hc, uint8_t addr, uint8_t max_packet,
                        uint8_t type, uint8_t req, uint16_t value,
                        uint16_t index, uint16_t len, void *data) {
    if (len > 2048U || (len && !data) || !ehci_class_lock(hc)) return false;
    usb_setup_t *setup = hc->setup_dma;
    setup->type = type;
    setup->req = req;
    setup->value = value;
    setup->index = index;
    setup->len = len;
    if (len) {
        if (type & USB_DIR_IN) kmemset(hc->class_dma, 0, len);
        else kmemcpy(hc->class_dma, data, len);
    }
    bool ok = ehci_control(hc, addr, max_packet, setup, hc->class_dma, len);
    __sync_synchronize();
    if (ok && len && (type & USB_DIR_IN)) kmemcpy(data, hc->class_dma, len);
    __sync_lock_release(&hc->transfer_lock);
    return ok;
}

#include "../usb/ehci_class.inc"

static bool usb_clear_halt(usb_mass_device_t *dev, uint8_t endpoint) {
    return usb_request(dev->hc, dev->addr, dev->max_packet0,
                       RT_HOST_TO_DEV | RT_STANDARD | RT_ENDPOINT,
                       REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT,
                       endpoint, 0, NULL);
}

static bool usb_msc_reset_recovery(usb_mass_device_t *dev) {
    bool ok;
    if (!dev || !dev->hc || dev->hc->faulted) return false;
    kprintf("  [USB:BOT] reset recovery addr=%u intf=%u\n",
            dev->addr, dev->interface_number);

    ok = usb_request(dev->hc, dev->addr, dev->max_packet0,
                     RT_HOST_TO_DEV | RT_CLASS | RT_INTERFACE,
                     MSC_REQ_RESET, 0, dev->interface_number, 0, NULL);
    usb_delay_ms(20);
    ok = usb_clear_halt(dev, dev->bulk_in) && ok;
    ok = usb_clear_halt(dev, dev->bulk_out) && ok;
    dev->toggle_in = 0;
    dev->toggle_out = 0;
    mmio_write(dev->hc, EHCI_USB_STS, 0x3F);
    return ok;
}

static bool ehci_bulk(usb_mass_device_t *dev, bool in, void *data,
                      uint32_t len) {
    ehci_controller_t *hc = dev->hc;
    uint8_t ep = in ? (dev->bulk_in & 0x0F) : (dev->bulk_out & 0x0F);
    uint16_t max_packet = in ? dev->bulk_in_packet : dev->bulk_out_packet;
    uint8_t *it = (uint8_t *)data;
    uint32_t left = len;
    uint32_t pid = in ? TD_PID_IN : TD_PID_OUT;
    uint32_t toggle = in ? dev->toggle_in : dev->toggle_out;

    if (!hc || hc->faulted || !g_mass_data_dma) return false;
    if (!max_packet || max_packet > 512U) return false;
    if (len == 0) return true;
    if (!data) return false;
    /* BOT may request >16 KiB in a single data stage; split it into safe
     * DMA batches while keeping a continuous data toggle sequence. */
    while (left) {
        uint32_t chunk = left > EHCI_BOT_BOUNCE_BYTES ?
                         EHCI_BOT_BOUNCE_BYTES : left;
        uint32_t packets = (chunk + max_packet - 1U) / max_packet;
        ehci_td_t *td;
        ehci_qh_t *qh;
        if (!in) kmemcpy(g_mass_data_dma, it, chunk);
        else kmemset(g_mass_data_dma, 0, chunk);
        td = ehci_alloc_td(hc);
        if (!td) return false;
        ehci_td_init(td, NULL, pid, toggle, g_mass_data_dma, chunk);
        qh = ehci_alloc_qh(hc, dev->addr, ep, max_packet);
        if (!qh) { ehci_free_chain(td); return false; }
        if (!ehci_run_qh(hc, qh, td, false, NULL, 5000U)) return false;
        __sync_synchronize();
        if (in) kmemcpy(it, g_mass_data_dma, chunk);
        toggle ^= packets & 1U;
        it += chunk;
        left -= chunk;
    }
    if (in) dev->toggle_in = (uint8_t)toggle;
    else dev->toggle_out = (uint8_t)toggle;
    return true;
}

static bool usb_msc_command_inner(usb_mass_device_t *dev, const uint8_t *cmd,
                            uint8_t cmd_len, bool in, void *data,
                            uint32_t data_len) {
    usb_msc_cbw_t *cbw = dev->cbw_dma;
    usb_msc_csw_t *csw = dev->csw_dma;
    uint32_t tag;

    if (!cbw || !csw || cmd_len == 0 || cmd_len > sizeof(cbw->cb))
        return false;
    kmemset(cbw, 0, sizeof(*cbw));
    cbw->signature = CBW_SIGNATURE;
    tag = ++dev->tag;
    cbw->tag = tag;
    cbw->data_len = data_len;
    cbw->flags = in ? USB_DIR_IN : 0;
    cbw->cb_len = cmd_len;
    kmemcpy(cbw->cb, cmd, cmd_len);
    g_bot_last_opcode = cmd[0];

    g_bot_phase = 1; /* CBW: always 31 bytes OUT */
    if (!ehci_bulk(dev, false, cbw, sizeof(*cbw))) goto failed;
    g_bot_phase = 2; /* optional data stage */
    if (data_len && !ehci_bulk(dev, in, data, data_len)) goto failed;
    kmemset(csw, 0, sizeof(*csw));
    g_bot_phase = 3; /* CSW: 13 bytes IN */
    if (!ehci_bulk(dev, true, csw, sizeof(*csw))) goto failed;
    if (csw->signature != CSW_SIGNATURE || csw->tag != tag ||
        csw->status != 0 || csw->residue != 0) {
        kprintf("  [USB:BOT] bad CSW signature=%x tag=%x expected=%x status=%u residue=%u\n",
                csw->signature, csw->tag, tag, csw->status, csw->residue);
        goto failed;
    }
    g_bot_phase = 0;
    return true;

failed:
    g_bot_last_failed_phase = g_bot_phase;
    g_bot_failures++;
    kprintf("  [USB:BOT] op=%x FAIL phase=%u (1=CBW 2=DATA 3=CSW) count=%u\n",
            cmd[0], g_bot_phase, g_bot_failures);
    g_bot_phase = 0;
    return false;
}

static bool usb_msc_command(usb_mass_device_t *dev, const uint8_t *cmd,
                            uint8_t cmd_len, bool in, void *data, uint32_t length) {
    if (!dev || !ehci_class_lock(dev->hc)) return false;
    bool ok = usb_msc_command_inner(dev, cmd, cmd_len, in, data, length);
    __sync_lock_release(&dev->hc->transfer_lock);
    return ok;
}

static bool usb_msc_command_retry(usb_mass_device_t *dev, const uint8_t *cmd,
                                  uint8_t cmd_len, bool in, void *data,
                                  uint32_t data_len) {
    if (usb_msc_command(dev, cmd, cmd_len, in, data, data_len))
        return true;
    if (!usb_msc_reset_recovery(dev))
        return false;
    if (usb_msc_command(dev, cmd, cmd_len, in, data, data_len))
        return true;
    if (g_usb_msc_fail_log_budget) {
        kprintf("  [USB] MSC command failed after recovery op=0x%x len=%u\n",
                cmd && cmd_len ? cmd[0] : 0, data_len);
        g_usb_msc_fail_log_budget--;
    }
    return false;
}

static bool usb_msc_read_capacity(usb_mass_device_t *dev) {
    uint8_t cmd[10];
    uint8_t *data = g_mass_capacity_dma;
    if (!data) return false;
    kmemset(cmd, 0, sizeof(cmd));
    kmemset(data, 0, 8);
    cmd[0] = 0x25;
    if (!usb_msc_command_retry(dev, cmd, sizeof(cmd), true, data, 8))
        return false;
    dev->sectors = rd32be(data) + 1U;
    return rd32be(data + 4) == BLOCK_SECTOR_SIZE && dev->sectors != 0;
}

static bool usb_msc_rw(usb_mass_device_t *dev, uint32_t lba, uint8_t count,
                       void *buffer, bool write) {
    uint8_t cmd[10];
    kmemset(cmd, 0, sizeof(cmd));
    cmd[0] = write ? 0x2A : 0x28;
    wr32be(cmd + 2, lba);
    cmd[7] = 0;
    cmd[8] = count;
    return usb_msc_command_retry(dev, cmd, sizeof(cmd), !write, buffer,
                                 (uint32_t)count * BLOCK_SECTOR_SIZE);
}

static bool usb_block_read(block_device_t *block, uint32_t lba, uint8_t count,
                           void *buffer) {
    usb_mass_device_t *dev = (usb_mass_device_t *)block->driver_data;
    bool ok = dev && buffer && count &&
              usb_msc_rw(dev, lba, count, buffer, false);
    if (ok) g_ehci_read_ok++;
    else {
        g_ehci_read_fail++;
        if (g_ehci_io_log_budget) {
            kprintf("[USB:IO] EHCI READ FAIL disk=%s lba=%u sectors=%u total_fail=%u\n",
                    block ? block->name : "?", lba, count, g_ehci_read_fail);
            g_ehci_io_log_budget--;
        }
    }
    return ok;
}

static bool usb_block_write(block_device_t *block, uint32_t lba, uint8_t count,
                            const void *buffer) {
    usb_mass_device_t *dev = (usb_mass_device_t *)block->driver_data;
    if (!dev || !buffer || !count ||
        !usb_msc_rw(dev, lba, count, (void *)buffer, true)) {
        g_ehci_write_fail++;
        if (g_ehci_io_log_budget) {
            kprintf("[USB:IO] EHCI WRITE FAIL disk=%s lba=%u sectors=%u total_fail=%u\n",
                    block ? block->name : "?", lba, count, g_ehci_write_fail);
            g_ehci_io_log_budget--;
        }
        return false;
    }
    /* SYNCHRONIZE CACHE(10), IMMED=0: match ATA/AHCI durable write
     * completion. Failed/unsupported flush is an I/O error, never success. */
    uint8_t flush[10];kmemset(flush,0,sizeof(flush));flush[0]=0x35U;
    return usb_msc_command_retry(dev,flush,sizeof(flush),false,NULL,0U);
}

static bool usb_try_mass_storage(ehci_controller_t *hc, uint32_t port) {
    usb_device_desc_t dd;
    if (hc->faulted) return false;
    uint8_t cfg[2048];
    usb_config_desc_t *cd = (usb_config_desc_t *)cfg;
    usb_interface_desc_t *picked_intf = NULL;
    uint8_t addr = (uint8_t)(port + 1U);
    uint8_t config_value;
    uint8_t bulk_in = 0, bulk_out = 0;
    uint16_t bulk_in_packet = 0, bulk_out_packet = 0;
    uint8_t *p;
    uint8_t *end;

    g_ehci_enum_port = port + 1U;
    g_ehci_enum_attempts++;
    ENUM_STAGE(1);
    kprintf("  [USB:ENUM] EHCI port=%u attempt=%u GET_DESCRIPTOR(8)\n",
            port + 1U, g_ehci_enum_attempts);
    kmemset(&dd, 0, sizeof(dd));
    if (!usb_request(hc, 0, 8, RT_DEV_TO_HOST | RT_STANDARD | RT_DEVICE,
                     REQ_GET_DESC, (DESC_DEVICE << 8), 0, 8, &dd)) {
        kprintf("  [USB:ENUM] port=%u FAIL descriptor8\n", port + 1U);
        g_ehci_enum_failures++;
        return false;
    }
    kprintf("  [USB:ENUM] descriptor8 len=%u type=%u ep0=%u\n",
            dd.len, dd.type, dd.max_packet0);
    if (dd.type != DESC_DEVICE || dd.len < 8U ||
        (dd.max_packet0 != 8U && dd.max_packet0 != 16U &&
         dd.max_packet0 != 32U && dd.max_packet0 != 64U)) {
        kprintf("  [USB:ENUM] descriptor8 invalido\n");
        g_ehci_enum_failures++;
        return false;
    }
    ENUM_STAGE(2);
    if (!usb_request(hc, 0, dd.max_packet0,
                     RT_HOST_TO_DEV | RT_STANDARD | RT_DEVICE,
                     REQ_SET_ADDR, addr, 0, 0, NULL)) {
        kprintf("  [USB:ENUM] FAIL SET_ADDRESS\n");
        g_ehci_enum_failures++;
        return false;
    }
    usb_delay_ms(10);
    ENUM_STAGE(3);

    if (!usb_request(hc, addr, dd.max_packet0,
                     RT_DEV_TO_HOST | RT_STANDARD | RT_DEVICE,
                     REQ_GET_DESC, (DESC_DEVICE << 8), 0, sizeof(dd), &dd)) {
        kprintf("  [USB:ENUM] FAIL descriptor18 addr=%u\n", addr);
        g_ehci_enum_failures++;
        return false;
    }
    kprintf("  [USB:ENUM] device vid=%x pid=%x configurations=%u\n",
            dd.vendor, dd.product, dd.configs);
    ENUM_STAGE(4);

    if (!usb_request(hc, addr, dd.max_packet0,
                     RT_DEV_TO_HOST | RT_STANDARD | RT_DEVICE,
                     REQ_GET_DESC, (DESC_CONFIG << 8), 0, 4, cfg)) {
        kprintf("  [USB:ENUM] FAIL config header\n");
        g_ehci_enum_failures++;
        return false;
    }
    if (cd->type != DESC_CONFIG || cd->total_len < sizeof(*cd) ||
        cd->total_len > sizeof(cfg)) {
        kprintf("  [USB:ENUM] config header invalido len=%u type=%u\n",
                cd->total_len, cd->type);
        g_ehci_enum_failures++;
        return false;
    }
    ENUM_STAGE(5);
    if (!usb_request(hc, addr, dd.max_packet0,
                     RT_DEV_TO_HOST | RT_STANDARD | RT_DEVICE,
                     REQ_GET_DESC, (DESC_CONFIG << 8), 0, cd->total_len, cfg)) {
        kprintf("  [USB:ENUM] FAIL full config len=%u\n", cd->total_len);
        g_ehci_enum_failures++;
        return false;
    }
    kprintf("  [USB:ENUM] config=%u interfaces=%u total=%u\n",
            cd->config_value, cd->interfaces, cd->total_len);
    ENUM_STAGE(6);

    config_value = cd->config_value;
    p = cfg + cd->len;
    end = cfg + cd->total_len;
    while (p + 2 <= end && p[0] >= 2U) {
        if (p + p[0] > end) break;
        if (p[1] == DESC_INTERFACE && p[0] >= sizeof(usb_interface_desc_t)) {
            usb_interface_desc_t *id = (usb_interface_desc_t *)p;
            if (picked_intf) break; /* endpoints belong to one interface */
            if (id->intf_class == USB_CLASS_MASS_STORAGE &&
                id->intf_subclass == USB_SUBCLASS_SCSI &&
                id->intf_protocol == USB_PROTOCOL_BULK_ONLY &&
                id->alternate == 0) {
                picked_intf = id;
                bulk_in = bulk_out = 0;
                kprintf("  [USB:ENUM] MSC interface=%u subclass=%x protocol=%x\n",
                        id->number, id->intf_subclass, id->intf_protocol);
            }
        } else if (picked_intf && p[1] == DESC_ENDPOINT && p[0] >= sizeof(usb_endpoint_desc_t)) {
            usb_endpoint_desc_t *ed = (usb_endpoint_desc_t *)p;
            if ((ed->attributes & 0x03) == USB_EP_ATTR_BULK) {
                if (ed->addr & USB_DIR_IN) {
                    bulk_in = ed->addr;
                    bulk_in_packet = rd16(&ed->max_packet);
                } else {
                    bulk_out = ed->addr;
                    bulk_out_packet = rd16(&ed->max_packet);
                }
            }
        }
        p += p[0];
    }

    if (picked_intf && (!bulk_in || !bulk_out)) {
        kprintf("  [USB:ENUM] no MSC BOT interface/endpoints in=%x out=%x\n",
                bulk_in, bulk_out);
        g_ehci_enum_failures++;
        return false;
    }
    kprintf("  [USB:ENUM] bulk in=%x mps=%u out=%x mps=%u\n",
            bulk_in, bulk_in_packet, bulk_out, bulk_out_packet);
    ENUM_STAGE(7);
    if (!usb_request(hc, addr, dd.max_packet0,
                     RT_HOST_TO_DEV | RT_STANDARD | RT_DEVICE,
                     REQ_SET_CONF, config_value, 0, 0, NULL)) {
        kprintf("  [USB:ENUM] FAIL SET_CONFIGURATION=%u\n", config_value);
        g_ehci_enum_failures++;
        return false;
    }
    /* ICH4 / older flash firmware may still be settling endpoints after
     * SET_CONFIGURATION. Never submit a BOT CBW immediately afterward. */
    usb_delay_ms(150);
    ENUM_STAGE(8);
    hc->root_devices[port] = usb_host_publish(hc, &ehci_class_ops, addr,
        (uint8_t)port, USB_SPEED_HIGH, (const uint8_t *)&dd, cfg, cd->total_len);
    if (!hc->root_devices[port]) return false;
    if (!picked_intf || g_usb_disk_count ||
        (g_mass.name[0] && vfs_volume_busy(g_mass.name))) return true;

    kmemset(&g_mass, 0, sizeof(g_mass));
    g_mass.hc = hc;
    g_mass.root_port = (uint8_t)port;
    g_mass.addr = addr;
    g_mass.max_packet0 = dd.max_packet0;
    g_mass.bulk_in = bulk_in;
    g_mass.bulk_out = bulk_out;
    g_mass.interface_number = picked_intf->number;
    g_mass.bulk_in_packet = bulk_in_packet;
    g_mass.bulk_out_packet = bulk_out_packet;
    if (!g_mass_cbw_dma)
        g_mass_cbw_dma = (usb_msc_cbw_t *)usb_alloc_aligned(64U, 64U);
    if (!g_mass_csw_dma)
        g_mass_csw_dma = (usb_msc_csw_t *)usb_alloc_aligned(64U, 64U);
    if (!g_mass_capacity_dma)
        g_mass_capacity_dma = (uint8_t *)usb_alloc_aligned(64U, 64U);
    if (!g_mass_data_dma)
        g_mass_data_dma = (uint8_t *)usb_alloc_aligned(
            EHCI_BOT_BOUNCE_BYTES, 4096U);
    g_mass.cbw_dma = g_mass_cbw_dma;
    g_mass.csw_dma = g_mass_csw_dma;
    if (!g_mass.cbw_dma || !g_mass.csw_dma || !g_mass_capacity_dma ||
        !g_mass_data_dma) {
        kprintf("  [USB:BOT] no memory for aligned DMA buffers\n");
        g_ehci_enum_failures++;
        return false;
    }
    g_mass.name[0] = 'u';
    g_mass.name[1] = 's';
    g_mass.name[2] = 'b';
    unsigned name_slot;
    for (name_slot = 0; name_slot < 10U; name_slot++) {
        g_mass.name[3] = (char)('0' + name_slot); g_mass.name[4] = '\0';
        if (!block_get(g_mass.name) && !vfs_volume_busy(g_mass.name)) break;
    }
    if (name_slot == 10U) return false;
    g_mass.name[4] = '\0';

    if (!usb_msc_read_capacity(&g_mass)) {
        kprintf("  [USB:ENUM] FAIL READ_CAPACITY / MSC BOT\n");
        g_ehci_enum_failures++;
        return false;
    }

    if (!block_register_ex(g_mass.name, BLOCK_DEVICE_USB, g_mass.sectors,
                           BLOCK_SECTOR_SIZE, false, &g_mass,
                           usb_block_read))
        return false;
    block_set_writer(g_mass.name, usb_block_write);
    g_usb_disk_count++;
    ENUM_STAGE(9);
    kprintf("  [USB] %s: mass storage %u sectores\n",
            g_mass.name, g_mass.sectors);
    return true;
}

/* Intel ICH4 uses UHCI companion controllers for full/low-speed USB.
 * An EHCI reset only enables the port for a HIGH-speed device. If the
 * connected device cannot be enabled by EHCI, set PORT_OWNER and let UHCI
 * enumerate it. Never hand over an enabled high-speed port. */
static bool ehci_hand_to_companion(ehci_controller_t *hc, uint32_t port,
                                   uint32_t status) {
    volatile uint32_t *reg = &hc->op[EHCI_PORTSC_BASE / 4U + port];
    if (!(status & PORT_CONNECT) || (status & PORT_ENABLE)) return false;
    if (!(status & PORT_OWNER)) {
        *reg = ((status | PORT_OWNER | PORT_POWER) & ~PORT_RESET) & ~PORT_RWC;
        usb_delay_ms(10);
    }
    status = *reg;
    kprintf("  [USB:ICH4] EHCI port %u -> UHCI owner=%u connect=%u status=%x\n",
            port + 1U, (status & PORT_OWNER) != 0,
            (status & PORT_CONNECT) != 0, status);
    return (status & PORT_OWNER) != 0;
}

static bool ehci_reset_port(ehci_controller_t *hc, uint32_t port) {
    volatile uint32_t *reg = &hc->op[EHCI_PORTSC_BASE / 4U + port];
    uint32_t status = *reg;
    if (!(status & PORT_CONNECT)) return false;
    if (status & PORT_OWNER) {
        kprintf("  [USB] puerto %u delegado a companion, status=%x\n", port, status);
        return false;
    }
    *reg = (status | PORT_POWER) & ~PORT_RWC;
    usb_delay_ms(20);
    status = *reg;
    *reg = (status | PORT_RESET | PORT_POWER) & ~PORT_RWC;
    usb_delay_ms(50);
    status = *reg;
    *reg = (status & ~PORT_RESET) & ~PORT_RWC;
    usb_delay_ms(100);
    status = *reg;
    kprintf("  [USB:ICH4] EHCI port %u after reset=%x connected=%u enabled=%u owner=%u\n",
            port + 1U, status, (status & PORT_CONNECT) != 0,
            (status & PORT_ENABLE) != 0, (status & PORT_OWNER) != 0);
    if ((status & PORT_CONNECT) && (status & PORT_ENABLE) &&
        !(status & PORT_OWNER)) {
        /* Port reset itself may latch CSC.  A pending stale W1C bit must
         * not be mistaken for a later physical unplug/replug by hotplug.
         * A real subsequent change latches CSC again after this ack. */
        *reg = (status & ~PORT_RWC) | (status & (1U << 1));
        __sync_synchronize();
    }
    if ((status & PORT_CONNECT) && !(status & (PORT_ENABLE | PORT_OWNER))) {
        /* No high-speed chirp: make the device visible to its UHCI
         * companion BEFORE usb_uhci_init() scans root ports. */
        (void)ehci_hand_to_companion(hc, port, status);
    }
    return (status & PORT_ENABLE) != 0 && !(status & PORT_OWNER);
}

static void ehci_take_ownership(const pci_device_t *pci, uint32_t hcc_params) {
    uint8_t eecp = (uint8_t)((hcc_params & HCCPARAMS_EECP_MASK) >>
                             HCCPARAMS_EECP_SHIFT);
    uint32_t guard = 0;

    while (eecp >= 0x40 && guard++ < 16) {
        uint32_t cap = pci_config_read32(pci->bus, pci->slot,
                                         pci->function, eecp);
        if ((cap & 0xFFU) == USBLEGSUP_CAPID) {
            if (cap & USBLEGSUP_HC_BIOS) {
                kprintf("  [USB] EHCI legacy BIOS owned, pidiendo ownership\n");
                pci_config_write32(pci->bus, pci->slot, pci->function,
                                   eecp, cap | USBLEGSUP_HC_OS);
                for (uint32_t i = 0; i < 100000; i++) {
                    cap = pci_config_read32(pci->bus, pci->slot,
                                            pci->function, eecp);
                    if (!(cap & USBLEGSUP_HC_BIOS) &&
                        (cap & USBLEGSUP_HC_OS))
                        break;
                }
            }
            pci_config_write32(pci->bus, pci->slot, pci->function,
                               eecp + 4, 0);
            return;
        }
        eecp = (uint8_t)((cap & USBLEGSUP_NEXT_MASK) >>
                         USBLEGSUP_NEXT_SHIFT);
    }
}

static void ehci_init_controller(const pci_device_t *pci) {
    pci_bar_info_t bar;
    ehci_controller_t *hc;
    uint32_t hcs;
    uint32_t hcc;
    uint32_t timeout;
    uint32_t cmd;

    if (g_hc_count >= EHCI_MAX_CONTROLLERS) return;
    if (!pci_get_bar_info(pci, 0, &bar) || bar.is_io || bar.is_64 ||
        !bar.base || bar.size < EHCI_PORTSC_BASE + 4U) {
        kprintf("  [USB:ICH4] EHCI PCI %u:%u.%u BAR0 MMIO invalida\n",
                pci->bus, pci->slot, pci->function);
        return;
    }
    /* The kernel identity map is otherwise write-back, including PCI MMIO.
     * AHCI already makes this exact call. On real ICH4 hardware the EHCI
     * command/status/port registers MUST be read uncached. */
    if (!paging_kernel_mmio_uncached(bar.base, bar.size)) {
        kprintf("  [USB:ICH4] EHCI MMIO uncached fallo base=%x size=%x\n",
                bar.base, bar.size);
        return;
    }
    if (sizeof(ehci_qh_t) != 128U || sizeof(ehci_td_t) != 64U) {
        kprintf("  [USB] EHCI struct size invalido qh=%u td=%u\n",
                sizeof(ehci_qh_t), sizeof(ehci_td_t));
        return;
    }

    if (!pci_enable_command(pci, PCI_COMMAND_MEMORY | PCI_COMMAND_BUSMASTER) ||
        (pci_config_read16(pci->bus, pci->slot, pci->function, 0x04) &
         (PCI_COMMAND_MEMORY | PCI_COMMAND_BUSMASTER)) !=
         (PCI_COMMAND_MEMORY | PCI_COMMAND_BUSMASTER)) {
        kprintf("  [USB:ICH4] EHCI no habilito PCI MMIO/busmaster\n");
        return;
    }

    hc = &g_hc[g_hc_count];
    kmemset(hc, 0, sizeof(*hc));
    hc->cap = (volatile uint8_t *)(uintptr_t)bar.base;
    hc->cap_len = hc->cap[0];
    hc->op = (volatile uint32_t *)(uintptr_t)(bar.base + hc->cap_len);
    hcs = *(volatile uint32_t *)(uintptr_t)(bar.base + 4);
    hcc = *(volatile uint32_t *)(uintptr_t)(bar.base + 8);
    hc->port_count = hcs & 0x0F;
    if (!hc->cap_len || hc->cap_len < 0x10U || hc->cap_len > 0x80U ||
        !hc->port_count ||
        EHCI_PORTSC_BASE + 4U * hc->port_count > bar.size) {
        kprintf("  [USB:ICH4] EHCI CAP invalida len=%u ports=%u size=%x\n",
                hc->cap_len, hc->port_count, bar.size);
        return;
    }
    kprintf("  [USB:ICH4] EHCI PCI %u:%u.%u id=%x:%x BAR=%x size=%x ports=%u cfg=%x\n",
            pci->bus, pci->slot, pci->function, pci->vendor_id,
            pci->device_id, bar.base, bar.size, hc->port_count,
            pci_config_read16(pci->bus, pci->slot, pci->function, 0x04));

    ehci_take_ownership(pci, hcc);

    cmd = mmio_read(hc, EHCI_USB_CMD);
    mmio_write(hc, EHCI_USB_CMD, cmd & ~CMD_RS);
    timeout = 100000;
    while (timeout-- && !(mmio_read(hc, EHCI_USB_STS) & STS_HCHALTED)) {}
    mmio_write(hc, EHCI_USB_CMD, CMD_HCRESET);
    timeout = 100000;
    while (timeout-- && (mmio_read(hc, EHCI_USB_CMD) & CMD_HCRESET)) {}

    hc->qh_pool = (ehci_qh_t *)usb_alloc_aligned(sizeof(ehci_qh_t) * 8,
                                                 EHCI_QH_ALIGN);
    hc->td_pool = (ehci_td_t *)usb_alloc_aligned(sizeof(ehci_td_t) * EHCI_MAX_TD,
                                                 EHCI_TD_ALIGN);
    hc->class_dma = usb_alloc_aligned(4096U, 4096U);
    hc->setup_dma = usb_alloc_aligned(64U, 64U);
    if (!hc->qh_pool || !hc->td_pool || !hc->class_dma || !hc->setup_dma) return;

    hc->async_head = &hc->qh_pool[0];
    kmemset(hc->async_head, 0, sizeof(*hc->async_head));
    hc->async_head->used = true;
    hc->async_head->qhlp = (uint32_t)(uintptr_t)hc->async_head | PTR_QH;
    hc->async_head->ch = QH_CH_H | QH_CH_EPS_HIGH | QH_CH_DTC |
                         (64U << 16);
    hc->async_head->caps = 1U << 30;
    hc->async_head->next_link = PTR_TERM;
    hc->async_head->alt_link = PTR_TERM;

    mmio_write(hc, EHCI_CTRLDSSEGMENT, 0);
    mmio_write(hc, EHCI_USB_INTR, 0);
    mmio_write(hc, EHCI_ASYNCLISTADDR,
               (uint32_t)(uintptr_t)hc->async_head);
    mmio_write(hc, EHCI_USB_STS, 0x3F);
    mmio_write(hc, EHCI_USB_CMD, CMD_RS | CMD_ASE);
    usb_delay_ms(10);
    /* Route ports to EHCI only once its run state has been requested.
     * Full/low-speed ports that do not enable after reset are then passed
     * back to the UHCI companion via PORT_OWNER below. */
    mmio_write(hc, EHCI_CONFIGFLAG, 1);
    usb_delay_ms(50);
    if ((mmio_read(hc, EHCI_USB_STS) & STS_HCHALTED) != 0U) {
        kprintf("  [USB:ICH4] EHCI detenido cmd=%x sts=%x\n",
                mmio_read(hc, EHCI_USB_CMD), mmio_read(hc, EHCI_USB_STS));
        return;
    }
    /* Only mark fully initialized controllers for the hotplug task. */
    g_hc_count++;
    kprintf("  [USB:ICH4] EHCI cfgflag=%x sts=%x\n",
            mmio_read(hc, EHCI_CONFIGFLAG), mmio_read(hc, EHCI_USB_STS));
    for (uint32_t port = 0; port < hc->port_count; ++port) {
        uint32_t st = hc->op[EHCI_PORTSC_BASE / 4U + port];
        kprintf("  [USB:ICH4] EHCI port %u before=%x CCS=%u PED=%u OWNER=%u\n",
                port + 1U, st, (st & PORT_CONNECT) != 0,
                (st & PORT_ENABLE) != 0, (st & PORT_OWNER) != 0);
    }

    kprintf("  [USB] EHCI %u puertos en MMIO %x\n", hc->port_count, bar.base);
    /* Enumeration follows UHCI/core initialization, so shared device state
     * is not subsequently erased. Companion handoff must happen first. */
    for (uint32_t port = 0; port < hc->port_count; port++)
        (void)ehci_reset_port(hc, port);
}

static void usb_storage_scan_ports(void) {
    for (uint32_t h = 0; h < g_hc_count; h++) {
        ehci_controller_t *hc = &g_hc[h];
        if (!hc->op || hc->faulted) continue;
        for (uint32_t port = 0; port < hc->port_count; port++) {
            uint32_t status = hc->op[EHCI_PORTSC_BASE / 4U + port];
            usb_device_t *old = hc->root_devices[port];
            if (old && (!(status & PORT_CONNECT) || (status & (1U << 1)))) {
                if (!ehci_class_lock(hc)) continue;
                usb_host_disconnect(old);
                hc->root_devices[port] = NULL;
                if (g_usb_disk_count && g_mass.hc == hc && g_mass.root_port == port) {
                    (void)block_unregister(g_mass.name);
                    g_usb_disk_count = 0;
                }
                __sync_lock_release(&hc->transfer_lock);
                old = NULL;
            }
            if (old || !(status & PORT_CONNECT) || (status & PORT_OWNER)) continue;
            if (ehci_reset_port(hc, port)) (void)usb_try_mass_storage(hc, port);
        }
    }
}

static void usb_storage_hotplug_main(void *arg UNUSED) {
    for (;;) {
        usb_storage_scan_ports();
        task_sleep(100);
    }
}

void usb_storage_init(void) {
    int index = -1;
    g_hc_count = 0;
    g_usb_disk_count = 0;
    for (;;) {
        index = pci_find_by_class(0x0C, 0x03, (uint32_t)(index + 1));
        if (index < 0) break;
        const pci_device_t *dev = pci_device_at((uint32_t)index);
        if (dev && dev->prog_if == 0x20)
            ehci_init_controller(dev);
    }

    /* USB 1.1 companion/legacy controllers, including Intel PIIX4M. */
    usb_uhci_init();
    usb_ohci_init();
    usb_storage_scan_ports();
}

/* Recovery-only, read-only diagnostics; never reinitialize a running HCD
 * from shell because that could invalidate registered block-device buffers. */
void usb_storage_diagnostics(void) {
    kprintf("[USB-DIAG] EHCI reads_ok=%u reads_fail=%u writes_fail=%u\n",
            g_ehci_read_ok, g_ehci_read_fail, g_ehci_write_fail);
    kprintf("[USB-DIAG] BOT last_op=%x last_failed_phase=%u failures=%u qh=%x td0=%x sts=%x unlink_recoveries=%u\n",
            g_bot_last_opcode, g_bot_last_failed_phase, g_bot_failures,
            g_bot_last_qh_token, g_bot_last_td_token,
            g_bot_last_usb_sts, g_ehci_unlink_failures);
    kprintf("[USB-DIAG] EHCI ready=%u enum_stage=%u port=%u attempts=%u failures=%u\n",
            g_hc_count, g_ehci_enum_stage, g_ehci_enum_port,
            g_ehci_enum_attempts, g_ehci_enum_failures);
    for (uint32_t i = 0; i < pci_device_count(); ++i) {
        const pci_device_t *pci = pci_device_at(i);
        if (!pci || pci->class_code != 0x0CU ||
            pci->subclass != 0x03U || pci->prog_if != 0x20U) continue;
        kprintf("[USB-DIAG] EHCI PCI %u:%u.%u %x:%x cmd=%x BAR0=%x\n",
                pci->bus, pci->slot, pci->function,
                pci->vendor_id, pci->device_id,
                pci_config_read16(pci->bus, pci->slot, pci->function, 0x04),
                pci_config_read32(pci->bus, pci->slot, pci->function, 0x10));
    }
    for (uint32_t i = 0; i < g_hc_count; ++i) {
        ehci_controller_t *hc = &g_hc[i];
        kprintf("[USB-DIAG] EHCI[%u] cmd=%x sts=%x configflag=%x ports=%u faulted=%u\n",
                i, mmio_read(hc, EHCI_USB_CMD), mmio_read(hc, EHCI_USB_STS),
                mmio_read(hc, EHCI_CONFIGFLAG), hc->port_count, hc->faulted);
        for (uint32_t port = 0; port < hc->port_count &&
                                port < EHCI_DIAG_MAX_PORTS; ++port) {
            uint32_t st = hc->op[EHCI_PORTSC_BASE / 4U + port];
            kprintf("  port%u status=%x connect=%u enable=%u owner=%u\n",
                    port + 1U, st, (st & PORT_CONNECT) != 0,
                    (st & PORT_ENABLE) != 0, (st & PORT_OWNER) != 0);
        }
    }
    usb_uhci_diagnostics();
    kprintf("[USB-DIAG] registered block devices=%u\n", block_count());
}

void usb_storage_start_hotplug_task(void) {
    if (g_hc_count != 0)
        (void)task_create("usb-hotplug", usb_storage_hotplug_main, NULL);
    usb_uhci_start_hotplug_task();
    usb_ohci_start_hotplug_task();
}
