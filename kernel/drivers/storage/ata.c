#include "../../include/ata.h"
#include "../../include/block.h"
#include "../../include/memory.h"
#include "../../include/pic.h"
#include "../../include/vga.h"
#include "../../include/klock.h"
#include "../../include/pit.h"
#include "../../include/task.h"
#include "../../include/smp.h"
#include "../../include/pci.h"

#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_SECONDARY_IO    0x170
#define ATA_SECONDARY_CTRL  0x376

#define ATA_REG_DATA        0
#define ATA_REG_ERROR       1
#define ATA_REG_FEATURES    1
#define ATA_REG_SECCOUNT0   2
#define ATA_REG_LBA0        3
#define ATA_REG_LBA1        4
#define ATA_REG_LBA2        5
#define ATA_REG_HDDEVSEL    6
#define ATA_REG_STATUS      7
#define ATA_REG_COMMAND     7

#define ATA_STATUS_ERR      0x01
#define ATA_STATUS_DRQ      0x08
#define ATA_STATUS_DF       0x20
#define ATA_STATUS_DRDY     0x40
#define ATA_STATUS_BSY      0x80

#define ATA_CTRL_SRST       0x04

#define ATA_CMD_IDENTIFY          0xEC
#define ATA_CMD_READ_PIO          0x20
#define ATA_CMD_READ_DMA          0xC8
#define ATA_CMD_WRITE_DMA         0xCA
#define ATA_CMD_READ_MULTIPLE     0xC4
#define ATA_CMD_WRITE_MULTIPLE    0xC5
#define ATA_CMD_SET_MULTIPLE      0xC6
#define ATA_CMD_WRITE_PIO         0x30
#define ATA_CMD_CACHE_FLUSH       0xE7
#define ATA_CMD_PACKET            0xA0
#define ATA_CMD_IDENTIFY_PACKET   0xA1

#define ATAPI_SIGNATURE_LBA1      0x14
#define ATAPI_SIGNATURE_LBA2      0xEB
#define ATAPI_DEFAULT_BLOCK_SIZE  2048U

typedef enum {
    ATA_TRANSPORT_ATA = 0,
    ATA_TRANSPORT_ATAPI
} ata_transport_t;

typedef struct {
    uint16_t io;
    uint16_t ctrl;
    uint8_t slave;
    ata_transport_t transport;
    uint32_t sectors;
    uint16_t block_size;
    uint8_t multiple_sectors;
    bool pio32;
    bool dma_capable;
    bool dma;
    char name[8];
} ata_device_t;

static ata_device_t g_ata_devices[4];
static uint32_t g_ata_device_count = 0;
static uint32_t g_ata_disk_count = 0;
static uint32_t g_atapi_count = 0;
typedef struct {
    uint16_t base;
    uint32_t allocation,buffer;
    uint32_t prd[2];
} ata_dma_channel_t;
static ata_dma_channel_t g_ata_dma[2];

/* Master and slave share the command/status registers of one IDE channel.
 * block.c serializes normal block I/O, but media refreshes (for example the
 * ISO9660 driver probing cd0) can call the ATA layer directly.  On SMP that
 * used to interleave an ATAPI PACKET command with an ata0/ata1 PIO transfer
 * and QEMU correctly aborted the write with ATA error 0x04.  Serialize at the
 * controller boundary as the final authority. */
static kmutex_t g_ata_primary_lock = KMUTEX_INITIALIZER;
static kmutex_t g_ata_secondary_lock = KMUTEX_INITIALIZER;

static kmutex_t *ata_channel_lock_for(const ata_device_t *dev) {
    if (!dev) return &g_ata_primary_lock;
    return dev->io == ATA_SECONDARY_IO
        ? &g_ata_secondary_lock : &g_ata_primary_lock;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void insw(uint16_t port, void *buffer, uint32_t word_count) {
    __asm__ volatile ("cld; rep insw"
                      : "+D"(buffer), "+c"(word_count)
                      : "d"(port)
                      : "memory");
}

static inline void outsw(uint16_t port, const void *buffer, uint32_t word_count) {
    __asm__ volatile ("cld; rep outsw"
                      : "+S"(buffer), "+c"(word_count)
                      : "d"(port)
                      : "memory");
}

static inline void insl(uint16_t port, void *buffer, uint32_t count) {
    __asm__ volatile ("cld; rep insl"
                      : "+D"(buffer), "+c"(count) : "d"(port) : "memory");
}

static void ata_delay_400ns(uint16_t ctrl) {
    (void)inb(ctrl);
    (void)inb(ctrl);
    (void)inb(ctrl);
    (void)inb(ctrl);
}

static bool ata_wait_not_busy(uint16_t io) {
    uint32_t start=pit_get_ticks(),hz=pit_get_frequency_hz();
    /* CACHE FLUSH can take much longer than 100000 port reads. Retain a
     * finite early-boot fallback when timer interrupts are unavailable. */
    for (uint32_t i = 0; i < 10000000U; i++) {
        if(hz && pit_get_ticks()-start>=5U*hz) return false;
        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status == 0xFF) return false;
        if ((status & ATA_STATUS_BSY) == 0) return true;
        /* Repeated port exits can starve QEMU's asynchronous completion
         * thread. Give runnable peers time while the serialized channel
         * stays owned. Early boot and IRQ-off callers retain bounded polling. */
        if((i&63U)==63U && smp_scheduler_started()) {
            uint32_t flags;__asm__ volatile("pushfl; popl %0":"=r"(flags));
            if(flags&0x200U)task_yield();
        }
    }
    return false;
}

