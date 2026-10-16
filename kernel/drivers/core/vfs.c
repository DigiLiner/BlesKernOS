#include "../../include/pager.h"
#include "../../include/vfs.h"
#include "../../include/block.h"
#include "../../include/memory.h"
#include "../../include/vga.h"
#include "../../include/iso9660.h"
#include "../../include/bootsplash.h"
#include "../../include/compat_mode.h"
#include "../../include/block.h"
#include "../../include/klock.h"
#include "../../include/kernel_domains.h"
#include "../../include/task.h"
#include "../../include/w3_profile.h"
#include "../../include/bfs4.h"

#define VFS_LISTDIR_HARD_LIMIT 8192U

#define VFS_MAX_VOLUMES BLOCK_MAX_DEVICES

typedef struct {
    bool used;
    char name[8];
    fat_fs_t fs;
} vfs_volume_t;

typedef enum {
    VFS_FILE_FAT = 0,
    VFS_FILE_ISO9660
} vfs_file_source_t;

/* BLES_WINE_SFX_IO_CACHE_FIX_20260723
 * FAT files opened by Win32 are cached while the handle is alive.  The old
 * vfs_write() rebuilt the complete file and replaced its cluster chain on
 * every small WriteFile call, leaving other open handles with stale cluster
 * metadata.  A coherent per-open cache makes synchronous reads stable and
 * turns an extractor's hundreds of small writes into one FAT commit. */
#define VFS_READ_CACHE_LIMIT  (16U * 1024U * 1024U)
#define VFS_WRITE_CACHE_LIMIT (64U * 1024U * 1024U)
#define VFS_CACHE_MIN_CAPACITY 4096U
/* Cache budget, not a native filesystem file-size limit. */
#define VFS_STREAM_THRESHOLD (1024U * 1024U)
#define VFS_FILE_POSITION_MAX 0x7fffffffU

typedef struct {
    bool used;
    uint32_t references;
    bool unlinked;
    uint32_t flags;
    uint32_t offset;
    uint64_t device, inode;
    char path[VFS_MAX_PATH];
    vfs_file_source_t source;
    union {
        struct {
            fat_dir_entry_t entry;
            fat_fs_t fs;
        } fat;
        iso9660_entry_t iso;
    } entry;
    uint8_t *cache;
    uint32_t cache_size;
    uint32_t cache_capacity;
    uint32_t cache_persisted_size;
    bool cache_loaded;
    bool cache_dirty;
    bool cache_append_only;
    fat_dir_entry_t *dir_cache;
    fat_dir_metadata_t *dir_metadata;
    uint32_t dir_cache_count;
    bool dir_cache_loaded;
} vfs_file_t;

struct vfs_page_reference {
    struct vfs_page_reference *next;
    uint32_t references;
    vfs_file_t file;
};
static vfs_page_reference_t *g_page_references;
static vfs_file_t g_files[VFS_MAX_OPEN_FILES];
static vfs_change_sink_t g_vfs_change_sink;
void vfs_observe_changes(vfs_change_sink_t sink) {
    g_vfs_change_sink=sink;
}
static void vfs_notify(const char *path,const char *renamed,uint32_t mask,bool directory) {
    if(g_vfs_change_sink)g_vfs_change_sink(path,renamed,mask,directory);
}
static bool vfs_page_resize_allowed(uint64_t device,uint64_t inode,uint32_t size) {
    for(vfs_page_reference_t *r=g_page_references;r;r=r->next)
        if(r->file.device==device && r->file.inode==inode &&
           vfs_page_reference_size(r)!=size)return false;
    return true;
}
static void vfs_page_written(vfs_file_t *file,uint32_t offset,const void *data,uint32_t length) {
    if(!g_page_references)return;
    if(!file->inode && !vfs_fd_identity((int)(file-g_files),&file->device,&file->inode))return;
    for(vfs_page_reference_t *r=g_page_references;r;r=r->next)
        if(r->file.device==file->device && r->file.inode==file->inode) {
            if(r->file.unlinked && r->file.cache_loaded) {
                if(offset>r->file.cache_size)kmemset(r->file.cache+r->file.cache_size,0,offset-r->file.cache_size);
                kmemcpy(r->file.cache+offset,data,length);
                if(offset+length>r->file.cache_size)r->file.cache_size=offset+length;
            } else if(!r->file.unlinked && offset+length>r->file.entry.fat.entry.size)
                r->file.entry.fat.entry.size=offset+length;
            pager_file_written(r,offset,data,length);
        }
}

/* Mantener la raiz en .data: los literales cortos pueden ser fusionados como
 * sufijos de otras cadenas por el linker, pero esta ruta se entrega como dato
 * estable a FAT durante montajes perezosos. */
static char g_vfs_root_path[2] = {'/', '\0'};
static bool g_vfs_cd_root = false;
/* Wine keeps many DLLs mapped across its service processes.  A 32-slot
 * table exhausted after the core libraries and forced subsequent PE sections
 * through the eager read/copy path. Keep the small-host limit, and allow more
 * live inode-backed views when at least 256 MiB of RAM is available. */
#define VFS_SHARED_MAP_SLOTS 128U
static vfs_shared_mapping_t g_shared_maps[VFS_SHARED_MAP_SLOTS];

static unsigned vfs_shared_map_limit(void) {
    return mm_total_bytes() >= 256U * 1024U * 1024U ? VFS_SHARED_MAP_SLOTS : 32U;
}
/* Private DLL images own frames, not VFS descriptions. Unreferenced images
 * remain hot until the byte budget or slot limit requires LRU eviction. */
#define VFS_PRIVATE_MAP_SLOTS 256U
static vfs_shared_mapping_t g_private_maps[VFS_PRIVATE_MAP_SLOTS];
static unsigned vfs_private_map_limit(void) {
    return mm_total_bytes() >= 256U * 1024U * 1024U ? VFS_PRIVATE_MAP_SLOTS : 32U;
}
static uint32_t g_private_bytes, g_private_entries, g_private_evictions;
static uint32_t g_private_age, g_vfs_open_files, g_vfs_open_peak;
static uint32_t g_vfs_writeback_maps;
static void vfs_private_invalidate(const char *path);
static bool vfs_mapping_attach(vfs_shared_mapping_t *m, int fd, bool writable);
static bool vfs_stream_file(const vfs_file_t *file, uint32_t size);
/* Most path lookups have no pending writes. Avoid scanning every open file
 * and mapping in that common case; VFS operations hold the VFS domain. */
static uint32_t g_vfs_dirty_files;
static uint32_t g_vfs_shared_file_maps;
static vfs_shared_mapping_t *vfs_shared_for_file(vfs_file_t *file);
static vfs_shared_mapping_t *vfs_private_find(vfs_file_t *file, bool idle);
typedef struct { uint64_t device, position; uint32_t inode; } vfs_identity_t;
/* Solo se reserva al consultar identidades Linux. Los IDs no se reutilizan
 * al borrar: un descriptor abierto conserva su identidad anterior. */
#define VFS_IDENTITIES 512U
typedef struct vfs_identity_chunk {
    struct vfs_identity_chunk *next;
    vfs_identity_t entries[VFS_IDENTITIES];
} vfs_identity_chunk;
static vfs_identity_chunk *g_identities;
static uint32_t g_next_inode=1;
static vfs_volume_t g_volumes[VFS_MAX_VOLUMES];
static char g_unmounted[VFS_MAX_VOLUMES][8];
/* Secondary FAT volumes are mounted lazily.  Once SMP is online, FileBrowser,
 * the terminal and background services may all resolve /ataN at the same
 * time.  Never let two CPUs construct the same cache slot concurrently. */
static kmutex_t g_volume_lock = KMUTEX_INITIALIZER;

/* BLES_WINE_INSTALL_DIAG_PERF_20260723 */
static const char *g_vfs_last_error = "none";

/* A volume root can arrive as "/" or as an empty inner path.  The latter is
 * a valid result for an exact mount-point path such as /ata1: after stripping
 * the device prefix there are zero components left.  Treat both forms as the
 * same root instead of feeding "" to fat_resolve_path(), which requires a
 * leading slash. */
static bool vfs_fat_path_is_root(const char *path) {
    return !path || path[0] == '\0' ||
           (path[0] == '/' && path[1] == '\0');
}

const char *vfs_last_error_text(void) {
    return g_vfs_last_error;
}

uint32_t vfs_open_file_count(void) {
    return g_vfs_open_files;
}

void vfs_dump_open_files(void) {
    kprintf("[VFS:diag] open handles %u/%u\n",
            vfs_open_file_count(), VFS_MAX_OPEN_FILES);
    for (uint32_t i = 0U; i < VFS_MAX_OPEN_FILES; i++) {
        vfs_file_t *file = &g_files[i];
        if (!file->used) continue;
        kprintf("[VFS:diag] fd=%u flags=%x off=%u size=%u dirty=%u path=%s\n",
                i, file->flags, file->offset,
                file->cache_loaded ? file->cache_size :
                    (file->source == VFS_FILE_FAT
                        ? file->entry.fat.entry.size : file->entry.iso.size),
                file->cache_dirty ? 1U : 0U, file->path);
    }
}

static int vfs_open_failure(const char *path, uint32_t flags,
                            const char *reason) {
    g_vfs_last_error = reason ? reason : "unknown";
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[VFS:diag] open FAIL path=%s flags=%x reason=%s "
            "open=%u/%u fat=%s\n",
            path ? path : "(null)", flags, g_vfs_last_error,
            vfs_open_file_count(), VFS_MAX_OPEN_FILES, fat_last_error());
#else
    (void)path;
    (void)flags;
#endif
    return -1;
}


static bool vfs_select_fat(const char *full, fat_fs_t *fs,
                           const char **inner);
static bool path_component_equals_ci(const char *a, const char *b);

static uint32_t vfs_access_mode(uint32_t flags) {
    return flags & VFS_O_RDWR;
}

static bool vfs_flags_readable(uint32_t flags) {
    uint32_t mode = vfs_access_mode(flags);
    return mode == VFS_O_RDONLY || mode == VFS_O_RDWR;
}

static bool vfs_flags_writable(uint32_t flags) {
    uint32_t mode = vfs_access_mode(flags);
    return mode == VFS_O_WRONLY || mode == VFS_O_RDWR;
}

static void vfs_drop_cache(vfs_file_t *file) {
    if (!file) return;
    if (file->cache_dirty) g_vfs_dirty_files--;
    if (file->cache) kfree(file->cache);
    file->cache = NULL;
    file->cache_size = 0U;
    file->cache_capacity = 0U;
    file->cache_persisted_size = 0U;
    file->cache_loaded = false;
    file->cache_dirty = false;
    file->cache_append_only = false;
}

static void vfs_drop_dir_cache(vfs_file_t *file) {
    if (!file) return;
    if (file->dir_cache) kfree(file->dir_cache);
    if (file->dir_metadata) kfree(file->dir_metadata);
    file->dir_metadata = NULL;
    file->dir_cache = NULL;
    file->dir_cache_count = 0U;
    file->dir_cache_loaded = false;
}

static bool vfs_load_fat_dir_cache(vfs_file_t *file) {
    uint32_t capacity = FAT_MAX_DIR_ENTRIES, count;
    if (!file || file->source != VFS_FILE_FAT ||
        !file->entry.fat.entry.is_directory) return false;
    if (file->dir_cache_loaded) return true;
    for (;;) {
        fat_dir_entry_t *entries = kmalloc(capacity * sizeof(*entries));
        fat_dir_metadata_t *metadata = kmalloc(capacity * sizeof(*metadata));
        if (!entries || !metadata ||
            !fat_list_dir_metadata(&file->entry.fat.fs, &file->entry.fat.entry,
                                   entries, metadata, capacity, &count)) {
            if (entries) kfree(entries);
            if (metadata) kfree(metadata);
            return false;
        }
        if (count < capacity || capacity >= VFS_LISTDIR_HARD_LIMIT) {
            if (!count) {
                kfree(entries); kfree(metadata);
                entries = NULL; metadata = NULL;
            } else if (count < capacity) {
                fat_dir_entry_t *small_entries = krealloc(entries, count * sizeof(*entries));
                fat_dir_metadata_t *small_metadata = krealloc(metadata, count * sizeof(*metadata));
                if (small_entries) entries = small_entries;
                if (small_metadata) metadata = small_metadata;
            }
            if (entries) (void)mm_set_allocation_owner(entries, 0U);
            if (metadata) (void)mm_set_allocation_owner(metadata, 0U);
            file->dir_cache = entries;
            file->dir_metadata = metadata;
            file->dir_cache_count = count;
            file->dir_cache_loaded = true;
            return true;
        }
        kfree(entries);
        kfree(metadata);
        capacity *= 2U;
        if (capacity > VFS_LISTDIR_HARD_LIMIT) capacity = VFS_LISTDIR_HARD_LIMIT;
    }
}

static bool vfs_cache_reserve(vfs_file_t *file, uint32_t needed,
                              uint32_t limit) {
    uint32_t capacity;
    uint8_t *grown;
    if (!file || needed > limit) return false;
    if (needed <= file->cache_capacity) return true;
    capacity = file->cache_capacity ? file->cache_capacity
                                    : VFS_CACHE_MIN_CAPACITY;
    while (capacity < needed) {
        if (capacity >= limit || capacity > limit / 2U) {
            capacity = limit;
            break;
        }
        capacity *= 2U;
    }
    if (capacity < needed) return false;
    grown = file->cache ? (uint8_t *)krealloc(file->cache, capacity)
                        : (uint8_t *)kmalloc(capacity);
    if (!grown) return false;
    /* Open descriptions survive the process that populated their cache. */
    (void)mm_set_allocation_owner(grown, 0U);
    (void)mm_set_allocation_subsystem(grown,MM_SUBSYSTEM_VFS);
    file->cache = grown;
    file->cache_capacity = capacity;
    return true;
}

