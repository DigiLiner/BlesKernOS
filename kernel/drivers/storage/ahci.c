/* SPDX-License-Identifier: MIT
 * AHCI SATA disk transport for BlesKernOS.
 * Original implementation from the AHCI 1.3.1 programming specification;
 * FreeBSD's driver was consulted for architecture only, no code copied.
 * One polled command slot per port; no ATAPI, NCQ or hotplug yet.
 */
#include "../../include/ahci.h"
#include "../../include/w3_profile.h"
#include "../../include/block.h"
#include "../../include/memory.h"
#include "../../include/paging.h"
#include "../../include/pci.h"
#include "../../include/pit.h"
#include "../../include/vga.h"

#define AHCI_MAX_DISKS 4U
#define AHCI_DMA_SECTORS 128U
#define AHCI_DMA_PAGES 16U
#define AHCI_HBA_GHC 0x04U
#define AHCI_HBA_PI 0x0cU
#define AHCI_PORT_BASE 0x100U
#define AHCI_PORT_SIZE 0x80U
#define AHCI_P_CLB 0x00U
#define AHCI_P_CLBU 0x04U
#define AHCI_P_FB 0x08U
#define AHCI_P_FBU 0x0cU
#define AHCI_P_IS 0x10U
#define AHCI_P_IE 0x14U
#define AHCI_P_CMD 0x18U
#define AHCI_P_TFD 0x20U
#define AHCI_P_SIG 0x24U
#define AHCI_P_SSTS 0x28U
#define AHCI_P_SERR 0x30U
#define AHCI_P_CI 0x38U
#define AHCI_CMD_ST 0x0001U
#define AHCI_CMD_FRE 0x0010U
#define AHCI_CMD_FR 0x4000U
#define AHCI_CMD_CR 0x8000U
#define AHCI_IS_TFES 0x40000000U
#define AHCI_GHC_AE 0x80000000U
#define AHCI_SIG_ATA 0x00000101U
#define ATA_IDENTIFY 0xecU
#define ATA_READ_DMA_EXT 0x25U
#define ATA_WRITE_DMA_EXT 0x35U
#define ATA_FLUSH_CACHE_EXT 0xeaU

typedef struct {
    volatile uint8_t *regs;
    uint32_t meta, bounce, sectors;
    uint8_t port_number;
    bool failed;
    char name[8];
} ahci_disk_t;

static ahci_disk_t g_disks[AHCI_MAX_DISKS];
static uint32_t g_disk_count;

static uint32_t mmio32(const volatile uint8_t *base, uint32_t offset) {
    return *(volatile const uint32_t *)(const void *)(base + offset);
}
static void mmio_write32(volatile uint8_t *base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(void *)(base + offset) = value;
}
static void pause_cpu(void) { __asm__ volatile ("pause" : : : "memory"); }

static bool port_wait_clear(volatile uint8_t *port, uint32_t reg, uint32_t mask) {
    uint32_t started = pit_get_ticks(), hz = pit_get_frequency_hz();
    for (uint32_t spin = 0; spin < 30000000U; spin++) {
        if (!(mmio32(port, reg) & mask)) return true;
        if (hz && pit_get_ticks() - started > 5U * hz) break;
        pause_cpu();
    }
    return false;
}

