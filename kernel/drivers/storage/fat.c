#include "../../include/fat.h"
#include "../../include/block.h"
#include "../../include/memory.h"
#include "../../include/vga.h"
#include "../../include/bootsplash.h"
#include "../../include/task.h"
#include "../../include/pit.h"
#include "../../include/smp.h"
#include "../../include/klock.h"
#include "../../include/w3_profile.h"
#include "../../include/bfs4.h"

static bool name_equals(const char *a, const char *b);
static bool fat_commit_table(const fat_fs_t *fs, const uint8_t *table);
static bool fat_read_sector_run(const fat_fs_t *fs, uint32_t sector,
                                uint32_t count, void *buffer);
static bool fat_write_sector_run(const fat_fs_t *fs, uint32_t sector,
                                 uint32_t count, const void *buffer);
static bool fat_device_writable(const fat_fs_t *fs);

/* FAT short entries reserve byte 12 for NT case flags and byte 13 for the
 * creation-time hundredths field. BlesKernOS has never emitted either field,
 * so entries created by us contain zero there. A high-nibble tag in byte 12
 * therefore gives us a backwards-compatible on-disk extension for the 12
 * POSIX permission/special bits (07777). Standard FAT readers ignore these
 * bytes for file contents, attributes, cluster chains and sizes. */
#define FAT_POSIX_MODE_TAG       0xA0U
#define FAT_POSIX_MODE_TAG_MASK  0xF0U
#define FAT_POSIX_MODE_MASK      0x0FFFU

static fat_fs_t g_current_fs;
static bool g_fs_ready = false;
static const char *g_active_name = "none";

/*
 * Cache de lectura FAT/VFS.
 *
 * La version anterior guardaba un solo sector y read_cluster() la saltaba al
 * usar fat_read_sector_run(). Como resolver una ruta vuelve a leer los
 * directorios padre, la cache se expulsaba entre el sector del directorio y
 * el sector de la FAT. Eso convertia cada doble clic del FileBrowser en
 * varias operaciones ATA PIO sincronas.
 *
 * Cuatro vias evitan expulsiones sistematicas entre directorios alineados
 * a clusters y sus sectores FAT. El tamaño depende de la RAM: además de
 * directorios, mantiene bibliotecas que varios procesos cargan repetidamente.
 * Tambien se alimenta desde las lecturas por lotes.
 */
#define FAT_SECTOR_CACHE_WAYS 4U
#define FAT_SECTOR_CACHE_SLOTS 131072U

typedef struct {
    bool valid;
    block_device_t *device;
    uint32_t lba;
    uint8_t data[BLOCK_SECTOR_SIZE];
} fat_sector_cache_entry_t;

static fat_sector_cache_entry_t g_sector_cache_small[32];
static fat_sector_cache_entry_t *g_sector_cache = g_sector_cache_small;
static uint32_t g_sector_cache_slots = 32U;
static uint8_t g_sector_cache_small_victim[8];
static uint8_t *g_sector_cache_victim = g_sector_cache_small_victim;
static uint32_t g_sector_cache_generation = 1U;
/* Protect both tags and sector bytes on every CPU. No disk I/O or sleeping
 * operations run under this lock; misses drop it before reading the device.
 * A generation change prevents publishing a read across an invalidation. */
static kspinlock_t g_sector_cache_lock = KSPINLOCK_INITIALIZER;

/* Grow the cache on supported hosts (at least 64 MiB RAM), and publish under
 * the cache lock so concurrent mounts/readers see one table. */
static void fat_sector_cache_grow(void) {
    uint32_t ram = mm_total_bytes();
    if (ram < 64U * 1024U * 1024U) return;
    /* Wine's shared bootstrap working set exceeds 16 MiB (shell32 alone
     * is 9 MiB). Retain the common DLLs while another process starts its
     * service, instead of evicting them during the parallel desktop load. */
    uint32_t slots = ram >= 512U * 1024U * 1024U ? FAT_SECTOR_CACHE_SLOTS :
                     ram >= 128U * 1024U * 1024U ? 8192U : 2048U;
    uint32_t irq = kspin_lock_irqsave(&g_sector_cache_lock);
    bool needed = g_sector_cache == g_sector_cache_small;
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
    if (!needed) return;
    fat_sector_cache_entry_t *table = kzalloc(sizeof(*table) * slots +
                                             slots / FAT_SECTOR_CACHE_WAYS);
    if (!table) return;
    (void)mm_set_allocation_owner(table, 0U);
    irq = kspin_lock_irqsave(&g_sector_cache_lock);
    if (g_sector_cache == g_sector_cache_small) {
        g_sector_cache = table;
        g_sector_cache_slots = slots;
        g_sector_cache_victim = (uint8_t *)(table + slots);
        table = NULL;
    }
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
    if (table) kfree(table);
}

/* statfs is also used to identify the filesystem during every Wine path
 * lookup. Cache an exact count from a scanned/committed FAT, not FSInfo
 * (which older BlesK versions did not maintain). */
static struct {
    bool valid;
    fat_fs_t fs;
    uint32_t free_clusters;
    uint32_t generation;
} g_space_cache;

static uint32_t fat_space_invalidate(void) {
    uint32_t irq = kspin_lock_irqsave(&g_sector_cache_lock);
    g_space_cache.valid = false;
    uint32_t generation = ++g_space_cache.generation;
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
    return generation;
}

static bool fat_space_cached(const fat_fs_t *fs, uint32_t *count,
                              uint32_t *generation) {
    uint32_t irq = kspin_lock_irqsave(&g_sector_cache_lock);
    bool hit = g_space_cache.valid &&
        g_space_cache.fs.device == fs->device &&
        g_space_cache.fs.volume_lba == fs->volume_lba &&
        g_space_cache.fs.type == fs->type &&
        g_space_cache.fs.total_clusters == fs->total_clusters &&
        g_space_cache.fs.fat_size_sectors == fs->fat_size_sectors;
    *generation = g_space_cache.generation;
    if (hit) *count = g_space_cache.free_clusters;
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
    return hit;
}

static void fat_space_publish(const fat_fs_t *fs, uint32_t count,
                               uint32_t generation) {
    uint32_t irq = kspin_lock_irqsave(&g_sector_cache_lock);
    if (generation == g_space_cache.generation) {
        g_space_cache.fs = *fs;
        g_space_cache.free_clusters = count;
        g_space_cache.valid = true;
    }
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
}

static uint32_t fat_sector_cache_slot(block_device_t *device, uint32_t lba) {
    uintptr_t tag = (uintptr_t)device;
    return (uint32_t)((lba ^ (uint32_t)(tag >> 4U)) &
                      (g_sector_cache_slots / FAT_SECTOR_CACHE_WAYS - 1U)) * FAT_SECTOR_CACHE_WAYS;
}

/* Caller holds g_sector_cache_lock. */
static fat_sector_cache_entry_t *fat_sector_cache_find(block_device_t *device,
                                                       uint32_t lba) {
    uint32_t base = fat_sector_cache_slot(device, lba);
    for (uint32_t way = 0; way < FAT_SECTOR_CACHE_WAYS; ++way) {
        fat_sector_cache_entry_t *entry = &g_sector_cache[base + way];
        if (entry->valid && entry->device == device && entry->lba == lba)
            return entry;
    }
    return NULL;
}

static bool fat_sector_cache_read(block_device_t *device, uint32_t lba,
                                  void *buffer) {
    fat_sector_cache_entry_t *entry;
    bool hit = false;
    if (!device || !buffer) return false;
    uint32_t irq = kspin_lock_irqsave(&g_sector_cache_lock);
    entry = fat_sector_cache_find(device, lba);
    if (entry) {
        kmemcpy(buffer, entry->data, BLOCK_SECTOR_SIZE);
        hit = true;
    }
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
    return hit;
}

static bool fat_sector_cache_contains(block_device_t *device, uint32_t lba) {
    bool hit;
    if (!device) return false;
    uint32_t irq = kspin_lock_irqsave(&g_sector_cache_lock);
    hit = fat_sector_cache_find(device, lba) != NULL;
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
    return hit;
}

static uint32_t fat_sector_cache_get_generation(void) {
    uint32_t generation;
    uint32_t irq = kspin_lock_irqsave(&g_sector_cache_lock);
    generation = g_sector_cache_generation;
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
    return generation;
}

static void fat_sector_cache_store(block_device_t *device, uint32_t lba,
                                   const void *buffer,
                                   uint32_t expected_generation) {
    fat_sector_cache_entry_t *entry;
    if (!device || !buffer) return;
    uint32_t irq = kspin_lock_irqsave(&g_sector_cache_lock);
    /* Una escritura puede completar mientras el PIO de lectura estaba en
     * curso. No publique datos anteriores a esa invalidacion. */
    if (g_sector_cache_generation != expected_generation) {
        kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
        return;
    }
    entry = fat_sector_cache_find(device, lba);
    if (!entry) {
        uint32_t base = fat_sector_cache_slot(device, lba);
        for (uint32_t way = 0; way < FAT_SECTOR_CACHE_WAYS; ++way) {
            if (!g_sector_cache[base + way].valid) {
                entry = &g_sector_cache[base + way];
                break;
            }
        }
        if (!entry) {
            uint32_t set = base / FAT_SECTOR_CACHE_WAYS;
            uint32_t way = g_sector_cache_victim[set];
            g_sector_cache_victim[set] = (way + 1U) % FAT_SECTOR_CACHE_WAYS;
            entry = &g_sector_cache[base + way];
        }
    }
    entry->device = device;
    entry->lba = lba;
    kmemcpy(entry->data, buffer, BLOCK_SECTOR_SIZE);
    entry->valid = true;
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
}

static void fat_sector_cache_invalidate_all(void) {
    fat_space_invalidate();
    uint32_t irq = kspin_lock_irqsave(&g_sector_cache_lock);
    g_sector_cache_generation++;
    if (!g_sector_cache_generation) g_sector_cache_generation = 1U;
    for (uint32_t i = 0; i < g_sector_cache_slots; i++)
        g_sector_cache[i].valid = false;
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
}

static void fat_sector_cache_invalidate_device(block_device_t *device) {
    fat_space_invalidate();
    if (!device) return;
    uint32_t irq = kspin_lock_irqsave(&g_sector_cache_lock);
    g_sector_cache_generation++;
    if (!g_sector_cache_generation) g_sector_cache_generation = 1U;
    for (uint32_t i = 0; i < g_sector_cache_slots; i++) {
        if (g_sector_cache[i].valid && g_sector_cache[i].device == device)
            g_sector_cache[i].valid = false;
    }
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
}

static void fat_sector_cache_invalidate_range(block_device_t *device,
                                              uint32_t first_lba,
                                              uint32_t count) {
    if (!device || !count) return;
    uint32_t irq = kspin_lock_irqsave(&g_sector_cache_lock);
    g_sector_cache_generation++;
    if (!g_sector_cache_generation) g_sector_cache_generation = 1U;
    /* Writes arrive in short sector runs. Probe their sets instead of
     * scanning the entire read cache for every metadata-sector write. */
    for (uint32_t i = 0; i < count; i++) {
        fat_sector_cache_entry_t *entry = fat_sector_cache_find(device, first_lba + i);
        if (entry) entry->valid = false;
    }
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
}

/* BLES_SETUP_FAT_BULK_CACHE_20260723
 * fat_store_path() normally loads the complete FAT on every file creation.
 * That is safe but catastrophic for an installer containing hundreds of
 * small files.  This cache keeps one volume allocation table resident while
 * Setup runs.  Dirty FAT sectors are still written after every file, so an
 * interrupted installation does not leave hundreds of uncommitted entries. */
typedef struct {
    bool active;
    fat_fs_t fs;
    uint8_t *table;
    uint8_t *dirty;
    uint32_t fat_bytes;
    uint32_t next_free_hint;
    uint32_t stores;
} fat_bulk_write_cache_t;

static fat_bulk_write_cache_t g_fat_bulk;

static bool fat_bulk_matches(const fat_fs_t *fs) {
    return fs && g_fat_bulk.active &&
           g_fat_bulk.fs.device == fs->device &&
           g_fat_bulk.fs.volume_lba == fs->volume_lba &&
           g_fat_bulk.fs.type == fs->type &&
           g_fat_bulk.fs.fat_size_sectors == fs->fat_size_sectors &&
           g_fat_bulk.fs.bytes_per_sector == fs->bytes_per_sector;
}

static void fat_bulk_release(void) {
    if (g_fat_bulk.table) kfree(g_fat_bulk.table);
    if (g_fat_bulk.dirty) kfree(g_fat_bulk.dirty);
    kmemset(&g_fat_bulk, 0, sizeof(g_fat_bulk));
}

static bool fat_bulk_reload(void) {
    if (!g_fat_bulk.active || !g_fat_bulk.table || !g_fat_bulk.dirty)
        return false;
    if (!fat_read_sector_run(&g_fat_bulk.fs,
                             g_fat_bulk.fs.first_fat_sector,
                             g_fat_bulk.fs.fat_size_sectors,
                             g_fat_bulk.table))
        return false;
    kmemset(g_fat_bulk.dirty, 0, g_fat_bulk.fs.fat_size_sectors);
    g_fat_bulk.next_free_hint = 2U;
    return true;
}

bool fat_bulk_write_begin(const fat_fs_t *fs) {
    if (fs && fs->type == FAT_TYPE_BFS4) return true;
    uint32_t start;
    if (!fs || !fat_device_writable(fs) || !fs->fat_size_sectors ||
        !fs->bytes_per_sector)
        return false;
    if (fat_bulk_matches(fs)) return true;
    fat_bulk_release();
    g_fat_bulk.fat_bytes = fs->fat_size_sectors * fs->bytes_per_sector;
    g_fat_bulk.table = (uint8_t *)kmalloc(g_fat_bulk.fat_bytes);
    g_fat_bulk.dirty = (uint8_t *)kzalloc(fs->fat_size_sectors);
    if (!g_fat_bulk.table || !g_fat_bulk.dirty) {
        fat_bulk_release();
        return false;
    }
    g_fat_bulk.fs = *fs;
    g_fat_bulk.active = true;
    g_fat_bulk.next_free_hint = 2U;
    start = pit_get_ticks();
    if (!fat_bulk_reload()) {
        fat_bulk_release();
        return false;
    }
    kprintf("[FAT:BULK] cache ready sectors=%u bytes=%u elapsed_ticks=%u\n",
            fs->fat_size_sectors, g_fat_bulk.fat_bytes,
            pit_get_ticks() - start);
    return true;
}

void fat_bulk_write_end(void) {
    if (g_fat_bulk.active)
        kprintf("[FAT:BULK] cache released stores=%u\n",
                g_fat_bulk.stores);
    fat_bulk_release();
}

bool fat_bulk_write_is_active(void) {
    return g_fat_bulk.active;
}

/* BLES_WINE_INSTALL_DIAG_PERF_20260723
 * Diagnostico conservador: solo guarda/imprime el ultimo fallo, nunca cada
 * operacion exitosa. */
/*
 * Directories grow through fat_extend_directory_chain(). Start with one
 * cluster: reserving 32 for every empty directory wrote 128 KiB per mkdir
 * on 4-KiB-cluster volumes, including Wine's numerous small directories.
 */
#define FAT_DIRECTORY_PREALLOC_CLUSTERS 1U
static const char *g_fat_last_error = "none";

const char *fat_last_error(void) {
    return g_fat_last_error;
}

static void fat_set_last_error(const char *error) {
    g_fat_last_error = error ? error : "unknown";
}

static const char *g_format_error = "sin error";
static uint32_t g_format_error_lba = FAT_FORMAT_LBA_NONE;

static bool fat_format_fail(const char *reason, uint32_t lba) {
    g_format_error = reason ? reason : "error desconocido";
    g_format_error_lba = lba;
    if (lba == FAT_FORMAT_LBA_NONE)
        kprintf("[FATFMT] ERROR: %s\n", g_format_error);
    else
        kprintf("[FATFMT] ERROR: %s (LBA %u)\n", g_format_error, lba);
    return false;
}

static bool fat_format_write_sector(block_device_t *device, uint32_t lba,
                                    const void *buffer, const char *stage) {
    for (uint32_t attempt = 1U; attempt <= 3U; attempt++) {
        if (block_write(device, lba, 1, buffer)) return true;
        kprintf("[FATFMT] escritura fallo: etapa=%s lba=%u intento=%u/3\n",
                stage ? stage : "?", lba, attempt);
        task_yield();
    }
    return fat_format_fail(stage, lba);
}

const char *fat_format_last_error(void) {
    return g_format_error;
}

uint32_t fat_format_last_error_lba(void) {
    return g_format_error_lba;
}

