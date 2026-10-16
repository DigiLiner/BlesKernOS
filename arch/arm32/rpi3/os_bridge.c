#include "os_bridge.h"
#include "sd.h"
#include "framebuffer.h"
#include "../../../kernel/include/block.h"
#include "../../../kernel/include/fat.h"
#include "../../../kernel/include/vfs.h"
#include "../../../kernel/include/memory.h"
#include "../../../kernel/include/vga.h"

#define ROOT_PREVIEW_ENTRIES 24U
static vfs_dir_entry_t g_root_entries[ROOT_PREVIEW_ENTRIES];
static uint32_t g_root_count;

static bool sd_block_read(block_device_t *dev UNUSED, uint32_t lba,
                          uint8_t count, void *buffer) {
    return rpi3_sd_read_blocks(lba, count, buffer) == 0;
}

int rpi3_bles_core_init(void) {
    fat_fs_t fs;
    heap_info_t heap;
    uint32_t count = 0U;

    mm_init();
    if (!mm_heap_size()) {
        kprintf("[BLES:MM] heap init FAILED\n");
        return -1;
    }
    mm_get_info(&heap);
    kprintf("[BLES:MM] shared kmalloc online heap=%u KiB free=%u KiB\n",
            (uint32_t)(heap.total_bytes / 1024U),
            (uint32_t)(heap.free_bytes / 1024U));

    block_init();
    if (!block_register_ex("sd0", BLOCK_DEVICE_SD, 0U, BLOCK_SECTOR_SIZE,
                           true, (void *)0, sd_block_read)) {
        kprintf("[BLES:BLOCK] sd0 registration FAILED\n");
        return -2;
    }
    kprintf("[BLES:BLOCK] sd0 registered through kernel/drivers/core/block.c\n");

    vfs_init();
    if (!vfs_mount("sd0")) {
        kprintf("[BLES:VFS] sd0 has no mountable FAT volume\n");
        return -3;
    }
    if (!vfs_get_fs_info(&fs)) {
        kprintf("[BLES:FAT] active filesystem unavailable\n");
        return -4;
    }
    kprintf("[BLES:FAT] FAT%u label=%s sectors=%u cluster=%u\n",
            (uint32_t)fs.type, fs.volume_label, fs.total_sectors,
            (uint32_t)fs.sectors_per_cluster);

    if (!vfs_listdir("/", g_root_entries, ROOT_PREVIEW_ENTRIES, &count)) {
        kprintf("[BLES:VFS] root listing FAILED\n");
        return -5;
    }
    g_root_count = count;
    kprintf("[BLES:VFS] / mounted; %u entries (showing <= %u)\n",
            count, ROOT_PREVIEW_ENTRIES);
    for (uint32_t i = 0U; i < count; ++i) {
        kprintf("  %c %s %u bytes\n",
                g_root_entries[i].type == VFS_NODE_DIR ? 'D' : 'F',
                g_root_entries[i].name, g_root_entries[i].size);
    }

    /* FAT/VFS success is reported through kprintf.  Do not write diagnostics
       directly to the framebuffer: Stage 8 bootsplash/GUI owns it now. */
    return 0;
}

uint32_t rpi3_bles_root_count(void) { return g_root_count; }