static bool ahci_command(ahci_disk_t *disk, uint8_t opcode, uint32_t lba,
                         uint8_t count, bool write, void *buffer) {
    W3P_SCOPE(opcode == ATA_FLUSH_CACHE_EXT ? W3P_AHCI_FLUSH : W3P_AHCI_CMD,
              (uint32_t)count * BLOCK_SECTOR_SIZE);
    volatile uint8_t *port = disk->regs;
    uint32_t bytes = (uint32_t)count * BLOCK_SECTOR_SIZE;
    if (disk->failed || count > AHCI_DMA_SECTORS || (count && !buffer)) return false;
    if ((mmio32(port, AHCI_P_CI) & 1U) ||
        !port_wait_clear(port, AHCI_P_TFD, 0x88U)) return false;
    uint8_t *meta = (uint8_t *)(uintptr_t)disk->meta;
    uint32_t *header = (uint32_t *)(void *)meta;
    uint8_t *cfis = meta + 0x500U;
    kmemset(meta, 0, 0x600U);
    header[0] = 5U | (write ? 0x40U : 0U) | (count ? (1U << 16U) : 0U);
    header[2] = disk->meta + 0x500U;
    cfis[0] = 0x27U; /* Register Host-to-Device FIS */
    cfis[1] = 0x80U; /* command */
    cfis[2] = opcode;
    cfis[4] = (uint8_t)lba;
    cfis[5] = (uint8_t)(lba >> 8U);
    cfis[6] = (uint8_t)(lba >> 16U);
    cfis[7] = 0x40U; /* LBA mode */
    cfis[8] = (uint8_t)(lba >> 24U);
    cfis[12] = count;
    if (count) {
        uint32_t *prd = (uint32_t *)(void *)(meta + 0x580U);
        if (write) kmemcpy((void *)(uintptr_t)disk->bounce, buffer, bytes);
        prd[0] = disk->bounce;
        prd[3] = bytes - 1U;
    }
    mmio_write32(port, AHCI_P_SERR, 0xffffffffU);
    mmio_write32(port, AHCI_P_IS, 0xffffffffU);
    __sync_synchronize();
    mmio_write32(port, AHCI_P_CI, 1U);
    uint32_t started = pit_get_ticks(), hz = pit_get_frequency_hz();
    for (uint32_t spin = 0; spin < 30000000U; spin++) {
        uint32_t status = mmio32(port, AHCI_P_IS);
        if (status & AHCI_IS_TFES) break;
        if (!(mmio32(port, AHCI_P_CI) & 1U)) {
            if (mmio32(port, AHCI_P_TFD) & 1U) break;
            __sync_synchronize();
            if (count && !write) kmemcpy(buffer, (const void *)(uintptr_t)disk->bounce, bytes);
            return true;
        }
        if (hz && pit_get_ticks() - started > 5U * hz) break;
        pause_cpu();
    }
    kprintf("[AHCI] %s port=%u cmd=%x lba=%u is=%x tfd=%x ci=%x\n",
            disk->name, disk->port_number, opcode, lba,
            mmio32(port, AHCI_P_IS), mmio32(port, AHCI_P_TFD),
            mmio32(port, AHCI_P_CI));
    disk->failed = true; /* Do not reuse DMA buffers with an active command. */
    return false;
}

static bool ahci_read(block_device_t *block, uint32_t lba, uint8_t count, void *buffer) {
    ahci_disk_t *disk = (ahci_disk_t *)block->driver_data;
    uint8_t *out = (uint8_t *)buffer;
    while (count) {
        uint8_t chunk = count > AHCI_DMA_SECTORS ? AHCI_DMA_SECTORS : count;
        if (!ahci_command(disk, ATA_READ_DMA_EXT, lba, chunk, false, out)) return false;
        count -= chunk;
        lba += chunk;
        out += (uint32_t)chunk * BLOCK_SECTOR_SIZE;
    }
    return true;
}

static bool ahci_write(block_device_t *block, uint32_t lba, uint8_t count,
                       const void *buffer) {
    ahci_disk_t *disk = (ahci_disk_t *)block->driver_data;
    const uint8_t *in = (const uint8_t *)buffer;
    while (count) {
        uint8_t chunk = count > AHCI_DMA_SECTORS ? AHCI_DMA_SECTORS : count;
        if (!ahci_command(disk, ATA_WRITE_DMA_EXT, lba, chunk, true, (void *)in)) return false;
        count -= chunk;
        lba += chunk;
        in += (uint32_t)chunk * BLOCK_SECTOR_SIZE;
    }
    /* Match IDE's durable completion rather than relying on QEMU's cache. */
    return ahci_command(disk, ATA_FLUSH_CACHE_EXT, 0U, 0U, false, NULL);
}