void fat_forget_device(const char *device_name) {
    block_device_t *device;
    if (!device_name || !device_name[0]) return;
    device = block_get(device_name);
    if (g_fs_ready && name_equals(device_name, g_active_name)) {
        kmemset(&g_current_fs, 0, sizeof(g_current_fs));
        g_fs_ready = false;
        g_active_name = "none";
        kprintf("[FATFMT] montaje FAT activo de %s invalidado\n", device_name);
    }
    fat_sector_cache_invalidate_device(device);
    if (g_fat_bulk.active && g_fat_bulk.fs.device == device)
        fat_bulk_write_end();
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void format_label(char output[11], const char *label) {
    uint32_t i = 0;
    for (; i < 11; i++) output[i] = ' ';
    if (!label || !label[0]) label = "BLESKERNOS";
    for (i = 0; i < 11 && label[i]; i++) {
        char c = label[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        output[i] = (c >= 32 && c <= 126 && c != '/' && c != '\\') ? c : '_';
    }
}

static bool fat_format_internal(const char *device_name,
                                const char *volume_label,
                                uint32_t boot_reserved_sectors) {
    block_device_t *device;
    uint8_t sector[BLOCK_SECTOR_SIZE];
    char label[11];
    uint32_t total;
    uint32_t reserved;
    uint32_t root_entries;
    uint32_t root_sectors;
    uint32_t fat_sectors = 1;
    uint32_t clusters = 0;
    uint32_t first_root;
    uint32_t first_data;
    uint32_t fsinfo_sector = 1U;
    uint32_t backup_boot_sector = 6U;
    uint32_t backup_fsinfo_sector = 7U;
    uint8_t sectors_per_cluster;
    uint8_t fat_bits;
    uint8_t media;

    g_format_error = "sin error";
    g_format_error_lba = FAT_FORMAT_LBA_NONE;
    kprintf("[FATFMT] inicio: destino=%s reserved=%u activo=%s ready=%u\n",
            device_name ? device_name : "(null)", boot_reserved_sectors,
            g_active_name ? g_active_name : "(null)", g_fs_ready ? 1U : 0U);

    if (!device_name || !device_name[0])
        return fat_format_fail("nombre de dispositivo invalido",
                               FAT_FORMAT_LBA_NONE);
    if (g_fs_ready && name_equals(device_name, g_active_name))
        return fat_format_fail("el dispositivo sigue montado como FAT activo",
                               FAT_FORMAT_LBA_NONE);
    device = block_get(device_name);
    if (!device)
        return fat_format_fail("el dispositivo no existe",
                               FAT_FORMAT_LBA_NONE);
    kprintf("[FATFMT] dispositivo: sectores=%u bps=%u ro=%u writer=%s tipo=%u\n",
            device->sector_count, device->sector_size,
            device->read_only ? 1U : 0U,
            device->write ? "si" : "no", device->type);
    if (device->read_only)
        return fat_format_fail("el dispositivo es de solo lectura",
                               FAT_FORMAT_LBA_NONE);
    if (!device->write)
        return fat_format_fail("el dispositivo no tiene funcion de escritura",
                               FAT_FORMAT_LBA_NONE);
    if (device->sector_size != BLOCK_SECTOR_SIZE)
        return fat_format_fail("tamano de sector no soportado",
                               FAT_FORMAT_LBA_NONE);
    if (device->sector_count < 128U)
        return fat_format_fail("el dispositivo es demasiado pequeno",
                               FAT_FORMAT_LBA_NONE);

    total = device->sector_count;
    if (total < 8400U) {
        if (boot_reserved_sectors)
            return fat_format_fail("el destino es demasiado pequeno para FAT32 arrancable",
                                   FAT_FORMAT_LBA_NONE);
        fat_bits = 12; sectors_per_cluster = 1; reserved = 1;
        root_entries = 224; media = device->type == BLOCK_DEVICE_FLOPPY
                                  ? 0xF0 : 0xF8;
    } else if (total < 65536U) {
        if (boot_reserved_sectors)
            return fat_format_fail("el destino es demasiado pequeno para FAT32 arrancable",
                                   FAT_FORMAT_LBA_NONE);
        fat_bits = 16; sectors_per_cluster = total < 32768U ? 2 : 4;
        reserved = 1; root_entries = 512; media = 0xF8;
    } else {
        fat_bits = 32; sectors_per_cluster = total < 532480U ? 1 : 8;
        reserved = boot_reserved_sectors >= 35U ? boot_reserved_sectors : 32U;
        root_entries = 0; media = 0xF8;
        if (boot_reserved_sectors >= 35U) {
            fsinfo_sector = reserved - 3U;
            backup_boot_sector = reserved - 2U;
            backup_fsinfo_sector = reserved - 1U;
        }
    }
    if (reserved >= total / 2U || reserved > 0xFFFFU)
        return fat_format_fail("cantidad de sectores reservados invalida",
                               FAT_FORMAT_LBA_NONE);

    root_sectors = ((root_entries * 32U) + 511U) / 512U;
    for (uint32_t pass = 0; pass < 16U; pass++) {
        uint32_t data;
        uint32_t next;
        if (total <= reserved + root_sectors + fat_sectors * 2U)
            return fat_format_fail("no hay espacio para FAT y datos",
                                   FAT_FORMAT_LBA_NONE);
        data = total - reserved - root_sectors - fat_sectors * 2U;
        clusters = data / sectors_per_cluster;
        if (fat_bits == 12)
            next = (((clusters + 2U) * 3U + 1U) / 2U + 511U) / 512U;
        else if (fat_bits == 16)
            next = ((clusters + 2U) * 2U + 511U) / 512U;
        else
            next = ((clusters + 2U) * 4U + 511U) / 512U;
        if (next == fat_sectors) break;
        fat_sectors = next;
    }
    if ((fat_bits == 12 && clusters >= 4085U) ||
        (fat_bits == 16 && (clusters < 4085U || clusters >= 65525U)) ||
        (fat_bits == 32 && clusters < 65525U))
        return fat_format_fail("cantidad de clusters incompatible con el tipo FAT",
                               FAT_FORMAT_LBA_NONE);

    first_root = reserved + fat_sectors * 2U;
    first_data = first_root + root_sectors;
    format_label(label, volume_label);
    kprintf("[FATFMT] layout: FAT%u total=%u reserved=%u spc=%u fatsz=%u "
            "first_data=%u clusters=%u\n",
            fat_bits, total, reserved, sectors_per_cluster, fat_sectors,
            first_data, clusters);

    /* El destino puede haber sido montado antes de entrar a Setup. El formato
       invalida siempre la cache antes de empezar a sobrescribir metadata. */
    fat_sector_cache_invalidate_all();

    kmemset(sector, 0, sizeof(sector));
    for (uint32_t lba = 0; lba < first_data; lba++) {
        if (!fat_format_write_sector(device, lba, sector,
                                     "no se pudo limpiar la metadata FAT"))
            return false;
        if ((lba & 511U) == 511U)
            kprintf("[FATFMT] limpiados %u/%u sectores de metadata\n",
                    lba + 1U, first_data);
    }
    if (fat_bits == 32) {
        for (uint32_t i = 0; i < sectors_per_cluster; i++)
            if (!fat_format_write_sector(device, first_data + i, sector,
                                         "no se pudo limpiar el cluster raiz"))
                return false;
    }

    kmemset(sector, 0, sizeof(sector));
    sector[0] = 0xEB; sector[1] = 0x3C; sector[2] = 0x90;
    kmemcpy(sector + 3, "BLESFMT ", 8);
    write_le16(sector + 11, 512);
    sector[13] = sectors_per_cluster;
    write_le16(sector + 14, (uint16_t)reserved);
    sector[16] = 2;
    write_le16(sector + 17, (uint16_t)root_entries);
    write_le16(sector + 19, total < 65536U ? (uint16_t)total : 0);
    sector[21] = media;
    write_le16(sector + 22, fat_bits == 32 ? 0 : (uint16_t)fat_sectors);
    write_le16(sector + 24, 63); write_le16(sector + 26, 255);
    write_le32(sector + 32, total >= 65536U ? total : 0);
    if (fat_bits == 32) {
        write_le32(sector + 36, fat_sectors);
        write_le32(sector + 44, 2);
        write_le16(sector + 48, (uint16_t)fsinfo_sector);
        write_le16(sector + 50, (uint16_t)backup_boot_sector);
        sector[64] = 0x80; sector[66] = 0x29;
        write_le32(sector + 67, pit_get_ticks());
        kmemcpy(sector + 71, label, 11);
        kmemcpy(sector + 82, "FAT32   ", 8);
    } else {
        sector[36] = device->type == BLOCK_DEVICE_FLOPPY ? 0 : 0x80;
        sector[38] = 0x29; write_le32(sector + 39, pit_get_ticks());
        kmemcpy(sector + 43, label, 11);
        kmemcpy(sector + 54, fat_bits == 12 ? "FAT12   " : "FAT16   ", 8);
    }
    sector[510] = 0x55; sector[511] = 0xAA;
    if (!fat_format_write_sector(device, 0, sector,
                                 "no se pudo escribir el sector de arranque"))
        return false;
    if (fat_bits == 32 &&
        !fat_format_write_sector(device, backup_boot_sector, sector,
                                 "no se pudo escribir el boot de respaldo"))
        return false;

    if (fat_bits == 32) {
        kmemset(sector, 0, sizeof(sector));
        write_le32(sector, 0x41615252U);
        write_le32(sector + 484, 0x61417272U);
        write_le32(sector + 488, 0xFFFFFFFFU);
        write_le32(sector + 492, 0xFFFFFFFFU);
        write_le32(sector + 508, 0xAA550000U);
        if (!fat_format_write_sector(device, fsinfo_sector, sector,
                                     "no se pudo escribir FSInfo") ||
            !fat_format_write_sector(device, backup_fsinfo_sector, sector,
                                     "no se pudo escribir FSInfo de respaldo"))
            return false;
    }

    for (uint32_t copy = 0; copy < 2U; copy++) {
        kmemset(sector, 0, sizeof(sector));
        if (fat_bits == 12) {
            sector[0] = media; sector[1] = 0xFF; sector[2] = 0xFF;
        } else if (fat_bits == 16) {
            write_le16(sector, (uint16_t)(0xFF00U | media));
            write_le16(sector + 2, 0xFFFFU);
        } else {
            write_le32(sector, 0x0FFFFFF8U);
            write_le32(sector + 4, 0xFFFFFFFFU);
            write_le32(sector + 8, 0x0FFFFFFFU);
        }
        if (!fat_format_write_sector(device,
                                     reserved + copy * fat_sectors, sector,
                                     "no se pudo escribir la cabecera FAT"))
            return false;
    }

    kmemset(sector, 0, sizeof(sector));
    kmemcpy(sector, label, 11); sector[11] = FAT_ATTR_VOLUME_ID;
    if (!fat_format_write_sector(device,
                                 fat_bits == 32 ? first_data : first_root,
                                 sector,
                                 "no se pudo escribir la etiqueta del volumen"))
        return false;
    fat_sector_cache_invalidate_all();
    kprintf("[FATFMT] formato FAT%u completado correctamente en %s\n",
            fat_bits, device_name);
    return true;
}

bool fat_format(const char *device_name, const char *volume_label) {
    return fat_format_internal(device_name, volume_label, 0U);
}

bool fat_format_bootable(const char *device_name, const char *volume_label,
                         uint32_t reserved_sectors) {
    if (reserved_sectors < 35U) {
        g_format_error = "FAT32 arrancable necesita al menos 35 sectores reservados";
        g_format_error_lba = FAT_FORMAT_LBA_NONE;
        kprintf("[FATFMT] ERROR: %s (reserved=%u)\n",
                g_format_error, reserved_sectors);
        return false;
    }
    return fat_format_internal(device_name, volume_label, reserved_sectors);
}

static void copy_name(const uint8_t *raw_name, char *out) {
    int i;
    for (i = 0; i < 8 && raw_name[i] != ' '; i++) {
        out[i] = (char)raw_name[i];
    }
    if (raw_name[8] != ' ' && raw_name[8] != 0) {
        out[i++] = '.';
        for (int j = 0; j < 3 && raw_name[8 + j] != ' '; j++) {
            out[i++] = (char)raw_name[8 + j];
        }
    }
    out[i] = '\0';
}

typedef struct {
    char name[FAT_MAX_NAME];
    uint8_t checksum;
    bool active;
} fat_lfn_state_t;

static uint8_t fat_short_checksum(const uint8_t raw[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1U) ? 0x80U : 0U) + (sum >> 1) + raw[i]);
    return sum;
}

static void fat_lfn_reset(fat_lfn_state_t *state) {
    if (!state) return;
    state->active = false;
    state->checksum = 0;
    state->name[0] = 0;
}

static void fat_lfn_put_char(fat_lfn_state_t *state, uint32_t index,
                             uint16_t value) {
    if (!state || index + 1 >= FAT_MAX_NAME) return;
    if (value == 0x0000 || value == 0xFFFF) {
        if (value == 0x0000) state->name[index] = '\0';
        return;
    }
    state->name[index] = value < 0x80 ? (char)value : '?';
    if (!state->name[index + 1]) state->name[index + 1] = '\0';
}

static bool fat_lfn_consume(fat_lfn_state_t *state, const uint8_t *entry) {
    static const uint8_t offsets[13] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };
    uint8_t ordinal;
    uint32_t base;
    if (!state || !entry || entry[11] != FAT_ATTR_LONG_NAME) return false;
    ordinal = entry[0] & 0x1F;
    if (!ordinal || ordinal > 20) {
        fat_lfn_reset(state);
        return false;
    }
    if (entry[0] & 0x40) {
        fat_lfn_reset(state);
        kmemset(state->name, 0, sizeof(state->name));
        state->active = true;
        state->checksum = entry[13];
    }
    if (!state->active || state->checksum != entry[13]) {
        fat_lfn_reset(state);
        return false;
    }
    base = (uint32_t)(ordinal - 1) * 13U;
    for (uint32_t i = 0; i < 13; i++)
        fat_lfn_put_char(state, base + i, read_le16(entry + offsets[i]));
    return true;
}

static bool name_equals(const char *a, const char *b) {
    char ca = *a;
    char cb = *b;
    while (ca && cb) {
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return false;
        a++;
        b++;
        ca = *a;
        cb = *b;
    }
    return ca == cb;
}

static bool parse_dir_entry(const uint8_t *entry, fat_dir_entry_t *out) {
    if (!entry || !out) return false;
    if (entry[0] == 0x00) return false;
    if (entry[0] == 0xE5) return false;
    if (entry[11] == FAT_ATTR_LONG_NAME) return false;
    if (entry[11] & FAT_ATTR_VOLUME_ID) return false;

    kmemset(out, 0, sizeof(*out));
    copy_name(entry, out->short_name);
    kstrcpy(out->name, out->short_name);
    out->attributes = entry[11];
    out->size = read_le32(entry + 28);
    out->first_cluster = (uint32_t)read_le16(entry + 26) | ((uint32_t)read_le16(entry + 20) << 16);
    out->is_directory = (out->attributes & FAT_ATTR_DIRECTORY) != 0;
    return true;
}