static bool ata_wait_drq(uint16_t io) {
    uint32_t start = pit_get_ticks(), hz = pit_get_frequency_hz();
    /* A fixed short poll count can expire while QEMU's asynchronous read is
     * still legitimately BSY, resetting the channel for a healthy request.
     * Bound elapsed time like ata_wait_not_busy(), with an early-boot cap.
     * ERR/DF are only meaningful once BSY has cleared. */
    for (uint32_t i = 0; i < 10000000U; i++) {
        if (hz && pit_get_ticks() - start >= 5U * hz) return false;
        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status == 0xFF) return false;
        if (status & ATA_STATUS_BSY) {
            if((i&63U)==63U && smp_scheduler_started()) {
                uint32_t flags;__asm__ volatile("pushfl; popl %0":"=r"(flags));
                if(flags&0x200U)task_yield();
            }
            continue;
        }
        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) return false;
        if (status & ATA_STATUS_DRQ) return true;
    }
    return false;
}

static uint8_t ata_status(uint16_t io) {
    return inb(io + ATA_REG_STATUS);
}

static void ata_log_failure(const ata_device_t *dev, const char *stage,
                            uint32_t lba, uint8_t count, uint32_t attempt) {
    uint8_t status;
    uint8_t error;

    if (!dev) return;
    status = ata_status(dev->io);
    error = inb(dev->io + ATA_REG_ERROR);
    kprintf("[ATA:WRITE] %s fallo dev=%s lba=%u count=%u intento=%u "
            "status=%x error=%x BSY=%u DRDY=%u DRQ=%u DF=%u ERR=%u\n",
            stage ? stage : "?", dev->name, lba, count, attempt,
            status, error,
            (status & ATA_STATUS_BSY) ? 1U : 0U,
            (status & ATA_STATUS_DRDY) ? 1U : 0U,
            (status & ATA_STATUS_DRQ) ? 1U : 0U,
            (status & ATA_STATUS_DF) ? 1U : 0U,
            (status & ATA_STATUS_ERR) ? 1U : 0U);
}

static bool ata_wait_command_complete(const ata_device_t *dev) {
    uint8_t status;

    if (!dev || !ata_wait_not_busy(dev->io)) return false;
    status = ata_status(dev->io);
    if (status == 0xFFU || status == 0U) return false;
    return (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) == 0U;
}

static void ata_soft_reset_ports(uint16_t io, uint16_t ctrl) {
    outb(ctrl, ATA_CTRL_SRST);
    /* SRST must remain asserted for at least 5 us. A single 400 ns
     * selection delay was too short and made cold ATAPI detection race
     * the controller reset. These port delays also work before IRQs. */
    for (unsigned i = 0; i < 16U; ++i) ata_delay_400ns(ctrl);
    outb(ctrl, 0U);
    /* Registers/status are not valid during the first 2 ms after reset.
     * In particular, transient zero is not evidence of an empty channel. */
    for (unsigned i = 0; i < 5000U; ++i) ata_delay_400ns(ctrl);

    /* Un reset ATA puede tardar varios milisegundos. Hacerlo tambien antes
     * del primer probe deja el canal en un estado determinista si el firmware
     * o un dispositivo PCI agregado por el hipervisor altero el timing de
     * arranque. */
    for (uint32_t i = 0; i < 1000000U; i++) {
        uint8_t status = ata_status(io);
        if (status == 0xFFU || (status & ATA_STATUS_BSY) == 0U) break;
    }
}

static void ata_soft_reset_channel(const ata_device_t *dev) {
    if (!dev) return;
    ata_soft_reset_ports(dev->io, dev->ctrl);
    /* Reset clears SET MULTIPLE for both drives on the channel. Fall back
     * to READ PIO for retries and subsequent requests on either drive. */
    for (uint32_t i = 0; i < g_ata_device_count; ++i)
        if (g_ata_devices[i].io == dev->io) {
            g_ata_devices[i].multiple_sectors = 0U;
            g_ata_devices[i].dma=false;
        }
}

/* ERR/DF are undefined while BSY is set: a reset can expose stale error
 * bits until the PACKET command is accepted. Use elapsed-time deadlines and
 * yield to QEMU's completion thread once task scheduling is available. */
static bool ata_wait_packet_state(uint16_t io, bool data_phase) {
    uint32_t start = pit_get_ticks(), hz = pit_get_frequency_hz();
    for (uint32_t i = 0; i < 10000000U; ++i) {
        if (hz && pit_get_ticks() - start >= 5U * hz) return false;
        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status == 0xFFU) return false;
        if (!(status & ATA_STATUS_BSY)) {
            if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) return false;
            if (((status & ATA_STATUS_DRQ) != 0U) == data_phase) return true;
        }
        if ((i & 63U) == 63U && smp_scheduler_started()) {
            uint32_t flags; __asm__ volatile("pushfl; popl %0":"=r"(flags));
            if (flags & 0x200U) task_yield();
        }
    }
    return false;
}

static bool ata_wait_packet_drq(uint16_t io) {
    return ata_wait_packet_state(io, true);
}

static bool ata_wait_packet_complete(uint16_t io) {
    return ata_wait_packet_state(io, false);
}

static bool ata_select_lba28(const ata_device_t *dev, uint32_t lba) {
    /* Al alternar master/slave en el mismo canal hay que esperar primero a
     * que termine el comando del dispositivo previamente seleccionado. */
    if (!ata_wait_not_busy(dev->io)) return false;
    outb(dev->io + ATA_REG_HDDEVSEL,
         (uint8_t)(0xE0 | (dev->slave << 4) | ((lba >> 24) & 0x0F)));
    ata_delay_400ns(dev->ctrl);
    return ata_wait_not_busy(dev->io);
}