static bool ahci_setup_port(volatile uint8_t *hba, uint32_t number) {
    volatile uint8_t *port = hba + AHCI_PORT_BASE + number * AHCI_PORT_SIZE;
    uint32_t ssts = mmio32(port, AHCI_P_SSTS);
    if ((ssts & 0x0fU) != 3U || ((ssts >> 8U) & 0x0fU) != 1U ||
        mmio32(port, AHCI_P_SIG) != AHCI_SIG_ATA) return false;
    if (g_disk_count >= AHCI_MAX_DISKS) return false;
    ahci_disk_t *disk = &g_disks[g_disk_count];
    kmemset(disk, 0, sizeof(*disk));
    disk->port_number = (uint8_t)number;
    disk->regs = port;
    if (!mm_vm_frame_alloc(1U, &disk->meta)) return false;
    if (!mm_vm_frame_alloc(AHCI_DMA_PAGES, &disk->bounce)) {
        mm_vm_frame_free(disk->meta, 1U);
        disk->meta = 0;
        return false;
    }
    /* Stop both DMA engines before replacing CLB and FB. */
    mmio_write32(port, AHCI_P_CMD, mmio32(port, AHCI_P_CMD) & ~AHCI_CMD_ST);
    if (!port_wait_clear(port, AHCI_P_CMD, AHCI_CMD_CR)) goto fail;
    mmio_write32(port, AHCI_P_CMD, mmio32(port, AHCI_P_CMD) & ~AHCI_CMD_FRE);
    if (!port_wait_clear(port, AHCI_P_CMD, AHCI_CMD_FR)) goto fail;
    kmemset((void *)(uintptr_t)disk->meta, 0, 4096U);
    mmio_write32(port, AHCI_P_CLB, disk->meta);
    mmio_write32(port, AHCI_P_CLBU, 0U);
    mmio_write32(port, AHCI_P_FB, disk->meta + 0x400U);
    mmio_write32(port, AHCI_P_FBU, 0U);
    mmio_write32(port, AHCI_P_IE, 0U); /* polled mode */
    mmio_write32(port, AHCI_P_SERR, 0xffffffffU);
    mmio_write32(port, AHCI_P_IS, 0xffffffffU);
    __sync_synchronize();
    mmio_write32(port, AHCI_P_CMD, mmio32(port, AHCI_P_CMD) | AHCI_CMD_FRE);
    mmio_write32(port, AHCI_P_CMD, mmio32(port, AHCI_P_CMD) | AHCI_CMD_ST);
    uint16_t *id = (uint16_t *)(uintptr_t)disk->bounce;
    if (!ahci_command(disk, ATA_IDENTIFY, 0U, 1U, false, id)) goto fail;
    if (!(id[83] & (1U << 10U))) goto fail; /* LBA48 is mandatory here. */
    disk->sectors = (uint32_t)id[100] | ((uint32_t)id[101] << 16U);
    if (!disk->sectors) goto fail;
    disk->name[0] = 'a'; disk->name[1] = 'h'; disk->name[2] = 'c';
    disk->name[3] = 'i'; disk->name[4] = (char)('0' + g_disk_count);
    if (!block_register_ex(disk->name, BLOCK_DEVICE_AHCI, disk->sectors,
                            BLOCK_SECTOR_SIZE, false, disk, ahci_read)) goto fail;
    block_set_writer(disk->name, ahci_write);
    g_disk_count++;
    kprintf("[AHCI] %s: port=%u sectors=%u DMA ready\n",
            disk->name, number, disk->sectors);
    return true;
fail:
    mmio_write32(port, AHCI_P_CMD, mmio32(port, AHCI_P_CMD) & ~AHCI_CMD_ST);
    if (port_wait_clear(port, AHCI_P_CMD, AHCI_CMD_CR)) {
        mmio_write32(port, AHCI_P_CMD, mmio32(port, AHCI_P_CMD) & ~AHCI_CMD_FRE);
        if (port_wait_clear(port, AHCI_P_CMD, AHCI_CMD_FR)) {
            mm_vm_frame_free(disk->bounce, AHCI_DMA_PAGES);
            mm_vm_frame_free(disk->meta, 1U);
        }
    }
    kmemset(disk, 0, sizeof(*disk));
    return false;
}

void ahci_init(void) {
    g_disk_count = 0;
    kmemset(g_disks, 0, sizeof(g_disks));
    for (uint32_t index = 0; index < pci_device_count(); index++) {
        const pci_device_t *pci = pci_device_at(index);
        if (!pci || pci->class_code != 0x01U || pci->subclass != 0x06U ||
            pci->prog_if != 0x01U) continue;
        pci_bar_info_t bar;
        if (!pci_get_bar_info(pci, 5U, &bar) || bar.is_io || bar.is_64 ||
            bar.size < AHCI_PORT_BASE + AHCI_PORT_SIZE ||
            !paging_kernel_mmio_uncached(bar.base, bar.size) ||
            !pci_enable_command(pci, PCI_COMMAND_MEMORY | PCI_COMMAND_BUSMASTER)) {
            kprintf("[AHCI] PCI controller has unsupported BAR5\n");
            continue;
        }
        volatile uint8_t *hba = (volatile uint8_t *)(uintptr_t)bar.base;
        uint32_t ghc = mmio32(hba, AHCI_HBA_GHC);
        mmio_write32(hba, AHCI_HBA_GHC, (ghc | AHCI_GHC_AE) & ~2U);
        uint32_t pi = mmio32(hba, AHCI_HBA_PI);
        kprintf("[AHCI] PCI %x:%x.%x ABAR=%x ports=%x\n",
                pci->bus, pci->slot, pci->function, bar.base, pi);
        for (uint32_t port = 0; port < 32U && g_disk_count < AHCI_MAX_DISKS; port++)
            if ((pi & (1U << port)) &&
                bar.size >= AHCI_PORT_BASE + (port + 1U) * AHCI_PORT_SIZE)
                (void)ahci_setup_port(hba, port);
    }
}
