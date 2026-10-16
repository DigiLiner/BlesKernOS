#ifndef BFS4_H
#define BFS4_H

#include "fat.h"
#include "block.h"
#include "../../sdk/include/bleskernos_defrag.h"

#define BFS4_BOOT_SECTORS 4096U
#define BFS4_BLOCK_SIZE 4096U
#define BFS4_MAGIC 0x34534642U

/* BFS4 uses the existing VFS directory-entry contract, while retaining
 * stable native inode numbers and a separate 4 KiB extent-based layout. */
bool bfs4_mount(fat_fs_t *fs, block_device_t *dev, uint32_t partition_lba);
bool bfs4_format(block_device_t *dev, const char *label);
bool bfs4_sync(const fat_fs_t *fs);
bool bfs4_unmount(const fat_fs_t *fs);
bool bfs4_repair(const fat_fs_t *fs,fat_repair_report_t *repair,fat_check_report_t *after);
bool bfs4_check(const fat_fs_t *fs, fat_check_report_t *report);
bool bfs4_resolve(const fat_fs_t *fs, const char *path, fat_dir_entry_t *entry);
bool bfs4_list(const fat_fs_t *fs, const fat_dir_entry_t *dir,
               fat_dir_entry_t *entries, fat_dir_metadata_t *metadata,
               uint32_t capacity, uint32_t *count);
bool bfs4_read(const fat_fs_t *fs, const fat_dir_entry_t *entry,
               uint32_t offset, void *buffer, uint32_t length, uint32_t *done);
bool bfs4_write(fat_fs_t *fs, const char *path, const void *data, uint32_t size);
bool bfs4_append(fat_fs_t *fs, const char *path, const void *data,
                 uint32_t old_size, uint32_t new_size);
/* Bounded-memory range I/O; extension and truncation zero newly exposed bytes. */
bool bfs4_write_at(fat_fs_t *fs, const char *path, uint32_t offset,
                   const void *data, uint32_t length);
bool bfs4_resize(fat_fs_t *fs, const char *path, uint32_t size);
bool bfs4_mkdir(fat_fs_t *fs, const char *path);
bool bfs4_remove(fat_fs_t *fs, const char *path);
bool bfs4_rename(fat_fs_t *fs, const char *old_path, const char *new_path);
bool bfs4_inode(const fat_fs_t *fs, const char *path, uint64_t *inode);
bool bfs4_get_mode(const fat_fs_t *fs, const char *path, uint16_t *mode);
bool bfs4_set_mode(fat_fs_t *fs, const char *path, uint16_t mode);
bool bfs4_get_times(const fat_fs_t *fs, const char *path,
                    uint32_t *access, uint32_t *modify);
bool bfs4_set_times(fat_fs_t *fs, const char *path,
                    bool set_access, uint64_t access,
                    bool set_modify, uint64_t modify);
bool bfs4_space(const fat_fs_t *fs, uint64_t *total, uint64_t *free);
/* Boot-time swap reservation: contiguous, fully allocated regular file.
 * Its inode stays pinned until reboot; raw page I/O bypasses filesystem locks. */
bool bfs4_swap_pin(const fat_fs_t *fs, const char *path,
                   block_device_t **device, uint32_t *lba, uint32_t *sectors);
void bfs4_swap_unpin(const fat_fs_t *fs); /* activation rollback only */

bool bfs4_defrag_begin(const fat_fs_t *fs,uint32_t owner,bool optimize);
bool bfs4_defrag_step(const fat_fs_t *fs,uint32_t owner);
bool bfs4_defrag_status(const fat_fs_t *fs,bk_defrag_info_t *out);
bool bfs4_defrag_cancel(const fat_fs_t *fs,uint32_t owner);
#endif