static bool parse_dir_entry_lfn(const uint8_t *entry, fat_lfn_state_t *lfn,
                                fat_dir_entry_t *out) {
    if (!entry || !lfn || !out) return false;
    if (entry[0] == 0xE5) {
        fat_lfn_reset(lfn);
        return false;
    }
    if (entry[11] == FAT_ATTR_LONG_NAME)
        return fat_lfn_consume(lfn, entry) && false;
    if (!parse_dir_entry(entry, out)) {
        fat_lfn_reset(lfn);
        return false;
    }
    if (lfn->active && lfn->name[0] &&
        lfn->checksum == fat_short_checksum(entry)) {
        kstrncpy(out->name, lfn->name, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
    }
    fat_lfn_reset(lfn);
    return true;
}

/* Directory lookup only needs the full parsed entry for its match. Avoid
 * clearing/copying hundreds of bytes for every other name in system32. */
static bool fat_match_dir_entry(const uint8_t *entry, fat_lfn_state_t *lfn,
                                const char *name, fat_dir_entry_t *out) {
    if (!entry[0] || entry[0] == 0xE5U) {
        fat_lfn_reset(lfn);
        return false;
    }
    if (entry[11] == FAT_ATTR_LONG_NAME) {
        (void)fat_lfn_consume(lfn, entry);
        return false;
    }
    if (!(entry[11] & FAT_ATTR_VOLUME_ID)) {
        char short_name[13];
        copy_name(entry, short_name);
        if (name_equals(short_name, name) ||
            (lfn->active && lfn->name[0] && name_equals(lfn->name, name) &&
             lfn->checksum == fat_short_checksum(entry)))
            return parse_dir_entry_lfn(entry, lfn, out);
    }
    fat_lfn_reset(lfn);
    return false;
}

static bool read_sector(const fat_fs_t *fs, uint32_t sector, void *buffer) {
    W3P_SCOPE(W3P_FAT_SECTOR_R, BLOCK_SECTOR_SIZE);
    uint32_t lba;
    uint32_t cache_generation;

    if (!fs || !fs->device || !buffer) return false;
    if (fs->bytes_per_sector != BLOCK_SECTOR_SIZE) return false;

    lba = fs->volume_lba + sector;
    if (fat_sector_cache_read(fs->device, lba, buffer)) {
        bootsplash_pulse();
        return true;
    }

    cache_generation = fat_sector_cache_get_generation();
    if (!block_read(fs->device, lba, 1, buffer)) return false;
    fat_sector_cache_store(fs->device, lba, buffer, cache_generation);
    bootsplash_pulse();
    return true;
}

static bool read_cluster(const fat_fs_t *fs, uint32_t cluster, void *buffer) {
    uint32_t sector;
    if (!fs || !buffer || cluster < 2U) return false;
    sector = fs->first_data_sector +
             ((cluster - 2U) * fs->sectors_per_cluster);
    /* La imagen ATA de BlesKernOS usa un sector por cluster. Pasar por
     * read_sector() permite reutilizar la cache FAT/VFS durante resolucion y
     * listado de rutas, en vez de saltarla con un block_read directo. */
    if (fs->sectors_per_cluster == 1U)
        return read_sector(fs, sector, buffer);
    return fat_read_sector_run(fs, sector, fs->sectors_per_cluster, buffer);
}

static uint32_t get_next_cluster(const fat_fs_t *fs, uint32_t cluster) {
    uint32_t fat_offset;
    uint32_t fat_sector;
    uint32_t entry_offset;
    uint8_t fat_entry[BLOCK_SECTOR_SIZE * 2U];
    uint32_t next;

    if (!fs || cluster < 2U || fs->bytes_per_sector != BLOCK_SECTOR_SIZE)
        return 0xFFFFFFFFU;

    /* BLES_WINE_SFX_IO_CACHE_FIX_20260723
     * A FAT lookup used to kmalloc/kfree two sectors for every cluster.
     * BlesKernOS accepts 512-byte FAT sectors at mount time, so a bounded
     * stack buffer is both safe and much cheaper during executable scans. */
    if (fs->type == FAT_TYPE_FAT12) {
        fat_offset = cluster + cluster / 2U;
        fat_sector = fs->first_fat_sector + fat_offset / fs->bytes_per_sector;
        entry_offset = fat_offset % fs->bytes_per_sector;
        if (!read_sector(fs, fat_sector, fat_entry)) return 0xFFFFFFFFU;
        if (entry_offset + 2U > fs->bytes_per_sector &&
            !read_sector(fs, fat_sector + 1U,
                         fat_entry + fs->bytes_per_sector))
            return 0xFFFFFFFFU;
        next = read_le16(fat_entry + entry_offset);
        next = (cluster & 1U) ? (next >> 4U) : (next & 0x0FFFU);
        return next >= FAT_CLUSTER_END_12 ? 0xFFFFFFFFU : next;
    }

    if (fs->type == FAT_TYPE_FAT16) {
        fat_offset = cluster * 2U;
        fat_sector = fs->first_fat_sector + fat_offset / fs->bytes_per_sector;
        entry_offset = fat_offset % fs->bytes_per_sector;
        if (!read_sector(fs, fat_sector, fat_entry)) return 0xFFFFFFFFU;
        if (entry_offset + 2U > fs->bytes_per_sector &&
            !read_sector(fs, fat_sector + 1U,
                         fat_entry + fs->bytes_per_sector))
            return 0xFFFFFFFFU;
        next = read_le16(fat_entry + entry_offset);
        return next >= FAT_CLUSTER_END_16 ? 0xFFFFFFFFU : next;
    }

    fat_offset = cluster * 4U;
    fat_sector = fs->first_fat_sector + fat_offset / fs->bytes_per_sector;
    entry_offset = fat_offset % fs->bytes_per_sector;
    if (!read_sector(fs, fat_sector, fat_entry)) return 0xFFFFFFFFU;
    if (entry_offset + 4U > fs->bytes_per_sector &&
        !read_sector(fs, fat_sector + 1U,
                     fat_entry + fs->bytes_per_sector))
        return 0xFFFFFFFFU;
    next = read_le32(fat_entry + entry_offset) & 0x0FFFFFFFU;
    return next >= FAT_CLUSTER_END_32 ? 0xFFFFFFFFU : next;
}

/* Ruta rapida para cargas completas (PE, DLL, caches VFS): agrupa clusters
 * fisicamente consecutivos en lecturas de hasta 255 sectores. */
static bool fat_read_full_file_fast(const fat_fs_t *fs,
                                    const fat_dir_entry_t *entry,
                                    void *buffer, uint32_t *bytes_read) {
    uint32_t cluster;
    uint32_t remaining;
    uint32_t out = 0U;
    uint32_t cluster_size;
    uint32_t max_run_clusters;

    if (!fs || !entry || !buffer || !bytes_read || entry->is_directory)
        return false;
    *bytes_read = 0U;
    if (!entry->size) return true;
    cluster = entry->first_cluster;
    if (cluster < 2U) return false;
    cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    max_run_clusters = 255U / fs->sectors_per_cluster;
    if (!max_run_clusters) max_run_clusters = 1U;
    remaining = entry->size;

    while (remaining) {
        uint32_t run_clusters = 1U;
        uint32_t next = get_next_cluster(fs, cluster);
        uint32_t run_bytes;
        uint32_t transfer;
        uint32_t full_sectors;
        uint32_t start_sector = fs->first_data_sector +
            (cluster - 2U) * fs->sectors_per_cluster;

        while (run_clusters < max_run_clusters &&
               next == cluster + run_clusters &&
               run_clusters * cluster_size < remaining) {
            run_clusters++;
            next = get_next_cluster(fs, cluster + run_clusters - 1U);
        }

        run_bytes = run_clusters * cluster_size;
        transfer = remaining < run_bytes ? remaining : run_bytes;
        full_sectors = transfer / fs->bytes_per_sector;

        if (full_sectors &&
            !fat_read_sector_run(fs, start_sector, full_sectors,
                                 (uint8_t *)buffer + out))
            return false;
        out += full_sectors * fs->bytes_per_sector;
        remaining -= full_sectors * fs->bytes_per_sector;

        if (remaining && transfer % fs->bytes_per_sector) {
            uint8_t tail[BLOCK_SECTOR_SIZE];
            uint32_t partial = transfer % fs->bytes_per_sector;
            if (!read_sector(fs, start_sector + full_sectors, tail))
                return false;
            kmemcpy((uint8_t *)buffer + out, tail, partial);
            out += partial;
            remaining -= partial;
        }

        if (!remaining) break;
        if (next == 0xFFFFFFFFU || next < 2U) return false;
        cluster = next;
    }

    *bytes_read = out;
    return out == entry->size;
}


static bool list_directory_chain(const fat_fs_t *fs, uint32_t start_cluster, fat_dir_entry_t *entries, uint32_t max_entries, uint32_t *count, fat_dir_metadata_t *metadata) {
    uint32_t current_cluster = start_cluster;
    uint32_t found = 0;
    bool done = false;
    uint8_t *cluster_buf = NULL;
    uint32_t cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    fat_lfn_state_t lfn;

    fat_lfn_reset(&lfn);

    if ((fs->type == FAT_TYPE_FAT12 || fs->type == FAT_TYPE_FAT16) && start_cluster == 0) {
        uint32_t root_dir_sector = fs->first_fat_sector + (fs->num_fats * fs->fat_size_sectors);
        uint32_t sector = root_dir_sector;
        uint32_t sectors_left = fs->root_dir_sectors;
        while (sectors_left > 0 && found < max_entries) {
            uint8_t sector_buf[512];
            if (!read_sector(fs, sector, sector_buf)) break;
            for (uint32_t i = 0; i < (fs->bytes_per_sector / 32); i++) {
                const uint8_t *entry = sector_buf + (i * 32);
                fat_dir_entry_t parsed;
                if (!parse_dir_entry_lfn(entry, &lfn, &parsed)) {
                    if (entry[0] == 0x00) {
                        done = true;
                        break;
                    }
                    continue;
                }
                if (found < max_entries) {
                    if (metadata) {
                        metadata[found].position = ((uint64_t)sector * fs->bytes_per_sector + i * 32U) / 32U + 2U;
                        metadata[found].mode = (entry[12] & FAT_POSIX_MODE_TAG_MASK) == 0xB0U
                            ? 0120000U : parsed.is_directory ? 0040000U : 0100000U;
                    }
                    entries[found++] = parsed;
                }
            }
            sectors_left--;
            sector++;
            bootsplash_pulse();
            task_yield();
            if (done) break;
        }
        if (count) *count = found;
        return true;
    }

    cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return false;

    while (current_cluster >= 2 && !done && found < max_entries) {
        if (!read_cluster(fs, current_cluster, cluster_buf)) break;
        for (uint32_t i = 0; i < (cluster_size / 32); i++) {
            const uint8_t *entry = cluster_buf + (i * 32);
            fat_dir_entry_t parsed;
            if (!parse_dir_entry_lfn(entry, &lfn, &parsed)) {
                if (entry[0] == 0x00) {
                    done = true;
                    break;
                }
                continue;
            }
            if (found < max_entries) {
                if (metadata) {
                    uint32_t sector = fs->first_data_sector + (current_cluster - 2U) * fs->sectors_per_cluster;
                    metadata[found].position = ((uint64_t)sector * fs->bytes_per_sector + i * 32U) / 32U + 2U;
                    metadata[found].mode = (entry[12] & FAT_POSIX_MODE_TAG_MASK) == 0xB0U
                        ? 0120000U : parsed.is_directory ? 0040000U : 0100000U;
                }
                entries[found++] = parsed;
            }
        }
        if (done) break;
        if (found >= max_entries) break;
        current_cluster = get_next_cluster(fs, current_cluster);
        if (current_cluster == 0xFFFFFFFF) break;
    }

    if (count) *count = found;
    kfree(cluster_buf);
    return true;
}


static bool find_directory_chain_entry(const fat_fs_t *fs,
                                       uint32_t start_cluster,
                                       const char *name,
                                       fat_dir_entry_t *result) {
    W3P_SCOPE(W3P_FAT_DIR_FIND, 0U);
    uint32_t cluster = start_cluster, visited = 0, sectors_since_yield = 0;
    uint8_t sector_buf[512];
    fat_lfn_state_t lfn;
    if (!fs || !name || !result) return false;
    bool fixed_root = start_cluster == 0U && fs->type != FAT_TYPE_FAT32;
    fat_lfn_reset(&lfn);
    /* Lookup needs only the sector containing the match. Avoid allocating a
     * whole cluster for every path component and reading past early matches.
     * Keep LFN state across sector/cluster boundaries, including FAT12/16. */
    while (fixed_root || (cluster >= 2U && visited++ <= fs->total_clusters)) {
        uint32_t first = fixed_root
            ? fs->first_fat_sector + fs->num_fats * fs->fat_size_sectors
            : fs->first_data_sector + (cluster - 2U) * fs->sectors_per_cluster;
        uint32_t count = fixed_root ? fs->root_dir_sectors : fs->sectors_per_cluster;
        for (uint32_t sector = 0; sector < count; ++sector) {
            if (!read_sector(fs, first + sector, sector_buf)) return false;
            for (uint32_t i = 0; i < fs->bytes_per_sector / 32U; ++i) {
                const uint8_t *raw = sector_buf + i * 32U;
                fat_dir_entry_t parsed;
                if (!fat_match_dir_entry(raw, &lfn, name, &parsed)) {
                    if (raw[0] == 0U) return false;
                    continue;
                }
                *result = parsed;
                return true;
            }
            if (++sectors_since_yield == 64U) {
                sectors_since_yield = 0;
                task_yield();
            }
        }
        if (fixed_root) break;
        cluster = get_next_cluster(fs, cluster);
        if (cluster == 0xFFFFFFFFU) break;
    }
    return false;
}

static bool looks_like_fat_bpb(const uint8_t *bpb) {
    uint16_t bytes_per_sector = read_le16(bpb + 11);
    uint8_t sectors_per_cluster = bpb[13];
    uint8_t num_fats = bpb[16];

    if (bpb[510] != 0x55 || bpb[511] != 0xAA) return false;
    if (bytes_per_sector != BLOCK_SECTOR_SIZE) return false;
    if (sectors_per_cluster == 0 || (sectors_per_cluster & (sectors_per_cluster - 1)) != 0) return false;
    if (num_fats == 0) return false;
    return true;
}

bool fat_mount(fat_fs_t *fs, block_device_t *dev, uint32_t volume_lba) {
    fat_sector_cache_grow();
    uint8_t bpb[512];
    uint32_t data_sectors;

    if (!fs || !dev) {
        kprintf("fat_mount: fs/dev NULL\n");
        return false;
    }

    kmemset(fs, 0, sizeof(*fs));

    if (!block_read(dev, volume_lba, 1, bpb)) {
        kprintf("fat_mount: block_read fallo\n");
        return false;
    }

    kprintf("fat_mount: block_read OK\n");
    kprintf("OEM: ");
    for (uint32_t i = 0; i < 8; i++) kprintf("%c", bpb[3 + i]);
    kprintf("\n");
    kprintf("Firma: %x %x\n", bpb[510], bpb[511]);

    if (!looks_like_fat_bpb(bpb)) {
        kprintf("fat_mount: BPB invalido\n");
        return false;
    }

    kprintf("fat_mount: BPB valido\n");

    fs->device = dev;
    fs->volume_lba = volume_lba;
    fs->bytes_per_sector = read_le16(bpb + 11);
    fs->sectors_per_cluster = bpb[13];
    fs->reserved_sector_count = read_le16(bpb + 14);
    fs->num_fats = bpb[16];
    fs->root_entry_count = read_le16(bpb + 17);
    fs->total_sectors = read_le16(bpb + 19);

    if (fs->total_sectors == 0) {
        fs->total_sectors = read_le32(bpb + 32);
    }

    fs->fat_size_sectors = read_le16(bpb + 22);
    fs->first_fat_sector = fs->reserved_sector_count;
    fs->root_dir_sectors = ((fs->root_entry_count * 32) + (fs->bytes_per_sector - 1)) / fs->bytes_per_sector;

    if (fs->fat_size_sectors == 0) {
        fs->fat_size_sectors = read_le32(bpb + 36);
    }

    fs->first_data_sector = fs->reserved_sector_count + (fs->num_fats * fs->fat_size_sectors) + fs->root_dir_sectors;

    kprintf("BPS=%u SPC=%u Reserved=%u FATs=%u Root=%u Total=%u FATSize=%u FirstData=%u\n",
        fs->bytes_per_sector,
        fs->sectors_per_cluster,
        fs->reserved_sector_count,
        fs->num_fats,
        fs->root_entry_count,
        fs->total_sectors,
        fs->fat_size_sectors,
        fs->first_data_sector);

    if (fs->total_sectors <= fs->first_data_sector) {
        kprintf("fat_mount: total_sectors <= first_data_sector\n");
        return false;
    }

    data_sectors = fs->total_sectors - fs->first_data_sector;
    fs->total_clusters = data_sectors / fs->sectors_per_cluster;

    kprintf("Clusters=%u\n", fs->total_clusters);

    fs->root_dir_cluster = 2;

    if (fs->total_clusters < 4085) {
        fs->type = FAT_TYPE_FAT12;
    } else if (fs->total_clusters < 65525) {
        fs->type = FAT_TYPE_FAT16;
    } else {
        fs->type = FAT_TYPE_FAT32;
    }

    kprintf("Tipo FAT=%u\n", fs->type);

    if (fs->type == FAT_TYPE_FAT32) {
        fs->root_dir_cluster = read_le32(bpb + 44);
        fs->first_data_sector = fs->reserved_sector_count + (fs->num_fats * fs->fat_size_sectors) + 0;
        fs->root_dir_sectors = 0;
        fs->total_clusters = (fs->total_sectors - fs->first_data_sector) / fs->sectors_per_cluster;
        kmemcpy(fs->volume_label, bpb + 71, 11);
        kmemcpy(fs->fs_name, bpb + 82, 8);
    } else {
        kmemcpy(fs->volume_label, bpb + 43, 11);
        kmemcpy(fs->fs_name, bpb + 54, 8);
    }

    fs->volume_label[11] = '\0';
    fs->fs_name[8] = '\0';

    kprintf("Label=%s\n", fs->volume_label);
    kprintf("FS=%s\n", fs->fs_name);

    kprintf("fat_mount OK\n");

    return true;
}

static bool parse_partition_name(const char *name, char *dev_name, uint32_t *part_index) {
    size_t len;
    if (!name || !dev_name || !part_index) return false;

    len = kstrlen(name);
    if (len < 3 || name[len - 2] != 'p') return false;
    if (name[len - 1] < '1' || name[len - 1] > '4') return false;
    if (len >= 8) return false;

    kmemset(dev_name, 0, 8);
    for (size_t i = 0; i < len - 2 && i < 7; i++) {
        dev_name[i] = name[i];
    }
    *part_index = (uint32_t)(name[len - 1] - '1');
    return true;
}

static bool read_partition_lba(block_device_t *dev, uint32_t part_index, uint32_t *lba) {
    uint8_t mbr[512];
    uint32_t off;
    if (!dev || !lba || part_index >= 4) return false;
    if (!block_read(dev, 0, 1, mbr)) return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return false;
    off = 446 + (part_index * 16);
    if (mbr[off + 4] == 0) return false;
    *lba = read_le32(mbr + off + 8);
    return *lba != 0;
}

static bool is_supported_partition_type(uint8_t type) {
    switch (type) {
        case 0x01: /* FAT12 */
        case 0x04: /* FAT16 <32M */
        case 0x06: /* FAT16 */
        case 0x0B: /* FAT32 CHS */
        case 0x0C: /* FAT32 LBA */
        case 0x0E: /* FAT16 LBA */
        case 0x11: /* FAT12 oculta */
        case 0x14: /* FAT16 oculta <32M */
        case 0x16: /* FAT16 oculta */
        case 0x1B: /* FAT32 CHS oculta */
        case 0x1C: /* FAT32 LBA oculta */
        case 0x1E: /* FAT16 LBA oculta */
            return true;
        default:
            return false;
    }
}

static bool is_extended_partition_type(uint8_t type) {
    return type == 0x05 || type == 0x0F || type == 0x85;
}

/* Busca volúmenes FAT lógicos encadenados mediante EBR. */
static bool fat_try_mount_extended(fat_fs_t *fs, block_device_t *dev,
                                   uint32_t extended_base,
                                   const char *active_name UNUSED) {
    uint8_t ebr[512];
    uint32_t ebr_lba = extended_base;

    if (!fs || !dev || !extended_base) return false;
    for (uint32_t depth = 0; depth < 16U; depth++) {
        uint32_t first_off = 446U;
        uint32_t next_off = 462U;
        uint8_t first_type;
        uint8_t next_type;
        uint32_t relative_lba;
        uint32_t volume_lba;
        uint32_t next_relative;

        if (dev->sector_count && ebr_lba >= dev->sector_count) return false;
        if (!block_read(dev, ebr_lba, 1, ebr)) return false;
        if (ebr[510] != 0x55 || ebr[511] != 0xAA) return false;

        first_type = ebr[first_off + 4U];
        relative_lba = read_le32(ebr + first_off + 8U);
        volume_lba = ebr_lba + relative_lba;
        if (is_supported_partition_type(first_type) && relative_lba &&
            (!dev->sector_count || volume_lba < dev->sector_count) &&
            fat_mount(fs, dev, volume_lba)) return true;

        next_type = ebr[next_off + 4U];
        next_relative = read_le32(ebr + next_off + 8U);
        if (!is_extended_partition_type(next_type) || !next_relative) break;
        if (next_relative > 0xFFFFFFFFU - extended_base) break;
        ebr_lba = extended_base + next_relative;
    }
    return false;
}

static bool partition_entry_lba(const uint8_t *sector, uint32_t index,
                                const block_device_t *dev, uint32_t *lba_out) {
    uint32_t off;
    uint8_t bootable;
    uint8_t type;
    uint32_t lba;
    uint32_t sectors;

    if (!sector || !lba_out || index >= 4) return false;

    off = 446 + (index * 16);
    bootable = sector[off + 0];
    type = sector[off + 4];
    lba = read_le32(sector + off + 8);
    sectors = read_le32(sector + off + 12);

    if (type == 0 || lba == 0 || sectors == 0) return false;
    if (bootable != 0x00 && bootable != 0x80) return false;
    if (!is_supported_partition_type(type)) return false;

    if (dev && dev->sector_count) {
        if (lba >= dev->sector_count) return false;
        if (sectors > dev->sector_count - lba) return false;
    }

    *lba_out = lba;
    return true;
}

static bool fat_try_mount_device(fat_fs_t *fs, block_device_t *dev,
                                 const char *active_name UNUSED) {
    uint8_t sector[512];

    if (!fs || !dev) return false;
    if (bfs4_mount(fs, dev, 0U)) return true;

    /*
     * Primero probamos superfloppy/volumen directo.
     *
     * Un boot sector FAT tambien termina en 55 AA, pero los bytes 446..509
     * pertenecen al codigo del bootloader, no a una tabla de particiones.
     * Si interpretamos esos bytes como MBR, aparecen "particiones" falsas y
     * nunca llegamos a montar LBA 0. Eso es justo lo que pasa en la imagen
     * de disquete de BlesKernOS.
     */
    if (fat_mount(fs, dev, 0)) return true;

    /*
     * Si LBA 0 no era FAT, entonces si intentamos tratarlo como MBR.
     * Solo aceptamos entradas de particion razonables para evitar falsos
     * positivos dentro de codigo de arranque.
     */
    if (!block_read(dev, 0, 1, sector)) return false;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return false;

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t lba = 0;
        if (!partition_entry_lba(sector, i, dev, &lba)) continue;
        if (fat_mount(fs, dev, lba)) return true;
    }

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t off = 446U + i * 16U;
        uint32_t lba = read_le32(sector + off + 8U);
        uint32_t count = read_le32(sector + off + 12U);
        if (sector[off + 4U] == 0x83U && lba && count &&
            lba < dev->sector_count && count <= dev->sector_count - lba &&
            bfs4_mount(fs, dev, lba)) return true;
    }

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t off = 446U + i * 16U;
        uint8_t type = sector[off + 4U];
        uint32_t base = read_le32(sector + off + 8U);
        if (!is_extended_partition_type(type) || !base) continue;
        if (fat_try_mount_extended(fs, dev, base, active_name)) return true;
    }

    return false;
}

static inline bool fat_running_in_user_cpl(void) {
#if defined(__arm__) && !defined(__aarch64__)
    /* The ARM32 port currently enters FAT from SVC/kernel mode only.  Once
     * ARM user mode exists this helper will become an arch hook rather than
     * encoding x86 CPL semantics in the filesystem. */
    return false;
#else
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    return (cs & 0x3U) == 0x3U;
#endif
}

