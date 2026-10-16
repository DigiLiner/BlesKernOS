#include "../../include/block.h"
#include "../../include/memory.h"
#include "../../include/task.h"
#include "../../include/bootsplash.h"
#include "../../include/perfmon.h"
#include "../../include/klock.h"
#include "../../include/w3_profile.h"
#include "../../include/vfs.h"

static block_device_t g_devices[BLOCK_MAX_DEVICES];
static uint32_t g_device_count = 0;
static block_io_stats_t g_io_stats;
static bool g_block_offline[BLOCK_MAX_DEVICES];
static block_write_fn_t volatile_writers[BLOCK_MAX_DEVICES];
static kmutex_t g_block_io_lock = KMUTEX_INITIALIZER;
static krwlock_t g_block_registry_lock = KRWLOCK_INITIALIZER;

/* Controller transactions may sleep while waiting for hardware. Use a
 * task-owned mutex rather than a CPU spinlock so unrelated render/GUI work
 * continues on the other processors. */
static void block_io_lock(void) {
    kmutex_lock(&g_block_io_lock);
}

static void block_io_unlock(void) {
    kmutex_unlock(&g_block_io_lock);
}

void block_init(void) {
    kmemset(&g_io_stats, 0, sizeof(g_io_stats));
    kmemset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;
    kmemset(g_block_offline, 0, sizeof(g_block_offline));
    kmemset(volatile_writers,0,sizeof(volatile_writers));
    kmutex_init(&g_block_io_lock);
    krwlock_init(&g_block_registry_lock);
}

bool block_register(const char *name, block_device_type_t type, uint32_t sector_count, void *driver_data, block_read_fn_t read) {
    return block_register_ex(name, type, sector_count, BLOCK_SECTOR_SIZE, false, driver_data, read);
}

/* Preserve the address of each block_device_t while filesystems may still
 * retain it. Tombstones are reusable only after VFS releases old handles. */
static int block_slot(const char *name, bool online_only) {
    if (!name) return -1;
    for (uint32_t i = 0; i < g_device_count; ++i)
        if (kstrcmp(g_devices[i].name, name) == 0 &&
            (!online_only || !g_block_offline[i])) return (int)i;
    return -1;
}

bool block_register_ex(const char *name, block_device_type_t type, uint32_t sector_count, uint16_t sector_size, bool read_only, void *driver_data, block_read_fn_t read) {
    block_device_t *dev;
    bool ok = false;
    if (!name || !name[0] || !read) return false;
    if (sector_size == 0) sector_size = BLOCK_SECTOR_SIZE;
    /* Never reuse a slot referenced by a detached volume. */
    bool busy = vfs_volume_busy(name);
    block_io_lock();
    krwlock_write_lock(&g_block_registry_lock);
    int slot = block_slot(name, false);
    if (slot < 0 && g_device_count < BLOCK_MAX_DEVICES)
        slot = (int)g_device_count++;
    if (slot >= 0 && (g_devices[slot].name[0] == '\0' ||
                      (g_block_offline[slot] && !busy))) {
        dev = &g_devices[slot];
        kmemset(dev, 0, sizeof(*dev));
        kstrncpy(dev->name, name, sizeof(dev->name) - 1);
        dev->type = type;
        dev->sector_count = sector_count;
        dev->sector_size = sector_size;
        dev->read_only = read_only;
        dev->driver_data = driver_data;
        dev->read = read;
        volatile_writers[slot] = NULL;
        g_block_offline[slot] = false;
        ok = true;
    }
    krwlock_write_unlock(&g_block_registry_lock);
    block_io_unlock();
    return ok;
}

bool block_unregister(const char *name) {
    bool removed = false;
    if (!name || !name[0]) return false;
    block_io_lock(); /* drain all in-flight block I/O first */
    krwlock_write_lock(&g_block_registry_lock);
    int slot = block_slot(name, true);
    if (slot >= 0) {
        g_block_offline[slot] = true;
        volatile_writers[slot] = NULL;
        removed = true;
    }
    krwlock_write_unlock(&g_block_registry_lock);
    block_io_unlock();
    if (removed) vfs_forget_volume(name);
    return removed;
}