static bool vfs_cache_load(vfs_file_t *file, bool required_for_write) {
    uint32_t size, got = 0U;
    uint32_t limit = required_for_write ? VFS_WRITE_CACHE_LIMIT
                                        : VFS_READ_CACHE_LIMIT;
    if (!file || file->source != VFS_FILE_FAT) return false;
    if (file->cache_loaded) return true;
    size = file->entry.fat.entry.size;
    if (size > limit || !vfs_cache_reserve(file, size, limit)) return false;
    if (size && (!fat_read_file_at(&file->entry.fat.fs,
                                   &file->entry.fat.entry, 0U,
                                   file->cache, size, &got) || got != size)) {
        vfs_drop_cache(file);
        return false;
    }
    file->cache_size = size;
    file->cache_persisted_size = size;
    file->cache_loaded = true;
    file->cache_dirty = false;
    file->cache_append_only = true;
    return true;
}

static void vfs_refresh_path_handles(const char *full, vfs_file_t *keep) {
    fat_fs_t fs;
    fat_dir_entry_t entry;
    const char *fat_path;
    if (!full || !vfs_select_fat(full, &fs, &fat_path) ||
        !fat_resolve_path(&fs, fat_path, &entry)) return;
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        vfs_file_t *file = &g_files[i];
        if (!file->used || file->unlinked || file->source != VFS_FILE_FAT ||
            !path_component_equals_ci(file->path, full)) continue;
        file->entry.fat.fs = fs;
        file->entry.fat.entry = entry;
        if (file != keep && file->cache_loaded && !file->cache_dirty)
            vfs_drop_cache(file);
    }
    for(vfs_page_reference_t *r=g_page_references;r;r=r->next) {
        vfs_file_t *file=&r->file;
        if(file->unlinked || file->source!=VFS_FILE_FAT ||
           !path_component_equals_ci(file->path,full))continue;
        file->entry.fat.fs=fs;file->entry.fat.entry=entry;
        if(file->cache_loaded)vfs_drop_cache(file);
    }
}

static bool vfs_flush_file(vfs_file_t *file) {
    fat_fs_t fs;
    const char *fat_path;
    if (!file || !file->used) {
        g_vfs_last_error = "invalid VFS file handle";
        return false;
    }
    vfs_shared_mapping_t *shared=vfs_shared_for_file(file);
    if(shared)return vfs_shared_sync(shared);
    if(file->unlinked)return true;
    if (!file->cache_dirty) return true;
    if (file->source != VFS_FILE_FAT || !file->cache_loaded) {
        g_vfs_last_error = "dirty cache is not a FAT file";
        return false;
    }
    if (!vfs_select_fat(file->path, &fs, &fat_path)) {
        g_vfs_last_error = "could not select FAT volume";
        return false;
    }
    bool stored = false;
    if (file->cache_append_only && file->cache_persisted_size > 0U &&
        file->cache_size > file->cache_persisted_size)
        stored = fat_append_path(&fs, fat_path, file->cache,
                                 file->cache_persisted_size, file->cache_size);
    if (!stored)
        stored = fat_write_path(&fs, fat_path, file->cache, file->cache_size);
    if (!stored) {
        g_vfs_last_error = fat_last_error();
        kprintf("[VFS:diag] flush FAIL path=%s size=%u reason=%s\n",
                file->path, file->cache_size, g_vfs_last_error);
        return false;
    }
    g_vfs_dirty_files--;
    file->cache_dirty = false;
    file->cache_persisted_size = file->cache_size;
    file->cache_append_only = true;
    vfs_private_invalidate(file->path);
    vfs_refresh_path_handles(file->path, file);
    file->entry.fat.entry.size = file->cache_size;
    g_vfs_last_error = "none";
    return true;
}

static bool vfs_sync_path(const char *full, vfs_file_t *except) {
    if (!full) return false;
    if (!g_vfs_dirty_files && !g_vfs_writeback_maps) return true;
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        vfs_file_t *file = &g_files[i];
        if (!file->used || file->unlinked || file == except || file->source != VFS_FILE_FAT ||
            !path_component_equals_ci(file->path, full)) continue;
        vfs_shared_mapping_t *shared=vfs_shared_for_file(file);
        if(!file->cache_dirty && (!shared || !shared->writable || shared->fd<0))continue;
        if (!vfs_flush_file(file)) return false;
    }
    return true;
}

static void vfs_copy_fat_fs(fat_fs_t *dst, const fat_fs_t *src) {
    if (!dst || !src) return;
    /* Keep this explicit.  A plain struct assignment (or kmemcpy >= 32 B)
     * becomes REP MOVSL on i386; FAT already avoids that path because stale
     * segment registers from Ring 3 can make it unsafe on real hardware. */
    dst->device = src->device;
    dst->volume_lba = src->volume_lba;
    dst->type = src->type;
    dst->bytes_per_sector = src->bytes_per_sector;
    dst->sectors_per_cluster = src->sectors_per_cluster;
    dst->reserved_sector_count = src->reserved_sector_count;
    dst->num_fats = src->num_fats;
    dst->root_entry_count = src->root_entry_count;
    dst->total_sectors = src->total_sectors;
    dst->fat_size_sectors = src->fat_size_sectors;
    dst->first_fat_sector = src->first_fat_sector;
    dst->first_data_sector = src->first_data_sector;
    dst->total_clusters = src->total_clusters;
    dst->root_dir_cluster = src->root_dir_cluster;
    dst->root_dir_sectors = src->root_dir_sectors;
    for (uint32_t i = 0U; i < sizeof(dst->volume_label); i++)
        dst->volume_label[i] = src->volume_label[i];
    for (uint32_t i = 0U; i < sizeof(dst->fs_name); i++)
        dst->fs_name[i] = src->fs_name[i];
}

static void vfs_clear_volume_cache(void) {
    kmutex_lock(&g_volume_lock);
    kmemset(g_volumes, 0, sizeof(g_volumes));
    kmemset(g_unmounted, 0, sizeof(g_unmounted));
    kmutex_unlock(&g_volume_lock);
}

static bool vfs_path_on_volume(const char *path, const char *name) {
    if (!path || !name || !name[0]) return false;
    uint32_t n = (uint32_t)kstrlen(name);
    return path[0] == '/' && kstrncmp(path + 1, name, n) == 0 &&
           (path[n + 1U] == '/' || path[n + 1U] == '\0');
}

bool vfs_volume_busy(const char *name) {
    if (!name) return false;
    const char *active = fat_get_active_name();
    if (active && kstrcmp(active, name) == 0) return true;
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; ++i)
        if (g_files[i].used && vfs_path_on_volume(g_files[i].path, name))
            return true;
    for (vfs_page_reference_t *r = g_page_references; r; r = r->next)
        if (vfs_path_on_volume(r->file.path, name)) return true;
    return false;
}

bool vfs_cdrom_busy(void) {
    if (g_vfs_cd_root) return true; /* installer/live CD root is never ejected */
    return vfs_volume_busy("CDROM");
}

void vfs_forget_volume(const char *name) {
    if (!name || !name[0]) return;
    kmutex_lock(&g_volume_lock);
    for (uint32_t i = 0; i < VFS_MAX_VOLUMES; i++) {
        if (!g_volumes[i].used ||
            kstrcmp(g_volumes[i].name, name) != 0) continue;
        kmemset(&g_volumes[i], 0, sizeof(g_volumes[i]));
    }
    kmutex_unlock(&g_volume_lock);
}

/* Resuelve /ata0/ruta, /ata1/ruta, /usb0/ruta, etc. sin cambiar el FAT
 * activo del sistema. Las rutas que no comienzan con un dispositivo siguen
 * apuntando al volumen de arranque por compatibilidad. */
static bool vfs_select_fat(const char *full, fat_fs_t *fs,
                           const char **inner) {
    char name[8];
    uint32_t length = 0;
    const char *active;
    const char *rest;
    block_device_t *device;
    fat_fs_t mounted;
    bool have_cached = false;
    int32_t free_slot = -1;

    if (!full || !fs || full[0] != '/') return false;
    rest = full + 1;
    while (rest[length] && rest[length] != '/' && length + 1 < sizeof(name))
        length++;
    if (length && (rest[length] == '\0' || rest[length] == '/')) {
        kmemset(name, 0, sizeof(name));
        kmemcpy(name, rest, length);
        active = fat_get_active_name();
        if (active && kstrcmp(active, name) == 0 && fat_get_current(fs)) {
            if (inner) *inner = rest[length] ? rest + length
                                             : g_vfs_root_path;
            return true;
        }

        /* Cache lookup is a short critical section.  The old implementation
         * mounted directly into an unused global slot and only set `used` at
         * the end. With many CPUs, two callers could therefore build the same
         * slot simultaneously and leave a half-ata0/half-ata1 fat_fs_t. */
        kmutex_lock(&g_volume_lock);
        for (uint32_t i = 0; i < VFS_MAX_VOLUMES; i++) {
            if (g_volumes[i].used &&
                kstrcmp(g_volumes[i].name, name) == 0) {
                vfs_copy_fat_fs(fs, &g_volumes[i].fs);
                have_cached = true;
                break;
            }
            if (!g_volumes[i].used && free_slot < 0) free_slot = (int32_t)i;
        }
        kmutex_unlock(&g_volume_lock);
        if (have_cached) {
            if (inner) *inner = rest[length] ? rest + length
                                             : g_vfs_root_path;
            return true;
        }

        for (uint32_t i=0;i<VFS_MAX_VOLUMES;i++)
            if(kstrcmp(g_unmounted[i],name)==0)return false;
        device = block_get(name);
        if (device) {
            /* Mount into a caller-local structure.  Cache insertion is only an
             * optimization: even if every cache slot is busy, /ataN remains
             * usable instead of becoming an inaccessible pseudo-directory. */
            kmemset(&mounted, 0, sizeof(mounted));
            if (!fat_mount_named(&mounted, name)) return false;

            kmutex_lock(&g_volume_lock);
            free_slot = -1;
            for (uint32_t i = 0; i < VFS_MAX_VOLUMES; i++) {
                if (g_volumes[i].used &&
                    kstrcmp(g_volumes[i].name, name) == 0) {
                    vfs_copy_fat_fs(&mounted, &g_volumes[i].fs);
                    free_slot = -2;
                    break;
                }
                if (!g_volumes[i].used && free_slot < 0)
                    free_slot = (int32_t)i;
            }
            if (free_slot >= 0) {
                vfs_volume_t *slot = &g_volumes[(uint32_t)free_slot];
                kmemset(slot, 0, sizeof(*slot));
                vfs_copy_fat_fs(&slot->fs, &mounted);
                kstrncpy(slot->name, name, sizeof(slot->name) - 1U);
                slot->name[sizeof(slot->name) - 1U] = '\0';
                slot->used = true;
            }
            kmutex_unlock(&g_volume_lock);

            vfs_copy_fat_fs(fs, &mounted);
            if (inner) *inner = rest[length] ? rest + length
                                             : g_vfs_root_path;
            return true;
        }
    }

    if (!fat_get_current(fs)) return false;
    if (inner) *inner = full;
    return true;
}

static bool vfs_path_has_block_prefix(const char *full) {
    char name[8];
    uint32_t length = 0U;
    if (!full || full[0] != '/' || !full[1]) return false;
    while (full[1U + length] && full[1U + length] != '/' &&
           length + 1U < sizeof(name)) {
        name[length] = full[1U + length];
        length++;
    }
    if (!length) return false;
    name[length] = '\0';
    return block_get(name) != NULL;
}

static bool vfs_iso_path(const char *full, const char **inner) {
    if (!full) return false;
    if (g_vfs_cd_root) {
        /* Durante Setup, / sigue siendo el ISO pero /ata0, /usb0 y /fd0
           deben seguir apuntando a volumenes FAT escribibles. */
        if (vfs_path_has_block_prefix(full)) return false;
        if (inner) *inner = full;
        return true;
    }
    if (kstrncmp(full, "/CDROM", 6) != 0) return false;
    if (full[6] != '\0' && full[6] != '/') return false;
    if (inner) *inner = full[6] ? full + 6 : "/";
    return true;
}

static void path_pop(char *path) {
    size_t len = kstrlen(path);
    if (len <= 1) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }
    if (path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
    while (len > 1 && path[len - 1] != '/') {
        path[len - 1] = '\0';
        len--;
    }
    if (len > 1) path[len - 1] = '\0';
}

static bool path_append_component(char *out, const char *component) {
    size_t len = kstrlen(out);
    size_t clen = kstrlen(component);
    if (clen == 0) return true;
    if (len + clen + 2 >= VFS_MAX_PATH) return false;
    if (len > 1) {
        out[len++] = '/';
        out[len] = '\0';
    }
    kstrcat(out, component);
    return true;
}

