#ifndef VFS_H
#define VFS_H

#include "types.h"
#include "fat.h"

#define VFS_MAX_PATH 260
#define VFS_MAX_NAME 256
#define VFS_MAX_OPEN_FILES 128
#define VFS_PRIVATE_IMAGE_MAX_BYTES (16U * 1024U * 1024U)
#define VFS_MAX_DIR_ENTRIES FAT_MAX_DIR_ENTRIES

#define VFS_O_RDONLY 0x0001
#define VFS_O_WRONLY 0x0002
#define VFS_O_RDWR   0x0003
#define VFS_O_CREATE 0x0010
#define VFS_O_TRUNC  0x0020
#define VFS_O_DIRECTORY 0x0040

/* Successful namespace/data operations, observed while KDOMAIN_VFS is held.
 * A sink must not block or call back into VFS. renamed is non-NULL for a
 * completed rename; consumers can correlate its old and new names. */
#define VFS_NOTIFY_ACCESS      0x001U
#define VFS_NOTIFY_MODIFY      0x002U
#define VFS_NOTIFY_ATTRIB      0x004U
#define VFS_NOTIFY_CLOSE_WRITE 0x008U
#define VFS_NOTIFY_CLOSE_READ  0x010U
#define VFS_NOTIFY_OPEN        0x020U
#define VFS_NOTIFY_MOVED_FROM  0x040U
#define VFS_NOTIFY_MOVED_TO    0x080U
#define VFS_NOTIFY_CREATE      0x100U
#define VFS_NOTIFY_DELETE      0x200U
#define VFS_NOTIFY_DELETE_SELF 0x400U
#define VFS_NOTIFY_MOVE_SELF   0x800U
typedef void (*vfs_change_sink_t)(const char *path,const char *renamed,
                                  uint32_t mask,bool directory);
void vfs_observe_changes(vfs_change_sink_t sink);

typedef enum {
    VFS_NODE_NONE = 0,
    VFS_NODE_FILE,
    VFS_NODE_DIR
} vfs_node_type_t;

typedef struct {
    char name[VFS_MAX_NAME];
    uint32_t size;
    vfs_node_type_t type;
    uint8_t attributes;
} vfs_dir_entry_t;

void vfs_init(void);
bool vfs_normalize_nofollow(const char *path,char *out);
int vfs_readlink(const char *path,char *target,uint32_t capacity);
int vfs_symlink(const char *target,const char *path);
bool vfs_identity_nofollow(const char *path,uint64_t *device,uint64_t *inode);
bool vfs_normalize_path(const char *path,char *out);
bool vfs_mount(const char *name);
bool vfs_attach_volume(const char *name);
bool vfs_unmount_volume(const char *name);
bool vfs_mount_default(void);
void vfs_forget_volume(const char *name);
bool vfs_volume_busy(const char *name);
bool vfs_cdrom_busy(void);
void vfs_use_cdrom_as_root(bool enabled);
bool vfs_cdrom_is_root(void);
bool vfs_get_fs_info(fat_fs_t *fs);
bool vfs_has_cdrom(void);
const char *vfs_get_mount_name(void);
const char *vfs_getcwd(void);
bool vfs_chdir(const char *path);
bool vfs_listdir(const char *path, vfs_dir_entry_t *entries, uint32_t max_entries, uint32_t *count);
int vfs_open(const char *path, uint32_t flags);
int vfs_read(int fd, void *buffer, uint32_t size);
int vfs_write(int fd, const void *buffer, uint32_t size);
/* BLES_WINE_SFX_IO_CACHE_FIX_20260723: commit the coherent per-open FAT cache. */
bool vfs_flush(int fd);
bool vfs_sync_all(void);
bool vfs_close(int fd);
bool vfs_retain(int fd);
int32_t vfs_status_flags(int fd);
bool vfs_set_status_flags(int fd,uint32_t flags);
int32_t vfs_seek(int fd, int32_t offset, uint32_t origin);
int32_t vfs_tell(int fd);
int32_t vfs_size(int fd);
bool vfs_truncate(int fd, uint32_t size);
bool vfs_stat(const char *path, vfs_dir_entry_t *entry);
bool vfs_identity(const char *path, uint64_t *device, uint64_t *inode);
bool vfs_fd_identity(int fd, uint64_t *device, uint64_t *inode);
/* Resident inode reference for demand paging; it consumes no descriptor slot.
 * Calls run under KDOMAIN_VFS and read into already resident kernel buffers. */