static void ata_select_packet(const ata_device_t *dev) {
    outb(dev->io + ATA_REG_HDDEVSEL, (uint8_t)(0xA0 | (dev->slave << 4)));
    ata_delay_400ns(dev->ctrl);
}

static uint32_t read_be32(const uint8_t *buffer) {
    return ((uint32_t)buffer[0] << 24) |
           ((uint32_t)buffer[1] << 16) |
           ((uint32_t)buffer[2] << 8) |
           (uint32_t)buffer[3];
}

static bool ata_identify_packet(uint16_t io, uint16_t ctrl, uint8_t slave, uint16_t *identify) {
    outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xA0 | (slave << 4)));
    ata_delay_400ns(ctrl);

    outb(io + ATA_REG_SECCOUNT0, 0);
    outb(io + ATA_REG_LBA0, 0);
    outb(io + ATA_REG_LBA1, 0);
    outb(io + ATA_REG_LBA2, 0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
    ata_delay_400ns(ctrl);

    if (!ata_wait_not_busy(io)) return false;
    if (!ata_wait_drq(io)) return false;

    insw(io + ATA_REG_DATA, identify, 256U);
    ata_delay_400ns(ctrl);
    return true;
}

static bool ata_atapi_begin_packet(const ata_device_t *dev, uint16_t max_byte_count) {
    ata_select_packet(dev);
    if (!ata_wait_not_busy(dev->io)) return false;

    outb(dev->io + ATA_REG_FEATURES, 0);
    outb(dev->io + ATA_REG_SECCOUNT0, 0);
    outb(dev->io + ATA_REG_LBA0, 0);
    outb(dev->io + ATA_REG_LBA1, (uint8_t)(max_byte_count & 0xFF));
    outb(dev->io + ATA_REG_LBA2, (uint8_t)((max_byte_count >> 8) & 0xFF));
    outb(dev->io + ATA_REG_COMMAND, ATA_CMD_PACKET);
    ata_delay_400ns(dev->ctrl);

    return ata_wait_packet_drq(dev->io);
}

static bool ata_atapi_packet_read(ata_device_t *dev, const uint8_t packet[12],
                                  uint16_t expected_bytes, void *buffer) {
    uint16_t byte_count;

    if (!dev || !packet || !buffer || expected_bytes == 0) return false;
    if (!ata_atapi_begin_packet(dev, expected_bytes)) return false;

    outsw(dev->io + ATA_REG_DATA, packet, 6);
    if (!ata_wait_drq(dev->io)) return false;

    byte_count = (uint16_t)(((uint16_t)inb(dev->io + ATA_REG_LBA2) << 8) |
                            inb(dev->io + ATA_REG_LBA1));
    /* Capacity and sense responses must be complete before parsing them. */
    if (byte_count != expected_bytes) return false;

    insw(dev->io + ATA_REG_DATA, buffer, byte_count / 2U);
    ata_delay_400ns(dev->ctrl);
    return ata_wait_packet_complete(dev->io);
}

static bool ata_atapi_update_capacity(ata_device_t *dev, block_device_t *block) {
    uint8_t packet[12];
    uint8_t capacity[8];
    uint32_t last_lba;
    uint32_t block_size;
    bool ok = false;
    kmutex_t *channel;

    if (!dev || dev->transport != ATA_TRANSPORT_ATAPI) return false;
    channel = ata_channel_lock_for(dev);
    kmutex_lock(channel);

    for (uint32_t attempt = 0; attempt < 3U; ++attempt) {
        kmemset(packet, 0, sizeof(packet));
        packet[0] = 0x25; /* READ CAPACITY(10) */
        if (ata_atapi_packet_read(dev, packet, sizeof(capacity), capacity)) {
            last_lba = read_be32(capacity);
            block_size = read_be32(capacity + 4);
            if (last_lba == 0xffffffffU || !block_size || block_size > 0xFFFFU) break;
            dev->sectors = last_lba + 1U;
            dev->block_size = (uint16_t)block_size;
            ok = true;
            break;
        }

        /* A reset/media change can leave UNIT ATTENTION pending. Consume
         * REQUEST SENSE before retrying, rather than declaring the CD empty.
         * Reset only an unfinished transport, not a normal CHECK CONDITION. */
        uint8_t status = ata_status(dev->io);
        if (status & (ATA_STATUS_BSY | ATA_STATUS_DRQ)) ata_soft_reset_channel(dev);
        uint8_t sense[18];
        kmemset(packet, 0, sizeof(packet));
        packet[0] = 0x03; /* REQUEST SENSE */
        packet[4] = sizeof(sense);
        if (ata_atapi_packet_read(dev, packet, sizeof(sense), sense)) {
            if ((sense[2] & 0x0fU) == 2U && sense[12] == 0x3aU) break; /* No medium. */
        } else {
            ata_soft_reset_channel(dev);
        }
    }
    if (!ok) {
        dev->sectors = 0;
        dev->block_size = ATAPI_DEFAULT_BLOCK_SIZE;
    }
    if (block) {
        block->sector_count = dev->sectors;
        block->sector_size = dev->block_size;
    }
    kmutex_unlock(channel);
    return ok;
}