static bool path_component_equals_ci(const char *a, const char *b) {
    char ca;
    char cb;

    if (!a || !b) return false;
    while (*a && *b) {
        ca = *a;
        cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Rename updates descendants too, so cached inode images keep a usable
 * path for invalidation and shared writeback after a directory moves. */
static bool vfs_renamed_path(const char *path,const char *old_path,
                             const char *new_path,char *out) {
    size_t prefix=kstrlen(old_path), length=kstrlen(path);
    char head[VFS_MAX_PATH];
    if(length<prefix) {kstrcpy(out,path);return true;}
    kmemcpy(head,path,prefix);head[prefix]=0;
    if(!path_component_equals_ci(head,old_path) ||
       (path[prefix] && path[prefix]!='/')) {kstrcpy(out,path);return true;}
    if(kstrlen(new_path)+length-prefix>=VFS_MAX_PATH)return false;
    kstrcpy(out,new_path);kstrcat(out,path+prefix);return true;
}

static int vfs_readlink_raw(const char *full,char *target,uint32_t capacity) {
    fat_fs_t fs;fat_dir_entry_t entry;const char *inner;uint16_t mode;uint32_t got;
    if(vfs_iso_path(full,&inner) || !vfs_select_fat(full,&fs,&inner) ||
       !fat_resolve_path(&fs,inner,&entry)) return -2;
    if(!fat_get_posix_mode(&fs,inner,&mode) || (mode&0170000U)!=0120000U) return -22;
    if(entry.size>=capacity || !fat_read_file(&fs,&entry,target,capacity-1,&got)) return -5;
    target[got]=0;return got;
}

static bool vfs_normalize_internal(const char *path, char *out,bool follow_final) {
    W3P_SCOPE(W3P_VFS_NORMALIZE, 0U);
    char raw[VFS_MAX_PATH];
    char component[VFS_MAX_NAME];
    uint32_t rpos = 0;

    if (!path || !out || !path[0]) return false;
    kmemset(raw, 0, sizeof(raw));
    kmemset(out, 0, VFS_MAX_PATH);

    /* Los nombres de dispositivo son puntos de montaje virtuales reservados.
     * "cd ata1" debe significar /ata1 aunque otra tarea tenga otro cwd. */
    bool device_root = path[0] != '/';
    if (device_root) {
        for (const char *cursor = path; *cursor; cursor++) {
            if (*cursor == '/') { device_root = false; break; }
        }
    }
    if (device_root && block_get(path)) {
        raw[0] = '/';
        kstrncpy(raw + 1, path, sizeof(raw) - 2U);
        raw[sizeof(raw) - 1U] = '\0';
    } else if (path[0] == '/') {
        kstrncpy(raw, path, sizeof(raw) - 1);
    } else {
        size_t used;
        size_t path_length;
        const char *cwd = task_current_cwd();
        kstrncpy(raw, cwd ? cwd : "/", sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
        used = kstrlen(raw);
        path_length = kstrlen(path);
        if (used && raw[used - 1] != '/') {
            if (used + 1 >= sizeof(raw)) return false;
            raw[used++] = '/';
            raw[used] = '\0';
        }
        if (used + path_length >= sizeof(raw)) return false;
        kmemcpy(raw + used, path, path_length);
        raw[used + path_length] = '\0';
    }

    unsigned links=0;
restart:
    rpos=0;
    out[0] = '/';
    out[1] = '\0';

    while (raw[rpos]) {
        uint32_t cpos = 0;
        while (raw[rpos] == '/') rpos++;
        if (!raw[rpos]) break;

        kmemset(component, 0, sizeof(component));
        while (raw[rpos] && raw[rpos] != '/') {
            if (cpos + 1 >= sizeof(component)) return false;
            component[cpos++] = raw[rpos++];
        }
        component[cpos] = '\0';

        if (kstrcmp(component, ".") == 0) continue;
        if (kstrcmp(component, "..") == 0) {
            path_pop(out);
            continue;
        }
        if (path_component_equals_ci(component, "WALLPAPERS"))
            kstrcpy(component, "WALLPAPR");
        if (!path_append_component(out, component)) return false;
        if(follow_final || raw[rpos]) {
            char target[VFS_MAX_PATH],expanded[VFS_MAX_PATH];
            if(vfs_readlink_raw(out,target,sizeof(target))>=0) {
                if(++links>40) return false;
                if(target[0]=='/') expanded[0]=0;
                else {kstrcpy(expanded,out);path_pop(expanded);}
                unsigned used=kstrlen(expanded),n=kstrlen(target),tail=kstrlen(raw+rpos);
                if(used+n+tail+2>=sizeof(expanded)) return false;
                if(used && expanded[used-1]!='/')expanded[used++]='/';
                kmemcpy(expanded+used,target,n);used+=n;
                kstrcpy(expanded+used,raw+rpos);kstrcpy(raw,expanded);
                goto restart;
            }
        }
    }
    return true;
}
bool vfs_normalize_path(const char *path,char *out) {
    return vfs_normalize_internal(path,out,true);
}
bool vfs_normalize_nofollow(const char *path,char *out) {
    return vfs_normalize_internal(path,out,false);
}
int vfs_readlink(const char *path,char *target,uint32_t capacity) {
    char full[VFS_MAX_PATH];
    if(!vfs_normalize_nofollow(path,full))return -40;
    return vfs_readlink_raw(full,target,capacity);
}
int vfs_symlink(const char *target,const char *path) {
    char full[VFS_MAX_PATH];fat_fs_t fs;fat_dir_entry_t entry;const char *inner;
    if(!target || !target[0])return -2;
    if(!vfs_normalize_nofollow(path,full))return -40;
    if(vfs_iso_path(full,&inner))return -30;
    if(!vfs_select_fat(full,&fs,&inner))return -2;
    if(fat_resolve_path(&fs,inner,&entry))return -17;
    if(!fat_write_path(&fs,inner,target,kstrlen(target)))return -5;
    if(!fat_set_posix_mode(&fs,inner,0120777U)) {fat_remove_path(&fs,inner);return -5;}
    vfs_notify(full,NULL,VFS_NOTIFY_CREATE,false);
    return 0;
}

void vfs_init(void) {
    (void)task_set_current_cwd("/");
    kmemset(g_files, 0, sizeof(g_files));
    g_vfs_open_files = g_vfs_open_peak = 0;
    kmutex_init(&g_volume_lock);
    vfs_clear_volume_cache();
}

void vfs_use_cdrom_as_root(bool enabled) {
    g_vfs_cd_root = enabled;
    (void)task_set_current_cwd("/");
}

bool vfs_cdrom_is_root(void) {
    return g_vfs_cd_root;
}

bool vfs_attach_volume(const char *name) {
    char path[VFS_MAX_PATH];fat_fs_t fs;const char *inner;
    if(!name || !block_get(name) || kstrlen(name)>=8)return false;
    for(uint32_t i=0;i<VFS_MAX_VOLUMES;i++)
        if(kstrcmp(g_unmounted[i],name)==0)g_unmounted[i][0]=0;
    path[0]='/';kstrncpy(path+1,name,sizeof(path)-2);path[sizeof(path)-1]=0;
    return vfs_select_fat(path,&fs,&inner);
}
bool vfs_unmount_volume(const char *name) {
    if(!name || !block_get(name) || kstrlen(name)>=8 || vfs_volume_busy(name))return false;
    const char *cwd=task_current_cwd();
    if(vfs_path_on_volume(cwd,name))return false;
    uint32_t slot=VFS_MAX_VOLUMES;
    for(uint32_t i=0;i<VFS_MAX_VOLUMES;i++) {
        if(kstrcmp(g_unmounted[i],name)==0)return true;
        if(!g_unmounted[i][0])slot=i;
    }
    if(slot==VFS_MAX_VOLUMES || !vfs_sync_all())return false;
    vfs_forget_volume(name);kstrncpy(g_unmounted[slot],name,7);return true;
}

bool vfs_mount(const char *name) {
    if (!fat_set_active(name)) return false;
    vfs_clear_volume_cache();
    (void)task_set_current_cwd("/");
    return true;
}

bool vfs_mount_default(void) {
    kprintf("[VFS] entrando a vfs_mount_default\n");
    kprintf("[VFS] block_count=%u\n", block_count());

    if (!fat_mount_default()) {
        kprintf("[VFS] fat_mount_default devolvio false\n");
        return false;
    }
    vfs_clear_volume_cache();
    (void)task_set_current_cwd("/");
    /* BLES_WINE_TEMP_SFX_FIX_20260723
     * TEMP is a system directory, not an application side effect.
     * Old WinZip SFX stubs read TEMP from the environment and may
     * never call GetTempPathA before their first _lcreat. */
    if (vfs_mkdir("/TEMP"))
        kprintf("[VFS] /TEMP listo\n");
    else
        kprintf("[VFS] warning: no se pudo preparar /TEMP\n");
    return true;
}

bool vfs_get_fs_info(fat_fs_t *fs) {
    return fat_get_current(fs);
}

bool vfs_has_cdrom(void) {
    return iso9660_is_mounted();
}

const char *vfs_get_mount_name(void) {
    return fat_get_active_name();
}

const char *vfs_getcwd(void) {
    return task_current_cwd();
}

bool vfs_chdir(const char *path) {
    fat_fs_t fs;
    fat_dir_entry_t entry;
    char full[VFS_MAX_PATH];
    const char *iso_path;
    iso9660_entry_t iso_entry;
    const char *fat_path;

    if (!vfs_normalize_path(path, full)) {
        kprintf("[VFS:CWD] normalize FAIL path=%s cwd=%s\n",
                path ? path : "(null)", task_current_cwd());
        return false;
    }
    if (vfs_iso_path(full, &iso_path)) {
        if (!iso9660_resolve(iso_path, &iso_entry) ||
            !iso_entry.is_directory) {
            kprintf("[VFS:CWD] ISO resolve FAIL full=%s\n", full);
            return false;
        }
        return task_set_current_cwd(full);
    }
    if (!vfs_select_fat(full, &fs, &fat_path)) {
        kprintf("[VFS:CWD] select FAIL full=%s cwd=%s active=%s\n",
                full, task_current_cwd(), fat_get_active_name());
        return false;
    }
    if (vfs_fat_path_is_root(fat_path)) {
        bool ok = task_set_current_cwd(full);
#ifdef BLESK_WINE_VERBOSE_TRACE
        kprintf("[VFS:CWD] root %s inner=%s device=%s result=%u pid=%u\n",
                full, (!fat_path || !fat_path[0]) ? "<root>" : fat_path,
                fs.device ? fs.device->name : "?", ok ? 1U : 0U,
                task_current_pid());
#endif
        return ok;
    }
    if (!fat_resolve_path(&fs, fat_path, &entry)) {
        kprintf("[VFS:CWD] resolve FAIL full=%s inner=%s device=%s\n",
                full, fat_path, fs.device ? fs.device->name : "?");
        return false;
    }
    if (!entry.is_directory) {
        kprintf("[VFS:CWD] not-dir full=%s inner=%s\n", full, fat_path);
        return false;
    }
    return task_set_current_cwd(full);
}

bool vfs_listdir(const char *path, vfs_dir_entry_t *entries, uint32_t max_entries, uint32_t *count) {
    fat_fs_t fs;
    fat_dir_entry_t dir;
    fat_dir_entry_t *fat_entries;
    char full[VFS_MAX_PATH];
    uint32_t found = 0;
    const char *iso_path;
    iso9660_entry_t iso_dir;
    iso9660_entry_t *iso_entries;
    const char *fat_path;

if (!entries || !count) return false;

if (!path || !path[0])
    path = ".";

if (!vfs_normalize_path(path, full)) return false;

if (vfs_iso_path(full, &iso_path)) {
    if (max_entries == 0U) { *count = 0U; return true; }
    if (max_entries > VFS_LISTDIR_HARD_LIMIT)
        max_entries = VFS_LISTDIR_HARD_LIMIT;
    iso_entries = (iso9660_entry_t *)kmalloc(sizeof(*iso_entries) * max_entries);
    if (!iso_entries) return false;
    if (!iso9660_resolve(iso_path, &iso_dir) || !iso_dir.is_directory ||
        !iso9660_list(&iso_dir, iso_entries, max_entries, &found)) {
        kfree(iso_entries);
        return false;
    }
    for (uint32_t i = 0; i < found; i++) {
        kmemset(&entries[i], 0, sizeof(entries[i]));
        kstrncpy(entries[i].name, iso_entries[i].name,
                 sizeof(entries[i].name) - 1);
        entries[i].name[sizeof(entries[i].name) - 1] = '\0';
        entries[i].size = iso_entries[i].size;
        entries[i].type = iso_entries[i].is_directory
                        ? VFS_NODE_DIR : VFS_NODE_FILE;
        entries[i].attributes = iso_entries[i].is_directory
                              ? FAT_ATTR_DIRECTORY : FAT_ATTR_READ_ONLY;
    }
    kfree(iso_entries);
    *count = found;
    return true;
}

if (!vfs_select_fat(full, &fs, &fat_path)) {
    kprintf("[VFS:LIST] select FAIL full=%s cwd=%s active=%s pid=%u\n",
            full, task_current_cwd(), fat_get_active_name(), task_current_pid());
    return false;
}

if (max_entries == 0U) { *count = 0U; return true; }
if (max_entries > VFS_LISTDIR_HARD_LIMIT)
    max_entries = VFS_LISTDIR_HARD_LIMIT;
fat_entries = (fat_dir_entry_t *)kmalloc(sizeof(*fat_entries) * max_entries);
if (!fat_entries) return false;

if (vfs_fat_path_is_root(fat_path)) {
    if (!fat_list_root(&fs, fat_entries, max_entries, &found)) {
        kprintf("vfs: fat_list_root fallo (%s)\n", full);
        kfree(fat_entries);
        return false;
    }
    for (uint32_t i = 0; i < found; i++) {
        kmemset(&entries[i], 0, sizeof(entries[i]));
        kstrncpy(entries[i].name, fat_entries[i].name,
                 sizeof(entries[i].name) - 1);
        entries[i].size = fat_entries[i].size;
        entries[i].type = fat_entries[i].is_directory
                        ? VFS_NODE_DIR : VFS_NODE_FILE;
        entries[i].attributes = fat_entries[i].attributes;
    }
#ifdef BLESK_WINE_VERBOSE_TRACE
    if (kstrcmp(full, "/") != 0)
        kprintf("[VFS:LIST] root=%s device=%s count=%u pid=%u\n",
                full, fs.device ? fs.device->name : "?", found,
                task_current_pid());
#endif
    kfree(fat_entries);
    *count = found;
    return true;
}

if (!fat_resolve_path(&fs, fat_path, &dir)) {
    kfree(fat_entries);
    return false;
}

if (!dir.is_directory) {
    kprintf("vfs: no es directorio (%s)\n", full);
    kfree(fat_entries);
    return false;
}

if (!fat_list_dir(&fs, &dir, fat_entries, max_entries, &found)) {
    kprintf("vfs: fat_list_dir fallo\n");
    kfree(fat_entries);
    return false;
}

    for (uint32_t i = 0; i < found; i++) {
        kmemset(&entries[i], 0, sizeof(entries[i]));
        if (path_component_equals_ci(fat_entries[i].name, "WALLPAPR"))
            kstrcpy(entries[i].name, "WALLPAPERS");
        else
            kstrncpy(entries[i].name, fat_entries[i].name,
                     sizeof(entries[i].name) - 1);
        entries[i].name[sizeof(entries[i].name) - 1] = '\0';
        entries[i].size = fat_entries[i].size;
        entries[i].attributes = fat_entries[i].attributes;
        entries[i].type = fat_entries[i].is_directory ? VFS_NODE_DIR : VFS_NODE_FILE;
    }
    if (kstrcmp(fat_path, "/") == 0 && kstrcmp(full, "/") == 0 &&
        iso9660_is_mounted() &&
        found < max_entries) {
        kmemset(&entries[found], 0, sizeof(entries[found]));
        kstrcpy(entries[found].name, "CDROM");
        entries[found].type = VFS_NODE_DIR;
        entries[found].attributes = FAT_ATTR_DIRECTORY | FAT_ATTR_READ_ONLY;
        found++;
    }
    *count = found;
    kfree(fat_entries);
    return true;
}

int vfs_open(const char *path, uint32_t flags) {
    W3P_SCOPE(W3P_VFS_OPEN, 0U);
    fat_fs_t fs;
    fat_dir_entry_t entry;
    char full[VFS_MAX_PATH];
    const char *iso_path;
    iso9660_entry_t iso_entry;
    const char *fat_path;
    bool writable;

    g_vfs_last_error = "none";
    if (!vfs_normalize_path(path, full))
        return vfs_open_failure(path, flags, "path normalization failed");
    if ((flags & VFS_O_RDWR) == 0U) flags |= VFS_O_RDONLY;
    writable = vfs_flags_writable(flags);

    if (!vfs_sync_path(full, NULL))
        return vfs_open_failure(full, flags, "pending write flush failed");

    if (writable) {
        bool exists;
        if (vfs_iso_path(full, &iso_path))
            return vfs_open_failure(full, flags, "ISO9660 is read-only");
        if (!vfs_select_fat(full, &fs, &fat_path))
            return vfs_open_failure(full, flags, "FAT volume selection failed");
        exists = fat_resolve_path(&fs, fat_path, &entry);
        if (exists && entry.is_directory)
            return vfs_open_failure(full, flags, "target is a directory");
        if (!exists && !(flags & VFS_O_CREATE))
            return vfs_open_failure(full, flags, "file does not exist");
        if (exists && (flags & VFS_O_TRUNC)) {
            uint64_t device, inode;
            if (!vfs_identity(full,&device,&inode))
                return vfs_open_failure(full,flags,"mapped-file identity lookup failed");
            if(!vfs_page_resize_allowed(device,inode,0U))
                return vfs_open_failure(full,flags,"truncate of a paged file is unsupported");
            for (unsigned i=0;i<VFS_SHARED_MAP_SLOTS;i++)
                if (g_shared_maps[i].references && g_shared_maps[i].device==device &&
                    g_shared_maps[i].inode==inode)
                    return vfs_open_failure(full,flags,"truncate of a mapped file is unsupported");
            for (unsigned i=0;i<VFS_PRIVATE_MAP_SLOTS;i++)
                if (g_private_maps[i].references && g_private_maps[i].device==device &&
                    g_private_maps[i].inode==inode)
                    return vfs_open_failure(full,flags,"truncate of a mapped file is unsupported");
        }
        if (exists && (flags & VFS_O_TRUNC)) vfs_private_invalidate(full);
        if ((!exists || (flags & VFS_O_TRUNC)) &&
            !fat_write_path(&fs, fat_path, NULL, 0U))
            return vfs_open_failure(full, flags,
                                    "FAT could not create/truncate file");
        if (!fat_resolve_path(&fs, fat_path, &entry))
            return vfs_open_failure(full, flags,
                                    "created file could not be resolved");
        if(!exists)vfs_notify(full,NULL,VFS_NOTIFY_CREATE,false);
        else if(flags&VFS_O_TRUNC)vfs_notify(full,NULL,VFS_NOTIFY_MODIFY,false);

        for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
            if (!g_files[i].used) {
                vfs_file_t *file = &g_files[i];
                kmemset(file, 0, sizeof(*file));
                file->used = true;
                if (++g_vfs_open_files > g_vfs_open_peak) g_vfs_open_peak = g_vfs_open_files;
                file->references = 1U;
                file->flags = flags;
                file->source = VFS_FILE_FAT;
                kstrncpy(file->path, full, sizeof(file->path) - 1U);
                file->entry.fat.fs = fs;
                file->entry.fat.entry = entry;
                if (!exists || (flags & VFS_O_TRUNC)) {
                    file->cache_loaded = true;
                    file->cache_size = 0U;
                    file->cache_append_only = true;
                }
                g_vfs_last_error = "none";
                vfs_notify(full,NULL,VFS_NOTIFY_OPEN,false);
                return i;
            }
        }
        vfs_dump_open_files();
        return vfs_open_failure(full, flags, "VFS file table exhausted");
    }

    if (vfs_iso_path(full, &iso_path)) {
        if (!iso9660_resolve(iso_path, &iso_entry) ||
            (iso_entry.is_directory && !(flags & VFS_O_DIRECTORY)))
            return vfs_open_failure(full, flags,
                                    "ISO file not found or is a directory");
        for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
            if (!g_files[i].used) {
                vfs_file_t *file = &g_files[i];
                kmemset(file, 0, sizeof(*file));
                file->used = true;
                if (++g_vfs_open_files > g_vfs_open_peak) g_vfs_open_peak = g_vfs_open_files;
                file->references = 1U;
                file->flags = flags;
                file->source = VFS_FILE_ISO9660;
                file->entry.iso = iso_entry;
                kstrncpy(file->path, full, sizeof(file->path) - 1U);
                g_vfs_last_error = "none";
                vfs_notify(full,NULL,VFS_NOTIFY_OPEN,iso_entry.is_directory);
                return i;
            }
        }
        vfs_dump_open_files();
        return vfs_open_failure(full, flags, "VFS file table exhausted");
    }

    if (!vfs_select_fat(full, &fs, &fat_path))
        return vfs_open_failure(full, flags, "FAT volume selection failed");
    if (!fat_resolve_path(&fs, fat_path, &entry))
        return vfs_open_failure(full, flags, "file not found");
    if (entry.is_directory && !(flags & VFS_O_DIRECTORY))
        return vfs_open_failure(full, flags, "target is a directory");

    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        if (!g_files[i].used) {
            vfs_file_t *file = &g_files[i];
            kmemset(file, 0, sizeof(*file));
            file->used = true;
            if (++g_vfs_open_files > g_vfs_open_peak) g_vfs_open_peak = g_vfs_open_files;
            file->references = 1U;
            file->flags = flags;
            file->source = VFS_FILE_FAT;
            file->entry.fat.entry = entry;
            file->entry.fat.fs = fs;
            kstrncpy(file->path, full, sizeof(file->path) - 1U);
            g_vfs_last_error = "none";
            vfs_notify(full,NULL,VFS_NOTIFY_OPEN,entry.is_directory);
            return i;
        }
    }
    vfs_dump_open_files();
    return vfs_open_failure(full, flags, "VFS file table exhausted");
}