static inline uint32_t fat_irq_save(void) {
#if defined(__arm__) && !defined(__aarch64__)
    uint32_t cpsr;
    __asm__ volatile ("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) : : "memory", "cc");
    return cpsr;
#else
    uint32_t flags;
    if (fat_running_in_user_cpl()) {
        __asm__ volatile ("pushfl; popl %0" : "=r"(flags) : : "memory");
        return flags;
    }
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
#endif
}

static inline void fat_irq_restore(uint32_t flags) {
#if defined(__arm__) && !defined(__aarch64__)
    /* CPSR.I == 0 means IRQs were enabled on entry. */
    if ((flags & (1U << 7)) == 0U)
        __asm__ volatile ("cpsie i" : : : "memory", "cc");
#else
    if (fat_running_in_user_cpl()) return;
    if (flags & (1U << 9)) {
        __asm__ volatile ("sti" : : : "memory");
    }
#endif
}

/*
 * IMPORTANTE:
 * En i386, el compilador puede transformar una copia de struct en REP MOVSL.
 * REP MOVSL usa DS:ESI como origen y ES:EDI como destino.
 *
 * Si alguna IRQ/syscall/task/ring3 dejó ES con un selector que no es el
 * selector de datos del kernel, la copia "*fs = g_current_fs" dispara
 * GENERAL PROTECTION FAULT (#13) en hardware real.
 *
 * Este helper deja los segmentos de datos apuntando otra vez al data segment
 * del kernel antes de copiar estructuras.
 */
static inline void fat_reload_kernel_segments(void) {
#if defined(__arm__) && !defined(__aarch64__)
    /* ARM has no x86-style DS/ES segment registers. */
    __asm__ volatile ("" : : : "memory");
#else
    __asm__ volatile (
        "cld\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        :
        : "ax", "memory"
    );
#endif
}

bool fat_get_current(fat_fs_t *fs) {
    uint32_t flags;

    if (!fs) return false;

    /*
     * Mantener la copia corta e indivisible: si una IRQ entra entre recargar
     * ES y copiar, y el handler no restaura ES, vuelve el bug.
     */
    flags = fat_irq_save();
    /*
     * Desde ring 3 no podemos cargar selectores DPL=0 en DS/ES/FS/GS.
     * En ese caso mantenemos los selectores actuales del task usuario,
     * que siguen siendo flat y alcanzan la memoria compartida del kernel.
     */
    if (!fat_running_in_user_cpl())
        fat_reload_kernel_segments();

    if (!g_fs_ready) {
        fat_irq_restore(flags);
        return false;
    }

    /*
     * No usar "*fs = g_current_fs".
     * Eso generó:
     *   rep movsl %ds:(%esi),%es:(%edi)
     * y en hardware real explotó con #GP si ES estaba sucio.
     */
    fs->device = g_current_fs.device;
    fs->volume_lba = g_current_fs.volume_lba;
    fs->type = g_current_fs.type;
    fs->bytes_per_sector = g_current_fs.bytes_per_sector;
    fs->sectors_per_cluster = g_current_fs.sectors_per_cluster;
    fs->reserved_sector_count = g_current_fs.reserved_sector_count;
    fs->num_fats = g_current_fs.num_fats;
    fs->root_entry_count = g_current_fs.root_entry_count;
    fs->total_sectors = g_current_fs.total_sectors;
    fs->fat_size_sectors = g_current_fs.fat_size_sectors;
    fs->first_fat_sector = g_current_fs.first_fat_sector;
    fs->first_data_sector = g_current_fs.first_data_sector;
    fs->total_clusters = g_current_fs.total_clusters;
    fs->root_dir_cluster = g_current_fs.root_dir_cluster;
    fs->root_dir_sectors = g_current_fs.root_dir_sectors;

    for (uint32_t i = 0; i < sizeof(fs->volume_label); i++) {
        fs->volume_label[i] = g_current_fs.volume_label[i];
    }

    for (uint32_t i = 0; i < sizeof(fs->fs_name); i++) {
        fs->fs_name[i] = g_current_fs.fs_name[i];
    }

    fat_irq_restore(flags);
    return true;
}
bool fat_mount_named(fat_fs_t *fs, const char *name) {
    block_device_t *dev;
    char dev_name[8];
    uint32_t part_index = 0;
    uint32_t lba = 0;

    if (!fs || !name) {
        kprintf("fat_mount_named: argumento NULL\n");
        return false;
    }

    if (parse_partition_name(name, dev_name, &part_index)) {
        kprintf("Es particion: %s p%u\n", dev_name, part_index);

        dev = block_get(dev_name);
        if (!dev) {
            kprintf("block_get(%s) = NULL\n", dev_name);
            return false;
        }

        kprintf("Dispositivo encontrado\n");

        if (!read_partition_lba(dev, part_index, &lba)) {
            kprintf("read_partition_lba fallo\n");
            return false;
        }

        kprintf("LBA = %u\n", lba);

        if (!bfs4_mount(fs, dev, lba) && !fat_mount(fs, dev, lba)) {
            kprintf("fat_mount fallo\n");
            return false;
        }
    } else {
        kprintf("No es particion, buscando %s\n", name);

        dev = block_get(name);
        if (!dev) {
            kprintf("block_get(%s) = NULL\n", name);
            return false;
        }

        kprintf("Dispositivo encontrado\n");

        if (!fat_try_mount_device(fs, dev, name)) {
            kprintf("fat_try_mount_device fallo\n");
            return false;
        }

        kprintf("fat_try_mount_device OK\n");
    }

    return true;
}

bool fat_set_active(const char *name) {
    fat_fs_t fs;

    if (!name) {
        kprintf("fat_set_active: name NULL\n");
        return false;
    }
    kprintf("fat_set_active(%s)\n", name);
    if (!fat_mount_named(&fs, name)) return false;

    g_current_fs = fs;
    g_fs_ready = true;
    g_active_name = name;

    kprintf("fat_set_active OK\n");
    return true;
}

bool fat_mount_default(void) {
    kprintf("[FAT] entrando a fat_mount_default\n");
    kprintf("[FAT] dispositivos registrados: %u\n", block_count());

    for (uint32_t debug_i = 0; debug_i < block_count(); debug_i++) {
        block_device_t *debug_dev = block_at(debug_i);
        if (debug_dev) {
            kprintf("[FAT] dev[%u]=%s tipo=%u sectores=%u\n",
                    debug_i,
                    debug_dev->name,
                    debug_dev->type,
                    debug_dev->sector_count);
        }
    }

    const char *preferred_ata[] = {"ata0", "ata1", "ata2", "ata3"};
    const char *preferred_ahci[] = {"ahci0", "ahci1", "ahci2", "ahci3"};
    const char *preferred_sd[] = {"sd0", "sd1", "sd2", "sd3"};
    const char *preferred_usb[] = {"usb0", "usb1", "usb2", "usb3"};
    bool have_ata = false;
    bool have_sd = false;

    /*
     * Si hay un disco ATA registrado, probarlo antes que fd0.
     *
     * QEMU puede exponer el controlador de disquete aunque el arranque real
     * venga desde IDE/ATA. Intentar fd0 primero en ese caso dispara lecturas
     * FDC fallidas en LBA 0 antes de llegar al disco correcto.
     */
    for (uint32_t i = 0; i < block_count(); i++) {
        block_device_t *dev = block_at(i);
        if (dev && dev->type == BLOCK_DEVICE_ATA) have_ata = true;
        if (dev && dev->type == BLOCK_DEVICE_SD) have_sd = true;
    }

    /* ARM boards normally boot from SD.  Keep this in the shared FAT probe
     * rather than teaching the ARM platform about FAT partition layouts. */
    if (have_sd) {
        for (uint32_t i = 0; i < sizeof(preferred_sd)/sizeof(preferred_sd[0]); i++) {
            kprintf("Intentando montar %s...\n", preferred_sd[i]);
            if (fat_set_active(preferred_sd[i])) {
                kprintf("Montado %s OK\n", preferred_sd[i]);
                return true;
            }
            kprintf("Fallo %s\n", preferred_sd[i]);
        }
    }

    if (have_ata) {
        for (uint32_t i = 0; i < sizeof(preferred_ata)/sizeof(preferred_ata[0]); i++) {
            kprintf("Intentando montar %s...\n", preferred_ata[i]);

            if (fat_set_active(preferred_ata[i])) {
                kprintf("Montado %s OK\n", preferred_ata[i]);
                return true;
            }

            kprintf("Fallo %s\n", preferred_ata[i]);
        }
    }

    /* A SATA-only boot has no legacy ataN device.  Probe AHCI before USB,
     * while retaining the historical IDE boot preference when both exist. */
    for (uint32_t i = 0; i < sizeof(preferred_ahci)/sizeof(preferred_ahci[0]); i++) {
        if (!block_get(preferred_ahci[i])) continue;
        kprintf("Intentando montar %s...\n", preferred_ahci[i]);
        if (fat_set_active(preferred_ahci[i])) {
            kprintf("Montado %s OK\n", preferred_ahci[i]);
            return true;
        }
        kprintf("Fallo %s\n", preferred_ahci[i]);
    }

    for (uint32_t i = 0; i < sizeof(preferred_usb)/sizeof(preferred_usb[0]); i++) {
        kprintf("Intentando montar %s...\n", preferred_usb[i]);

        if (fat_set_active(preferred_usb[i])) {
            kprintf("Montado %s OK\n", preferred_usb[i]);
            return true;
        }

        kprintf("Fallo %s\n", preferred_usb[i]);
    }

    return false;
}

const char *fat_get_active_name(void) {
    return g_active_name;
}

void fat_print_info(const fat_fs_t *fs) {
    if (!fs) return;
    if (fs->type == FAT_TYPE_BFS4)
        kprintf("BFS4 device=%s lba=%u label=%s\n",
                fs->device ? fs->device->name : "?", fs->volume_lba,
                fs->volume_label);
    else
        kprintf("FAT%u device=%s lba=%u label=%s\n", fs->type,
                fs->device ? fs->device->name : "?", fs->volume_lba,
                fs->volume_label);
    kprintf("  bytes/sector=%u sectors/cluster=%u\n", fs->bytes_per_sector, fs->sectors_per_cluster);
    kprintf("  clusters=%u root_cluster=%u\n", fs->total_clusters, fs->root_dir_cluster);
}

bool fat_list_root(const fat_fs_t *fs, fat_dir_entry_t *entries, uint32_t max_entries, uint32_t *count) {
    if (fs && fs->type == FAT_TYPE_BFS4)
        return bfs4_list(fs, NULL, entries, NULL, max_entries, count);
    if (!fs || !entries || !count) return false;
    if (fs->type == FAT_TYPE_FAT12 || fs->type == FAT_TYPE_FAT16) {
        return list_directory_chain(fs, 0, entries, max_entries, count, NULL);
    }
    return list_directory_chain(fs, fs->root_dir_cluster, entries, max_entries, count, NULL);
}

bool fat_list_dir(const fat_fs_t *fs, const fat_dir_entry_t *dir, fat_dir_entry_t *entries, uint32_t max_entries, uint32_t *count) {
    if (fs && fs->type == FAT_TYPE_BFS4)
        return bfs4_list(fs, dir, entries, NULL, max_entries, count);
    if (!fs || !entries || !count) return false;
    if (!dir || dir->first_cluster == 0) return fat_list_root(fs, entries, max_entries, count);
    if (!dir->is_directory) return false;
    return list_directory_chain(fs, dir->first_cluster, entries, max_entries, count, NULL);
}

bool fat_list_dir_metadata(const fat_fs_t *fs, const fat_dir_entry_t *dir,
    fat_dir_entry_t *entries, fat_dir_metadata_t *metadata,
    uint32_t max_entries, uint32_t *count) {
    if (fs && fs->type == FAT_TYPE_BFS4)
        return bfs4_list(fs, dir, entries, metadata, max_entries, count);
    if (!fs || !entries || !metadata || !count || (dir && !dir->is_directory)) return false;
    uint32_t cluster = dir ? dir->first_cluster : 0U;
    if (!cluster && fs->type == FAT_TYPE_FAT32) cluster = fs->root_dir_cluster;
    return list_directory_chain(fs, cluster, entries, max_entries, count, metadata);
}

static bool fat_find_in_dir(const fat_fs_t *fs, const fat_dir_entry_t *dir,
                            const char *name, fat_dir_entry_t *entry) {
    uint32_t cluster;
    if (!fs || !name || !entry) return false;
    cluster = (!dir || dir->first_cluster == 0U)
        ? ((fs->type == FAT_TYPE_FAT32) ? fs->root_dir_cluster : 0U)
        : dir->first_cluster;
    if (dir && !dir->is_directory) return false;
    return find_directory_chain_entry(fs, cluster, name, entry);
}

bool fat_find_file(const fat_fs_t *fs, const char *name, fat_dir_entry_t *entry) {
    if (fs && fs->type == FAT_TYPE_BFS4) {
        char path[FAT_MAX_NAME + 2U];
        if (!name || kstrlen(name) >= FAT_MAX_NAME) return false;
        path[0] = '/'; kstrcpy(path + 1, name);
        return bfs4_resolve(fs, path, entry);
    }
    return fat_find_in_dir(fs, NULL, name, entry);
}

static bool fat_resolve_path_uncached(const fat_fs_t *fs, const char *path, fat_dir_entry_t *entry) {
    fat_dir_entry_t current;
    fat_dir_entry_t next;
    char component[FAT_MAX_NAME];
    uint32_t pos = 0;

    if (!fs || !path || !entry) return false;
    if (path[0] != '/') return false;

    kmemset(&current, 0, sizeof(current));
    current.is_directory = true;
    current.first_cluster = (fs->type == FAT_TYPE_FAT32) ? fs->root_dir_cluster : 0;
    current.name[0] = '/';
    current.name[1] = '\0';

    while (*path == '/') path++;
    if (*path == '\0') {
        *entry = current;
        return true;
    }

    while (true) {
        pos = 0;
        kmemset(component, 0, sizeof(component));
        while (*path && *path != '/') {
            if (pos + 1 >= sizeof(component)) return false;
            component[pos++] = *path++;
        }
        component[pos] = '\0';

        while (*path == '/') path++;
        if (component[0] == '\0' || kstrcmp(component, ".") == 0) {
            if (*path == '\0') {
                *entry = current;
                return true;
            }
            continue;
        }
        if (kstrcmp(component, "..") == 0 && current.first_cluster == 0) {
            if (*path == '\0') {
                *entry = current;
                return true;
            }
            continue;
        }

        if (!current.is_directory) return false;
        if (!fat_find_in_dir(fs, &current, component, &next)) return false;

        current = next;
        if (*path == '\0') {
            *entry = current;
            return true;
        }
    }
}

/* Positive path entries avoid re-walking all parents during the repeated
 * symlink/stat/identity checks of a single Linux openat. Every disk write
 * advances the sector-cache generation, so size, cluster and names cannot
 * survive a mutation. Failed lookups (including I/O failures) aren't cached. */
#define FAT_PATH_CACHE_SLOTS 32U
static struct {
    block_device_t *device;
    uint32_t volume_lba, generation;
    char path[260];
    fat_dir_entry_t entry;
} g_path_cache[FAT_PATH_CACHE_SLOTS];

bool fat_resolve_path(const fat_fs_t *fs, const char *path, fat_dir_entry_t *entry) {
    if (fs && fs->type == FAT_TYPE_BFS4) return bfs4_resolve(fs, path, entry);
    W3P_SCOPE(W3P_FAT_RESOLVE, 0U);
    uint32_t hash = 2166136261U, length = 0U, generation, irq;
    if (!fs || !path || !entry) return false;
    for (const char *p = path; *p; ++p) {
        uint8_t ch = (uint8_t)*p;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        hash = (hash ^ ch) * 16777619U;
        ++length;
    }
    if (length >= sizeof(g_path_cache[0].path))
        return fat_resolve_path_uncached(fs, path, entry);
    uint32_t slot = hash & (FAT_PATH_CACHE_SLOTS - 1U);
    irq = kspin_lock_irqsave(&g_sector_cache_lock);
    generation = g_sector_cache_generation;
    if (g_path_cache[slot].generation == generation &&
        g_path_cache[slot].device == fs->device &&
        g_path_cache[slot].volume_lba == fs->volume_lba &&
        name_equals(g_path_cache[slot].path, path)) {
        *entry = g_path_cache[slot].entry;
        kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
        return true;
    }
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
    if (!fat_resolve_path_uncached(fs, path, entry)) return false;
    irq = kspin_lock_irqsave(&g_sector_cache_lock);
    if (g_sector_cache_generation == generation) {
        g_path_cache[slot].device = fs->device;
        g_path_cache[slot].volume_lba = fs->volume_lba;
        g_path_cache[slot].entry = *entry;
        kmemcpy(g_path_cache[slot].path, path, length + 1U);
        g_path_cache[slot].generation = generation;
    }
    kspin_unlock_irqrestore(&g_sector_cache_lock, irq);
    return true;
}

bool fat_read_file_at(const fat_fs_t *fs, const fat_dir_entry_t *entry,
                      uint32_t file_offset, void *buffer, uint32_t max_size,
                      uint32_t *bytes_read) {
    if (fs && fs->type == FAT_TYPE_BFS4)
        return bfs4_read(fs, entry, file_offset, buffer, max_size, bytes_read);
    uint8_t *cluster_buf = NULL;
    uint32_t cluster = entry ? entry->first_cluster : 0U;
    uint32_t bytes_left = entry ? entry->size : 0U;
    uint32_t out_offset = 0U;
    uint32_t cluster_size;
    uint32_t skipped = 0U;
    uint32_t clusters_since_yield = 0U;
    bool io_ok = true;

    if (!fs || !entry || !buffer || !bytes_read) return false;
    *bytes_read = 0U;
    if (entry->is_directory) return false;
    if (file_offset >= entry->size || max_size == 0U) return true;
    if (!cluster) return false;
    if (file_offset == 0U && max_size >= entry->size)
        return fat_read_full_file_fast(fs, entry, buffer, bytes_read);

    cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return false;

    while (cluster >= 2U && bytes_left > 0U) {
        uint32_t cluster_file_bytes;
        uint32_t local_offset = 0U;
        cluster_file_bytes = cluster_size;
        if (cluster_file_bytes > bytes_left) cluster_file_bytes = bytes_left;

        if (skipped + cluster_file_bytes <= file_offset) {
            skipped += cluster_file_bytes;
            bytes_left -= cluster_file_bytes;
        } else {
            uint32_t chunk = cluster_file_bytes;
            /* Seek walks allocation links, not the contents of every
             * preceding cluster. ELF loaders repeatedly read near EOF. */
            if (!read_cluster(fs, cluster, cluster_buf)) {
                io_ok = false;
                break;
            }
            if (file_offset > skipped) {
                local_offset = file_offset - skipped;
                chunk -= local_offset;
            }
            if (chunk > max_size - out_offset) chunk = max_size - out_offset;
            if (chunk) {
                kmemcpy((uint8_t *)buffer + out_offset,
                        cluster_buf + local_offset, chunk);
                out_offset += chunk;
            }
            skipped += cluster_file_bytes;
            bytes_left -= cluster_file_bytes;
        }

        /* Yielding for every 512-byte cluster made a 1 MiB SFX perform about
         * two thousand scheduler round trips.  Keep cooperation, but only
         * once per 64 clusters (32 KiB on the current FAT32 images). */
        if (++clusters_since_yield >= 64U) {
            clusters_since_yield = 0U;
            bootsplash_pulse();
            task_yield();
        }
        if (bytes_left == 0U || out_offset == max_size) break;
        cluster = get_next_cluster(fs, cluster);
        if (cluster == 0xFFFFFFFFU) {
            io_ok = false;
            break;
        }
    }

    *bytes_read = out_offset;
    kfree(cluster_buf);
    return io_ok;
}

bool fat_read_file(const fat_fs_t *fs, const fat_dir_entry_t *entry, void *buffer, uint32_t max_size, uint32_t *bytes_read) {
    return fat_read_file_at(fs, entry, 0, buffer, max_size, bytes_read);
}

static bool fat_device_writable(const fat_fs_t *fs) {
    return fs && fs->device && !fs->device->read_only && fs->device->write;
}

/*
 * ATA PIO is polled synchronously.  Sending a whole 255-sector command is
 * legal, but on the emulated secondary disk it can time out while a Win32
 * application is updating its INI file.  FAT metadata is read and committed
 * in these helpers, so bound those transactions to a short, reliable burst.
 */
#define FAT_METADATA_IO_SECTORS 32U

static bool write_sector(const fat_fs_t *fs, uint32_t sector, const void *buffer) {
    W3P_SCOPE(W3P_FAT_SECTOR_W, BLOCK_SECTOR_SIZE);
    uint32_t lba;
    uint32_t generation;

    if (!fat_device_writable(fs) || !buffer) return false;
    lba = fs->volume_lba + sector;
    if (!block_write(fs->device, lba, 1, buffer)) {
        fat_sector_cache_invalidate_range(fs->device, lba, 1U);
        return false;
    }

    fat_sector_cache_invalidate_range(fs->device, lba, 1U);
    generation = fat_sector_cache_get_generation();
    fat_sector_cache_store(fs->device, lba, buffer, generation);
    return true;
}

/* Transfiere sectores consecutivos en comandos ATA grandes. El codigo anterior
 * emitia un WRITE PIO + CACHE FLUSH por cada sector de 512 bytes. */
static bool fat_read_sector_run(const fat_fs_t *fs, uint32_t sector,
                                uint32_t count, void *buffer) {
    uint8_t *out = (uint8_t *)buffer;
    if (!fs || !fs->device || !buffer || !count) return false;
    if (fs->bytes_per_sector != BLOCK_SECTOR_SIZE) return false;

    while (count) {
        uint32_t lba = fs->volume_lba + sector;
        uint32_t cache_generation;
        if (fat_sector_cache_read(fs->device, lba, out)) {
            sector++;
            count--;
            out += BLOCK_SECTOR_SIZE;
            continue;
        }

        /* Mantener comandos PIO agrupados, pero cortar antes de un sector que
         * ya esta residente para no releerlo ni expulsar datos utiles. */
        uint32_t miss_count = 1U;
        while (miss_count < count &&
               miss_count < FAT_METADATA_IO_SECTORS &&
               !fat_sector_cache_contains(fs->device, lba + miss_count))
            miss_count++;

        cache_generation = fat_sector_cache_get_generation();
        if (!block_read(fs->device, lba, (uint8_t)miss_count, out))
            return false;
        for (uint32_t i = 0; i < miss_count; i++)
            fat_sector_cache_store(fs->device, lba + i,
                                   out + i * BLOCK_SECTOR_SIZE,
                                   cache_generation);
        sector += miss_count;
        count -= miss_count;
        out += miss_count * BLOCK_SECTOR_SIZE;
    }
    bootsplash_pulse();
    return true;
}

static bool fat_write_sector_run(const fat_fs_t *fs, uint32_t sector,
                                 uint32_t count, const void *buffer) {
    W3P_SCOPE(W3P_FAT_SECTOR_W, count * BLOCK_SECTOR_SIZE);
    const uint8_t *in = (const uint8_t *)buffer;
    uint32_t first_lba;
    uint32_t final_lba;
    uint32_t generation;
    if (!fat_device_writable(fs) || !buffer || !count) return false;
    first_lba = fs->volume_lba + sector;
    final_lba = first_lba + count;
    while (count) {
        uint8_t chunk = count > FAT_METADATA_IO_SECTORS
            ? FAT_METADATA_IO_SECTORS : (uint8_t)count;
        if (!block_write(fs->device, fs->volume_lba + sector, chunk, in)) {
            fat_sector_cache_invalidate_range(fs->device, first_lba,
                                              final_lba - first_lba);
            return false;
        }
        sector += chunk;
        count -= chunk;
        in += (uint32_t)chunk * fs->bytes_per_sector;
    }
    fat_sector_cache_invalidate_range(fs->device, first_lba,
                                      final_lba - first_lba);
    generation = fat_sector_cache_get_generation();
    for (uint32_t i = 0U; i < final_lba - first_lba; i++)
        fat_sector_cache_store(fs->device, first_lba + i,
                               (const uint8_t *)buffer +
                                   i * fs->bytes_per_sector,
                               generation);
    return true;
}


static uint16_t fat12_table_get(const uint8_t *fat, uint32_t cluster) {
    uint32_t offset = cluster + cluster / 2;
    uint16_t value = read_le16(fat + offset);
    return (cluster & 1) ? (value >> 4) : (value & 0x0FFF);
}

static void fat12_table_set(uint8_t *fat, uint32_t cluster, uint16_t value) {
    uint32_t offset = cluster + cluster / 2;
    value &= 0x0FFF;
    if (cluster & 1) {
        fat[offset] = (uint8_t)((fat[offset] & 0x0F) | (value << 4));
        fat[offset + 1] = (uint8_t)(value >> 4);
    } else {
        fat[offset] = (uint8_t)value;
        fat[offset + 1] = (uint8_t)((fat[offset + 1] & 0xF0) |
                                    ((value >> 8) & 0x0F));
    }
}

static uint32_t fat_table_get(const fat_fs_t *fs, const uint8_t *fat,
                              uint32_t cluster) {
    if (!fs || !fat) return 0xFFFFFFFF;
    if (fs->type == FAT_TYPE_FAT12) return fat12_table_get(fat, cluster);
    if (fs->type == FAT_TYPE_FAT16) {
        return read_le16(fat + cluster * 2U);
    }
    if (fs->type == FAT_TYPE_FAT32) {
        return read_le32(fat + cluster * 4U) & 0x0FFFFFFFU;
    }
    return 0xFFFFFFFF;
}

static void fat_table_set(const fat_fs_t *fs, uint8_t *fat, uint32_t cluster,
                          uint32_t value) {
    if (!fs || !fat) return;
    if (fs->type == FAT_TYPE_FAT12) {
        fat12_table_set(fat, cluster, (uint16_t)value);
    } else if (fs->type == FAT_TYPE_FAT16) {
        uint32_t offset = cluster * 2U;
        fat[offset] = (uint8_t)value;
        fat[offset + 1] = (uint8_t)(value >> 8);
    } else if (fs->type == FAT_TYPE_FAT32) {
        uint32_t offset = cluster * 4U;
        uint32_t old_high = fat[offset + 3] & 0xF0U;
        value &= 0x0FFFFFFFU;
        fat[offset] = (uint8_t)value;
        fat[offset + 1] = (uint8_t)(value >> 8);
        fat[offset + 2] = (uint8_t)(value >> 16);
        fat[offset + 3] = (uint8_t)((value >> 24) | old_high);
    }
}

static uint32_t fat_table_entry_offset(const fat_fs_t *fs, uint32_t cluster) {
    if (!fs) return 0U;
    if (fs->type == FAT_TYPE_FAT12) return cluster + cluster / 2U;
    if (fs->type == FAT_TYPE_FAT16) return cluster * 2U;
    return cluster * 4U;
}

static uint32_t fat_table_entry_size(const fat_fs_t *fs) {
    if (!fs) return 0U;
    if (fs->type == FAT_TYPE_FAT12 || fs->type == FAT_TYPE_FAT16) return 2U;
    return 4U;
}

static void fat_table_set_dirty(const fat_fs_t *fs, uint8_t *fat,
                                uint32_t cluster, uint32_t value,
                                uint8_t *dirty) {
    uint32_t offset;
    uint32_t bytes;
    fat_table_set(fs, fat, cluster, value);
    if (!fs || !dirty) return;
    offset = fat_table_entry_offset(fs, cluster);
    bytes = fat_table_entry_size(fs);
    dirty[offset / fs->bytes_per_sector] = 1U;
    dirty[(offset + bytes - 1U) / fs->bytes_per_sector] = 1U;
}


static uint32_t fat_eoc_value(const fat_fs_t *fs) {
    if (!fs) return 0xFFFFFFFF;
    if (fs->type == FAT_TYPE_FAT12) return 0x0FFF;
    if (fs->type == FAT_TYPE_FAT16) return 0xFFFF;
    if (fs->type == FAT_TYPE_FAT32) return 0x0FFFFFFF;
    return 0xFFFFFFFF;
}

static bool fat_cluster_is_eoc(const fat_fs_t *fs, uint32_t cluster) {
    if (!fs) return true;
    if (fs->type == FAT_TYPE_FAT12) return cluster >= FAT_CLUSTER_END_12;
    if (fs->type == FAT_TYPE_FAT16) return cluster >= FAT_CLUSTER_END_16;
    if (fs->type == FAT_TYPE_FAT32) return cluster >= FAT_CLUSTER_END_32;
    return true;
}

typedef struct {
    const fat_fs_t *fs;
    uint8_t *table;
    uint32_t *owners;
    uint8_t *directories_seen;
    uint32_t cluster_limit;
    uint32_t next_owner;
    fat_check_report_t *report;
    fat_repair_report_t *repair;
} fat_check_context_t;

static void fat_check_truncate(fat_check_context_t *context,
                               uint32_t cluster) {
    if (!context || !context->repair || cluster < 2U ||
        cluster >= context->cluster_limit) return;
    fat_table_set(context->fs, context->table, cluster,
                  fat_eoc_value(context->fs));
    context->repair->chains_truncated++;
}

static bool fat_check_cluster_is_bad(const fat_fs_t *fs, uint32_t value) {
    if (fs->type == FAT_TYPE_FAT12) return value == FAT_CLUSTER_BAD_12;
    if (fs->type == FAT_TYPE_FAT16) return value == FAT_CLUSTER_BAD_16;
    return fs->type == FAT_TYPE_FAT32 && value == FAT_CLUSTER_BAD_32;
}

static bool fat_check_cluster_is_reserved(const fat_fs_t *fs,
                                          uint32_t value) {
    if (fs->type == FAT_TYPE_FAT12)
        return value >= 0x0FF0U && value < FAT_CLUSTER_BAD_12;
    if (fs->type == FAT_TYPE_FAT16)
        return value >= 0xFFF0U && value < FAT_CLUSTER_BAD_16;
    return fs->type == FAT_TYPE_FAT32 &&
           value >= 0x0FFFFFF0U && value < FAT_CLUSTER_BAD_32;
}

static uint32_t fat_check_chain(fat_check_context_t *context,
                                const fat_dir_entry_t *entry,
                                uint32_t owner) {
    const fat_fs_t *fs = context->fs;
    uint32_t cluster = entry->first_cluster;
    uint32_t previous = 0U;
    uint32_t count = 0;
    uint32_t fragments = 0;
    uint32_t cluster_bytes =
        fs->bytes_per_sector * fs->sectors_per_cluster;
    uint32_t expected = entry->is_directory ? 0U :
        entry->size / cluster_bytes +
        (entry->size % cluster_bytes != 0U ? 1U : 0U);

    if (cluster == 0U) {
        if (entry->is_directory || entry->size != 0U) {
            context->report->invalid_chains++;
        }
        if (!entry->is_directory && expected != 0U)
            context->report->size_mismatches++;
        return 0;
    }

    while (cluster >= 2U && cluster < context->cluster_limit) {
        uint32_t next;
        if (count >= fs->total_clusters) {
            context->report->circular_chains++;
            break;
        }
        if (context->owners[cluster] != 0U) {
            if (context->owners[cluster] == owner) {
                context->report->circular_chains++;
                fat_check_truncate(context, previous);
            } else {
                context->report->crosslinked_clusters++;
                if (previous)
                    fat_check_truncate(context, previous);
                else if (context->repair)
                    context->repair->unrepaired_crosslinks++;
            }
            break;
        }
        context->owners[cluster] = owner;
        if (!count) fragments = 1U;
        count++;
        context->report->referenced_clusters++;
        next = fat_table_get(fs, context->table, cluster);
        if (next == 0U || next == 1U) {
            context->report->invalid_chains++;
            fat_check_truncate(context, cluster);
            break;
        }
        if (fat_check_cluster_is_bad(fs, next) ||
            fat_check_cluster_is_reserved(fs, next)) {
            context->report->invalid_chains++;
            fat_check_truncate(context, cluster);
            break;
        }
        if (fat_cluster_is_eoc(fs, next)) break;
        if (next < 2U || next >= context->cluster_limit) {
            context->report->invalid_chains++;
            fat_check_truncate(context, cluster);
            break;
        }
        if (context->repair && !entry->is_directory && expected != 0U &&
            count == expected) {
            fat_check_truncate(context, cluster);
            break;
        }
        if (!entry->is_directory && next != cluster + 1U) fragments++;
        previous = cluster;
        cluster = next;
    }

    if (cluster < 2U || cluster >= context->cluster_limit) {
        context->report->invalid_chains++;
    }
    if (!entry->is_directory && count != expected) {
        context->report->size_mismatches++;
    }
    if (!entry->is_directory && fragments) {
        context->report->total_fragments += fragments;
        if (fragments > 1U) context->report->fragmented_files++;
    }
    return count;
}

static bool fat_check_dot_entry(const fat_dir_entry_t *entry) {
    return entry && (name_equals(entry->name, ".") ||
                     name_equals(entry->name, "..") ||
                     name_equals(entry->short_name, ".") ||
                     name_equals(entry->short_name, ".."));
}

static void fat_check_walk_directory(fat_check_context_t *context,
                                     const fat_dir_entry_t *directory,
                                     uint32_t depth) {
    fat_dir_entry_t *entries;
    uint32_t count = 0;

    if (depth > 32U) {
        context->report->directory_errors++;
        return;
    }
    entries = (fat_dir_entry_t *)kmalloc(
        sizeof(*entries) * FAT_MAX_DIR_ENTRIES);
    if (!entries) {
        context->report->io_errors++;
        return;
    }
    if (!fat_list_dir(context->fs, directory, entries,
                      FAT_MAX_DIR_ENTRIES, &count)) {
        context->report->directory_errors++;
        kfree(entries);
        return;
    }
    if (count == FAT_MAX_DIR_ENTRIES)
        context->report->directory_errors++;

    for (uint32_t i = 0; i < count; i++) {
        fat_dir_entry_t *entry = &entries[i];
        uint32_t owner;
        if (fat_check_dot_entry(entry)) continue;
        owner = context->next_owner++;
        if (entry->is_directory)
            context->report->directories++;
        else
            context->report->files++;
        (void)fat_check_chain(context, entry, owner);

        if (entry->is_directory && entry->first_cluster >= 2U &&
            entry->first_cluster < context->cluster_limit) {
            if (context->directories_seen[entry->first_cluster]) {
                context->report->directory_errors++;
                continue;
            }
            context->directories_seen[entry->first_cluster] = 1U;
            fat_check_walk_directory(context, entry, depth + 1U);
        }
        if ((i & 15U) == 15U) task_yield();
    }
    kfree(entries);
}

bool fat_check_current(fat_check_report_t *report) {
    fat_fs_t fs;
    fat_check_context_t context;
    uint8_t boot[BLOCK_SECTOR_SIZE];
    uint8_t compare[BLOCK_SECTOR_SIZE];
    uint8_t *table = NULL;
    uint32_t *owners = NULL;
    uint8_t *directories_seen = NULL;
    uint32_t table_bytes;
    uint32_t required_bytes;
    uint32_t cluster_limit;

    if (!report) return false;
    kmemset(report, 0, sizeof(*report));
    report->fat_copies_match = true;
    report->backup_boot_matches = true;
    if (!fat_get_current(&fs) || !fs.device) return false;
    if (fs.type == FAT_TYPE_BFS4) return bfs4_check(&fs, report);
    if (!read_sector(&fs, 0, boot)) {
        report->io_errors++;
        report->errors++;
        return false;
    }
    report->boot_sector_valid = looks_like_fat_bpb(boot);
    if (!report->boot_sector_valid) {
        report->errors++;
        return false;
    }

    if (fs.type == FAT_TYPE_FAT32) {
        uint16_t backup_sector = read_le16(boot + 50);
        if (!backup_sector || backup_sector >= fs.reserved_sector_count ||
            !read_sector(&fs, backup_sector, compare)) {
            report->backup_boot_matches = false;
            report->warnings++;
            if (backup_sector) report->io_errors++;
        } else if (kmemcmp(boot, compare, sizeof(boot)) != 0) {
            report->backup_boot_matches = false;
            report->warnings++;
        }
    }

    table_bytes = fs.fat_size_sectors * fs.bytes_per_sector;
    cluster_limit = fs.total_clusters + 2U;
    if (fs.type == FAT_TYPE_FAT12)
        required_bytes = (cluster_limit * 3U + 1U) / 2U;
    else if (fs.type == FAT_TYPE_FAT16)
        required_bytes = cluster_limit * 2U;
    else
        required_bytes = cluster_limit * 4U;
    if (!table_bytes || required_bytes > table_bytes) {
        report->errors++;
        return false;
    }

    table = (uint8_t *)kmalloc(table_bytes);
    owners = (uint32_t *)kzalloc(cluster_limit * sizeof(uint32_t));
    directories_seen = (uint8_t *)kzalloc(cluster_limit);
    if (!table || !owners || !directories_seen) {
        report->io_errors++;
        goto cleanup;
    }
    for (uint32_t sector = 0; sector < fs.fat_size_sectors; sector++) {
        if (!read_sector(&fs, fs.first_fat_sector + sector,
                         table + sector * fs.bytes_per_sector)) {
            report->io_errors++;
            goto cleanup;
        }
        if ((sector & 31U) == 31U) task_yield();
    }

    for (uint32_t copy = 1; copy < fs.num_fats; copy++) {
        for (uint32_t sector = 0; sector < fs.fat_size_sectors; sector++) {
            if (!read_sector(&fs, fs.first_fat_sector +
                             copy * fs.fat_size_sectors + sector, compare)) {
                report->io_errors++;
                report->fat_copies_match = false;
                goto cleanup;
            }
            if (kmemcmp(table + sector * fs.bytes_per_sector, compare,
                        fs.bytes_per_sector) != 0) {
                report->fat_copies_match = false;
                report->fat_mismatch_sectors++;
            }
        }
    }

    kmemset(&context, 0, sizeof(context));
    context.fs = &fs;
    context.table = table;
    context.owners = owners;
    context.directories_seen = directories_seen;
    context.cluster_limit = cluster_limit;
    context.next_owner = 2U;
    context.report = report;
    report->directories = 1U;

    if (fs.type == FAT_TYPE_FAT32) {
        fat_dir_entry_t root;
        kmemset(&root, 0, sizeof(root));
        root.is_directory = true;
        root.first_cluster = fs.root_dir_cluster;
        if (fs.root_dir_cluster >= 2U &&
            fs.root_dir_cluster < cluster_limit) {
            directories_seen[fs.root_dir_cluster] = 1U;
            (void)fat_check_chain(&context, &root, 1U);
        } else {
            report->invalid_chains++;
        }
    }
    fat_check_walk_directory(&context, NULL, 0U);

    {
        uint32_t free_run = 0U;
        for (uint32_t cluster = 2U; cluster < cluster_limit; cluster++) {
        uint32_t value = fat_table_get(&fs, table, cluster);
        if (value == 0U) {
            report->free_clusters++;
            free_run++;
            if (free_run > report->largest_free_run)
                report->largest_free_run = free_run;
        } else if (fat_check_cluster_is_bad(&fs, value)) {
            free_run = 0U;
            report->bad_clusters++;
        } else if (fat_check_cluster_is_reserved(&fs, value)) {
            free_run = 0U;
            report->reserved_clusters++;
        } else {
            free_run = 0U;
            report->allocated_clusters++;
            if (!owners[cluster]) report->lost_clusters++;
        }
        if ((cluster & 1023U) == 0U) task_yield();
        }
    }
    report->completed = true;

cleanup:
    report->errors += report->io_errors + report->fat_mismatch_sectors +
        report->lost_clusters + report->crosslinked_clusters +
        report->circular_chains + report->invalid_chains +
        report->size_mismatches + report->directory_errors;
    report->warnings += report->bad_clusters + report->reserved_clusters;
    if (table) kfree(table);
    if (owners) kfree(owners);
    if (directories_seen) kfree(directories_seen);
    return report->completed;
}

bool fat_repair_current(fat_repair_report_t *repair,
                        fat_check_report_t *after) {
    fat_check_report_t before;
    fat_check_report_t traversal;
    fat_fs_t fs;
    fat_check_context_t context;
    uint8_t boot[BLOCK_SECTOR_SIZE];
    uint8_t *table = NULL;
    uint32_t *owners = NULL;
    uint8_t *directories_seen = NULL;
    uint32_t table_bytes;
    uint32_t cluster_limit;
    bool table_changed = false;
    bool success = false;

    if (!repair) return false;
    kmemset(repair, 0, sizeof(*repair));
    if (after) kmemset(after, 0, sizeof(*after));
    repair->attempted = true;
    if (!fat_check_current(&before) || !fat_get_current(&fs) || !fs.device)
        return false;
    if(fs.type==FAT_TYPE_BFS4) {
        fat_check_report_t bfs_after;
        return bfs4_repair(&fs,repair,after?after:&bfs_after);
    }
    repair->errors_before = before.errors;
    if (fs.device->read_only || !fs.device->write) {
        repair->read_only = true;
        return false;
    }

    table_bytes = fs.fat_size_sectors * fs.bytes_per_sector;
    cluster_limit = fs.total_clusters + 2U;
    table = (uint8_t *)kmalloc(table_bytes);
    owners = (uint32_t *)kzalloc(cluster_limit * sizeof(uint32_t));
    directories_seen = (uint8_t *)kzalloc(cluster_limit);
    if (!table || !owners || !directories_seen) goto cleanup;
    for (uint32_t sector = 0; sector < fs.fat_size_sectors; sector++) {
        if (!read_sector(&fs, fs.first_fat_sector + sector,
                         table + sector * fs.bytes_per_sector))
            goto cleanup;
        if ((sector & 31U) == 31U) task_yield();
    }

    kmemset(&traversal, 0, sizeof(traversal));
    kmemset(&context, 0, sizeof(context));
    context.fs = &fs;
    context.table = table;
    context.owners = owners;
    context.directories_seen = directories_seen;
    context.cluster_limit = cluster_limit;
    context.next_owner = 2U;
    context.report = &traversal;
    context.repair = repair;

    if (fs.type == FAT_TYPE_FAT32) {
        fat_dir_entry_t root;
        kmemset(&root, 0, sizeof(root));
        root.is_directory = true;
        root.first_cluster = fs.root_dir_cluster;
        if (fs.root_dir_cluster >= 2U &&
            fs.root_dir_cluster < cluster_limit) {
            directories_seen[fs.root_dir_cluster] = 1U;
            (void)fat_check_chain(&context, &root, 1U);
        } else traversal.directory_errors++;
    }
    fat_check_walk_directory(&context, NULL, 0U);

    /* Nunca liberar supuestos huerfanos si el arbol no pudo recorrerse entero:
       podrian pertenecer a la parte que no fue visible para el analizador. */
    if (traversal.directory_errors || traversal.io_errors) {
        repair->scan_incomplete = true;
        goto cleanup;
    }

    for (uint32_t cluster = 2U; cluster < cluster_limit; cluster++) {
        uint32_t value = fat_table_get(&fs, table, cluster);
        if (!owners[cluster] && value != 0U &&
            !fat_check_cluster_is_bad(&fs, value) &&
            !fat_check_cluster_is_reserved(&fs, value)) {
            fat_table_set(&fs, table, cluster, 0U);
            repair->lost_clusters_freed++;
        }
        if ((cluster & 1023U) == 0U) task_yield();
    }
    table_changed = repair->chains_truncated != 0U ||
                    repair->lost_clusters_freed != 0U ||
                    !before.fat_copies_match;
    if (table_changed) {
        if (!fat_commit_table(&fs, table)) {
            repair->write_errors++;
            goto cleanup;
        }
        repair->fat_copies_synchronized = true;
    } else {
        repair->fat_copies_synchronized = before.fat_copies_match;
    }

    if (fs.type == FAT_TYPE_FAT32 && !before.backup_boot_matches) {
        uint16_t backup_sector;
        if (!read_sector(&fs, 0, boot)) {
            repair->write_errors++;
            goto cleanup;
        }
        backup_sector = read_le16(boot + 50);
        if (!backup_sector || backup_sector >= fs.reserved_sector_count ||
            !write_sector(&fs, backup_sector, boot)) {
            repair->write_errors++;
            goto cleanup;
        }
        repair->backup_boot_repaired = true;
    } else {
        repair->backup_boot_repaired = before.backup_boot_matches;
    }

    /* Fuerza una lectura posterior desde el dispositivo. */
    fat_sector_cache_invalidate_all();
    if (after) {
        if (!fat_check_current(after)) goto cleanup;
        repair->errors_after = after->errors;
    } else repair->errors_after = 0U;
    repair->completed = true;
    success = true;

cleanup:
    if (table) kfree(table);
    if (owners) kfree(owners);
    if (directories_seen) kfree(directories_seen);
    return success;
}

static void fat_entry_set_cluster(uint8_t *entry, uint32_t cluster) {
    entry[20] = (uint8_t)(cluster >> 16);
    entry[21] = (uint8_t)(cluster >> 24);
    entry[26] = (uint8_t)cluster;
    entry[27] = (uint8_t)(cluster >> 8);
}

static bool fat_make_short_name(const char *name, uint8_t raw[11]) {
    int base = 0;
    int ext = 0;
    bool in_ext = false;
    if (!name || !name[0]) return false;
    kmemset(raw, ' ', 11);
    while (*name) {
        char c = *name++;
        if (c == '.') {
            if (in_ext) return false;
            in_ext = true;
            continue;
        }
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c == '/' || c == '\\' || c == ' ' || (uint8_t)c < 32) return false;
        if (!in_ext) {
            if (base >= 8) return false;
            raw[base++] = (uint8_t)c;
        } else {
            if (ext >= 3) return false;
            raw[8 + ext++] = (uint8_t)c;
        }
    }
    return base > 0;
}