static void ata_log_read_failure(const ata_device_t *dev, const char *stage,
                                 uint32_t lba, uint8_t count,
                                 uint32_t attempt) {
    uint8_t status;
    uint8_t error;

    if (!dev) return;
    status = ata_status(dev->io);
    error = inb(dev->io + ATA_REG_ERROR);
    kprintf("[ATA:READ] %s fallo dev=%s lba=%u count=%u intento=%u "
            "status=%x error=%x BSY=%u DRDY=%u DRQ=%u DF=%u ERR=%u\n",
            stage ? stage : "?", dev->name, lba, count, attempt,
            status, error,
            (status & ATA_STATUS_BSY) ? 1U : 0U,
            (status & ATA_STATUS_DRDY) ? 1U : 0U,
            (status & ATA_STATUS_DRQ) ? 1U : 0U,
            (status & ATA_STATUS_DF) ? 1U : 0U,
            (status & ATA_STATUS_ERR) ? 1U : 0U);
}

/* Execute one complete READ PIO transaction while the IDE channel mutex is
 * held.  A failed PIO read can leave unread data/DRQ state in the controller,
 * so the caller must reset the channel before retrying. */
/* QEMU PIIX bus-master DMA uses one bounded, physically contiguous bounce
 * window per serialized channel. A PRD never crosses a 64 KiB boundary. */
static bool ata_dma_transfer(ata_device_t *dev,uint32_t lba,uint32_t count,
                             void *buffer,bool writing,bool durable) {
    ata_dma_channel_t *dma=&g_ata_dma[dev->io==ATA_SECONDARY_IO?1:0];
    uint8_t *data=buffer;
    if(!dev->dma || !dma->base || !dma->buffer || !buffer || !count ||
       lba>0x0fffffffU || count>0x10000000U-lba ||
       lba>=dev->sectors || count>dev->sectors-lba)return false;
    while(count) {
        uint32_t sectors=count>128U?128U:count,bytes=sectors*BLOCK_SECTOR_SIZE;
        if(!ata_select_lba28(dev,lba))return false;
        outb(dma->base,0U);
        outb(dma->base+2U,(inb(dma->base+2U)&0x60U)|6U);
        dma->prd[0]=dma->buffer;dma->prd[1]=0x80000000U|(bytes&0xffffU);
        if(writing)kmemcpy((void *)dma->buffer,data,bytes);
        __asm__ volatile("":::"memory");
        outl(dma->base+4U,(uint32_t)(uintptr_t)dma->prd);
        outb(dma->base,writing?0U:8U);
        outb(dev->io+ATA_REG_SECCOUNT0,(uint8_t)sectors);
        outb(dev->io+ATA_REG_LBA0,(uint8_t)lba);
        outb(dev->io+ATA_REG_LBA1,(uint8_t)(lba>>8));
        outb(dev->io+ATA_REG_LBA2,(uint8_t)(lba>>16));
        outb(dev->io+ATA_REG_COMMAND,writing?ATA_CMD_WRITE_DMA:ATA_CMD_READ_DMA);
        outb(dma->base,writing?1U:9U);
        uint32_t start=pit_get_ticks(),hz=pit_get_frequency_hz();
        bool complete=false;
        for(uint32_t spin=0;spin<10000000U;spin++) {
            uint8_t status=inb(dma->base+2U);
            if(status&2U)break;
            if(!(status&1U) && (status&4U)) {complete=true;break;}
            if(hz && pit_get_ticks()-start>=5U*hz)break;
            if((spin&63U)==63U && smp_scheduler_started()) {
                uint32_t flags;__asm__ volatile("pushfl; popl %0":"=r"(flags));
                if(flags&0x200U)task_yield();
            }
        }
        /* Stop synchronously before resetting or reusing the bounce window. */
        outb(dma->base,writing?0U:8U);
        uint8_t status=inb(dma->base+2U);
        outb(dma->base+2U,(status&0x60U)|6U);
        if(!complete || (status&2U) || !ata_wait_command_complete(dev))return false;
        __asm__ volatile("":::"memory");
        if(!writing)kmemcpy(data,(void *)dma->buffer,bytes);
        lba+=sectors;count-=sectors;data+=bytes;
    }
    if(writing && durable) {
        outb(dev->io+ATA_REG_COMMAND,ATA_CMD_CACHE_FLUSH);
        ata_delay_400ns(dev->ctrl);
        if(!ata_wait_command_complete(dev))return false;
    }
    return true;
}

static void ata_try_dma(void) {
    /* Reserve only real available frames. A RAM-size threshold prevented
     * low-memory guests from using DMA precisely when swap needs it most.
     * Keep at least 512 KiB in the user frame pool after the bounce window. */
    if(mm_vm_frame_free_pages()<32U+128U)return;
    bool eligible=false;
    for(uint32_t i=0;i<g_ata_device_count;i++)
        if(g_ata_devices[i].transport==ATA_TRANSPORT_ATA &&
           g_ata_devices[i].pio32 && g_ata_devices[i].dma_capable)eligible=true;
    if(!eligible)return;
    const pci_device_t *controller=NULL;
    for(uint32_t i=0;i<pci_device_count();i++) {
        const pci_device_t *p=pci_device_at(i);
        if(p && p->vendor_id==0x8086U && (p->device_id==0x7010U || p->device_id==0x7111U) &&
           p->class_code==1U && p->subclass==1U && !(p->prog_if&5U) &&
           (p->bars[4]&1U) && (p->bars[4]&0xfff0U)) {controller=p;break;}
    }
    if(!controller || !pci_enable_command(controller,PCI_COMMAND_IO|PCI_COMMAND_BUSMASTER))return;
    for(uint32_t i=0;i<g_ata_device_count;i++) {
        ata_device_t *dev=&g_ata_devices[i];
        if(dev->transport!=ATA_TRANSPORT_ATA || !dev->pio32 || !dev->dma_capable)continue;
        ata_dma_channel_t *dma=&g_ata_dma[dev->io==ATA_SECONDARY_IO?1:0];
        if(!dma->buffer) {
            if(mm_vm_frame_free_pages()<32U+128U)continue;
            if(!mm_vm_frame_alloc(32U,&dma->allocation))continue;
            dma->buffer=(dma->allocation+65535U)&~65535U;
            dma->base=(uint16_t)(controller->bars[4]&0xfff0U)+(dev->io==ATA_SECONDARY_IO?8U:0U);
        }
        if(!ata_select_lba28(dev,0U))continue;
        outb(dev->io+ATA_REG_FEATURES,3U);outb(dev->io+ATA_REG_SECCOUNT0,0x22U);
        outb(dev->io+ATA_REG_COMMAND,0xefU);ata_delay_400ns(dev->ctrl);
        dev->dma=ata_wait_command_complete(dev);
        if(dev->dma)kprintf("  [ATA] %s: QEMU PIIX DMA read/write enabled\n",dev->name);
    }
}