int32_t vfs_status_flags(int fd) {
    if(fd<0 || fd>=VFS_MAX_OPEN_FILES || !g_files[fd].used)return -1;
    return ((g_files[fd].flags&VFS_O_RDWR)-1)|(g_files[fd].flags&0x800)|
        (vfs_is_directory(fd)?0x10000:0);
}
bool vfs_set_status_flags(int fd,uint32_t flags) {
    if(fd<0 || fd>=VFS_MAX_OPEN_FILES || !g_files[fd].used)return false;
    g_files[fd].flags=(g_files[fd].flags&~0x800U)|(flags&0x800);return true;
}

static int vfs_read_internal(int fd, void *buffer, uint32_t size) {
    W3P_SCOPE(W3P_VFS_READ, size);
    vfs_file_t *file;
    uint32_t bytes_read = 0U;
    uint32_t available;
    bool must_cache;

    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !buffer) return -1;
    file = &g_files[fd];
    if (!file->used || !vfs_flags_readable(file->flags)) return -1;
    if (vfs_is_directory(fd)) return -21;
    if (!size) return 0;
    vfs_shared_mapping_t *shared=vfs_shared_for_file(file);
    if (!shared && g_private_entries) {
        if (!vfs_sync_path(file->path,file) ||
            (file->cache_dirty && !vfs_flush_file(file))) return -1;
        shared=vfs_private_find(file,true);
        if (shared) shared->last_used=++g_private_age;
    }
    if(shared) {
        if(file->offset>=shared->size)return 0;
        if(size>shared->size-file->offset)size=shared->size-file->offset;
        kmemcpy(buffer,(void *)(shared->backing+file->offset),size);
        file->offset+=size;return (int)size;
    }

    if (file->source == VFS_FILE_ISO9660) {
        if (!iso9660_read_at(&file->entry.iso, file->offset,
                             buffer, size, &bytes_read)) return -1;
        file->offset += bytes_read;
        return (int)bytes_read;
    }

    if (!vfs_sync_path(file->path, file)) return -1;
    must_cache = vfs_flags_writable(file->flags) &&
                 !vfs_stream_file(file, file->entry.fat.entry.size);
    if (file->cache_loaded || must_cache ||
        (!compat_mode_is_low_memory() &&
         !vfs_stream_file(file, file->entry.fat.entry.size) &&
         file->entry.fat.entry.size <= VFS_READ_CACHE_LIMIT &&
         (file->entry.fat.entry.size <= 64U * 1024U || size >= 64U * 1024U))) {
        /* Architecture probes read only seven ELF bytes, often reopening
         * the same DLL. Avoid loading megabytes for each tiny probe; bulk
         * reads and small files still use the existing full-file cache. */
        if (!file->cache_loaded && !vfs_cache_load(file, must_cache)) {
            if (must_cache) return -1;
        } else {
            if (file->offset >= file->cache_size) return 0;
            available = file->cache_size - file->offset;
            if (size > available) size = available;
            kmemcpy(buffer, file->cache + file->offset, size);
            file->offset += size;
            return (int)size;
        }
    }

    if (!fat_read_file_at(&file->entry.fat.fs, &file->entry.fat.entry,
                          file->offset, buffer, size, &bytes_read)) return -1;
    file->offset += bytes_read;
    return (int)bytes_read;
}

int vfs_read(int fd,void *buffer,uint32_t size) {
    int result=vfs_read_internal(fd,buffer,size);
    if(result>0 && !g_files[fd].unlinked)
        vfs_notify(g_files[fd].path,NULL,VFS_NOTIFY_ACCESS,false);
    return result;
}

static bool vfs_stream_file(const vfs_file_t *file, uint32_t size) {
    /* Wine's registry is written in small stdio chunks. Streaming as soon
     * as it crosses 1 MiB journals every subsequent chunk separately. Keep
     * a bounded full-file cache on hosts with room; close/fsync still flush
     * durably, and small-memory machines retain the original threshold. */
    uint32_t threshold=mm_total_bytes()>=128U*1024U*1024U
        ? 8U*VFS_STREAM_THRESHOLD : VFS_STREAM_THRESHOLD;
    return file && !file->unlinked && file->source == VFS_FILE_FAT &&
           file->entry.fat.fs.type == FAT_TYPE_BFS4 &&
           (size > threshold || file->entry.fat.entry.size > threshold);
}

/* Flush dirty aliases before bypassing the full-file cache. Refresh all open
 * descriptions even after an I/O failure, which may have changed some bytes. */
static int vfs_stream_write(vfs_file_t *file, const void *buffer, uint32_t size) {
    fat_fs_t fs;
    const char *path;
    if (!vfs_sync_path(file->path, file) || !vfs_flush_file(file) ||
        !vfs_select_fat(file->path, &fs, &path)) return -1;
    vfs_drop_cache(file);
    vfs_private_invalidate(file->path);
    bool ok = bfs4_write_at(&fs, path, file->offset, buffer, size);
    vfs_refresh_path_handles(file->path, NULL);
    if (!ok) return -1;
    file->offset += size;
    return (int)size;
}