static bool fat_split_parent(const char *path, char *parent, char *name) {
    size_t len;
    size_t slash = 0;
    if (!path || path[0] != '/') return false;
    len = kstrlen(path);
    if (len < 2 || len >= 260) return false;
    for (size_t i = 1; i < len; i++) if (path[i] == '/') slash = i;
    if (slash == 0) {
        kstrcpy(parent, "/");
        kstrncpy(name, path + 1, FAT_MAX_NAME - 1);
    } else {
        kstrncpy(parent, path, slash);
        parent[slash] = '\0';
        kstrncpy(name, path + slash + 1, FAT_MAX_NAME - 1);
    }
    name[FAT_MAX_NAME - 1] = '\0';
    return name[0] != '\0';
}

static bool fat_name_needs_lfn(const char *name, const uint8_t raw[11]) {
    char canonical[13];
    if (!name) return false;
    copy_name(raw, canonical);
    return kstrcmp(name, canonical) != 0;
}

static bool fat_make_alias(const char *name, uint32_t number,
                           uint8_t raw[11]) {
    const char *dot = NULL;
    const char *p;
    int base = 0;
    int ext = 0;
    char digits[7];
    int digit_count = 0;
    if (!name || !name[0] || number == 0 || number > 999999) return false;
    for (p = name; *p; p++) if (*p == '.') dot = p;
    kmemset(raw, ' ', 11);
    p = name;
    while (*p && p != dot && base < 6) {
        char c = *p++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
            raw[base++] = (uint8_t)c;
    }
    if (!base) raw[base++] = '_';
    while (number && digit_count < (int)sizeof(digits)) {
        digits[digit_count++] = (char)('0' + number % 10U);
        number /= 10U;
    }
    if (base + 1 + digit_count > 8) base = 8 - 1 - digit_count;
    raw[base++] = '~';
    while (digit_count) raw[base++] = (uint8_t)digits[--digit_count];
    if (dot) {
        p = dot + 1;
        while (*p && ext < 3) {
            char c = *p++;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                raw[8 + ext++] = (uint8_t)c;
        }
    }
    return true;
}

static bool fat_find_free_run(const fat_fs_t *fs, const fat_dir_entry_t *dir,
                              uint32_t required, uint32_t *sector_out,
                              uint32_t *offset_out, bool *bridge_needed) {
    W3P_SCOPE(W3P_FAT_FREE_RUN, 0U);
    bool fixed_root;
    uint32_t cluster;
    bool saw_end_marker = false;
    bool run_has_prior_end_marker = false;
    if (!fs || !dir || !dir->is_directory || !required || required > 21U ||
        !sector_out || !offset_out) return false;
    if (bridge_needed) *bridge_needed = false;

    fixed_root = dir->first_cluster == 0U && fs->type != FAT_TYPE_FAT32;
    cluster = fixed_root ? 0U : dir->first_cluster;
    if (!fixed_root && cluster < 2U) return false;

    for (;;) {
        uint32_t first_sector = fixed_root
            ? fs->first_fat_sector + fs->num_fats * fs->fat_size_sectors
            : fs->first_data_sector +
              (cluster - 2U) * fs->sectors_per_cluster;
        uint32_t sectors = fixed_root ? fs->root_dir_sectors
                                      : fs->sectors_per_cluster;
        uint32_t run = 0U;
        uint32_t run_sector = 0U;
        uint32_t run_offset = 0U;

        for (uint32_t s = 0; s < sectors; s++) {
            uint8_t buffer[BLOCK_SECTOR_SIZE];
            if (!read_sector(fs, first_sector + s, buffer)) return false;
            for (uint32_t off = 0; off < fs->bytes_per_sector; off += 32U) {
                if (buffer[off] == 0U || buffer[off] == 0xE5U) {
                    if (!run) {
                        run_sector = first_sector + s;
                        run_offset = off;
                        run_has_prior_end_marker = saw_end_marker;
                    }
                    if (buffer[off] == 0U) saw_end_marker = true;
                    if (++run >= required) {
                        *sector_out = run_sector;
                        *offset_out = run_offset;
                        if (bridge_needed)
                            *bridge_needed = run_has_prior_end_marker;
                        return true;
                    }
                } else {
                    run = 0U;
                }
            }
        }

        if (fixed_root) break;
        cluster = get_next_cluster(fs, cluster);
        if (cluster == 0xFFFFFFFFU || cluster < 2U) break;
    }
    return false;
}

/* FAT marks 0x00 as end-of-directory, not merely as a free slot.
 *
 * Directories created by BlesKernOS are preallocated across several clusters.
 * fat_find_free_run() deliberately keeps an LFN + short entry inside one
 * cluster because fat_write_named_entry() advances by physical sectors.
 * When the tail of one cluster has fewer free slots than an LFN needs, the
 * chosen run can therefore begin in the next cluster.  If a 0x00 entry was
 * skipped at the end of the previous cluster, normal FAT readers stop there
 * and the newly-written file becomes invisible immediately after creation.
 *
 * Before publishing such a later run, convert any earlier 0x00 markers to the
 * deleted-entry marker 0xE5.  The first untouched 0x00 after the new entry
 * remains the directory terminator, preserving standard FAT semantics. */