static bool ata_read_block_once(block_device_t *block, uint32_t lba,
                                uint8_t count, void *buffer,
                                uint32_t attempt) {
    ata_device_t *dev = (ata_device_t *)block->driver_data;
    uint16_t *out = (uint16_t *)buffer;

    if (!dev || !buffer || count == 0U || lba > 0x0FFFFFFFU ||
        lba + (uint32_t)count > 0x10000000U)
        return false;

    if (!ata_select_lba28(dev, lba)) {
        ata_log_read_failure(dev, "select", lba, count, attempt);
        return false;
    }

    outb(dev->io + ATA_REG_SECCOUNT0, count);
    outb(dev->io + ATA_REG_LBA0, (uint8_t)(lba & 0xFFU));
    outb(dev->io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFFU));
    outb(dev->io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFFU));
    outb(dev->io + ATA_REG_COMMAND,
         dev->multiple_sectors ? ATA_CMD_READ_MULTIPLE : ATA_CMD_READ_PIO);
    ata_delay_400ns(dev->ctrl);

    for (uint32_t sector = 0U; sector < (uint32_t)count;) {
        if (!ata_wait_drq(dev->io)) {
            ata_log_read_failure(dev, "wait DRQ", lba + sector,
                                 count, attempt);
            return false;
        }
        uint32_t burst = dev->multiple_sectors ? dev->multiple_sectors : 1U;
        if (burst > (uint32_t)count - sector) burst = (uint32_t)count - sector;
        if(dev->pio32)insl(dev->io + ATA_REG_DATA,out,burst * BLOCK_SECTOR_SIZE / 4U);
        else insw(dev->io + ATA_REG_DATA, out, burst * BLOCK_SECTOR_SIZE / 2U);
        out += burst * BLOCK_SECTOR_SIZE / 2U;
        sector += burst;
        ata_delay_400ns(dev->ctrl);
    }

    /* Do not hand the shared channel back while the device is still finishing
     * the command.  The old path returned immediately after the last insw(),
     * so a following metadata transaction could inherit a transient BUSY/ERR
     * state and fail its first and only FAT read. */
    if (!ata_wait_command_complete(dev)) {
        ata_log_read_failure(dev, "command complete", lba, count, attempt);
        return false;
    }
    return true;
}

static bool ata_read_block(block_device_t *block, uint32_t lba, uint8_t count,
                           void *buffer) {
    ata_device_t *dev;
    kmutex_t *channel;
    bool ok = false;

    if (!block || !buffer || count == 0U) return false;
    dev = (ata_device_t *)block->driver_data;
    if (!dev || lba > 0x0FFFFFFFU ||
        lba + (uint32_t)count > 0x10000000U)
        return false;

    channel = ata_channel_lock_for(dev);
    kmutex_lock(channel);

    if(dev->dma) {
        if(ata_dma_transfer(dev,lba,count,buffer,false,true)) {kmutex_unlock(channel);return true;}
        kprintf("[ATA:DMA] read failed; resetting %s and falling back to PIO\n",dev->name);
        dev->dma=false;ata_soft_reset_channel(dev);
    }

    for (uint32_t attempt = 1U; attempt <= 3U; attempt++) {
        if (ata_read_block_once(block, lba, count, buffer, attempt)) {
            ok = true;
            break;
        }

        /* Unlike a failed select before any command was issued, a failed DRQ
         * wait may leave a partially consumed READ PIO transaction active.
         * Resetting is cheap on this error path and gives the next attempt a
         * defined controller state. */
        kprintf("[ATA:READ] reseteando canal de %s antes del reintento\n",
                dev->name);
        ata_soft_reset_channel(dev);
    }

    kmutex_unlock(channel);
    return ok;
}

/* BLESKERNOS_ATAPI_VMWARE_READ_V2
 * ATAPI PIO is a byte stream split into one or more data phases.  A phase is
 * not guaranteed to map 1:1 to a logical CD sector.  VMware is particularly
 * good at exposing the old assumption because a failed multi-block command
 * can leave the PACKET state machine active.  Consume exactly the byte count
 * published by LBA1/LBA2, accumulate phases, and reset/retry on protocol or
 * timeout failures so the next request starts from a clean channel. */
static bool ata_atapi_wait_phase(const ata_device_t *dev, bool *complete) {
    if (!dev || !complete || !ata_wait_not_busy(dev->io)) return false;
    uint8_t status = ata_status(dev->io);
    if (status == 0xFFU || (status & (ATA_STATUS_ERR | ATA_STATUS_DF | ATA_STATUS_BSY))) return false;
    *complete = (status & ATA_STATUS_DRQ) == 0U;
    return true;
}

