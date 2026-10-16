#ifndef BLOCK_H
#define BLOCK_H

#include "types.h"

#define BLOCK_MAX_DEVICES 24
#define BLOCK_SECTOR_SIZE 512

typedef enum {
    BLOCK_DEVICE_NONE = 0,
    BLOCK_DEVICE_ATA,
    BLOCK_DEVICE_FLOPPY,
    BLOCK_DEVICE_ATAPI,
    BLOCK_DEVICE_USB,
    BLOCK_DEVICE_SD,
    BLOCK_DEVICE_AHCI
} block_device_type_t;

typedef struct block_device block_device_t;
typedef bool (*block_read_fn_t)(block_device_t *dev, uint32_t lba, uint8_t count, void *buffer);
typedef bool (*block_write_fn_t)(block_device_t *dev, uint32_t lba, uint8_t count, const void *buffer);

struct block_device {
    char name[8];
    block_device_type_t type;
    uint32_t sector_count;
    uint32_t base_lba;
    uint16_t sector_size;
    bool read_only;
    void *driver_data;
    block_read_fn_t read;
    block_write_fn_t write;
};

/* Counters for successful and failed block requests; no extra I/O. */
typedef struct {
    uint32_t ata_reads, ata_read_sectors;
    uint32_t atapi_reads, atapi_read_sectors;
    uint32_t ata_writes, ata_write_sectors;
    uint32_t failed_requests;
} block_io_stats_t;
void block_get_io_stats(block_io_stats_t *out);

void block_init(void);
bool block_register(const char *name, block_device_type_t type, uint32_t sector_count, void *driver_data, block_read_fn_t read);
bool block_register_ex(const char *name, block_device_type_t type, uint32_t sector_count, uint16_t sector_size, bool read_only, void *driver_data, block_read_fn_t read);
/* Register a bounded view into an existing disk. The view reuses the parent
 * controller callbacks but all block I/O is translated by base_lba. */
bool block_register_view(const char *name, block_device_t *parent,
                         uint32_t base_lba, uint32_t sector_count);
block_device_t *block_get(const char *name);
/* Safely retire a removable device without shifting its stable slot. */
bool block_unregister(const char *name);
uint32_t block_count(void);
block_device_t *block_at(uint32_t index);
bool block_read(block_device_t *dev, uint32_t lba, uint8_t count, void *buffer);
bool block_set_writer(const char *name, block_write_fn_t write);
bool block_write(block_device_t *dev, uint32_t lba, uint8_t count, const void *buffer);
/* Volatile backing (swap) needs completed/coherent writes, but no durability
 * across a machine shutdown. Filesystem transactions always use block_write.
 * Kept outside block_device_t to preserve the existing driver ABI. */
bool block_set_volatile_writer(const char *name,block_write_fn_t write);
bool block_write_volatile(block_device_t *dev,uint32_t lba,uint8_t count,const void *buffer);
const char *block_type_name(block_device_type_t type);

#endif