static bool fat_bridge_end_markers_before_slot(const fat_fs_t *fs,
                                               const fat_dir_entry_t *dir,
                                               uint32_t target_sector,
                                               uint32_t target_offset,
                                               uint32_t *bridged_out) {
    bool fixed_root;
    uint32_t cluster;
    uint32_t bridged = 0U;
    uint32_t visited = 0U;

    if (bridged_out) *bridged_out = 0U;
    if (!fs || !dir || !dir->is_directory ||
        target_offset >= fs->bytes_per_sector ||
        (target_offset & 31U) != 0U)
        return false;

    fixed_root = dir->first_cluster == 0U && fs->type != FAT_TYPE_FAT32;
    cluster = fixed_root ? 0U : dir->first_cluster;
    if (!fixed_root && cluster < 2U) return false;

    for (;;) {
        uint32_t first_sector = fixed_root
            ? fs->first_fat_sector + fs->num_fats * fs->fat_size_sectors
            : fs->first_data_sector +
              (cluster - 2U) * fs->sectors_per_cluster;
        uint32_t sectors = fixed_root ? fs->root_dir_sectors
                                      : fs->sectors_per_cluster;

        for (uint32_t s = 0U; s < sectors; s++) {
            uint8_t data[BLOCK_SECTOR_SIZE];
            bool dirty = false;
            uint32_t sector = first_sector + s;

            if (!read_sector(fs, sector, data)) return false;
            for (uint32_t off = 0U; off < fs->bytes_per_sector; off += 32U) {
                if (sector == target_sector && off == target_offset) {
                    if (dirty && !write_sector(fs, sector, data)) return false;
                    if (bridged_out) *bridged_out = bridged;
                    return true;
                }
                if (data[off] == 0x00U) {
                    data[off] = 0xE5U;
                    dirty = true;
                    bridged++;
                }
            }
            if (dirty && !write_sector(fs, sector, data)) return false;
        }

        if (fixed_root) break;
        if (visited++ > fs->total_clusters) break;
        cluster = get_next_cluster(fs, cluster);
        if (cluster == 0xFFFFFFFFU || cluster < 2U) break;
    }

    return false;
}

static void fat_write_lfn_entry(uint8_t entry[32], const char *name,
                                uint8_t ordinal, uint8_t count,
                                uint8_t checksum) {
    static const uint8_t offsets[13] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };
    uint32_t base = (uint32_t)(ordinal - 1) * 13U;
    size_t length = kstrlen(name);
    kmemset(entry, 0xFF, 32);
    entry[0] = ordinal | (ordinal == count ? 0x40 : 0);
    entry[11] = FAT_ATTR_LONG_NAME;
    entry[12] = 0;
    entry[13] = checksum;
    entry[26] = 0;
    entry[27] = 0;
    for (uint32_t i = 0; i < 13; i++) {
        uint32_t index = base + i;
        uint16_t value = index < length ? (uint8_t)name[index]
                         : (index == length ? 0x0000 : 0xFFFF);
        entry[offsets[i]] = (uint8_t)value;
        entry[offsets[i] + 1] = (uint8_t)(value >> 8);
    }
}

/* The slot finder keeps each name in one cluster. Coalesce its LFN and
 * short entries by sector, publishing the short entry last. One ATA flush
 * per 32-byte LFN slot made Wine's builtin installation needlessly slow. */
static bool fat_write_named_entry(const fat_fs_t *fs, uint32_t sector,
                                  uint32_t offset, const char *name,
                                  uint8_t lfn_count, const uint8_t entry[32]) {
    uint8_t data[512];
    uint8_t checksum = fat_short_checksum(entry);
    if (!read_sector(fs, sector, data)) return false;
    for (uint32_t slot = 0; slot <= lfn_count; ++slot) {
        if (slot < lfn_count)
            fat_write_lfn_entry(data + offset, name,
                                (uint8_t)(lfn_count - slot), lfn_count, checksum);
        else
            kmemcpy(data + offset, entry, 32);
        offset += 32U;
        if (offset == 512U || slot == lfn_count) {
            if (!write_sector(fs, sector, data)) return false;
            if (slot == lfn_count) break;
            ++sector;
            offset = 0;
            if (!read_sector(fs, sector, data)) return false;
        }
    }
    return true;
}

static bool fat_find_dir_slot(const fat_fs_t *fs, const fat_dir_entry_t *dir,
                              const uint8_t raw_name[11], uint32_t *sector_out,
                              uint32_t *offset_out, fat_dir_entry_t *old,
                              bool *exists) {
    W3P_SCOPE(W3P_FAT_SLOT_FIND, 0U);
    bool fixed_root;
    uint32_t cluster;
    uint32_t free_sector = 0xFFFFFFFFU;
    uint32_t free_offset = 0U;

    if (!fs || !dir || !dir->is_directory || !raw_name ||
        !sector_out || !offset_out || !exists) return false;

    fixed_root = dir->first_cluster == 0U && fs->type != FAT_TYPE_FAT32;
    cluster = fixed_root ? 0U : dir->first_cluster;
    if (!fixed_root && cluster < 2U) return false;

    for (;;) {
        uint32_t first_sector = fixed_root
            ? fs->first_fat_sector + fs->num_fats * fs->fat_size_sectors
            : fs->first_data_sector +
              (cluster - 2U) * fs->sectors_per_cluster;
        uint32_t sectors = fixed_root ? fs->root_dir_sectors
                                      : fs->sectors_per_cluster;

        for (uint32_t s = 0; s < sectors; s++) {
            uint8_t buffer[BLOCK_SECTOR_SIZE];
            if (!read_sector(fs, first_sector + s, buffer)) return false;
            for (uint32_t off = 0; off < fs->bytes_per_sector; off += 32U) {
                uint8_t *entry = buffer + off;
                /* 0x00 terminates a FAT directory. No valid alias can
                 * follow it, even when the directory has spare clusters. */
                if (entry[0] == 0U) {
                    if (free_sector == 0xFFFFFFFFU) {
                        free_sector = first_sector + s;
                        free_offset = off;
                    }
                    *sector_out = free_sector;
                    *offset_out = free_offset;
                    *exists = false;
                    return true;
                }
                if (entry[0] == 0xE5U &&
                    free_sector == 0xFFFFFFFFU) {
                    free_sector = first_sector + s;
                    free_offset = off;
                }
                if (entry[0] && entry[0] != 0xE5U &&
                    entry[11] != FAT_ATTR_LONG_NAME &&
                    kmemcmp(entry, raw_name, 11) == 0) {
                    if (old) parse_dir_entry(entry, old);
                    *sector_out = first_sector + s;
                    *offset_out = off;
                    *exists = true;
                    return true;
                }
            }
        }

        if (fixed_root) break;
        cluster = get_next_cluster(fs, cluster);
        if (cluster == 0xFFFFFFFFU || cluster < 2U) break;
    }

    if (free_sector == 0xFFFFFFFFU) return false;
    *sector_out = free_sector;
    *offset_out = free_offset;
    *exists = false;
    return true;
}

/* Retain a copy of the last successfully committed allocation table.
 * Each transaction still edits its own buffer and commits synchronously.
 * Reusing disk contents avoids reading megabytes for every tiny prefix file.
 * The space-cache generation invalidates this snapshot before any FAT write.
 * Keep large optional allocations away from the low-memory boot path. */
static kmutex_t g_allocation_cache_lock = KMUTEX_INITIALIZER;
static struct {
    uint8_t *table;
    uint32_t bytes, generation;
    fat_fs_t fs;
} g_allocation_cache;

/* Wine creates thousands of small files outside the explicit setup bulk
 * writer. Starting each transaction's free-cluster scan at 2 makes the Nth
 * allocation revisit all N-1 occupied clusters. This hint only changes the
 * starting point: every candidate is still checked in the current FAT and
 * the search wraps, so a stale hint cannot allocate an occupied cluster. */
static struct {
    block_device_t *device;
    uint32_t volume_lba;
    uint32_t next;
} g_allocation_hint;

static uint32_t fat_allocation_hint_get(const fat_fs_t *fs) {
    uint32_t next = 2U;
    kmutex_lock(&g_allocation_cache_lock);
    if (g_allocation_hint.device == fs->device &&
        g_allocation_hint.volume_lba == fs->volume_lba &&
        g_allocation_hint.next >= 2U &&
        g_allocation_hint.next < fs->total_clusters + 2U)
        next = g_allocation_hint.next;
    kmutex_unlock(&g_allocation_cache_lock);
    return next;
}

static void fat_allocation_hint_set(const fat_fs_t *fs, uint32_t next) {
    kmutex_lock(&g_allocation_cache_lock);
    g_allocation_hint.device = fs->device;
    g_allocation_hint.volume_lba = fs->volume_lba;
    g_allocation_hint.next = next < fs->total_clusters + 2U ? next : 2U;
    kmutex_unlock(&g_allocation_cache_lock);
}

static bool fat_read_allocation_table(const fat_fs_t *fs, uint8_t *table) {
    uint32_t bytes = fs->fat_size_sectors * fs->bytes_per_sector;
    uint32_t count, generation;
    bool hit;
    kmutex_lock(&g_allocation_cache_lock);
    hit = fat_space_cached(fs, &count, &generation) &&
        g_allocation_cache.table && g_allocation_cache.bytes == bytes &&
        g_allocation_cache.generation == generation &&
        g_allocation_cache.fs.device == fs->device &&
        g_allocation_cache.fs.volume_lba == fs->volume_lba &&
        g_allocation_cache.fs.type == fs->type &&
        g_allocation_cache.fs.total_clusters == fs->total_clusters;
    if (hit) kmemcpy(table, g_allocation_cache.table, bytes);
    kmutex_unlock(&g_allocation_cache_lock);
    if (hit) return true;
    return fat_read_sector_run(fs, fs->first_fat_sector,
                               fs->fat_size_sectors, table);
}

static void fat_cache_allocation_table(const fat_fs_t *fs,
                                        const uint8_t *table,
                                        uint32_t generation) {
    uint32_t bytes = fs->fat_size_sectors * fs->bytes_per_sector;
    if (mm_total_bytes() < 64U * 1024U * 1024U || bytes > 4U * 1024U * 1024U)
        return;
    kmutex_lock(&g_allocation_cache_lock);
    if (g_allocation_cache.bytes != bytes) {
        uint8_t *next = (uint8_t *)krealloc(g_allocation_cache.table, bytes);
        if (!next) {
            kmutex_unlock(&g_allocation_cache_lock);
            return;
        }
        g_allocation_cache.table = next;
        (void)mm_set_allocation_owner(next, 0U);
        g_allocation_cache.bytes = bytes;
    }
    kmemcpy(g_allocation_cache.table, table, bytes);
    g_allocation_cache.fs = *fs;
    g_allocation_cache.generation = generation;
    kmutex_unlock(&g_allocation_cache_lock);
}

static uint32_t fat_count_free(const fat_fs_t *fs, const uint8_t *table) {
    uint32_t count = 0;
    for (uint32_t cluster = 2; cluster < fs->total_clusters + 2U; cluster++)
        if (fat_table_get(fs, table, cluster) == 0) count++;
    return count;
}

/* A tiny prefix file normally changes one FAT32 sector. Recounting every
 * cluster at each commit makes directory/file creation proportional to the
 * volume size. Derive the change from the previous committed snapshot when
 * it is still authoritative; retain the full scan for a cold cache and FAT12
 * (whose packed entries can straddle sectors). */
static uint32_t fat_count_free_after_dirty(const fat_fs_t *fs,
                                           const uint8_t *table,
                                           const uint8_t *dirty) {
    uint32_t count, generation;
    bool hit;
    if (fs->type != FAT_TYPE_FAT32) return fat_count_free(fs, table);
    kmutex_lock(&g_allocation_cache_lock);
    hit = fat_space_cached(fs, &count, &generation) &&
        g_allocation_cache.table &&
        g_allocation_cache.bytes == fs->fat_size_sectors * fs->bytes_per_sector &&
        g_allocation_cache.generation == generation &&
        g_allocation_cache.fs.device == fs->device &&
        g_allocation_cache.fs.volume_lba == fs->volume_lba &&
        g_allocation_cache.fs.type == fs->type &&
        g_allocation_cache.fs.total_clusters == fs->total_clusters;
    if (hit) {
        for (uint32_t sector = 0; sector < fs->fat_size_sectors; sector++) {
            if (!dirty[sector]) continue;
            uint32_t first = sector * (fs->bytes_per_sector / 4U);
            uint32_t end = first + fs->bytes_per_sector / 4U;
            if (first < 2U) first = 2U;
            if (end > fs->total_clusters + 2U) end = fs->total_clusters + 2U;
            for (uint32_t cluster = first; cluster < end; cluster++) {
                bool old_free = fat_table_get(fs, g_allocation_cache.table, cluster) == 0U;
                bool new_free = fat_table_get(fs, table, cluster) == 0U;
                if (old_free && !new_free) count--;
                else if (!old_free && new_free) count++;
            }
        }
    }
    kmutex_unlock(&g_allocation_cache_lock);
    return hit ? count : fat_count_free(fs, table);
}

static bool fat_commit_table(const fat_fs_t *fs, const uint8_t *table) {
    if (!fs || !table) return false;
    uint32_t generation = fat_space_invalidate();
    /* Las copias secundarias se escriben primero. Los sectores se agrupan para
     * evitar un CACHE FLUSH ATA por cada bloque de 512 bytes. */
    for (uint8_t pass = 0; pass < fs->num_fats; pass++) {
        uint8_t copy = (uint8_t)((pass + 1U) % fs->num_fats);
        if (!fat_write_sector_run(
                fs,
                fs->first_fat_sector + copy * fs->fat_size_sectors,
                fs->fat_size_sectors,
                table))
            return false;
    }
    fat_space_publish(fs, fat_count_free(fs, table), generation);
    fat_cache_allocation_table(fs, table, generation);
    return true;
}

static bool fat_commit_dirty_table(const fat_fs_t *fs, const uint8_t *table,
                                   const uint8_t *dirty) {
    W3P_SCOPE(W3P_FAT_COMMIT, 0U);
    if (!fs || !table || !dirty) return false;
    uint32_t free_clusters = fat_count_free_after_dirty(fs, table, dirty);
    uint32_t generation = fat_space_invalidate();
    for (uint8_t pass = 0; pass < fs->num_fats; pass++) {
        uint8_t copy = (uint8_t)((pass + 1U) % fs->num_fats);
        uint32_t sector = 0U;
        while (sector < fs->fat_size_sectors) {
            uint32_t run;
            while (sector < fs->fat_size_sectors && !dirty[sector]) sector++;
            if (sector >= fs->fat_size_sectors) break;
            run = 1U;
            while (sector + run < fs->fat_size_sectors &&
                   dirty[sector + run] && run < 255U)
                run++;
            if (!fat_write_sector_run(
                    fs,
                    fs->first_fat_sector +
                        copy * fs->fat_size_sectors + sector,
                    run,
                    table + sector * fs->bytes_per_sector))
                return false;
            sector += run;
        }
    }
    fat_space_publish(fs, free_clusters, generation);
    fat_cache_allocation_table(fs, table, generation);
    return true;
}

static bool fat_write_chain_data(const fat_fs_t *fs, const uint8_t *table,
                                 uint32_t first_cluster, const void *data,
                                 uint32_t size) {
    uint32_t cluster = first_cluster;
    uint32_t written = 0U;
    uint32_t cluster_size;
    uint32_t max_run_clusters;

    if (!size) return true;
    if (!fs || !table || !data || cluster < 2U) return false;
    cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    max_run_clusters = 255U / fs->sectors_per_cluster;
    if (!max_run_clusters) max_run_clusters = 1U;

    while (written < size) {
        uint32_t run_clusters = 1U;
        uint32_t next = fat_table_get(fs, table, cluster);
        uint32_t start_sector = fs->first_data_sector +
            (cluster - 2U) * fs->sectors_per_cluster;
        uint32_t run_capacity;
        uint32_t transfer;
        uint32_t full_sectors;

        while (run_clusters < max_run_clusters &&
               next == cluster + run_clusters &&
               written + run_clusters * cluster_size < size) {
            run_clusters++;
            next = fat_table_get(fs, table,
                                 cluster + run_clusters - 1U);
        }

        run_capacity = run_clusters * cluster_size;
        transfer = size - written;
        if (transfer > run_capacity) transfer = run_capacity;
        full_sectors = transfer / fs->bytes_per_sector;

        if (full_sectors &&
            !fat_write_sector_run(
                fs, start_sector, full_sectors,
                (const uint8_t *)data + written))
            return false;
        written += full_sectors * fs->bytes_per_sector;

        if (written < size && transfer % fs->bytes_per_sector) {
            uint8_t tail[BLOCK_SECTOR_SIZE];
            uint32_t partial = transfer % fs->bytes_per_sector;
            kmemset(tail, 0, sizeof(tail));
            kmemcpy(tail, (const uint8_t *)data + written, partial);
            if (!write_sector(fs, start_sector + full_sectors, tail))
                return false;
            written += partial;
        }

        if (written >= size) break;
        if (next == 0xFFFFFFFFU || next < 2U) return false;
        cluster = next;
    }
    return true;
}

/* Grow a cluster-backed directory when every existing directory slot is used.
 *
 * Host-created FAT directories (for example BUILD in the source image) often
 * start with a single cluster.  Self-hosting then creates enough objects and
 * subdirectories to exhaust those 16 entries.  FAT directories are ordinary
 * cluster chains, so reaching the end of the current chain must not be treated
 * as a permanent "directory full" condition: allocate one free cluster, zero
 * it, append it to the chain, and let the normal slot search retry.
 *
 * FAT12/16 fixed root directories are the only exception; their root table has
 * a fixed size and cannot be extended this way. */
static bool fat_extend_directory_chain(const fat_fs_t *fs,
                                       const fat_dir_entry_t *dir) {
    uint32_t fat_bytes;
    uint32_t tail;
    uint32_t next;
    uint32_t free_cluster = 0U;
    uint32_t next_candidate = 2U;
    uint8_t *table = NULL;
    uint8_t *dirty = NULL;
    bool using_bulk;
    bool ok = false;

    if (!fs || !dir || !dir->is_directory) return false;
    if (dir->first_cluster == 0U && fs->type != FAT_TYPE_FAT32)
        return false;
    if (dir->first_cluster < 2U) return false;

    fat_bytes = fs->fat_size_sectors * fs->bytes_per_sector;
    using_bulk = fat_bulk_matches(fs);
    if (using_bulk) {
        table = g_fat_bulk.table;
        dirty = g_fat_bulk.dirty;
        if (!table || !dirty) return false;
        kmemset(dirty, 0, fs->fat_size_sectors);
        next_candidate = g_fat_bulk.next_free_hint;
        if (next_candidate < 2U ||
            next_candidate >= fs->total_clusters + 2U)
            next_candidate = 2U;
    } else {
        table = (uint8_t *)kmalloc(fat_bytes);
        dirty = (uint8_t *)kzalloc(fs->fat_size_sectors);
        if (!table || !dirty) goto cleanup;
        if (!fat_read_allocation_table(fs, table))
            goto cleanup;
        next_candidate = fat_allocation_hint_get(fs);
    }

    tail = dir->first_cluster;
    for (uint32_t visited = 0U; visited <= fs->total_clusters; visited++) {
        next = fat_table_get(fs, table, tail);
        if (fat_cluster_is_eoc(fs, next)) break;
        if (next < 2U || next >= fs->total_clusters + 2U)
            goto cleanup;
        tail = next;
        if (visited == fs->total_clusters) goto cleanup;
    }

    {
        uint32_t limit = fs->total_clusters + 2U;
        for (uint32_t candidate = next_candidate; candidate < limit; candidate++) {
            if (fat_table_get(fs, table, candidate) == 0U) {
                free_cluster = candidate;
                next_candidate = candidate + 1U;
                break;
            }
        }
        if (!free_cluster && next_candidate > 2U) {
            for (uint32_t candidate = 2U; candidate < next_candidate; candidate++) {
                if (fat_table_get(fs, table, candidate) == 0U) {
                    free_cluster = candidate;
                    next_candidate = candidate + 1U;
                    break;
                }
            }
        }
    }
    if (!free_cluster) goto cleanup;

    /* Zero the still-unlinked cluster first.  If a later FAT commit fails,
     * the cluster remains free rather than leaving a half-published chain. */
    {
        uint8_t zeros[BLOCK_SECTOR_SIZE];
        uint32_t first_sector = fs->first_data_sector +
            (free_cluster - 2U) * fs->sectors_per_cluster;
        kmemset(zeros, 0, sizeof(zeros));
        for (uint32_t s = 0U; s < fs->sectors_per_cluster; s++)
            if (!write_sector(fs, first_sector + s, zeros))
                goto cleanup;
    }

    fat_table_set_dirty(fs, table, tail, free_cluster, dirty);
    fat_table_set_dirty(fs, table, free_cluster, fat_eoc_value(fs), dirty);
    if (!fat_commit_dirty_table(fs, table, dirty)) goto cleanup;

    if (using_bulk) {
        g_fat_bulk.next_free_hint = next_candidate;
        g_fat_bulk.stores++;
    } else fat_allocation_hint_set(fs, next_candidate);
    kprintf("[FAT:DIR] extended directory cluster=%u -> %u\n",
            tail, free_cluster);
    ok = true;

cleanup:
    if (using_bulk) {
        if (g_fat_bulk.active && g_fat_bulk.dirty)
            kmemset(g_fat_bulk.dirty, 0, fs->fat_size_sectors);
    } else {
        if (dirty) kfree(dirty);
        if (table) kfree(table);
    }
    return ok;
}