typedef struct vfs_page_reference vfs_page_reference_t;
vfs_page_reference_t *vfs_page_reference_acquire(int fd);
void vfs_page_reference_release(vfs_page_reference_t *reference);
uint32_t vfs_page_reference_size(vfs_page_reference_t *reference);
bool vfs_page_reference_identity(vfs_page_reference_t *reference,uint64_t *device,uint64_t *inode);
void vfs_page_reference_retain(vfs_page_reference_t *reference);
bool vfs_page_reference_read(vfs_page_reference_t *reference,uint32_t offset,
                             void *buffer,uint32_t length,uint32_t *read_bytes);
typedef struct vfs_shared_mapping {
    uint32_t backing,length,size,references;
    uint64_t device,inode;
    int fd;
    bool writable;
    bool unlinked;
    bool cached, stale;
    uint32_t last_used;
    char path[VFS_MAX_PATH];
} vfs_shared_mapping_t;
vfs_shared_mapping_t *vfs_shared_acquire(int fd,uint32_t length);
vfs_shared_mapping_t *vfs_private_acquire(int fd);
bool vfs_has_shared_mapping(int fd);
void vfs_cache_perf(uint32_t *entries, uint32_t *bytes, uint32_t *evictions,
                    uint32_t *handles_peak, uint32_t *handles, uint32_t *private_fds);
/* Opportunistic MAP_PRIVATE cache users leave a few shared-map slots free for
 * real MAP_SHARED/anonymous IPC objects. */
bool vfs_shared_prefetch_room(void);
bool vfs_shared_sync(vfs_shared_mapping_t *mapping);
bool vfs_shared_release(vfs_shared_mapping_t *mapping);
const char *vfs_fd_path(int fd);
bool vfs_is_directory(int fd);
/* FAT-specific directory iterator used by Linux VFAT_IOCTL_READDIR_BOTH.
 * The VFS keeps a per-open directory cache so repeated ioctls advance
 * by index without rescanning the FAT directory on every entry. */
bool vfs_fat_directory(int fd, const fat_dir_entry_t **entries, uint32_t *count);
bool vfs_fat_directory_identity(int fd, uint32_t index, uint64_t *inode, uint16_t *mode);
bool vfs_fat_dir_entry_at(int fd, uint32_t index, fat_dir_entry_t *entry, bool *end);
bool vfs_mkdir(const char *path);
bool vfs_mkdir_mode(const char *path, uint16_t mode);
bool vfs_get_mode(const char *path, uint16_t *mode);
bool vfs_set_mode(const char *path, uint16_t mode);
bool vfs_fd_mode(int fd, uint16_t *mode);
bool vfs_set_fd_mode(int fd, uint16_t mode);
bool vfs_set_times(const char *path, bool set_access, uint64_t access_seconds,
                   bool set_modify, uint64_t modify_seconds);
bool vfs_get_times(const char *path, uint32_t *access_seconds,
                   uint32_t *modify_seconds);
bool vfs_remove(const char *path);
int vfs_transfer_check(const char *source,const char *directory);
bool vfs_rename(const char *old_path, const char *new_path);
bool vfs_get_space(uint64_t *total_bytes, uint64_t *free_bytes);
/* Like vfs_get_space_path(), but also reports the actual disk backend.
 * Linux statfs needs this distinction: BFS4 is byte-transparent but is not
 * fully Unicode case-insensitive, so it must not masquerade as VFAT. */
bool vfs_get_optical_space(const char *path,uint64_t *total_bytes);
bool vfs_get_space_path_type(const char *path, uint64_t *total_bytes,
                             uint64_t *free_bytes, fat_type_t *type);
bool vfs_get_space_path(const char *path, uint64_t *total_bytes,
                        uint64_t *free_bytes);
bool vfs_read_all(const char *path, void **buffer, uint32_t *size);
bool vfs_write_all(const char *path, const void *buffer, uint32_t size);
/* Keep the target FAT allocation table cached while an installer writes many
 * files. The path only selects the target volume; usually use "/". */
bool vfs_bulk_write_begin(const char *path);
void vfs_bulk_write_end(void);

/* Diagnostico liviano para fallos de instaladores Win32. */
const char *vfs_last_error_text(void);
uint32_t vfs_open_file_count(void);
void vfs_dump_open_files(void);

#endif