static int vfs_write_internal(int fd, const void *buffer, uint32_t size) {
    W3P_SCOPE(W3P_VFS_WRITE, size);
    vfs_file_t *file;
    uint32_t end;

    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !g_files[fd].used) return -1;
    file = &g_files[fd];
    if (!vfs_flags_writable(file->flags) ||
        file->source != VFS_FILE_FAT || (size && !buffer)) return -1;
    if (!size) return 0;
    if (file->offset > VFS_FILE_POSITION_MAX || size > VFS_FILE_POSITION_MAX - file->offset)
        return -27; /* EFBIG: current seek/stat contract uses signed 32-bit sizes. */
    vfs_shared_mapping_t *shared=vfs_shared_for_file(file);
    if(shared) {
        if(!vfs_mapping_attach(shared,fd,true))return -1;
        if(file->offset>shared->length || size>shared->length-file->offset)return -27;
        end=file->offset+size;
        if(file->offset>shared->size)kmemset((void *)(shared->backing+shared->size),0,file->offset-shared->size);
        kmemcpy((void *)(shared->backing+file->offset),buffer,size);
        if(end>shared->size)shared->size=end;
        shared->writable=true;file->offset=end;return (int)size;
    }
    end = file->offset + size;
    if (vfs_stream_file(file, end)) return vfs_stream_write(file, buffer, size);
    if (!vfs_sync_path(file->path, file)) return -1;
    if (!vfs_cache_load(file, true))
        return file->entry.fat.fs.type==FAT_TYPE_BFS4
            ? vfs_stream_write(file,buffer,size) : -1;

    end = file->offset + size;
    if (end < file->offset || end > VFS_WRITE_CACHE_LIMIT) return -1;
    if (!vfs_cache_reserve(file, end, VFS_WRITE_CACHE_LIMIT))
        return file->entry.fat.fs.type==FAT_TYPE_BFS4
            ? vfs_stream_write(file,buffer,size) : -1;
    if (file->offset > file->cache_size)
        kmemset(file->cache + file->cache_size, 0,
                file->offset - file->cache_size);
    if (file->offset < file->cache_persisted_size)
        file->cache_append_only = false;
    kmemcpy(file->cache + file->offset, buffer, size);
    file->offset = end;
    if (end > file->cache_size) file->cache_size = end;
    file->entry.fat.entry.size = file->cache_size;
    if (!file->cache_dirty) g_vfs_dirty_files++;
    file->cache_dirty = true;
    return (int)size;
}

int vfs_write(int fd,const void *buffer,uint32_t size) {
    uint32_t offset=(fd>=0 && fd<VFS_MAX_OPEN_FILES && g_files[fd].used)?g_files[fd].offset:0;
    /* An orphan's resident snapshot must grow before modifying its live FD.
     * A failure leaves both views unchanged and never needs disk allocation. */
    if(fd>=0 && fd<VFS_MAX_OPEN_FILES && g_files[fd].used && size && offset+size>=offset)
        for(vfs_page_reference_t *r=g_page_references;r;r=r->next)
            if(r->file.unlinked && r->file.device==g_files[fd].device &&
               r->file.inode==g_files[fd].inode && offset+size>r->file.cache_size) {
                if(!vfs_cache_reserve(&r->file,offset+size,VFS_WRITE_CACHE_LIMIT))return -1;
                (void)mm_set_allocation_owner(r->file.cache,0U);
                (void)mm_set_allocation_subsystem(r->file.cache,MM_SUBSYSTEM_VFS);
            }
    int result=vfs_write_internal(fd,buffer,size);
    if(result>0) {
        vfs_page_written(&g_files[fd],offset,buffer,(uint32_t)result);
        if(!g_files[fd].unlinked)vfs_notify(g_files[fd].path,NULL,VFS_NOTIFY_MODIFY,false);
    }
    return result;
}

bool vfs_flush(int fd) {
    W3P_SCOPE(W3P_VFS_FLUSH, 0U);
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !g_files[fd].used) return false;
    return vfs_flush_file(&g_files[fd]);
}

bool vfs_sync_all(void) {
    fat_fs_t fs;
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; ++i)
        if (g_files[i].used && g_files[i].cache_dirty &&
            !vfs_flush_file(&g_files[i])) return false;
    for (uint32_t i = 0; i < VFS_SHARED_MAP_SLOTS; ++i)
        if (g_shared_maps[i].references && !vfs_shared_sync(&g_shared_maps[i])) return false;
    for (uint32_t i = 0; i < VFS_PRIVATE_MAP_SLOTS; ++i)
        if (g_private_maps[i].references && !vfs_shared_sync(&g_private_maps[i])) return false;
    if (!fat_get_current(&fs)) return true;
    return fs.type != FAT_TYPE_BFS4 || bfs4_sync(&fs);
}

bool vfs_close(int fd) {
    W3P_SCOPE(W3P_VFS_CLOSE, 0U);
    bool ok;
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !g_files[fd].used) return false;
    if (g_files[fd].references > 1U) {
        g_files[fd].references--;
        return true;
    }
    ok = vfs_flush_file(&g_files[fd]);
    if(!g_files[fd].unlinked)
        vfs_notify(g_files[fd].path,NULL,
                   vfs_flags_writable(g_files[fd].flags)?VFS_NOTIFY_CLOSE_WRITE:VFS_NOTIFY_CLOSE_READ,
                   vfs_is_directory(fd));
    vfs_drop_cache(&g_files[fd]);
    vfs_drop_dir_cache(&g_files[fd]);
    kmemset(&g_files[fd], 0, sizeof(g_files[fd]));
    g_vfs_open_files--;
    return ok;
}

bool vfs_retain(int fd) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !g_files[fd].used ||
        g_files[fd].references == 0xFFFFFFFFU) return false;
    g_files[fd].references++;
    return true;
}

vfs_page_reference_t *vfs_page_reference_acquire(int fd) {
    uint64_t device,inode;
    if(fd<0 || fd>=VFS_MAX_OPEN_FILES || !g_files[fd].used ||
       !vfs_flags_readable(g_files[fd].flags) || vfs_is_directory(fd) ||
       g_files[fd].unlinked || !vfs_fd_identity(fd,&device,&inode))return NULL;
    vfs_file_t *file=&g_files[fd];
    if(!vfs_sync_path(file->path,NULL))return NULL;
    for(vfs_page_reference_t *r=g_page_references;r;r=r->next)
        if(r->file.device==device && r->file.inode==inode && !r->file.unlinked) {
            if(r->references==0xffffffffU)return NULL;
            r->references++;return r;
        }
    vfs_page_reference_t *r=kzalloc(sizeof(*r));
    if(!r)return NULL;
    (void)mm_set_allocation_owner(r,0U);
    (void)mm_set_allocation_subsystem(r,MM_SUBSYSTEM_VFS);
    r->references=1;
    r->file.used=true;r->file.references=1;r->file.flags=VFS_O_RDONLY;
    r->file.source=file->source;r->file.device=device;r->file.inode=inode;
    r->file.entry=file->entry;
    kstrncpy(r->file.path,file->path,sizeof(r->file.path)-1);
    r->next=g_page_references;g_page_references=r;return r;
}
void vfs_page_reference_retain(vfs_page_reference_t *r) {
    if(r && r->references && r->references<0xffffffffU)r->references++;
}
void vfs_page_reference_release(vfs_page_reference_t *r) {
    if(!r || !r->references)return;
    if(--r->references)return;
    vfs_page_reference_t **link=&g_page_references;
    while(*link && *link!=r)link=&(*link)->next;
    if(*link)*link=r->next;
    vfs_drop_cache(&r->file);kfree(r);
}
uint32_t vfs_page_reference_size(vfs_page_reference_t *r) {
    if(!r || !r->references)return 0;
    return r->file.source==VFS_FILE_ISO9660?r->file.entry.iso.size:
        (r->file.cache_loaded?r->file.cache_size:r->file.entry.fat.entry.size);
}
bool vfs_page_reference_identity(vfs_page_reference_t *r,uint64_t *device,uint64_t *inode) {
    if(!r || !r->references || !device || !inode)return false;
    *device=r->file.device;*inode=r->file.inode;return true;
}
bool vfs_page_reference_read(vfs_page_reference_t *r,uint32_t offset,
                             void *buffer,uint32_t length,uint32_t *read_bytes) {
    if(!r || !r->references || !buffer || !read_bytes || offset+length<offset)return false;
    *read_bytes=0;
    vfs_file_t *file=&r->file;
    if(!file->unlinked && !vfs_sync_path(file->path,NULL))return false;
    /* Entry snapshots are refreshed by flush/write/rename operations under
     * VFS. Rewalking every DLL's directory on each 4 KiB fault adds needless
     * synchronous I/O and defeats demand paging startup performance. */
    uint32_t size=vfs_page_reference_size(r);
    if(offset>=size || !length)return true;
    if(length>size-offset)length=size-offset;
    vfs_shared_mapping_t *shared=vfs_shared_for_file(file);
    if(shared) {
        if(offset>=shared->size)return true;
        if(length>shared->size-offset)length=shared->size-offset;
        kmemcpy(buffer,(void *)(uintptr_t)(shared->backing+offset),length);
        *read_bytes=length;return true;
    }
    if(file->cache_loaded) {
        kmemcpy(buffer,file->cache+offset,length);*read_bytes=length;return true;
    }
    if(file->source==VFS_FILE_ISO9660)
        return iso9660_read_at(&file->entry.iso,offset,buffer,length,read_bytes);
    return fat_read_file_at(&file->entry.fat.fs,&file->entry.fat.entry,offset,buffer,length,read_bytes);
}
static bool vfs_page_snapshot(const char *path) {
    for(vfs_page_reference_t *r=g_page_references;r;r=r->next) {
        vfs_file_t *file=&r->file;
        if(file->unlinked || file->source!=VFS_FILE_FAT ||
           !path_component_equals_ci(file->path,path))continue;
        if(!vfs_cache_load(file,false))return false;
        if(file->cache) {
            (void)mm_set_allocation_owner(file->cache,0U);
            (void)mm_set_allocation_subsystem(file->cache,MM_SUBSYSTEM_VFS);
        }
    }
    return true;
}

int32_t vfs_seek(int fd, int32_t offset, uint32_t origin) {
    vfs_file_t *file;
    int64_t base;
    int64_t target;
    uint32_t size;
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !g_files[fd].used) return -1;
    file = &g_files[fd];
    size=(uint32_t)vfs_size(fd);
    if (origin == 0U) base = 0;
    else if (origin == 1U) base = file->offset;
    else if (origin == 2U) base = size;
    else return -1;
    target = base + offset;
    if (target < 0 || target > 0x7FFFFFFFLL) return -1;
    if (!target && vfs_is_directory(fd)) vfs_drop_dir_cache(file);
    file->offset = (uint32_t)target;
    return (int32_t)file->offset;
}

int32_t vfs_tell(int fd) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !g_files[fd].used) return -1;
    return (int32_t)g_files[fd].offset;
}

int32_t vfs_size(int fd) {
    vfs_file_t *file;
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !g_files[fd].used) return -1;
    file = &g_files[fd];
    vfs_shared_mapping_t *shared=vfs_shared_for_file(file);
    if(shared)return (int32_t)shared->size;
    return (int32_t)(file->source == VFS_FILE_ISO9660 ? file->entry.iso.size
        : (file->cache_loaded ? file->cache_size
                              : file->entry.fat.entry.size));
}

static bool vfs_truncate_internal(int fd, uint32_t size) {
    vfs_file_t *file;
    uint32_t old_size;
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !g_files[fd].used) return false;
    file = &g_files[fd];
    uint64_t device,inode;
    if(!vfs_fd_identity(fd,&device,&inode) || !vfs_page_resize_allowed(device,inode,size))return false;
    /* Cambiar las paginas accesibles de todos los aliases requiere invalidar
     * PTEs y SIGBUS. Rechazar ese caso hasta implementar dicha operacion. */
    vfs_shared_mapping_t *shared=vfs_shared_for_file(file);
    if(shared)return shared->size==size && vfs_flags_writable(file->flags);
    if (file->source != VFS_FILE_FAT || !vfs_flags_writable(file->flags) ||
        size > VFS_FILE_POSITION_MAX) return false;
    if (vfs_stream_file(file, size)) {
        fat_fs_t fs;
        const char *path;
        if (!vfs_sync_path(file->path, file) || !vfs_flush_file(file) ||
            !vfs_select_fat(file->path, &fs, &path)) return false;
        vfs_drop_cache(file);
        vfs_private_invalidate(file->path);
        bool ok = bfs4_resize(&fs, path, size);
        vfs_refresh_path_handles(file->path, NULL);
        if (ok && file->offset > size) file->offset = size;
        return ok;
    }
    if (size > VFS_WRITE_CACHE_LIMIT || !vfs_sync_path(file->path, file) ||
        !vfs_cache_load(file, true)) return false;
    old_size = file->cache_size;
    if (!vfs_cache_reserve(file, size, VFS_WRITE_CACHE_LIMIT)) return false;
    if (size > old_size) kmemset(file->cache + old_size, 0, size - old_size);
    if (size < file->cache_persisted_size)
        file->cache_append_only = false;
    file->cache_size = size;
    file->entry.fat.entry.size = size;
    if (file->offset > size) file->offset = size;
    if (!file->cache_dirty) g_vfs_dirty_files++;
    file->cache_dirty = true;
    return true;
}

bool vfs_truncate(int fd,uint32_t size) {
    int32_t before=vfs_size(fd);
    bool ok=vfs_truncate_internal(fd,size);
    if(ok && before!=(int32_t)size && !g_files[fd].unlinked)
        vfs_notify(g_files[fd].path,NULL,VFS_NOTIFY_MODIFY,false);
    return ok;
}