static bool fat_store_path(fat_fs_t *fs, const char *path, const void *data,
                           uint32_t size, bool directory) {
    char parent_path[260] = {0};
    char name[FAT_MAX_NAME] = {0};
    uint8_t raw_name[11];
    fat_dir_entry_t parent;
    fat_dir_entry_t old;
    fat_dir_entry_t resolved;
    bool exists = false;
    bool resolved_exists = false;
    uint32_t dir_sector = 0U, dir_offset = 0U;
    uint32_t fat_bytes;
    uint8_t *table = NULL;
    uint8_t *dirty = NULL;
    uint32_t cluster_size = 0U;
    uint32_t needed = 0U;
    uint32_t first = 0U;
    uint32_t previous = 0U;
    uint32_t next_candidate = 2U;
    bool needs_lfn = false;
    bool bridge_needed = false;
    uint8_t lfn_count = 0U;
    bool using_bulk = false;
    bool fat_committed = false;
    const char *failure = "unknown";
#ifdef BLESK_FAT_PROFILE
    uint32_t profile_start=pit_get_ticks(), profile_lookup=profile_start;
    uint32_t profile_alloc=profile_start, profile_data=profile_start;
#endif

    fat_set_last_error("none");

#define FAT_STORE_FAIL(reason) do { failure = (reason); goto fail; } while (0)

    if (!fat_device_writable(fs))
        FAT_STORE_FAIL("device is read-only or has no writer");
    if (!fs ||
        (fs->type != FAT_TYPE_FAT12 &&
         fs->type != FAT_TYPE_FAT16 &&
         fs->type != FAT_TYPE_FAT32))
        FAT_STORE_FAIL("invalid FAT filesystem");
    if (!fat_split_parent(path, parent_path, name))
        FAT_STORE_FAIL("invalid path or file name");
    if (!fat_resolve_path(fs, parent_path, &parent) || !parent.is_directory)
        FAT_STORE_FAIL("parent directory not found");

    resolved_exists = fat_resolve_path(fs, path, &resolved);
    if (resolved_exists) {
        if (!fat_make_short_name(resolved.short_name, raw_name))
            FAT_STORE_FAIL("existing short name is invalid");
    } else if (fat_make_short_name(name, raw_name)) {
        needs_lfn = fat_name_needs_lfn(name, raw_name);
    } else {
        bool alias_ready = false;
        needs_lfn = true;
        for (uint32_t n = 1U; n < 1000U && !alias_ready; n++) {
            uint32_t alias_sector, alias_offset;
            fat_dir_entry_t alias_old;
            bool alias_exists;
            if (!fat_make_alias(name, n, raw_name))
                FAT_STORE_FAIL("could not make an 8.3 alias");
            if (!fat_find_dir_slot(fs, &parent, raw_name,
                                   &alias_sector, &alias_offset,
                                   &alias_old, &alias_exists)) {
                if (!fat_extend_directory_chain(fs, &parent) ||
                    !fat_find_dir_slot(fs, &parent, raw_name,
                                       &alias_sector, &alias_offset,
                                       &alias_old, &alias_exists))
                    FAT_STORE_FAIL("directory table full while choosing alias");
            }
            alias_ready = !alias_exists;
        }
        if (!alias_ready)
            FAT_STORE_FAIL("no free 8.3 alias");
    }

    if (needs_lfn)
        lfn_count = (uint8_t)((kstrlen(name) + 12U) / 13U);

    if (!fat_find_dir_slot(fs, &parent, raw_name, &dir_sector,
                           &dir_offset, &old, &exists)) {
        if (!fat_extend_directory_chain(fs, &parent) ||
            !fat_find_dir_slot(fs, &parent, raw_name, &dir_sector,
                               &dir_offset, &old, &exists))
            FAT_STORE_FAIL("directory table full and could not extend directory");
    }
    if (!exists && lfn_count &&
        !fat_find_free_run(fs, &parent, (uint32_t)lfn_count + 1U,
                           &dir_sector, &dir_offset, &bridge_needed)) {
        if (!fat_extend_directory_chain(fs, &parent) ||
            !fat_find_free_run(fs, &parent, (uint32_t)lfn_count + 1U,
                               &dir_sector, &dir_offset, &bridge_needed))
            FAT_STORE_FAIL("no contiguous directory slots after extending directory");
    }
    if (exists && old.is_directory != directory)
        FAT_STORE_FAIL("file/directory type conflict");

#ifdef BLESK_FAT_PROFILE
    profile_lookup=pit_get_ticks();profile_alloc=profile_data=profile_lookup;
#endif
    /* Empty regular files have no cluster chain. Creating one (or rewriting
     * an already empty one) only changes directory entries; do not read the
     * entire allocation table for Wine's O_CREAT/temporary-file probes.
     * Truncating a nonempty file still follows the transaction below. */
    if (!directory && !size && (!exists || old.first_cluster < 2U))
        goto publish_entry;

    fat_bytes = fs->fat_size_sectors * fs->bytes_per_sector;
    using_bulk = fat_bulk_matches(fs);
    if (using_bulk) {
        table = g_fat_bulk.table;
        dirty = g_fat_bulk.dirty;
        kmemset(dirty, 0, fs->fat_size_sectors);
        next_candidate = g_fat_bulk.next_free_hint;
        if (next_candidate < 2U ||
            next_candidate >= fs->total_clusters + 2U)
            next_candidate = 2U;
    } else {
        table = (uint8_t *)kmalloc(fat_bytes);
        dirty = (uint8_t *)kzalloc(fs->fat_size_sectors);
        if (!table || !dirty)
            FAT_STORE_FAIL("not enough memory for FAT transaction");
        if (!fat_read_allocation_table(fs, table))
            FAT_STORE_FAIL("could not read FAT table");
        next_candidate = fat_allocation_hint_get(fs);
    }

    if (exists && old.first_cluster >= 2U) {
        uint32_t cluster = old.first_cluster;
        while (cluster >= 2U && cluster < fs->total_clusters + 2U &&
               !fat_cluster_is_eoc(fs, cluster)) {
            uint32_t next = fat_table_get(fs, table, cluster);
            fat_table_set_dirty(fs, table, cluster, 0U, dirty);
            cluster = next;
        }
        if (cluster >= 2U && cluster < fs->total_clusters + 2U)
            fat_table_set_dirty(fs, table, cluster, 0U, dirty);
    }

    cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    needed = directory ? FAT_DIRECTORY_PREALLOC_CLUSTERS
                       : ((size + cluster_size - 1U) / cluster_size);

    for (uint32_t n = 0U; n < needed; n++) {
        uint32_t cluster = 0U;
        uint32_t limit = fs->total_clusters + 2U;
        for (uint32_t candidate = next_candidate; candidate < limit; candidate++) {
            if (fat_table_get(fs, table, candidate) == 0U) {
                cluster = candidate;
                next_candidate = candidate + 1U;
                break;
            }
        }
        if (!cluster && next_candidate > 2U) {
            for (uint32_t candidate = 2U; candidate < next_candidate; candidate++) {
                if (fat_table_get(fs, table, candidate) == 0U) {
                    cluster = candidate;
                    next_candidate = candidate + 1U;
                    break;
                }
            }
        }
        if (!cluster)
            FAT_STORE_FAIL("no free clusters");
        fat_table_set_dirty(fs, table, cluster, fat_eoc_value(fs), dirty);
        if (previous)
            fat_table_set_dirty(fs, table, previous, cluster, dirty);
        else
            first = cluster;
        previous = cluster;
    }

    if (!fat_commit_dirty_table(fs, table, dirty))
        FAT_STORE_FAIL("could not commit FAT sectors");
    fat_committed = true;
#ifdef BLESK_FAT_PROFILE
    profile_alloc=pit_get_ticks();
#endif
    if (using_bulk) {
        g_fat_bulk.next_free_hint = next_candidate;
        g_fat_bulk.stores++;
    } else fat_allocation_hint_set(fs, next_candidate);

    if (directory) {
        uint32_t directory_bytes = needed * cluster_size;
        uint8_t *directory_data = (uint8_t *)kzalloc(directory_bytes);
        if (!directory_data)
            FAT_STORE_FAIL("not enough memory for directory initialization");
        kmemset(directory_data, ' ', 11U);
        directory_data[0] = '.';
        directory_data[11] = FAT_ATTR_DIRECTORY;
        fat_entry_set_cluster(directory_data, first);
        kmemset(directory_data + 32U, ' ', 11U);
        directory_data[32] = '.';
        directory_data[33] = '.';
        directory_data[43] = FAT_ATTR_DIRECTORY;
        fat_entry_set_cluster(directory_data + 32U, parent.first_cluster);
        if (!fat_write_chain_data(fs, table, first,
                                  directory_data, directory_bytes)) {
            kfree(directory_data);
            FAT_STORE_FAIL("could not initialize directory clusters");
        }
        kfree(directory_data);
    } else if (size && !fat_write_chain_data(fs, table, first, data, size)) {
        FAT_STORE_FAIL("could not write file data clusters");
    }

#ifdef BLESK_FAT_PROFILE
    profile_data=pit_get_ticks();
#endif
publish_entry:
    {
        uint8_t entry[32] = {0};
        if (!exists && lfn_count && bridge_needed) {
            uint32_t bridged = 0U;
            if (!fat_bridge_end_markers_before_slot(fs, &parent, dir_sector,
                                                    dir_offset, &bridged))
                FAT_STORE_FAIL("could not preserve FAT directory end marker");
            if (bridged)
                kprintf("[FAT:DIR] bridged %u end marker(s) before %s\n",
                        bridged, path);
        }
        if (exists) {
            uint8_t sector_data[512];
            if (!read_sector(fs, dir_sector, sector_data))
                FAT_STORE_FAIL("could not read destination directory sector");
            /* Preserve POSIX mode and legacy creation metadata on rewrite. */
            entry[12] = sector_data[dir_offset + 12U];
            entry[13] = sector_data[dir_offset + 13U];
        }
        kmemcpy(entry, raw_name, 11U);
        entry[11] = directory ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
        fat_entry_set_cluster(entry, first);
        entry[28] = (uint8_t)size;
        entry[29] = (uint8_t)(size >> 8);
        entry[30] = (uint8_t)(size >> 16);
        entry[31] = (uint8_t)(size >> 24);
        if (!fat_write_named_entry(fs, dir_sector, dir_offset, name,
                                   exists ? 0 : lfn_count, entry))
            FAT_STORE_FAIL("could not write destination directory entry");
    }

    if (!using_bulk) {
        kfree(dirty);
        kfree(table);
    } else {
        kmemset(dirty, 0, fs->fat_size_sectors);
    }
#ifdef BLESK_FAT_PROFILE
    uint32_t profile_end=pit_get_ticks();
    if(profile_end-profile_start>=10U)
        kprintf("[FAT:SLOW-STORE] pid=%u bytes=%u dir=%u lookup=%u alloc=%u data=%u publish=%u path=%s\n",
                task_current_process_id(),size,directory?1U:0U,
                profile_lookup-profile_start,profile_alloc-profile_lookup,
                profile_data-profile_alloc,profile_end-profile_data,path);
#endif
    fat_set_last_error("none");
#undef FAT_STORE_FAIL
    return true;

fail:
    fat_set_last_error(failure);
    kprintf("[FAT:diag] store FAIL path=%s size=%u dir=%u stage=%s "
            "parent=%s needed=%u first=%u\n",
            path ? path : "(null)", size, directory ? 1U : 0U,
            failure, parent_path[0] ? parent_path : "(unknown)",
            needed, first);
    if (using_bulk) {
        /* Before the FAT commit the resident table may contain allocations
         * that never reached disk. Reload it only on that uncommon path. */
        if (!fat_committed && !fat_bulk_reload())
            fat_bulk_write_end();
        else if (g_fat_bulk.active && g_fat_bulk.dirty)
            kmemset(g_fat_bulk.dirty, 0, fs->fat_size_sectors);
    } else {
        if (dirty) kfree(dirty);
        if (table) kfree(table);
    }
#undef FAT_STORE_FAIL
    return false;
}

/* Commit only the appended part of a VFS write cache. Existing bytes in the
 * cache are known unchanged, so the old chain remains allocated. Data goes
 * out before the new FAT links and directory size become visible. */
bool fat_append_path(fat_fs_t *fs, const char *path, const void *data,
                     uint32_t old_size, uint32_t new_size) {
    if (fs && fs->type == FAT_TYPE_BFS4)
        return bfs4_append(fs, path, data, old_size, new_size);
    char parent_path[260] = {0}, name[FAT_MAX_NAME] = {0};
    uint8_t raw_name[11], sector_data[BLOCK_SECTOR_SIZE];
    fat_dir_entry_t parent, resolved, old;
    uint32_t dir_sector, dir_offset, cluster_size, old_count, new_count;
    uint32_t fat_bytes, first, tail = 0U, next_candidate = 2U;
    uint32_t position, cluster, cluster_index, target_index;
    uint8_t *table = NULL, *dirty = NULL;
    bool exists = false, using_bulk = false, committed = false, ok = false;

    if (!fat_device_writable(fs) || !path || !data || new_size <= old_size ||
        (fs->type != FAT_TYPE_FAT12 && fs->type != FAT_TYPE_FAT16 &&
         fs->type != FAT_TYPE_FAT32)) return false;
    if (!fat_split_parent(path, parent_path, name) ||
        !fat_resolve_path(fs, parent_path, &parent) || !parent.is_directory ||
        !fat_resolve_path(fs, path, &resolved) || resolved.is_directory ||
        resolved.size != old_size ||
        !fat_make_short_name(resolved.short_name, raw_name) ||
        !fat_find_dir_slot(fs, &parent, raw_name, &dir_sector, &dir_offset,
                           &old, &exists) || !exists || old.size != old_size ||
        old.first_cluster != resolved.first_cluster) return false;

    cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    if (!cluster_size || fs->bytes_per_sector != BLOCK_SECTOR_SIZE) return false;
    old_count = (old_size + cluster_size - 1U) / cluster_size;
    new_count = (new_size + cluster_size - 1U) / cluster_size;
    if (new_count > fs->total_clusters) return false;
    if ((!old_count && old.first_cluster >= 2U) ||
        (old_count && old.first_cluster < 2U)) return false;

    fat_bytes = fs->fat_size_sectors * fs->bytes_per_sector;
    using_bulk = fat_bulk_matches(fs);
    if (using_bulk) {
        table = g_fat_bulk.table;
        dirty = g_fat_bulk.dirty;
        kmemset(dirty, 0, fs->fat_size_sectors);
        next_candidate = g_fat_bulk.next_free_hint;
        if (next_candidate < 2U || next_candidate >= fs->total_clusters + 2U)
            next_candidate = 2U;
    } else {
        table = (uint8_t *)kmalloc(fat_bytes);
        dirty = (uint8_t *)kzalloc(fs->fat_size_sectors);
        if (!table || !dirty || !fat_read_allocation_table(fs, table))
            goto cleanup;
        next_candidate = fat_allocation_hint_get(fs);
    }

    first = old.first_cluster;
    cluster = first;
    for (uint32_t i = 0U; i < old_count; i++) {
        uint32_t next;
        if (cluster < 2U || cluster >= fs->total_clusters + 2U) goto cleanup;
        tail = cluster;
        next = fat_table_get(fs, table, cluster);
        if (i + 1U < old_count) {
            if (next < 2U || fat_cluster_is_eoc(fs, next)) goto cleanup;
            cluster = next;
        } else if (!fat_cluster_is_eoc(fs, next)) {
            /* A prior interrupted commit may have left extra clusters.
             * Let the regular rewrite path reclaim that chain. */
            goto cleanup;
        }
    }

    for (uint32_t i = old_count; i < new_count; i++) {
        uint32_t free_cluster = 0U;
        uint32_t limit = fs->total_clusters + 2U;
        for (uint32_t candidate = next_candidate; candidate < limit; candidate++)
            if (fat_table_get(fs, table, candidate) == 0U) {
                free_cluster = candidate;
                next_candidate = candidate + 1U;
                break;
            }
        if (!free_cluster && next_candidate > 2U)
            for (uint32_t candidate = 2U; candidate < next_candidate; candidate++)
                if (fat_table_get(fs, table, candidate) == 0U) {
                    free_cluster = candidate;
                    next_candidate = candidate + 1U;
                    break;
                }
        if (!free_cluster) goto cleanup;
        fat_table_set_dirty(fs, table, free_cluster, fat_eoc_value(fs), dirty);
        if (tail) fat_table_set_dirty(fs, table, tail, free_cluster, dirty);
        else first = free_cluster;
        tail = free_cluster;
    }

    /* Rewrite at most the one existing partial sector plus appended sectors.
     * The full VFS cache supplies the unchanged prefix of that sector. */
    position = old_size & ~(uint32_t)(BLOCK_SECTOR_SIZE - 1U);
    target_index = position / cluster_size;
    cluster = first;
    for (cluster_index = 0U; cluster_index < target_index; cluster_index++) {
        cluster = fat_table_get(fs, table, cluster);
        if (cluster < 2U || cluster >= fs->total_clusters + 2U) goto cleanup;
    }
    while (position < new_size) {
        uint32_t local = position % cluster_size;
        uint32_t last = cluster;
        uint32_t run_clusters = 1U;
        uint32_t span = cluster_size - local;
        uint32_t max_run_clusters = 255U / fs->sectors_per_cluster;
        uint32_t sector = fs->first_data_sector +
            (cluster - 2U) * fs->sectors_per_cluster +
            local / BLOCK_SECTOR_SIZE;
        uint32_t bytes = new_size - position;
        uint32_t full_sectors;
        if (!max_run_clusters) max_run_clusters = 1U;
        while (span < bytes && run_clusters < max_run_clusters) {
            uint32_t next = fat_table_get(fs, table, last);
            if (next != last + 1U) break;
            last = next;
            run_clusters++;
            span += cluster_size;
        }
        if (bytes > span) bytes = span;
        full_sectors = bytes / BLOCK_SECTOR_SIZE;
        if (full_sectors && !fat_write_sector_run(fs, sector, full_sectors,
                                                  (const uint8_t *)data + position))
            goto cleanup;
        if (bytes % BLOCK_SECTOR_SIZE) {
            kmemset(sector_data, 0, sizeof(sector_data));
            kmemcpy(sector_data,
                    (const uint8_t *)data + position +
                        full_sectors * BLOCK_SECTOR_SIZE,
                    bytes % BLOCK_SECTOR_SIZE);
            if (!write_sector(fs, sector + full_sectors, sector_data))
                goto cleanup;
        }
        position += bytes;
        if (position < new_size) {
            cluster = fat_table_get(fs, table, last);
            if (cluster < 2U || cluster >= fs->total_clusters + 2U)
                goto cleanup;
        }
    }

    if (new_count > old_count) {
        if (!fat_commit_dirty_table(fs, table, dirty)) goto cleanup;
        committed = true;
    }
    if (!read_sector(fs, dir_sector, sector_data)) goto cleanup;
    fat_entry_set_cluster(sector_data + dir_offset, first);
    sector_data[dir_offset + 28U] = (uint8_t)new_size;
    sector_data[dir_offset + 29U] = (uint8_t)(new_size >> 8);
    sector_data[dir_offset + 30U] = (uint8_t)(new_size >> 16);
    sector_data[dir_offset + 31U] = (uint8_t)(new_size >> 24);
    if (!write_sector(fs, dir_sector, sector_data)) goto cleanup;
    if (using_bulk) {
        g_fat_bulk.next_free_hint = next_candidate;
        g_fat_bulk.stores++;
    } else fat_allocation_hint_set(fs, next_candidate);
    fat_set_last_error("none");
    ok = true;

cleanup:
    if (using_bulk) {
        if (!ok && !committed && !fat_bulk_reload())
            fat_bulk_write_end();
        else if (g_fat_bulk.active && g_fat_bulk.dirty)
            kmemset(g_fat_bulk.dirty, 0, fs->fat_size_sectors);
    } else {
        if (dirty) kfree(dirty);
        if (table) kfree(table);
    }
    return ok;
}

bool fat_write_path(fat_fs_t *fs, const char *path, const void *data,
                    uint32_t size) {
    if (fs && fs->type == FAT_TYPE_BFS4)
        return bfs4_write(fs, path, data, size);
    if (size && !data) return false;
    return fat_store_path(fs, path, data, size, false);
}

bool fat_mkdir_path(fat_fs_t *fs, const char *path) {
    if (fs && fs->type == FAT_TYPE_BFS4) return bfs4_mkdir(fs, path);
    fat_dir_entry_t existing;
    if (fs && path && fat_resolve_path(fs, path, &existing))
        return existing.is_directory;
    return fat_store_path(fs, path, NULL, 0, true);
}

typedef struct {
    uint32_t sector[21];
    uint16_t offset[21];
    uint8_t count;
    fat_dir_entry_t entry;
} fat_entry_location_t;