static void ata_atapi_read_bytes(uint16_t io, uint8_t *dst, uint16_t bytes) {
    uint16_t words = (uint16_t)(bytes / 2U);
    if (words) insw(io + ATA_REG_DATA, dst, words);
    if (bytes & 1U) {
        uint16_t tail = inw(io + ATA_REG_DATA);
        dst[bytes - 1U] = (uint8_t)(tail & 0xFFU);
    }
}

static bool ata_atapi_read_block(block_device_t *block, uint32_t lba, uint8_t count, void *buffer) {
    ata_device_t *dev = (ata_device_t *)block->driver_data;
    uint8_t packet[12];
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t expected;
    kmutex_t *channel;
    bool ok = false;

    if (!dev || !buffer || count == 0U) return false;
    if (dev->block_size == 0U) dev->block_size = ATAPI_DEFAULT_BLOCK_SIZE;
    expected = (uint32_t)dev->block_size * (uint32_t)count;
    channel = ata_channel_lock_for(dev);
    kmutex_lock(channel);

    for (uint32_t attempt = 0U; attempt < 3U && !ok; attempt++) {
        uint32_t transferred = 0U;
        bool protocol_ok = true;

        kmemset(packet, 0, sizeof(packet));
        packet[0] = 0xA8; /* READ(12) */
        packet[2] = (uint8_t)((lba >> 24) & 0xFFU);
        packet[3] = (uint8_t)((lba >> 16) & 0xFFU);
        packet[4] = (uint8_t)((lba >> 8) & 0xFFU);
        packet[5] = (uint8_t)(lba & 0xFFU);
        packet[6] = 0U;
        packet[7] = 0U;
        packet[8] = 0U;
        packet[9] = count;

        /* Permit multi-sector PIO phases instead of negotiating one 2 KiB
         * phase per CD sector. Keep the even byte count well below the
         * 16-bit ATAPI ceiling; devices may still return smaller phases. */
        uint16_t phase_limit = (uint16_t)(expected > 32768U ? 32768U : expected);
        if (!ata_atapi_begin_packet(dev, phase_limit)) {
            protocol_ok = false;
        } else {
            outsw(dev->io + ATA_REG_DATA, packet, 6U);
            ata_delay_400ns(dev->ctrl);
        }

        while (protocol_ok) {
            bool complete = false;
            uint16_t byte_count;

            if (!ata_atapi_wait_phase(dev, &complete)) {
                protocol_ok = false;
                break;
            }
            if (complete) {
                ok = transferred == expected;
                break;
            }

            byte_count = (uint16_t)(((uint16_t)inb(dev->io + ATA_REG_LBA2) << 8) |
                                    inb(dev->io + ATA_REG_LBA1));
            if (byte_count == 0U ||
                (uint32_t)byte_count > expected - transferred) {
                protocol_ok = false;
                break;
            }

            ata_atapi_read_bytes(dev->io, dst + transferred, byte_count);
            transferred += byte_count;
            ata_delay_400ns(dev->ctrl);
        }

        if (!ok) {
            uint8_t status = ata_status(dev->io);
            uint8_t error = inb(dev->io + ATA_REG_ERROR);
            if (attempt == 2U) {
                kprintf("[ATAPI] READ(12) fallo dev=%s lba=%u count=%u got=%u/%u status=%x error=%x\n",
                        dev->name, lba, count, transferred, expected,
                        status, error);
            }
            /* Abort any half-consumed PACKET command before retrying or
             * returning to ISO9660.  block_read() serializes the controller,
             * so resetting the shared IDE channel here cannot race another
             * disk transaction. */
            ata_soft_reset_channel(dev);
            ata_select_packet(dev);
            (void)ata_wait_not_busy(dev->io);
        }
    }

    kmutex_unlock(channel);
    return ok;
}