bool vfs_stat(const char *path, vfs_dir_entry_t *out) {
    W3P_SCOPE(W3P_VFS_STAT, 0U);
    fat_fs_t fs;
    fat_dir_entry_t fat_entry;
    iso9660_entry_t iso_entry;
    char full[VFS_MAX_PATH];
    const char *inner;
    if (!path || !out || !vfs_normalize_path(path, full) ||
        !vfs_sync_path(full, NULL)) return false;
    kmemset(out, 0, sizeof(*out));
    if (vfs_iso_path(full, &inner)) {
        if (!iso9660_resolve(inner, &iso_entry)) return false;
        kstrncpy(out->name, iso_entry.name, sizeof(out->name) - 1U);
        out->size = iso_entry.size;
        out->type = iso_entry.is_directory ? VFS_NODE_DIR : VFS_NODE_FILE;
        out->attributes = iso_entry.is_directory ? FAT_ATTR_DIRECTORY : FAT_ATTR_READ_ONLY;
        return true;
    }
    if (!vfs_select_fat(full, &fs, &inner)) return false;
    if (vfs_fat_path_is_root(inner)) {
        fat_entry.name[0] = '/';
        fat_entry.name[1] = '\0';
        fat_entry.size = 0U;
        fat_entry.is_directory = true;
        fat_entry.attributes = FAT_ATTR_DIRECTORY;
        fat_entry.first_cluster = fs.type == FAT_TYPE_FAT32
                                ? fs.root_dir_cluster : 0U;
    } else if (!fat_resolve_path(&fs, inner, &fat_entry)) return false;
    kstrncpy(out->name, fat_entry.name, sizeof(out->name) - 1U);
    out->size = fat_entry.size;
    out->type = fat_entry.is_directory ? VFS_NODE_DIR : VFS_NODE_FILE;
    out->attributes = fat_entry.attributes;
    return true;
}

const char *vfs_fd_path(int fd) {
    return fd>=0 && fd<VFS_MAX_OPEN_FILES && g_files[fd].used ? g_files[fd].path : NULL;
}
bool vfs_is_directory(int fd) {
    if (!vfs_fd_path(fd)) return false;
    return g_files[fd].source==VFS_FILE_FAT ? g_files[fd].entry.fat.entry.is_directory :
        g_files[fd].entry.iso.is_directory;
}

bool vfs_fat_dir_entry_at(int fd, uint32_t index, fat_dir_entry_t *entry,
                          bool *end) {
    vfs_file_t *file;
    if (!entry || !end || fd < 0 || fd >= VFS_MAX_OPEN_FILES ||
        !g_files[fd].used) return false;
    file = &g_files[fd];
    if (file->source != VFS_FILE_FAT || !file->entry.fat.entry.is_directory)
        return false;
    if (!vfs_load_fat_dir_cache(file)) return false;
    if (index >= file->dir_cache_count) {
        *end = true;
        kmemset(entry, 0, sizeof(*entry));
        return true;
    }
    *entry = file->dir_cache[index];
    *end = false;
    return true;
}
static bool vfs_disk_identity(const char *path,uint64_t *device,uint64_t *inode) {
    char full[VFS_MAX_PATH]; const char *inner; fat_fs_t fs; iso9660_entry_t iso;
    if (!vfs_normalize_nofollow(path,full)) return false;
    if (vfs_iso_path(full,&inner)) {
        if (!iso9660_resolve(inner,&iso)) return false;
        /* La interfaz del driver optico no expone la posicion del registro. */
        *device=0x8000; *inode=iso.extent+1U; return true;
    }
    if (!vfs_select_fat(full,&fs,&inner) || !fat_path_inode(&fs,inner,inode)) return false;
    unsigned index=0;
    /* Las vistas de particion y el disco padre representan el mismo disco. */
    for (; index < block_count(); index++) {
        block_device_t *candidate = block_at(index);
        if (candidate && candidate->driver_data == fs.device->driver_data &&
            candidate->read == fs.device->read) break;
    }
    *device=((uint64_t)(index+1)<<32)|(fs.device->base_lba+fs.volume_lba);
    return true;
}
static bool vfs_identity_from_position(uint64_t device, uint64_t position, uint64_t *inode) {
    vfs_identity_t *free_entry=NULL;
    for(vfs_identity_chunk *chunk=g_identities;chunk;chunk=chunk->next)
    for(unsigned i=0;i<VFS_IDENTITIES;i++) {
        vfs_identity_t *e=&chunk->entries[i];
        if (!e->inode) { if(!free_entry)free_entry=e;continue; }
        if(e->device==device && e->position==position) { *inode=e->inode;return true; }
    }
    if(!g_next_inode)return false;
    if(!free_entry) {
        vfs_identity_chunk *chunk=mm_alloc_guarded_stack(sizeof(*chunk),false,NULL,NULL);
        if(!chunk)return false;
        chunk->next=g_identities;g_identities=chunk;free_entry=&chunk->entries[0];
    }
    *free_entry=(vfs_identity_t){device,position,g_next_inode++};
    *inode=free_entry->inode;return true;
}
bool vfs_identity_nofollow(const char *path,uint64_t *device,uint64_t *inode) {
    uint64_t position;
    return vfs_disk_identity(path,device,&position) &&
        vfs_identity_from_position(*device,position,inode);
}

bool vfs_fat_directory(int fd, const fat_dir_entry_t **entries, uint32_t *count) {
    if (!vfs_fd_path(fd) || !vfs_load_fat_dir_cache(&g_files[fd])) return false;
    *entries = g_files[fd].dir_cache;
    *count = g_files[fd].dir_cache_count;
    return true;
}

bool vfs_fat_directory_identity(int fd, uint32_t index, uint64_t *inode, uint16_t *mode) {
    uint64_t device, directory_inode;
    if (!vfs_fd_path(fd) || !vfs_load_fat_dir_cache(&g_files[fd]) ||
        index >= g_files[fd].dir_cache_count ||
        !vfs_fd_identity(fd, &device, &directory_inode)) return false;
    *mode = g_files[fd].dir_metadata[index].mode;
    return vfs_identity_from_position(device, g_files[fd].dir_metadata[index].position, inode);
}

bool vfs_identity(const char *path,uint64_t *device,uint64_t *inode) {
    char full[VFS_MAX_PATH];
    return vfs_normalize_path(path,full) && vfs_identity_nofollow(full,device,inode);
}
bool vfs_fd_identity(int fd,uint64_t *device,uint64_t *inode) {
    if (!vfs_fd_path(fd)) return false;
    vfs_file_t *f=&g_files[fd];
    if (!f->inode && !vfs_identity(f->path,&f->device,&f->inode)) return false;
    *device=f->device; *inode=f->inode; return true;
}

static uint32_t vfs_private_budget(void) {
    if (mm_total_bytes() >= 1024U * 1024U * 1024U) return 128U * 1024U * 1024U;
    return mm_total_bytes() >= 256U * 1024U * 1024U ? 64U * 1024U * 1024U : 8U * 1024U * 1024U;
}

static vfs_shared_mapping_t *vfs_private_find(vfs_file_t *file, bool idle) {
    for(unsigned i=0;i<VFS_PRIVATE_MAP_SLOTS;i++) {
        vfs_shared_mapping_t *m=&g_private_maps[i];
        if(m->backing && (idle || m->references) &&
           (!m->stale || (m->unlinked && file->unlinked)) &&
           m->device==file->device && m->inode==file->inode)return m;
    }
    return NULL;
}

static vfs_shared_mapping_t *vfs_shared_for_file(vfs_file_t *file) {
    if(!file || !file->used || (!g_vfs_shared_file_maps && !g_private_entries))return NULL;
    if(!file->inode && !vfs_identity(file->path,&file->device,&file->inode))return NULL;
    for(unsigned i=0;i<VFS_SHARED_MAP_SLOTS;i++) {
        vfs_shared_mapping_t *m=&g_shared_maps[i];
        if(m->references && m->fd>=0 && m->device==file->device && m->inode==file->inode)return m;
    }
    return vfs_private_find(file,false);
}

static void vfs_private_drop(vfs_shared_mapping_t *m) {
    g_private_bytes-=m->length;g_private_entries--;
    mm_vm_frame_free(m->backing,m->length/4096U);
    kmemset(m,0,sizeof(*m));
}

static void vfs_private_invalidate(const char *path) {
    for(unsigned i=0;i<VFS_PRIVATE_MAP_SLOTS;i++) {
        vfs_shared_mapping_t *m=&g_private_maps[i];
        if(!m->backing || !path_component_equals_ci(m->path,path))continue;
        if(m->references)m->stale=true;
        else vfs_private_drop(m);
    }
}

static bool vfs_mapping_attach(vfs_shared_mapping_t *m,int fd,bool writable) {
    if(m->fd<0) {
        if(!vfs_retain(fd))return false;
        m->fd=fd;g_vfs_shared_file_maps++;
    }
    if(writable && !m->writable) {m->writable=true;g_vfs_writeback_maps++;}
    return true;
}

bool vfs_has_shared_mapping(int fd) {
    if(!vfs_fd_path(fd))return false;
    vfs_file_t *f=&g_files[fd];
    if(!vfs_fd_identity(fd,&f->device,&f->inode))return false;
    return vfs_shared_for_file(f)!=NULL || vfs_private_find(f,true)!=NULL;
}
bool vfs_shared_prefetch_room(void) {
    for(unsigned i=0;i<vfs_private_map_limit();i++)
        if(!g_private_maps[i].references)return true;
    return false;
}

vfs_shared_mapping_t *vfs_private_acquire(int fd) {
    uint64_t device,inode;
    if(!vfs_fd_path(fd) || vfs_is_directory(fd) || !vfs_fd_identity(fd,&device,&inode))return NULL;
    vfs_file_t *file=&g_files[fd];
    /* A hot idle image must not hide pending writes in another description. */
    if(!vfs_sync_path(file->path,file) || (file->cache_dirty && !vfs_flush_file(file)))return NULL;
    vfs_shared_mapping_t *m=vfs_shared_for_file(file);
    if(!m)m=vfs_private_find(file,true);
    if(m) {m->references++;m->last_used=++g_private_age;return m;}
    uint32_t size=(uint32_t)vfs_size(fd),length=(size+4095U)&~4095U;
    if(!length || length>VFS_PRIVATE_IMAGE_MAX_BYTES || length>vfs_private_budget())return NULL;
    for(;;) {
        vfs_shared_mapping_t *free_slot=NULL,*victim=NULL;
        for(unsigned i=0;i<vfs_private_map_limit();i++) {
            vfs_shared_mapping_t *candidate=&g_private_maps[i];
            if(!candidate->backing) {if(!free_slot)free_slot=candidate;continue;}
            if(!candidate->references && (!victim ||
               (uint32_t)(g_private_age-candidate->last_used)>(uint32_t)(g_private_age-victim->last_used)))victim=candidate;
        }
        if(free_slot && g_private_bytes<=vfs_private_budget()-length) {m=free_slot;break;}
        if(!victim)return NULL;
        vfs_private_drop(victim);g_private_evictions++;
    }
    uint32_t backing,got=0;
    if(!pager_frame_alloc(length/4096U,&backing))return NULL;
    bool ok;
    if(file->cache_loaded && file->cache_size==size) {
        kmemcpy((void *)backing,file->cache,size);got=size;ok=true;
    } else if(file->source==VFS_FILE_ISO9660)
        ok=iso9660_read_at(&file->entry.iso,0,(void *)backing,size,&got);
    else ok=fat_read_file_at(&file->entry.fat.fs,&file->entry.fat.entry,0,(void *)backing,size,&got);
    if(!ok || got!=size) {mm_vm_frame_free(backing,length/4096U);return NULL;}
    if(length>size)kmemset((void *)(backing+size),0,length-size);
    *m=(vfs_shared_mapping_t){.backing=backing,.length=length,.size=size,.references=1,
                             .device=device,.inode=inode,.fd=-1,.cached=true,
                             .unlinked=file->unlinked,.last_used=++g_private_age};
    kstrncpy(m->path,file->path,sizeof(m->path)-1);
    g_private_bytes+=length;g_private_entries++;
    return m;
}