static bool fat_locate_in_directory_uncached(const fat_fs_t *fs,
                                    const fat_dir_entry_t *dir,
                                    const char *name,
                                    fat_entry_location_t *location) {
    uint32_t cluster;
    uint32_t root_sector = 0;
    uint32_t root_sectors = 0;
    fat_lfn_state_t lfn;
    uint32_t pending_sector[20];
    uint16_t pending_offset[20];
    uint8_t pending = 0;
    bool fixed_root;
    if (!fs || !dir || !name || !location || !dir->is_directory) return false;
    fixed_root = dir->first_cluster == 0 && fs->type != FAT_TYPE_FAT32;
    if (fixed_root) {
        root_sector = fs->first_fat_sector + fs->num_fats * fs->fat_size_sectors;
        root_sectors = fs->root_dir_sectors;
        cluster = 0;
    } else {
        cluster = dir->first_cluster;
        if (cluster < 2) return false;
    }
    fat_lfn_reset(&lfn);
    for (;;) {
        uint32_t first = fixed_root ? root_sector :
            fs->first_data_sector + (cluster - 2U) * fs->sectors_per_cluster;
        uint32_t sectors = fixed_root ? root_sectors : fs->sectors_per_cluster;
        for (uint32_t s = 0; s < sectors; s++) {
            uint8_t data[512];
            if (!read_sector(fs, first + s, data)) return false;
            for (uint32_t off = 0; off < fs->bytes_per_sector; off += 32U) {
                uint8_t *raw = data + off;
                fat_dir_entry_t parsed;
                if (raw[0] == 0x00) return false;
                if (raw[0] == 0xE5) {
                    pending = 0;
                    fat_lfn_reset(&lfn);
                    continue;
                }
                if (raw[11] == FAT_ATTR_LONG_NAME) {
                    if (pending < 20) {
                        pending_sector[pending] = first + s;
                        pending_offset[pending++] = (uint16_t)off;
                    } else pending = 0;
                    (void)fat_lfn_consume(&lfn, raw);
                    continue;
                }
                if (!fat_match_dir_entry(raw, &lfn, name, &parsed)) {
                    pending = 0;
                    continue;
                }
                if (name_equals(parsed.name, name) ||
                    name_equals(parsed.short_name, name)) {
                    kmemset(location, 0, sizeof(*location));
                    for (uint8_t i = 0; i < pending; i++) {
                        location->sector[i] = pending_sector[i];
                        location->offset[i] = pending_offset[i];
                    }
                    location->sector[pending] = first + s;
                    location->offset[pending] = (uint16_t)off;
                    location->count = (uint8_t)(pending + 1U);
                    location->entry = parsed;
                    return true;
                }
                pending = 0;
            }
        }
        if (fixed_root) break;
        cluster = get_next_cluster(fs, cluster);
        if (cluster == 0xFFFFFFFF || cluster < 2) break;
    }
    return false;
}

/* stat, permission and symlink probes revisit the same short entry. Keep its
 * complete LFN location too, with the same write generation as the path
 * cache; mutations cannot reuse a stale location for chmod/rename/unlink. */
#define FAT_LOCATION_CACHE_SLOTS 32U
static struct {
    block_device_t *device;
    uint32_t volume_lba, cluster, generation;
    char name[FAT_MAX_NAME];
    fat_entry_location_t location;
} g_location_cache[FAT_LOCATION_CACHE_SLOTS];

static bool fat_locate_in_directory(const fat_fs_t *fs,
                                    const fat_dir_entry_t *dir,
                                    const char *name,
                                    fat_entry_location_t *location) {
    if(!fs || !dir || !dir->is_directory || !name || !location)return false;
    uint32_t hash=2166136261U ^ dir->first_cluster,length=0U;
    for(const char *p=name;*p;p++) {
        uint8_t ch=(uint8_t)*p;
        if(ch>='A' && ch<='Z')ch+='a'-'A';
        hash=(hash^ch)*16777619U;length++;
    }
    if(length>=FAT_MAX_NAME)return false;
    uint32_t slot=hash&(FAT_LOCATION_CACHE_SLOTS-1U);
    uint32_t irq=kspin_lock_irqsave(&g_sector_cache_lock);
    uint32_t generation=g_sector_cache_generation;
    if(g_location_cache[slot].device==fs->device &&
       g_location_cache[slot].volume_lba==fs->volume_lba &&
       g_location_cache[slot].cluster==dir->first_cluster &&
       g_location_cache[slot].generation==generation &&
       name_equals(g_location_cache[slot].name,name)) {
        *location=g_location_cache[slot].location;
        kspin_unlock_irqrestore(&g_sector_cache_lock,irq);return true;
    }
    kspin_unlock_irqrestore(&g_sector_cache_lock,irq);
    if(!fat_locate_in_directory_uncached(fs,dir,name,location))return false;
    irq=kspin_lock_irqsave(&g_sector_cache_lock);
    if(generation==g_sector_cache_generation) {
        g_location_cache[slot].device=fs->device;
        g_location_cache[slot].volume_lba=fs->volume_lba;
        g_location_cache[slot].cluster=dir->first_cluster;
        g_location_cache[slot].generation=generation;
        kmemcpy(g_location_cache[slot].name,name,length+1U);
        g_location_cache[slot].location=*location;
    }
    kspin_unlock_irqrestore(&g_sector_cache_lock,irq);return true;
}

/* Identidad del registro corto, compartida por el alias 8.3 y el nombre largo.
 * No usamos first_cluster: vale cero para todos los archivos vacios y cambia
 * cuando FAT reemplaza la cadena al escribir. */
bool fat_path_inode(const fat_fs_t *fs, const char *path, uint64_t *inode) {
    if (fs && fs->type == FAT_TYPE_BFS4) return bfs4_inode(fs, path, inode);
    char parent_path[260], name[FAT_MAX_NAME];
    fat_dir_entry_t parent;
    fat_entry_location_t location;
    if (!path[0] || (path[0]=='/' && !path[1])) { *inode=1; return true; }
    if (!fat_split_parent(path,parent_path,name) ||
        !fat_resolve_path(fs,parent_path,&parent) ||
        !fat_locate_in_directory(fs,&parent,name,&location)) return false;
    unsigned i=location.count-1U;
    *inode=((uint64_t)location.sector[i]*fs->bytes_per_sector+location.offset[i])/32U+2U;
    return true;
}

bool fat_get_posix_mode(const fat_fs_t *fs, const char *path, uint16_t *mode) {
    if (fs && fs->type == FAT_TYPE_BFS4) return bfs4_get_mode(fs, path, mode);
    char parent_path[260], name[FAT_MAX_NAME];
    fat_dir_entry_t parent;
    fat_entry_location_t location;
    uint8_t data[BLOCK_SECTOR_SIZE];
    uint8_t index;
    const uint8_t *entry;
    if (!fs || !path || !mode || !path[0] ||
        (path[0] == '/' && !path[1]) ||
        !fat_split_parent(path, parent_path, name) ||
        !fat_resolve_path(fs, parent_path, &parent) ||
        !fat_locate_in_directory(fs, &parent, name, &location) ||
        !location.count)
        return false;
    index = (uint8_t)(location.count - 1U);
    if (!read_sector(fs, location.sector[index], data)) return false;
    entry = data + location.offset[index];
    uint8_t tag=entry[12] & FAT_POSIX_MODE_TAG_MASK;
    if (tag != FAT_POSIX_MODE_TAG && tag != 0xB0U) return false;
    *mode = (uint16_t)((((uint16_t)entry[12] & 0x0FU) << 8U) |
                       (uint16_t)entry[13]);
    if(tag==0xB0U) *mode|=0120000U;
    return true;
}

bool fat_set_posix_mode(fat_fs_t *fs, const char *path, uint16_t mode) {
    if (fs && fs->type == FAT_TYPE_BFS4) return bfs4_set_mode(fs, path, mode);
    char parent_path[260], name[FAT_MAX_NAME];
    fat_dir_entry_t parent;
    fat_entry_location_t location;
    uint8_t data[BLOCK_SECTOR_SIZE];
    uint8_t index;
    uint8_t *entry;
    if (!fat_device_writable(fs) || !path || !path[0] ||
        (path[0] == '/' && !path[1]) ||
        !fat_split_parent(path, parent_path, name) ||
        !fat_resolve_path(fs, parent_path, &parent) ||
        !fat_locate_in_directory(fs, &parent, name, &location) ||
        !location.count)
        return false;
    index = (uint8_t)(location.count - 1U);
    if (!read_sector(fs, location.sector[index], data)) return false;
    entry = data + location.offset[index];
    uint8_t tag=(mode&0170000U)==0120000U?0xB0U:FAT_POSIX_MODE_TAG;
    mode &= FAT_POSIX_MODE_MASK;
    entry[12] = (uint8_t)(tag | ((mode >> 8U) & 0x0FU));
    entry[13] = (uint8_t)(mode & 0xFFU);
    return write_sector(fs, location.sector[index], data);
}

static void fat_encode_unix_time(uint64_t seconds, uint16_t *date,
                                 uint16_t *time) {
    static const uint8_t month_days[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t capped = seconds > 0xffffffffULL ? 0xffffffffU : (uint32_t)seconds;
    uint32_t days = capped / 86400U;
    uint32_t day_seconds = capped % 86400U;
    uint32_t year = 1970U, month = 1U;
    while (year < 2107U) {
        uint32_t leap = (year % 4U == 0U &&
                         (year % 100U != 0U || year % 400U == 0U));
        uint32_t length = leap ? 366U : 365U;
        if (days < length) break;
        days -= length;
        year++;
    }
    if (year < 1980U) {
        *date = (1U << 5U) | 1U;
        *time = 0U;
        return;
    }
    if (year >= 2107U && days >= 365U) {
        *date = (uint16_t)(((2107U - 1980U) << 9U) | (12U << 5U) | 31U);
        *time = (23U << 11U) | (59U << 5U) | 29U;
        return;
    }
    for (month = 1U; month <= 12U; month++) {
        uint32_t length = month_days[month - 1U];
        if (month == 2U && (year % 4U == 0U) &&
            (year % 100U != 0U || year % 400U == 0U)) length++;
        if (days < length) break;
        days -= length;
    }
    *date = (uint16_t)(((year - 1980U) << 9U) | (month << 5U) |
                       ((uint32_t)days + 1U));
    *time = (uint16_t)(((day_seconds / 3600U) << 11U) |
                       (((day_seconds / 60U) % 60U) << 5U) |
                       ((day_seconds % 60U) / 2U));
}

static bool fat_decode_unix_time(uint16_t date, uint16_t time,
                                 uint32_t *seconds) {
    static const uint8_t month_days[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t year = 1980U + (date >> 9U);
    uint32_t month = (date >> 5U) & 15U;
    uint32_t day = date & 31U;
    uint32_t hour = time >> 11U;
    uint32_t minute = (time >> 5U) & 63U;
    uint32_t second = (time & 31U) * 2U;
    uint32_t leap = (year % 4U == 0U &&
                     (year % 100U != 0U || year % 400U == 0U));
    uint32_t days = 0U, month_length;
    if (!seconds || month < 1U || month > 12U || !day ||
        hour > 23U || minute > 59U || second > 59U) return false;
    month_length = month_days[month - 1U] + (month == 2U && leap ? 1U : 0U);
    if (day > month_length) return false;
    for (uint32_t y = 1970U; y < year; y++)
        days += (y % 4U == 0U && (y % 100U != 0U || y % 400U == 0U))
            ? 366U : 365U;
    for (uint32_t m = 1U; m < month; m++)
        days += month_days[m - 1U] + (m == 2U && leap ? 1U : 0U);
    days += day - 1U;
    uint64_t value = (uint64_t)days * 86400U +
                     hour * 3600U + minute * 60U + second;
    if (value > 0xffffffffULL) return false;
    *seconds = (uint32_t)value;
    return true;
}

bool fat_get_times(const fat_fs_t *fs, const char *path,
                   uint32_t *access_seconds, uint32_t *modify_seconds) {
    if (fs && fs->type == FAT_TYPE_BFS4)
        return bfs4_get_times(fs, path, access_seconds, modify_seconds);
    char parent_path[260], name[FAT_MAX_NAME];
    fat_dir_entry_t parent;
    fat_entry_location_t location;
    uint8_t data[BLOCK_SECTOR_SIZE];
    const uint8_t *entry;
    uint32_t accessed, modified;
    if (!fs || !path || !access_seconds || !modify_seconds || !path[0] ||
        (path[0] == '/' && !path[1]) ||
        !fat_split_parent(path, parent_path, name) ||
        !fat_resolve_path(fs, parent_path, &parent) ||
        !fat_locate_in_directory(fs, &parent, name, &location) ||
        !location.count ||
        !read_sector(fs, location.sector[location.count - 1U], data)) return false;
    entry = data + location.offset[location.count - 1U];
    if (!fat_decode_unix_time(read_le16(entry + 24U),
                              read_le16(entry + 22U), &modified)) return false;
    if (!fat_decode_unix_time(read_le16(entry + 18U), 0U, &accessed))
        accessed = modified;
    *access_seconds = accessed;
    *modify_seconds = modified;
    return true;
}

bool fat_set_times(fat_fs_t *fs, const char *path,
                   bool set_access, uint64_t access_seconds,
                   bool set_modify, uint64_t modify_seconds) {
    if (fs && fs->type == FAT_TYPE_BFS4)
        return bfs4_set_times(fs, path, set_access, access_seconds,
                              set_modify, modify_seconds);
    char parent_path[260], name[FAT_MAX_NAME];
    fat_dir_entry_t parent;
    fat_entry_location_t location;
    uint8_t data[BLOCK_SECTOR_SIZE], *entry;
    uint8_t index;
    uint16_t date, time;
    if (!fat_device_writable(fs) || !path || !path[0] ||
        (path[0] == '/' && !path[1]) ||
        !fat_split_parent(path, parent_path, name) ||
        !fat_resolve_path(fs, parent_path, &parent) ||
        !fat_locate_in_directory(fs, &parent, name, &location) ||
        !location.count) return false;
    if (!set_access && !set_modify) return true;
    index = (uint8_t)(location.count - 1U);
    if (!read_sector(fs, location.sector[index], data)) return false;
    entry = data + location.offset[index];
    if (set_access) {
        fat_encode_unix_time(access_seconds, &date, &time);
        write_le16(entry + 18U, date);
    }
    if (set_modify) {
        fat_encode_unix_time(modify_seconds, &date, &time);
        write_le16(entry + 22U, time);
        write_le16(entry + 24U, date);
    }
    return write_sector(fs, location.sector[index], data);
}

static bool fat_mark_location_deleted(const fat_fs_t *fs,
                                      const fat_entry_location_t *location) {
    if (!fs || !location || !location->count) return false;
    for (uint8_t i = 0; i < location->count;) {
        uint8_t data[512];
        uint32_t sector = location->sector[i];
        if (!read_sector(fs, sector, data)) return false;
        do {
            data[location->offset[i]] = 0xE5;
            i++;
        } while (i < location->count && location->sector[i] == sector);
        if (!write_sector(fs, sector, data)) return false;
    }
    return true;
}

static bool fat_release_chain(const fat_fs_t *fs, uint32_t first_cluster) {
    uint32_t fat_bytes;
    uint8_t *table;
    uint8_t *dirty;
    uint32_t cluster;
    bool ok = false;

    if (!fs || first_cluster < 2U) return true;
    fat_bytes = fs->fat_size_sectors * fs->bytes_per_sector;
    table = (uint8_t *)kmalloc(fat_bytes);
    dirty = (uint8_t *)kzalloc(fs->fat_size_sectors);
    if (!table || !dirty) goto cleanup;
    if (!fat_read_allocation_table(fs, table))
        goto cleanup;

    cluster = first_cluster;
    while (cluster >= 2U && cluster < fs->total_clusters + 2U) {
        uint32_t next = fat_table_get(fs, table, cluster);
        fat_table_set_dirty(fs, table, cluster, 0U, dirty);
        if (fat_cluster_is_eoc(fs, next) || next < 2U ||
            next >= fs->total_clusters + 2U)
            break;
        cluster = next;
    }
    ok = fat_commit_dirty_table(fs, table, dirty);

cleanup:
    if (dirty) kfree(dirty);
    if (table) kfree(table);
    return ok;
}

bool fat_remove_path(fat_fs_t *fs, const char *path) {
    if (fs && fs->type == FAT_TYPE_BFS4) return bfs4_remove(fs, path);
    char parent_path[260];
    char name[FAT_MAX_NAME];
    fat_dir_entry_t parent;
    fat_entry_location_t location;
    if (!fat_device_writable(fs) || !path ||
        !fat_split_parent(path, parent_path, name) ||
        !fat_resolve_path(fs, parent_path, &parent) ||
        !fat_locate_in_directory(fs, &parent, name, &location)) return false;
    if (location.entry.is_directory) {
        fat_dir_entry_t children[64];
        uint32_t count = 0;
        if (!fat_list_dir(fs, &location.entry, children, 64, &count)) return false;
        for (uint32_t i = 0; i < count; i++)
            if (kstrcmp(children[i].name, ".") != 0 &&
                kstrcmp(children[i].name, "..") != 0) return false;
    }
    if (!fat_mark_location_deleted(fs, &location)) return false;
    return fat_release_chain(fs, location.entry.first_cluster);
}

bool fat_rename_path(fat_fs_t *fs, const char *old_path, const char *new_path) {
    if (fs && fs->type == FAT_TYPE_BFS4)
        return bfs4_rename(fs, old_path, new_path);
    char old_parent_path[260], new_parent_path[260];
    char old_name[FAT_MAX_NAME], new_name[FAT_MAX_NAME];
    fat_dir_entry_t parent, destination;
    fat_entry_location_t old_location;
    fat_entry_location_t destination_location;
    uint8_t raw_name[11];
    bool needs_lfn = false;
    bool bridge_needed = false;
    uint8_t lfn_count = 0;
    uint32_t sector, offset;
    fat_dir_entry_t unused;
    bool exists;
    uint8_t short_entry[32];
    if (!fat_device_writable(fs) || !old_path || !new_path ||
        !fat_split_parent(old_path, old_parent_path, old_name) ||
        !fat_split_parent(new_path, new_parent_path, new_name) ||
        !name_equals(old_parent_path, new_parent_path) ||
        !fat_resolve_path(fs, old_parent_path, &parent) ||
        !fat_locate_in_directory(fs, &parent, old_name, &old_location)) return false;
    /* POSIX rename replaces an existing regular file. Wine relies on this
     * when it commits each registry hive from regXXXX.tmp. Reuse the target
     * directory slots so its long-name entries and short-name checksum stay
     * valid. Publish the new chain before removing the temporary name, then
     * reclaim the old target chain only after both names are consistent. */
    if (fat_locate_in_directory(fs, &parent, new_name, &destination_location)) {
        uint8_t source_index = (uint8_t)(old_location.count - 1U);
        uint8_t target_index = (uint8_t)(destination_location.count - 1U);
        uint8_t source_sector[512], target_sector[512];
        if (old_location.sector[source_index] == destination_location.sector[target_index] &&
            old_location.offset[source_index] == destination_location.offset[target_index])
            return true;
        destination = destination_location.entry;
        if (old_location.entry.is_directory || destination.is_directory ||
            !read_sector(fs, old_location.sector[source_index], source_sector) ||
            !read_sector(fs, destination_location.sector[target_index], target_sector))
            return false;
        uint8_t *target = target_sector + destination_location.offset[target_index];
        kmemcpy(raw_name, target, 11);
        kmemcpy(short_entry, source_sector + old_location.offset[source_index], 32);
        kmemcpy(short_entry, raw_name, 11);
        kmemcpy(target, short_entry, 32);
        if (!write_sector(fs, destination_location.sector[target_index], target_sector))
            return false;
        if (!fat_mark_location_deleted(fs, &old_location)) return false;
        return fat_release_chain(fs, destination.first_cluster);
    }
    if (fat_make_short_name(new_name, raw_name)) {
        needs_lfn = fat_name_needs_lfn(new_name, raw_name);
    } else {
        bool ready = false;
        needs_lfn = true;
        for (uint32_t n = 1; n < 1000 && !ready; n++) {
            if (!fat_make_alias(new_name, n, raw_name) ||
                !fat_find_dir_slot(fs, &parent, raw_name, &sector, &offset,
                                   &unused, &exists)) return false;
            ready = !exists;
        }
        if (!ready) return false;
    }
    if (needs_lfn) lfn_count = (uint8_t)((kstrlen(new_name) + 12U) / 13U);
    if (lfn_count) {
        if (!fat_find_free_run(fs, &parent, (uint32_t)lfn_count + 1U,
                               &sector, &offset, &bridge_needed)) return false;
    } else if (!fat_find_dir_slot(fs, &parent, raw_name, &sector, &offset,
                                  &unused, &exists) || exists) return false;
    {
        uint8_t data[512];
        uint8_t old_index = (uint8_t)(old_location.count - 1U);
        if (!read_sector(fs, old_location.sector[old_index], data)) return false;
        kmemcpy(short_entry, data + old_location.offset[old_index], 32);
        kmemcpy(short_entry, raw_name, 11);
    }
    if (bridge_needed &&
        !fat_bridge_end_markers_before_slot(fs, &parent, sector, offset, NULL))
        return false;
    if (!fat_write_named_entry(fs, sector, offset, new_name, lfn_count,
                               short_entry)) return false;
    return fat_mark_location_deleted(fs, &old_location);
}

bool fat_get_space(const fat_fs_t *fs, uint64_t *total_bytes,
                   uint64_t *free_bytes) {
    if (fs && fs->type == FAT_TYPE_BFS4)
        return bfs4_space(fs, total_bytes, free_bytes);
    uint32_t free_clusters = 0, generation;
    uint64_t cluster_size;
    if (!fs || !total_bytes || !free_bytes) return false;
    if (!fat_space_cached(fs, &free_clusters, &generation)) {
        uint32_t fat_bytes = fs->fat_size_sectors * fs->bytes_per_sector;
        uint8_t *table = (uint8_t *)kmalloc(fat_bytes);
        if (!table) return false;
        if (!fat_read_sector_run(fs, fs->first_fat_sector,
                                  fs->fat_size_sectors, table)) {
            kfree(table);
            return false;
        }
        free_clusters = fat_count_free(fs, table);
        kfree(table);
        fat_space_publish(fs, free_clusters, generation);
    }
    cluster_size = (uint64_t)fs->bytes_per_sector * fs->sectors_per_cluster;
    *total_bytes = (uint64_t)fs->total_clusters * cluster_size;
    *free_bytes = (uint64_t)free_clusters * cluster_size;
    return true;
}