static bool ata_write_block_once(block_device_t *block, uint32_t lba,
                                 uint8_t count, const void *buffer,
                                 uint32_t attempt, bool durable) {
    ata_device_t *dev = (ata_device_t *)block->driver_data;
    const uint16_t *in = (const uint16_t *)buffer;

    if (!dev || !buffer || count == 0U || lba > 0x0FFFFFFFU ||
        lba + (uint32_t)count > 0x10000000U)
        return false;

    if (!ata_select_lba28(dev, lba)) {
        ata_log_failure(dev, "select", lba, count, attempt);
        return false;
    }

    outb(dev->io + ATA_REG_SECCOUNT0, count);
    outb(dev->io + ATA_REG_LBA0, (uint8_t)lba);
    outb(dev->io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(dev->io + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    outb(dev->io + ATA_REG_COMMAND,
         dev->multiple_sectors ? ATA_CMD_WRITE_MULTIPLE : ATA_CMD_WRITE_PIO);
    ata_delay_400ns(dev->ctrl);

    for (uint32_t sector = 0; sector < (uint32_t)count;) {
        if (!ata_wait_drq(dev->io)) {
            ata_log_failure(dev, "wait DRQ", lba + sector,
                            count, attempt);
            return false;
        }

        uint32_t burst = dev->multiple_sectors ? dev->multiple_sectors : 1U;
        if (burst > (uint32_t)count - sector) burst = (uint32_t)count - sector;
        outsw(dev->io + ATA_REG_DATA, in, burst * BLOCK_SECTOR_SIZE / 2U);
        in += burst * BLOCK_SECTOR_SIZE / 2U;
        sector += burst;
        ata_delay_400ns(dev->ctrl);

        /*
         * Después de cada bloque el dispositivo puede mantener BSY mientras
         * vuelca su buffer. Esperar aquí evita enviar el siguiente bloque o
         * CACHE FLUSH mientras todavía procesa el anterior.
         */
        if (!ata_wait_command_complete(dev)) {
            ata_log_failure(dev, "complete sector", lba + sector,
                            count, attempt);
            return false;
        }
    }

    if (!ata_wait_not_busy(dev->io)) {
        ata_log_failure(dev, "before flush", lba, count, attempt);
        return false;
    }

    if(durable) {
        outb(dev->io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
        ata_delay_400ns(dev->ctrl);
        if (!ata_wait_command_complete(dev)) {
            ata_log_failure(dev, "cache flush", lba, count, attempt);
            return false;
        }
    }

    return true;
}

static bool ata_write_block_mode(block_device_t *block, uint32_t lba, uint8_t count,
                            const void *buffer, bool durable) {
    ata_device_t *dev;
    kmutex_t *channel;
    bool ok = false;

    if (!block || !buffer || count == 0U) return false;
    dev = (ata_device_t *)block->driver_data;
    if (!dev) return false;
    channel = ata_channel_lock_for(dev);
    kmutex_lock(channel);

    if(dev->dma) {
        if(ata_dma_transfer(dev,lba,count,(void *)buffer,true,durable)) {kmutex_unlock(channel);return true;}
        kprintf("[ATA:DMA] write failed; resetting %s and falling back to PIO\n",dev->name);
        dev->dma=false;ata_soft_reset_channel(dev);
    }

    for (uint32_t attempt = 1U; attempt <= 3U; attempt++) {
        if (ata_write_block_once(block, lba, count, buffer, attempt, durable)) {
            ok = true;
            break;
        }

        if (dev->multiple_sectors || attempt >= 2U) {
            kprintf("[ATA:WRITE] reseteando canal de %s antes del reintento\n",
                    dev->name);
            ata_soft_reset_channel(dev);
        } else {
            ata_delay_400ns(dev->ctrl);
        }
    }

    kmutex_unlock(channel);
    return ok;
}

static bool ata_write_block(block_device_t *block,uint32_t lba,uint8_t count,const void *buffer) {
    return ata_write_block_mode(block,lba,count,buffer,true);
}
static bool ata_write_swap(block_device_t *block,uint32_t lba,uint8_t count,const void *buffer) {
    return ata_write_block_mode(block,lba,count,buffer,false);
}

static void ata_register_ata(uint16_t io, uint16_t ctrl, uint8_t slave, const uint16_t *identify) {
    ata_device_t *dev;

    if (g_ata_device_count >= 4) return;

    dev = &g_ata_devices[g_ata_device_count++];
    kmemset(dev, 0, sizeof(*dev));
    dev->io = io;
    dev->ctrl = ctrl;
    dev->slave = slave;
    dev->transport = ATA_TRANSPORT_ATA;
    dev->sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    dev->block_size = BLOCK_SECTOR_SIZE;
    dev->dma_capable=(identify[49]&0x100U) && (identify[63]&4U);
    /* QEMU explicitly implements DWORD data-port reads and advertises them
     * in IDENTIFY word 48. Restrict this acceleration to its disk model;
     * word 48 alone is obsolete on modern physical ATA drives. IDENTIFY,
     * writes, ATAPI and other disks retain the 16-bit transport. */
    static const char qemu_model[]="QEMU HARDDISK";
    dev->pio32=(identify[48]&1U)!=0U;
    for(uint32_t i=0;i<sizeof(qemu_model)-1U;i++) {
        uint16_t word=identify[27U+i/2U];
        char c=(char)((i&1U)?word&0xffU:word>>8);
        if(c!=qemu_model[i])dev->pio32=false;
    }

    if (dev->sectors == 0) {
        kprintf("[ATA:PROBE] HDD io=%x slave=%u IDENTIFY sin capacidad LBA28\n",
                io, slave);
        g_ata_device_count--;
        return;
    }

    dev->name[0] = 'a';
    dev->name[1] = 't';
    dev->name[2] = 'a';
    dev->name[3] = (char)('0' + g_ata_disk_count++);
    dev->name[4] = '\0';

    /* READ MULTIPLE reduces controller exits and DRQ polling for sequential
     * DLL reads. Only enable a power-of-two burst advertised by IDENTIFY;
     * disks without this capability keep the single-sector PIO path. */
    uint32_t multiple = identify[47] & 0xffU;
    if ((identify[47] & 0x8000U) && multiple >= 2U) {
        uint8_t burst = 2U;
        while ((uint32_t)burst * 2U <= multiple && burst < 16U) burst *= 2U;
        if (ata_select_lba28(dev, 0U)) {
            outb(dev->io + ATA_REG_SECCOUNT0, burst);
            outb(dev->io + ATA_REG_COMMAND, ATA_CMD_SET_MULTIPLE);
            ata_delay_400ns(dev->ctrl);
            if (ata_wait_command_complete(dev)) dev->multiple_sectors = burst;
        }
    }

    if (!block_register_ex(dev->name, BLOCK_DEVICE_ATA, dev->sectors,
                           dev->block_size, false, dev, ata_read_block)) {
        g_ata_device_count--;
        return;
    }
    block_set_writer(dev->name, ata_write_block);
    block_set_volatile_writer(dev->name, ata_write_swap);
    kprintf("  [ATA] %s: %u sectores de %u bytes\n",
            dev->name, dev->sectors, dev->block_size);
    kprintf("  [ATA] %s: read burst=%u sectores\n", dev->name,
            dev->multiple_sectors ? dev->multiple_sectors : 1U);
    kprintf("  [ATA] %s: PIO read width=%u bits\n",dev->name,dev->pio32?32U:16U);
}

static void ata_register_atapi(uint16_t io, uint16_t ctrl, uint8_t slave,
                               const uint16_t *identify UNUSED) {
    ata_device_t *dev;
    block_device_t *block;

    if (g_ata_device_count >= 4) return;

    dev = &g_ata_devices[g_ata_device_count++];
    kmemset(dev, 0, sizeof(*dev));
    dev->io = io;
    dev->ctrl = ctrl;
    dev->slave = slave;
    dev->transport = ATA_TRANSPORT_ATAPI;
    dev->block_size = ATAPI_DEFAULT_BLOCK_SIZE;
    dev->name[0] = 'c';
    dev->name[1] = 'd';
    dev->name[2] = (char)('0' + g_atapi_count++);
    dev->name[3] = '\0';

    ata_atapi_update_capacity(dev, NULL);
    if (!block_register_ex(dev->name, BLOCK_DEVICE_ATAPI, dev->sectors,
                           dev->block_size, true, dev, ata_atapi_read_block)) {
        g_ata_device_count--;
        return;
    }

    block = block_get(dev->name);
    ata_atapi_update_capacity(dev, block);
    if (dev->sectors) {
        kprintf("  [ATAPI] %s: %u bloques de %u bytes\n",
                dev->name, dev->sectors, dev->block_size);
    } else {
        kprintf("  [ATAPI] %s: unidad detectada sin medio listo\n", dev->name);
    }
}

static void ata_probe_device(uint16_t io, uint16_t ctrl, uint8_t slave) {
    uint16_t identify[256];
    uint8_t status;
    uint8_t lba1;
    uint8_t lba2;

    outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xA0 | (slave << 4)));
    ata_delay_400ns(ctrl);

    outb(io + ATA_REG_SECCOUNT0, 0);
    outb(io + ATA_REG_LBA0, 0);
    outb(io + ATA_REG_LBA1, 0);
    outb(io + ATA_REG_LBA2, 0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns(ctrl);

    status = inb(io + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) {
        kprintf("[ATA:PROBE] io=%x slave=%u ausente (status=%x)\n",
                io, slave, status);
        return;
    }
    if (!ata_wait_not_busy(io)) {
        kprintf("[ATA:PROBE] io=%x slave=%u timeout BSY (status=%x)\n",
                io, slave, ata_status(io));
        return;
    }

    lba1 = inb(io + ATA_REG_LBA1);
    lba2 = inb(io + ATA_REG_LBA2);

    if (lba1 == 0 && lba2 == 0) {
        if (!ata_wait_drq(io)) {
            kprintf("[ATA:PROBE] HDD io=%x slave=%u IDENTIFY sin DRQ (status=%x error=%x)\n",
                    io, slave, ata_status(io), inb(io + ATA_REG_ERROR));
            return;
        }
        insw(io + ATA_REG_DATA, identify, 256U);
        ata_delay_400ns(ctrl);
        ata_register_ata(io, ctrl, slave, identify);
        return;
    }

    if (lba1 == ATAPI_SIGNATURE_LBA1 && lba2 == ATAPI_SIGNATURE_LBA2) {
        if (!ata_identify_packet(io, ctrl, slave, identify)) {
            kprintf("[ATA:PROBE] ATAPI io=%x slave=%u IDENTIFY PACKET fallo\n",
                    io, slave);
            return;
        }
        ata_register_atapi(io, ctrl, slave, identify);
        return;
    }
    kprintf("[ATA:PROBE] io=%x slave=%u firma desconocida %x/%x status=%x\n",
            io, slave, lba1, lba2, ata_status(io));
}

static void ata_probe(uint16_t io, uint16_t ctrl) {
    /* The currently selected position may be empty even when its peer is
     * populated (for example slave-only CD-ROM). Probe each selection. */
    ata_probe_device(io, ctrl, 0);
    ata_probe_device(io, ctrl, 1);
}

void ata_init(void) {
    kmutex_init(&g_ata_primary_lock);
    kmutex_init(&g_ata_secondary_lock);
    kmemset(g_ata_devices, 0, sizeof(g_ata_devices));
    g_ata_device_count = 0;
    g_ata_disk_count = 0;
    g_atapi_count = 0;

    /* QEMU puede cambiar el layout/timing PCI al agregar dispositivos como
     * AC97. El IDE legacy sigue en 1F0/170, pero no debemos heredar ningun
     * estado BSY/DRQ del firmware antes de IDENTIFY. */
    ata_soft_reset_ports(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL);
    ata_soft_reset_ports(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL);

    ata_probe(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL);
    ata_probe(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL);

    /* IDENTIFY/PACKET deja seleccionado el ultimo master/slave sondeado. Un
     * reset final garantiza que la primera lectura de BFS4/FAT no herede una
     * fase ATAPI o DRQ residual. Cada acceso vuelve a seleccionar su unidad. */
    ata_soft_reset_ports(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL);
    ata_soft_reset_ports(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL);
    kprintf("  [ATA] canales IDE normalizados despues del probe\n");
    ata_try_dma();
}

bool ata_refresh_media(const char *name) {
    block_device_t *block = block_get(name);
    ata_device_t *dev;

    if (!block || block->type != BLOCK_DEVICE_ATAPI) return false;
    dev = (ata_device_t *)block->driver_data;
    if (!dev) return false;

    return ata_atapi_update_capacity(dev, block);
}