static bool vfs_page_promote(vfs_shared_mapping_t *m) {
    for(vfs_page_reference_t *r=g_page_references;r;r=r->next)
        if(r->file.device==m->device && r->file.inode==m->inode &&
           !pager_file_promote(r,m))return false;
    return true;
}
vfs_shared_mapping_t *vfs_shared_acquire(int fd,uint32_t length) {
    vfs_shared_mapping_t *m=NULL;uint32_t size=length,backing;
    uint64_t device=0,inode=0;
    if(fd>=0) {
        if(!vfs_fd_path(fd) || vfs_is_directory(fd) || !vfs_fd_identity(fd,&device,&inode))return NULL;
        if(!vfs_sync_path(g_files[fd].path,&g_files[fd]) ||
           (g_files[fd].cache_dirty && !vfs_flush_file(&g_files[fd])))return NULL;
        m=vfs_shared_for_file(&g_files[fd]);
        if(!m)m=vfs_private_find(&g_files[fd],true);
        if(m) {
            if(!vfs_mapping_attach(m,fd,vfs_flags_writable(g_files[fd].flags)))return NULL;
            m->references++;m->last_used=++g_private_age;
            if(!vfs_page_promote(m)){(void)vfs_shared_release(m);return NULL;}
            return m;
        }
        if(!vfs_flush(fd))return NULL;
        size=(uint32_t)vfs_size(fd);length=(size+4095U)&~4095U;
    }
    if(!length || length>VFS_WRITE_CACHE_LIMIT)return NULL;
    for(unsigned i=0;i<vfs_shared_map_limit();i++)if(!g_shared_maps[i].references){m=&g_shared_maps[i];break;}
    if(!m || !pager_frame_alloc(length/4096U,&backing))return NULL;
    if(fd>=0) {
        int32_t position=vfs_tell(fd);vfs_seek(fd,0,0);
        int32_t got=vfs_read(fd,(void *)backing,size);vfs_seek(fd,position,0);
        if(got!=(int32_t)size || !vfs_retain(fd)){mm_vm_frame_free(backing,length/4096U);return NULL;}
    }
    *m=(vfs_shared_mapping_t){.backing=backing,.length=length,.size=size,.references=1,
                             .device=device,.inode=inode,.fd=fd,
                             .writable=fd<0 || vfs_flags_writable(g_files[fd].flags)};
    m->unlinked=fd>=0 && g_files[fd].unlinked;
    if(fd>=0) {g_vfs_shared_file_maps++;if(m->writable)g_vfs_writeback_maps++;}
    if(fd>=0 && !vfs_page_promote(m)){(void)vfs_shared_release(m);return NULL;}
    return m;
}
bool vfs_shared_sync(vfs_shared_mapping_t *m) {
    if(!m || !m->references)return false;
    if(m->fd<0 || !m->writable || m->unlinked)return true;
    vfs_file_t *f=&g_files[m->fd];fat_fs_t fs;const char *inner;
    if(!vfs_select_fat(f->path,&fs,&inner) || !fat_write_path(&fs,inner,(void *)m->backing,m->size))return false;
    for(unsigned i=0;i<VFS_MAX_OPEN_FILES;i++) {
        vfs_file_t *other=&g_files[i];
        if(other->used && other->inode==m->inode && other->device==m->device)vfs_drop_cache(other);
    }
    vfs_refresh_path_handles(f->path,NULL);return true;
}
bool vfs_shared_release(vfs_shared_mapping_t *m) {
    if(!m || !m->references)return false;
    if(m->references>1){m->references--;return true;}
    if(!vfs_shared_sync(m))return false;
    int fd=m->fd;
    if(fd>=0) {g_vfs_shared_file_maps--;if(m->writable)g_vfs_writeback_maps--;}
    if(m->cached && fd<0 && !m->unlinked && !m->stale) {
        m->references=0;m->last_used=++g_private_age;return true;
    }
    if(m->cached)vfs_private_drop(m);
    else {mm_vm_frame_free(m->backing,m->length/4096U);kmemset(m,0,sizeof(*m));}
    if(fd>=0)vfs_close(fd);
    return true;
}

void vfs_cache_perf(uint32_t *entries,uint32_t *bytes,uint32_t *evictions,
                    uint32_t *peak,uint32_t *handles,uint32_t *private_fds) {
    *entries=g_private_entries;*bytes=g_private_bytes;*evictions=g_private_evictions;
    *peak=g_vfs_open_peak;*handles=g_vfs_open_files;*private_fds=0;
    uint32_t demand_entries,demand_bytes,demand_released;
    pager_file_cache_perf(&demand_entries,&demand_bytes,&demand_released);
    *entries+=demand_entries;*bytes+=demand_bytes;*evictions+=demand_released;
    for(unsigned i=0;i<VFS_PRIVATE_MAP_SLOTS;i++)
        if(g_private_maps[i].backing && g_private_maps[i].fd>=0)(*private_fds)++;
}

bool vfs_mkdir_mode(const char *path, uint16_t mode) {
    fat_fs_t fs;
    fat_dir_entry_t existing;
    char full[VFS_MAX_PATH];
    const char *iso_path;
    const char *fat_path;
    if (!path || !vfs_normalize_path(path, full) ||
        vfs_iso_path(full, &iso_path)) return false;
    if (!vfs_select_fat(full, &fs, &fat_path)) return false;
    if (fat_resolve_path(&fs, fat_path, &existing))
        return existing.is_directory;
    if (!fat_mkdir_path(&fs, fat_path)) return false;
    if (!fat_set_posix_mode(&fs, fat_path, (uint16_t)(mode & 07777U))) {
        /* Do not report success for a directory whose requested security
         * mode could not be committed. The just-created directory is empty,
         * so rollback is safe and keeps mkdir atomic from userspace. */
        (void)fat_remove_path(&fs, fat_path);
        return false;
    }
    vfs_notify(full,NULL,VFS_NOTIFY_CREATE,true);
    return true;
}

bool vfs_mkdir(const char *path) {
    return vfs_mkdir_mode(path, 0755U);
}

bool vfs_get_mode(const char *path, uint16_t *mode) {
    fat_fs_t fs;
    fat_dir_entry_t fat_entry;
    iso9660_entry_t iso_entry;
    char full[VFS_MAX_PATH];
    const char *inner;
    uint16_t stored;
    if (!path || !mode || !vfs_normalize_path(path, full) ||
        !vfs_sync_path(full, NULL)) return false;
    if (vfs_iso_path(full, &inner)) {
        if (!iso9660_resolve(inner, &iso_entry)) return false;
        *mode = iso_entry.is_directory ? 0755U : 0644U;
        return true;
    }
    if (!vfs_select_fat(full, &fs, &inner)) return false;
    if (vfs_fat_path_is_root(inner)) {
        *mode = 0755U;
        return true;
    }
    if (!fat_resolve_path(&fs, inner, &fat_entry)) return false;
    if (fat_get_posix_mode(&fs, inner, &stored)) {
        *mode = (uint16_t)(stored & 07777U);
        return true;
    }
    /* Volumes/files created by older BlesKernOS builds contain no extension.
     * Preserve their historical observable permissions. */
    *mode = fat_entry.is_directory ? 0755U : 0644U;
    return true;
}

bool vfs_set_mode(const char *path, uint16_t mode) {
    fat_fs_t fs;
    fat_dir_entry_t entry;
    char full[VFS_MAX_PATH];
    const char *inner;
    const char *iso_path;
    if (!path || !vfs_normalize_path(path, full) ||
        !vfs_sync_path(full, NULL) || vfs_iso_path(full, &iso_path) ||
        !vfs_select_fat(full, &fs, &inner) || vfs_fat_path_is_root(inner) ||
        !fat_resolve_path(&fs, inner, &entry)) return false;
    bool ok=fat_set_posix_mode(&fs, inner, (uint16_t)(mode & 07777U));
    if(ok)vfs_notify(full,NULL,VFS_NOTIFY_ATTRIB,entry.is_directory);
    return ok;
}

bool vfs_fd_mode(int fd, uint16_t *mode) {
    const char *path = vfs_fd_path(fd);
    return path && vfs_get_mode(path, mode);
}

bool vfs_set_fd_mode(int fd, uint16_t mode) {
    const char *path = vfs_fd_path(fd);
    return path && vfs_set_mode(path, mode);
}

bool vfs_set_times(const char *path, bool set_access, uint64_t access_seconds,
                   bool set_modify, uint64_t modify_seconds) {
    fat_fs_t fs;
    fat_dir_entry_t entry;
    char full[VFS_MAX_PATH];
    const char *inner, *iso_path;
    if (!path || !vfs_normalize_path(path, full) ||
        !vfs_sync_path(full, NULL) || vfs_iso_path(full, &iso_path) ||
        !vfs_select_fat(full, &fs, &inner) || vfs_fat_path_is_root(inner) ||
        !fat_resolve_path(&fs, inner, &entry)) return false;
    bool ok=fat_set_times(&fs, inner, set_access, access_seconds,
                         set_modify, modify_seconds);
    if(ok && (set_access || set_modify))vfs_notify(full,NULL,VFS_NOTIFY_ATTRIB,entry.is_directory);
    return ok;
}

bool vfs_get_times(const char *path, uint32_t *access_seconds,
                   uint32_t *modify_seconds) {
    fat_fs_t fs;
    char full[VFS_MAX_PATH];
    const char *inner, *iso_path;
    if (!path || !access_seconds || !modify_seconds ||
        !vfs_normalize_path(path, full) || !vfs_sync_path(full, NULL) ||
        vfs_iso_path(full, &iso_path) ||
        !vfs_select_fat(full, &fs, &inner) || vfs_fat_path_is_root(inner))
        return false;
    return fat_get_times(&fs, inner, access_seconds, modify_seconds);
}

bool vfs_remove(const char *path) {
    fat_fs_t fs;
    char full[VFS_MAX_PATH];
    const char *iso_path;
    const char *fat_path;
    if (!path || !vfs_normalize_nofollow(path, full) ||
        !vfs_sync_path(full, NULL) || vfs_iso_path(full, &iso_path) ||
        !vfs_select_fat(full, &fs, &fat_path)) return false;
    uint64_t device=0,position=0;
    if (g_identities && !vfs_disk_identity(full,&device,&position))return false;
    fat_dir_entry_t removed;
    bool directory=g_vfs_change_sink && fat_resolve_path(&fs,fat_path,&removed) && removed.is_directory;
    /* Wine unlinks its temporary section files before mmap. Keep open
     * descriptions alive and never recreate their names when later flushed. */
    for(int i=0;i<VFS_MAX_OPEN_FILES;i++) {
        vfs_file_t *file=&g_files[i];
        if(!file->used || file->unlinked || file->source!=VFS_FILE_FAT ||
           file->entry.fat.entry.is_directory || !path_component_equals_ci(file->path,full))continue;
        if(!vfs_fd_identity(i,&file->device,&file->inode) ||
           (!vfs_shared_for_file(file) && !vfs_cache_load(file,vfs_flags_writable(file->flags))))return false;
    }
    if(!vfs_page_snapshot(full) || !fat_remove_path(&fs, fat_path))return false;
    for(vfs_page_reference_t *r=g_page_references;r;r=r->next)
        if(!r->file.unlinked && path_component_equals_ci(r->file.path,full))r->file.unlinked=true;
    for(unsigned i=0;i<VFS_PRIVATE_MAP_SLOTS;i++) {
        vfs_shared_mapping_t *m=&g_private_maps[i];
        if(m->backing && path_component_equals_ci(m->path,full))m->unlinked=true;
    }
    vfs_private_invalidate(full);
    for(int i=0;i<VFS_MAX_OPEN_FILES;i++)
        if(g_files[i].used && path_component_equals_ci(g_files[i].path,full))g_files[i].unlinked=true;
    for(unsigned i=0;i<VFS_SHARED_MAP_SLOTS;i++) {
        vfs_shared_mapping_t *m=&g_shared_maps[i];
        if(m->references && m->fd>=0 && !kstrcmp(g_files[m->fd].path,full))m->unlinked=true;
    }
    for(vfs_identity_chunk *chunk=g_identities;chunk;chunk=chunk->next)
        for(unsigned i=0;i<VFS_IDENTITIES;i++)
            if(chunk->entries[i].device==device && chunk->entries[i].position==position)
                chunk->entries[i].inode=0;
    vfs_notify(full,NULL,VFS_NOTIFY_DELETE,directory);
    return true;
}

/* -1 rejects a destination inside the source, including volume aliases;
 * 0 means different volumes, 1 means the same writable filesystem. */
int vfs_transfer_check(const char *source,const char *directory) {
    char a[VFS_MAX_PATH],b[VFS_MAX_PATH];
    const char *ai,*bi;fat_fs_t af,bf;
    if(!vfs_normalize_path(source,a) || !vfs_normalize_path(directory,b))return -1;
    bool acd=vfs_iso_path(a,&ai),bcd=vfs_iso_path(b,&bi);
    if(!acd && !vfs_select_fat(a,&af,&ai))return -1;
    if(!bcd && !vfs_select_fat(b,&bf,&bi))return -1;
    bool same=acd ? bcd : !bcd && af.device==bf.device && af.volume_lba==bf.volume_lba;
    if(!same)return 0;
    size_t n=kstrlen(ai),m=kstrlen(bi);
    if(m>=n) {
        bool prefix=true;
        for(size_t i=0;i<n;++i) {
            char x=ai[i],y=bi[i];
            if(x>='A' && x<='Z')x+='a'-'A';
            if(y>='A' && y<='Z')y+='a'-'A';
            if(x!=y){prefix=false;break;}
        }
        if(prefix && (n==1 || !bi[n] || bi[n]=='/'))return -1;
    }
    return 1;
}