static block_device_t *block_find_unlocked(const char *name) {
    if (!name) return NULL;
    int slot = block_slot(name, true);
    return slot >= 0 ? &g_devices[slot] : NULL;
    return NULL;
}

bool block_register_view(const char *name, block_device_t *parent,
                         uint32_t base_lba, uint32_t sector_count) {
    block_device_t *view;
    bool ok = false;
    if (!name || !name[0] || !parent || !parent->read || !sector_count)
        return false;
    for (uint32_t i = 0; i < g_device_count; ++i)
        if (parent == &g_devices[i] && g_block_offline[i]) return false;
    if (parent->sector_count &&
        (base_lba >= parent->sector_count ||
         sector_count > parent->sector_count - base_lba)) return false;

    krwlock_write_lock(&g_block_registry_lock);
    if (g_device_count < BLOCK_MAX_DEVICES && !block_find_unlocked(name)) {
        view = &g_devices[g_device_count++];
        kmemset(view, 0, sizeof(*view));
        kstrncpy(view->name, name, sizeof(view->name) - 1U);
        view->type = parent->type;
        view->sector_count = sector_count;
        view->base_lba = parent->base_lba + base_lba;
        view->sector_size = parent->sector_size;
        view->read_only = parent->read_only;
        view->driver_data = parent->driver_data;
        view->read = parent->read;
        view->write = parent->write;
        for(uint32_t i=0;i<g_device_count-1U;i++)if(parent==&g_devices[i])volatile_writers[g_device_count-1U]=volatile_writers[i];
        ok = true;
    }
    krwlock_write_unlock(&g_block_registry_lock);
    return ok;
}

block_device_t *block_get(const char *name) {
    block_device_t *result;
    krwlock_read_lock(&g_block_registry_lock);
    result = block_find_unlocked(name);
    krwlock_read_unlock(&g_block_registry_lock);
    return result;
}

uint32_t block_count(void) {
    uint32_t count;
    krwlock_read_lock(&g_block_registry_lock);
    count = g_device_count;
    krwlock_read_unlock(&g_block_registry_lock);
    return count;
}

block_device_t *block_at(uint32_t index) {
    block_device_t *result = NULL;
    krwlock_read_lock(&g_block_registry_lock);
    if (index < g_device_count && !g_block_offline[index]) result = &g_devices[index];
    krwlock_read_unlock(&g_block_registry_lock);
    return result;
}

static bool block_read_v4_untracked(block_device_t *dev, uint32_t lba, uint8_t count, void *buffer) {
    W3P_SCOPE(W3P_BLOCK_READ, (uint32_t)count * BLOCK_SECTOR_SIZE);
    bool result;
    uint64_t perf_started;
    if (!dev || !dev->read || !buffer || count == 0) return false;
    if (dev->sector_count &&
        (lba >= dev->sector_count || count > dev->sector_count - lba)) return false;
    if (dev->base_lba > 0xffffffffU - lba ||
        count - 1U > 0xffffffffU - (dev->base_lba + lba)) return false;

    bootsplash_pulse();
    perf_started = perfmon_scope_begin();
    block_io_lock();
    int slot = (dev >= g_devices && dev < g_devices + g_device_count)
        ? (int)(dev - g_devices) : -1;
    result = (slot < 0 || !g_block_offline[slot]) &&
             dev->read(dev, dev->base_lba + lba, count, buffer);
    block_io_unlock();
    perfmon_block_complete((uint32_t)dev->type, false, count, result,
                           perf_started);
    bootsplash_pulse();
    return result;
}

bool block_read(block_device_t *dev, uint32_t lba, uint8_t count, void *buffer) {
    bool v4_result = block_read_v4_untracked(dev, lba, count, buffer);
    if (dev && dev->type == BLOCK_DEVICE_ATA) {
        __sync_fetch_and_add(&g_io_stats.ata_reads, 1U);
        __sync_fetch_and_add(&g_io_stats.ata_read_sectors, count);
    } else if (dev && dev->type == BLOCK_DEVICE_ATAPI) {
        __sync_fetch_and_add(&g_io_stats.atapi_reads, 1U);
        __sync_fetch_and_add(&g_io_stats.atapi_read_sectors, count);
    }
    if (!v4_result) __sync_fetch_and_add(&g_io_stats.failed_requests, 1U);
    return v4_result;
}