bool vfs_rename(const char *old_path, const char *new_path) {
    fat_fs_t old_fs, new_fs;
    fat_dir_entry_t replaced_entry;
    char old_full[VFS_MAX_PATH], new_full[VFS_MAX_PATH];
    const char *iso_path;
    const char *old_inner, *new_inner;
    if (!old_path || !new_path ||
        !vfs_normalize_nofollow(old_path, old_full) ||
        !vfs_normalize_nofollow(new_path, new_full) ||
        !vfs_sync_path(old_full, NULL) || !vfs_sync_path(new_full, NULL) ||
        vfs_iso_path(old_full, &iso_path) || vfs_iso_path(new_full, &iso_path) ||
        !vfs_select_fat(old_full, &old_fs, &old_inner) ||
        !vfs_select_fat(new_full, &new_fs, &new_inner) ||
        old_fs.device != new_fs.device || old_fs.volume_lba != new_fs.volume_lba)
        return false;
    if (path_component_equals_ci(old_full, new_full)) return true;
    fat_dir_entry_t source_entry;
    bool directory=g_vfs_change_sink && fat_resolve_path(&old_fs,old_inner,&source_entry) && source_entry.is_directory;
    bool replacing = fat_resolve_path(&new_fs, new_inner, &replaced_entry);
    uint64_t replaced_device = 0, replaced_position = 0;
    if (replacing) {
        if (replaced_entry.is_directory) return false;
        if (g_identities &&
            !vfs_disk_identity(new_full, &replaced_device, &replaced_position)) return false;
        /* A replaced file may still have open Linux descriptions. Keep their
         * cached contents available, but prevent a later close from writing
         * the old name over the newly renamed file. */
        for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
            vfs_file_t *file = &g_files[i];
            if (!file->used || file->unlinked || file->source != VFS_FILE_FAT ||
                !path_component_equals_ci(file->path, new_full)) continue;
            if (!vfs_fd_identity(i, &file->device, &file->inode) ||
                (!vfs_shared_for_file(file) &&
                 !vfs_cache_load(file, vfs_flags_writable(file->flags)))) return false;
        }
    }
    uint64_t device=0,position=0,new_position;
    if(g_identities && !vfs_disk_identity(old_full,&device,&position))return false;
    char renamed[VFS_MAX_PATH];
    for(unsigned i=0;i<VFS_PRIVATE_MAP_SLOTS;i++)
        if(g_private_maps[i].backing && !g_private_maps[i].unlinked &&
           !vfs_renamed_path(g_private_maps[i].path,old_full,new_full,renamed))return false;
    for(unsigned i=0;i<VFS_MAX_OPEN_FILES;i++)
        if(g_files[i].used && !g_files[i].unlinked &&
           !vfs_renamed_path(g_files[i].path,old_full,new_full,renamed))return false;
    for(vfs_page_reference_t *r=g_page_references;r;r=r->next)
        if(!r->file.unlinked && !vfs_renamed_path(r->file.path,old_full,new_full,renamed))return false;
    if(replacing && !vfs_page_snapshot(new_full))return false;
    if(!fat_rename_path(&old_fs, old_inner, new_inner))return false;
    if (replacing) {
        vfs_notify(new_full,NULL,VFS_NOTIFY_DELETE_SELF,false);
        for(vfs_page_reference_t *r=g_page_references;r;r=r->next)
            if(!r->file.unlinked && path_component_equals_ci(r->file.path,new_full))r->file.unlinked=true;
        for(unsigned i=0;i<VFS_PRIVATE_MAP_SLOTS;i++)
            if(g_private_maps[i].backing && path_component_equals_ci(g_private_maps[i].path,new_full))
                g_private_maps[i].unlinked=true;
        vfs_private_invalidate(new_full);
        for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
            if (g_files[i].used && path_component_equals_ci(g_files[i].path, new_full))
                g_files[i].unlinked = true;
        for (unsigned i = 0; i < VFS_SHARED_MAP_SLOTS; i++) {
            vfs_shared_mapping_t *m = &g_shared_maps[i];
            if (m->references && m->fd >= 0 &&
                path_component_equals_ci(g_files[m->fd].path, new_full)) m->unlinked = true;
        }
        for (vfs_identity_chunk *chunk = g_identities; chunk; chunk = chunk->next)
            for (unsigned i = 0; i < VFS_IDENTITIES; i++)
                if (chunk->entries[i].device == replaced_device &&
                    chunk->entries[i].position == replaced_position)
                    chunk->entries[i].inode = 0;
    }
    if(g_identities && fat_path_inode(&old_fs,new_inner,&new_position))
        for(vfs_identity_chunk *chunk=g_identities;chunk;chunk=chunk->next)
        for(unsigned i=0;i<VFS_IDENTITIES;i++)
            if(chunk->entries[i].device==device && chunk->entries[i].position==position)
                chunk->entries[i].position=new_position;
    for(unsigned i=0;i<VFS_PRIVATE_MAP_SLOTS;i++) {
        vfs_shared_mapping_t *m=&g_private_maps[i];
        if(m->backing && !m->unlinked && vfs_renamed_path(m->path,old_full,new_full,renamed))
            kstrcpy(m->path,renamed);
    }
    for(unsigned i=0;i<VFS_MAX_OPEN_FILES;i++) {
        vfs_file_t *f=&g_files[i];
        if(f->used && !f->unlinked && vfs_renamed_path(f->path,old_full,new_full,renamed))
            kstrcpy(f->path,renamed);
    }
    for(vfs_page_reference_t *r=g_page_references;r;r=r->next)
        if(!r->file.unlinked && vfs_renamed_path(r->file.path,old_full,new_full,renamed))
            kstrcpy(r->file.path,renamed);
    vfs_notify(old_full,new_full,VFS_NOTIFY_MOVE_SELF,directory);
    return true;
}

bool vfs_get_space(uint64_t *total_bytes, uint64_t *free_bytes) {
    fat_fs_t fs;
    return fat_get_current(&fs) && fat_get_space(&fs, total_bytes, free_bytes);
}

/* Optical volumes are mounted independently of FAT/BFS. Never route their
 * geometry queries through fat_get_current(), which reports the system disk. */
bool vfs_get_optical_space(const char *path,uint64_t *total_bytes) {
    char full[VFS_MAX_PATH];const char *inner;
    if(!path || !total_bytes || !vfs_normalize_path(path,full) ||
       !vfs_iso_path(full,&inner) || !iso9660_is_mounted())return false;
    block_device_t *device=block_get("cd0");
    if(!device || device->type!=BLOCK_DEVICE_ATAPI || !device->sector_count || !device->sector_size)return false;
    *total_bytes=(uint64_t)device->sector_count*device->sector_size;
    return true;
}

bool vfs_get_space_path_type(const char *path, uint64_t *total_bytes,
                             uint64_t *free_bytes, fat_type_t *type) {
    fat_fs_t fs;
    char full[VFS_MAX_PATH];
    const char *inner;
    if (!path || !total_bytes || !free_bytes ||
        !vfs_normalize_path(path, full)) return false;
    if(vfs_iso_path(full,&inner)) {
        if(!vfs_get_optical_space(full,total_bytes))return false;
        *free_bytes=0;if(type)*type=FAT_TYPE_NONE;return true;
    }
    if(!vfs_select_fat(full,&fs,&inner))return false;
    if (!fat_get_space(&fs, total_bytes, free_bytes)) return false;
    if (type) *type = fs.type;
    return true;
}

bool vfs_get_space_path(const char *path, uint64_t *total_bytes,
                        uint64_t *free_bytes) {
    return vfs_get_space_path_type(path, total_bytes, free_bytes, NULL);
}

bool vfs_write_all(const char *path, const void *buffer, uint32_t size) {
    fat_fs_t fs;
    char full[VFS_MAX_PATH];
    const char *iso_path;
    const char *fat_path;
    bool ok;
    if (!path || (size && !buffer) || !vfs_normalize_path(path, full) ||
        !vfs_sync_path(full, NULL) || vfs_iso_path(full, &iso_path)) return false;
    if (!vfs_select_fat(full, &fs, &fat_path)) return false;
    for(vfs_page_reference_t *r=g_page_references;r;r=r->next)
        if(!r->file.unlinked && path_component_equals_ci(r->file.path,full) &&
           vfs_page_reference_size(r)!=size)return false;
    /* As with O_TRUNC, do not resize a live mapping. Same-size native
     * replacements must update its backing or a later msync could restore
     * the old contents over the new file. */
    for(unsigned i=0;i<VFS_PRIVATE_MAP_SLOTS;i++) {
        vfs_shared_mapping_t *m=&g_private_maps[i];
        if(m->references && !m->unlinked && path_component_equals_ci(m->path,full) &&
           m->size!=size)return false;
    }
    for(unsigned i=0;i<VFS_SHARED_MAP_SLOTS;i++) {
        vfs_shared_mapping_t *m=&g_shared_maps[i];
        if(m->references && m->fd>=0 && !m->unlinked &&
           path_component_equals_ci(g_files[m->fd].path,full) && m->size!=size)return false;
    }
    fat_dir_entry_t previous;
    bool existed=!g_vfs_change_sink || fat_resolve_path(&fs,fat_path,&previous);
    ok = fat_write_path(&fs, fat_path, buffer, size);
    if (ok) {
        if(!existed)vfs_notify(full,NULL,VFS_NOTIFY_CREATE,false);
        vfs_notify(full,NULL,VFS_NOTIFY_MODIFY,false);
        for(unsigned i=0;i<VFS_PRIVATE_MAP_SLOTS;i++) {
            vfs_shared_mapping_t *m=&g_private_maps[i];
            if(!m->backing || m->unlinked || !path_component_equals_ci(m->path,full))continue;
            if(m->references) {kmemcpy((void *)m->backing,buffer,size);m->stale=false;}
            else vfs_private_drop(m);
        }
        for(unsigned i=0;i<VFS_SHARED_MAP_SLOTS;i++) {
            vfs_shared_mapping_t *m=&g_shared_maps[i];
            if(m->references && m->fd>=0 && !m->unlinked &&
               path_component_equals_ci(g_files[m->fd].path,full))
                kmemcpy((void *)m->backing,buffer,size);
        }
        for(vfs_page_reference_t *r=g_page_references;r;r=r->next)
            if(!r->file.unlinked && path_component_equals_ci(r->file.path,full))
                pager_file_written(r,0U,buffer,size);
        g_vfs_last_error = "none";
        vfs_refresh_path_handles(full, NULL);
    } else {
        g_vfs_last_error = fat_last_error();
    }
    return ok;
}

bool vfs_bulk_write_begin(const char *path) {
    fat_fs_t fs;
    char full[VFS_MAX_PATH];
    const char *iso_path;
    const char *fat_path;
    if (!path || !vfs_normalize_path(path, full) ||
        vfs_iso_path(full, &iso_path) ||
        !vfs_select_fat(full, &fs, &fat_path))
        return false;
    return fat_bulk_write_begin(&fs);
}

void vfs_bulk_write_end(void) {
    fat_bulk_write_end();
}

/* ELF exec reads its executable and interpreter through read_all(), not
 * mmap(). Keep those Wine runtime images in the same FD-neutral LRU. */
static bool vfs_read_cached_image(const char *path,void *data,uint32_t size) {
    if(mm_total_bytes()<256U*1024U*1024U || !size || size>VFS_PRIVATE_IMAGE_MAX_BYTES)return false;
    char head[14];
    size_t n=kstrlen(path);
    bool runtime=false;
    const char *roots[]={"/WINEGATE/","/OPT/WINE/","/SYSTEM/WINE/"};
    for(unsigned i=0;i<3;i++) {
        size_t len=kstrlen(roots[i]);
        if(n<len)continue;
        kmemcpy(head,path,len);head[len]=0;
        if(path_component_equals_ci(head,roots[i])) {runtime=true;break;}
    }
    if(!runtime)return false;
    bool copied=false;
    kernel_domains_enter(KDOMAIN_VFS);
    int fd=vfs_open(path,VFS_O_RDONLY);
    if(fd>=0) {
        uint8_t magic[4];
        bool hot=vfs_has_shared_mapping(fd);
        bool elf=hot || (vfs_read(fd,magic,4)==4 && magic[0]==0x7f &&
                          magic[1]=='E' && magic[2]=='L' && magic[3]=='F');
        vfs_shared_mapping_t *m=elf?vfs_private_acquire(fd):NULL;
        if(m) {
            if(m->size==size) {kmemcpy(data,(void *)m->backing,size);copied=true;}
            vfs_shared_release(m);
        }
        vfs_close(fd);
    }
    kernel_domains_exit(KDOMAIN_VFS);
    return copied;
}

bool vfs_read_all(const char *path, void **buffer, uint32_t *size) {
    fat_fs_t fs;
    fat_dir_entry_t entry;
    char full[VFS_MAX_PATH];
    uint8_t *data;
    const char *iso_path;
    iso9660_entry_t iso_entry;
    uint32_t file_size;
    uint32_t bytes_read = 0U;
    const char *fat_path;
    bool from_iso = false;

    if (!buffer || !size) return false;
    *buffer = NULL;
    *size = 0;
    if (!vfs_normalize_path(path, full) || !vfs_sync_path(full, NULL)) return false;
    if (vfs_iso_path(full, &iso_path)) {
        if (!iso9660_resolve(iso_path, &iso_entry) ||
            iso_entry.is_directory) return false;
        file_size = iso_entry.size;
        from_iso = true;
    } else {
        if (!vfs_select_fat(full, &fs, &fat_path) ||
            !fat_resolve_path(&fs, fat_path, &entry) ||
            entry.is_directory) return false;
        file_size = entry.size;
    }
    data = (uint8_t *)kmalloc(file_size + 1);
    if(data)(void)mm_set_allocation_subsystem(data,MM_SUBSYSTEM_VFS);
    if (!data) return false;

    if (file_size == 0) {
        data[0] = '\0';
        *buffer = data;
        *size = 0;
        return true;
    }

    /* vfs_read_all ya posee el buffer final. Pasar por vfs_open/vfs_read
     * creaba además una cache del archivo completo, duplicando temporalmente
     * cada .DVR, .BEX y paquete gráfico. Leer directamente reduce el pico de
     * memoria casi a la mitad en el perfil recomendado de 8 MiB. */
    bootsplash_pulse();
    if(vfs_read_cached_image(full,data,file_size)) {
        bytes_read=file_size;
    } else if (from_iso) {
        if (!iso9660_read_at(&iso_entry, 0U, data, file_size, &bytes_read))
            bytes_read = 0U;
    } else {
        if (!fat_read_file_at(&fs, &entry, 0U, data, file_size, &bytes_read))
            bytes_read = 0U;
    }
    bootsplash_pulse();
    if (bytes_read != file_size) {
        kfree(data);
        return false;
    }

    data[file_size] = '\0';
    *buffer = data;
    *size = file_size;
    return true;
}