bool block_set_writer(const char *name, block_write_fn_t write) {
    block_device_t *dev;
    bool ok = false;
    krwlock_write_lock(&g_block_registry_lock);
    dev = block_find_unlocked(name);
    if (dev) {
        dev->write = write;
        ok = true;
    }
    krwlock_write_unlock(&g_block_registry_lock);
    return ok;
}

static bool block_write_with_v4_untracked(block_device_t *dev,uint32_t lba,uint8_t count,const void *buffer,block_write_fn_t writer) {
    W3P_SCOPE(W3P_BLOCK_WRITE, (uint32_t)count * BLOCK_SECTOR_SIZE);
    bool result;
    uint64_t perf_started;
    if (!dev || !writer || !buffer || count == 0) return false;
    if (dev->read_only) return false;
    if (dev->sector_count &&
        (lba >= dev->sector_count || count > dev->sector_count - lba)) return false;
    if (dev->base_lba > 0xffffffffU - lba ||
        count - 1U > 0xffffffffU - (dev->base_lba + lba)) return false;

    perf_started = perfmon_scope_begin();
    block_io_lock();
    int slot = (dev >= g_devices && dev < g_devices + g_device_count)
        ? (int)(dev - g_devices) : -1;
    result = (slot < 0 || !g_block_offline[slot]) &&
             writer(dev, dev->base_lba + lba, count, buffer);
    block_io_unlock();
    perfmon_block_complete((uint32_t)dev->type, true, count, result,
                           perf_started);
    return result;
}

static bool block_write_with(block_device_t *dev, uint32_t lba, uint8_t count, const void *buffer, block_write_fn_t writer) {
    bool v4_result = block_write_with_v4_untracked(dev, lba, count, buffer, writer);
    if (dev && dev->type == BLOCK_DEVICE_ATA) {
        __sync_fetch_and_add(&g_io_stats.ata_writes, 1U);
        __sync_fetch_and_add(&g_io_stats.ata_write_sectors, count);
    }
    if (!v4_result) __sync_fetch_and_add(&g_io_stats.failed_requests, 1U);
    return v4_result;
}


bool block_write(block_device_t *dev,uint32_t lba,uint8_t count,const void *buffer) {
    return block_write_with(dev,lba,count,buffer,dev?dev->write:NULL);
}
bool block_set_volatile_writer(const char *name,block_write_fn_t writer) {
    bool ok=false;krwlock_write_lock(&g_block_registry_lock);
    for(uint32_t i=0;i<g_device_count;i++)if(kstrcmp(g_devices[i].name,name)==0) {
        volatile_writers[i]=writer;ok=true;break;
    }
    krwlock_write_unlock(&g_block_registry_lock);return ok;
}
bool block_write_volatile(block_device_t *dev,uint32_t lba,uint8_t count,const void *buffer) {
    block_write_fn_t writer=dev?dev->write:NULL;
    krwlock_read_lock(&g_block_registry_lock);
    for(uint32_t i=0;i<g_device_count;i++)if(dev==&g_devices[i] && volatile_writers[i]) {
        writer=volatile_writers[i];break;
    }
    krwlock_read_unlock(&g_block_registry_lock);
    return block_write_with(dev,lba,count,buffer,writer);
}

void block_get_io_stats(block_io_stats_t *out) {
    if (!out) return;
    block_io_lock();
    *out = g_io_stats;
    block_io_unlock();
}

const char *block_type_name(block_device_type_t type) {
    switch (type) {
        case BLOCK_DEVICE_ATA: return "ata";
        case BLOCK_DEVICE_FLOPPY: return "floppy";
        case BLOCK_DEVICE_ATAPI: return "atapi";
        case BLOCK_DEVICE_USB: return "usb-storage";
        case BLOCK_DEVICE_SD: return "sd";
        case BLOCK_DEVICE_AHCI: return "ahci";
        default: return "none";
    }
}
