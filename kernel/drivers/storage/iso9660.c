#include "../../include/iso9660.h"
#include "../../include/ata.h"
#include "../../include/memory.h"
#include "../../include/vga.h"
#include "../../include/driver.h"
#include "../../include/klock.h"

#define ISO_SECTOR_SIZE 2048U
#define ISO_PVD_LBA 16U

static block_device_t *g_iso_device;
static bool g_iso_mounted;
static uint32_t g_iso_signature;
static iso9660_entry_t g_iso_root;
static bool g_iso_joliet;
/* Read-only media can share cached sectors across files and directory walks.
 * Allocate once at mount; a remount invalidates all entries. No disk I/O is
 * performed while holding the spinlock. Allocation failure keeps direct I/O. */
#define ISO_CACHE_SLOTS 512U
#define ISO_CACHE_WAYS 4U
typedef struct {
    uint32_t lba, age;
    bool valid;
    uint8_t data[ISO_SECTOR_SIZE];
} iso_cache_entry_t;
static iso_cache_entry_t *g_iso_cache;
static uint32_t g_iso_cache_age, g_iso_cache_generation;
static kspinlock_t g_iso_cache_lock = KSPINLOCK_INITIALIZER;
static bool g_iso_bulk_reads;

static bool iso_impl_read_at(const iso9660_entry_t *entry, uint32_t offset,
                             void *buffer, uint32_t size, uint32_t *bytes_read);

static void iso_cache_reset(void) {
    iso_cache_entry_t *created = NULL;
    if (!g_iso_cache) created = kzalloc(ISO_CACHE_SLOTS * sizeof(*created));
    if (created) (void)mm_set_allocation_owner(created, 0U);
    uint32_t irq = kspin_lock_irqsave(&g_iso_cache_lock);
    if (!g_iso_cache) {g_iso_cache = created;created = NULL;}
    ++g_iso_cache_generation;
    g_iso_cache_age = 0;
    if (g_iso_cache)
        for (uint32_t i = 0; i < ISO_CACHE_SLOTS; ++i) g_iso_cache[i].valid = false;
    g_iso_bulk_reads = true;
    kspin_unlock_irqrestore(&g_iso_cache_lock, irq);
    if (created) kfree(created);
}

static bool iso_cache_lookup(uint32_t lba, void *buffer, uint32_t *generation) {
    uint32_t irq = kspin_lock_irqsave(&g_iso_cache_lock);
    *generation = g_iso_cache_generation;
    bool found = false;
    if (g_iso_cache) {
        uint32_t base = (lba % (ISO_CACHE_SLOTS / ISO_CACHE_WAYS)) * ISO_CACHE_WAYS;
        for (uint32_t i = 0; i < ISO_CACHE_WAYS; ++i) {
            iso_cache_entry_t *e = &g_iso_cache[base + i];
            if (e->valid && e->lba == lba) {
                kmemcpy(buffer, e->data, ISO_SECTOR_SIZE);
                e->age = ++g_iso_cache_age;found = true;break;
            }
        }
    }
    kspin_unlock_irqrestore(&g_iso_cache_lock, irq);
    return found;
}

static void iso_cache_publish(uint32_t lba, const void *buffer, uint32_t generation) {
    uint32_t irq = kspin_lock_irqsave(&g_iso_cache_lock);
    if (g_iso_cache && generation == g_iso_cache_generation) {
        uint32_t base = (lba % (ISO_CACHE_SLOTS / ISO_CACHE_WAYS)) * ISO_CACHE_WAYS;
        iso_cache_entry_t *victim = &g_iso_cache[base];
        for (uint32_t i = 0; i < ISO_CACHE_WAYS; ++i) {
            iso_cache_entry_t *e = &g_iso_cache[base + i];
            if (!e->valid || e->lba == lba) {victim = e;break;}
            if (e->age < victim->age) victim = e;
        }
        kmemcpy(victim->data, buffer, ISO_SECTOR_SIZE);
        victim->lba = lba;victim->age = ++g_iso_cache_age;victim->valid = true;
    }
    kspin_unlock_irqrestore(&g_iso_cache_lock, irq);
}

static bool iso_read_sector(uint32_t lba, void *buffer) {
    uint32_t generation;
    if (iso_cache_lookup(lba, buffer, &generation)) return true;
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if (block_read(g_iso_device, lba, 1, buffer)) {
            iso_cache_publish(lba, buffer, generation);
            return true;
        }
        (void)ata_refresh_media("cd0");
    }
    return false;
}

static uint32_t iso_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool iso_name_equal(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

static bool iso_parse_record(const uint8_t *record, iso9660_entry_t *entry) {
    uint8_t name_len;
    uint32_t out = 0;

    if (!record || !entry || record[0] < 34) return false;
    name_len = record[32];
    if (33U + name_len > record[0]) return false;
    if (name_len == 1 && (record[33] == 0 || record[33] == 1)) return false;
    if (g_iso_joliet && (name_len & 1U)) return false;

    kmemset(entry, 0, sizeof(*entry));
    entry->extent = iso_le32(record + 2);
    entry->size = iso_le32(record + 10);
    entry->is_directory = (record[25] & 0x02) != 0;
    for (uint32_t i = 0; i < name_len;) {
        uint32_t c = record[33 + i++];
        if (g_iso_joliet) c = (c << 8) | record[33 + i++];
        if (c == ';') break;
        /* Joliet identifiers are UCS-2BE. Export UTF-8 like the VFS, without
         * silently truncating names or accepting separators/surrogates. */
        if (!c || c == '/' || (c >= 0xd800U && c <= 0xdfffU)) return false;
        uint32_t bytes = !g_iso_joliet || c < 0x80U ? 1U : c < 0x800U ? 2U : 3U;
        if (out + bytes >= sizeof(entry->name)) return false;
        if (bytes == 3U) entry->name[out++] = (char)(0xe0U | (c >> 12));
        if (bytes >= 2U)
            entry->name[out++] = (char)((bytes == 2U ? 0xc0U : 0x80U) |
                                      ((c >> 6) & (bytes == 2U ? 0x1fU : 0x3fU)));
        entry->name[out++] = (char)(bytes == 1U ? c : 0x80U | (c & 0x3fU));
    }
    while (out > 0 && entry->name[out - 1] == '.') out--;
    entry->name[out] = '\0';
    return out != 0;
}

static uint32_t iso_pvd_signature(const uint8_t *pvd) {
    uint32_t hash = 2166136261U;
    for (uint32_t i = 0; i < ISO_SECTOR_SIZE; ++i)
        hash = (hash ^ pvd[i]) * 16777619U;
    return hash ? hash : 1U;
}

static bool iso_impl_mount_default(void) {
    uint8_t sector[ISO_SECTOR_SIZE];

    g_iso_mounted = false;
    g_iso_signature = 0U;
    g_iso_joliet = false;
    g_iso_device = block_get("cd0");
    if (!g_iso_device || g_iso_device->type != BLOCK_DEVICE_ATAPI) {
        g_iso_device = NULL;return false;
    }
    iso_cache_reset();
    if (!ata_refresh_media("cd0") || g_iso_device->sector_count <= ISO_PVD_LBA ||
        g_iso_device->sector_size != ISO_SECTOR_SIZE ||
        !iso_read_sector(ISO_PVD_LBA, sector)) {
        g_iso_device = NULL;
        return false;
    }
    if (sector[0] != 1 || kmemcmp(sector + 1, "CD001", 5) != 0 ||
        sector[6] != 1) {
        g_iso_device = NULL;
        return false;
    }

    g_iso_signature = iso_pvd_signature(sector);
    kmemset(&g_iso_root, 0, sizeof(g_iso_root));
    kstrcpy(g_iso_root.name, "/");
    g_iso_root.extent = iso_le32(sector + 156 + 2);
    g_iso_root.size = iso_le32(sector + 156 + 10);
    g_iso_root.is_directory = true;
    if (!g_iso_root.extent || !g_iso_root.size) {g_iso_device = NULL;return false;}
    /* Windows installers need the supplementary tree's long names. The
     * primary tree can contain only aliases such as SOFTDR~1.DLL, even though
     * the game later loads softdrawz.dll. Keep primary-only media supported. */
    uint8_t best_level = 0U;
    for (uint32_t lba = ISO_PVD_LBA + 1U;
         lba < ISO_PVD_LBA + 64U && lba < g_iso_device->sector_count; ++lba) {
        if (!iso_read_sector(lba, sector) ||
            kmemcmp(sector + 1, "CD001", 5) || sector[6] != 1U) break;
        if (sector[0] == 255U) break;
        if (sector[0] != 2U || sector[88] != '%' || sector[89] != '/') continue;
        uint8_t level = sector[90] == '@' ? 1U : sector[90] == 'C' ? 2U :
                        sector[90] == 'E' ? 3U : 0U;
        uint32_t extent = iso_le32(sector + 158), size = iso_le32(sector + 166);
        if (level <= best_level || sector[156] < 34U || !extent || !size ||
            extent >= g_iso_device->sector_count ||
            (size - 1U) / ISO_SECTOR_SIZE >= g_iso_device->sector_count - extent)
            continue;
        best_level = level;
        g_iso_root.extent = extent;
        g_iso_root.size = size;
        g_iso_joliet = true;
    }
    g_iso_mounted = true;
    return true;
}

static void iso_impl_unmount(void) {
    g_iso_mounted = false;
    g_iso_signature = 0U;
    g_iso_device = NULL;
    g_iso_joliet = false;
    kmemset(&g_iso_root, 0, sizeof(g_iso_root));
    iso_cache_reset();
}

static uint32_t iso_impl_media_signature(void) {
    return g_iso_mounted ? g_iso_signature : 0U;
}

static bool iso_impl_is_mounted(void) {
    return g_iso_mounted;
}

static bool iso_impl_list(const iso9660_entry_t *directory, iso9660_entry_t *entries,
                  uint32_t max_entries, uint32_t *count) {
    uint8_t *data;
    uint32_t position = 0;
    uint32_t found = 0;
    uint32_t got = 0;

    if (!directory || !directory->is_directory || !entries || !count ||
        !g_iso_mounted) return false;
    *count = 0;
    if (directory->size == 0 || directory->size > 128U * 1024U)
        return false;
    data = (uint8_t *)kmalloc(directory->size);
    if (!data) return false;

    iso9660_entry_t file = *directory;
    file.is_directory = false;
    if (!iso_impl_read_at(&file, 0, data, directory->size, &got) ||
        got != directory->size) {
        kfree(data);
        return false;
    }

    while (position < directory->size && found < max_entries) {
        uint8_t length = data[position];
        if (length == 0) {
            position = ((position / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }
        if (length < 34) break;
        if ((position % ISO_SECTOR_SIZE) + length > ISO_SECTOR_SIZE) {
            position = ((position / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }
        if (position + length > directory->size) break;
        if (iso_parse_record(data + position, &entries[found])) found++;
        position += length;
    }
    kfree(data);
    *count = found;
    return true;
}

static bool iso_find_child(const iso9660_entry_t *directory,
                           const char *wanted, iso9660_entry_t *entry) {
    uint8_t sector[ISO_SECTOR_SIZE];
    uint32_t consumed = 0U;
    uint32_t sector_index = 0U;

    if (!directory || !directory->is_directory || !wanted || !*wanted ||
        !entry || !g_iso_mounted) return false;

    while (consumed < directory->size) {
        uint32_t base = sector_index * ISO_SECTOR_SIZE;
        uint32_t off = 0U;
        if (!iso_read_sector(directory->extent + sector_index, sector))
            return false;

        while (off < ISO_SECTOR_SIZE && base + off < directory->size) {
            uint8_t length = sector[off];
            iso9660_entry_t candidate;

            if (!length) break;
            if (length < 34U || off + length > ISO_SECTOR_SIZE) break;
            if (base + off + length > directory->size) break;
            if (iso_parse_record(sector + off, &candidate) &&
                iso_name_equal(candidate.name, wanted)) {
                *entry = candidate;
                return true;
            }
            off += length;
        }
        consumed += ISO_SECTOR_SIZE;
        sector_index++;
    }
    return false;
}

static bool iso_impl_resolve(const char *path, iso9660_entry_t *entry) {
    iso9660_entry_t current = g_iso_root;
    char component[ISO9660_MAX_NAME];

    if (!g_iso_mounted || !path || !entry || path[0] != '/') return false;
    while (*path == '/') path++;
    if (!*path) {
        *entry = current;
        return true;
    }

    while (*path) {
        uint32_t pos = 0U;
        while (*path && *path != '/') {
            if (pos + 1U >= sizeof(component)) return false;
            component[pos++] = *path++;
        }
        component[pos] = '\0';
        while (*path == '/') path++;
        if (!current.is_directory ||
            !iso_find_child(&current, component, &current))
            return false;
    }
    *entry = current;
    return true;
}

static bool iso_impl_read_at(const iso9660_entry_t *entry, uint32_t offset,
                     void *buffer, uint32_t size, uint32_t *bytes_read) {
    uint8_t sector[ISO_SECTOR_SIZE];
    uint8_t *out = (uint8_t *)buffer;
    uint32_t total = 0;

    if (!entry || !buffer || !bytes_read || !g_iso_device) return false;
    *bytes_read = 0;
    if (offset >= entry->size) return true;
    if (size > entry->size - offset) size = entry->size - offset;

    while (total < size) {
        uint32_t absolute = offset + total;
        uint32_t lba = entry->extent + absolute / ISO_SECTOR_SIZE;
        uint32_t within = absolute % ISO_SECTOR_SIZE;
        uint32_t chunk = ISO_SECTOR_SIZE - within;

        /*
         * Los archivos grandes (en particular los WAD de Doom) suelen estar
         * alineados a sectores. Leer varios sectores con un único READ(12)
         * evita miles de transacciones ATAPI independientes.
         */
        if (within == 0 && size - total >= ISO_SECTOR_SIZE) {
            uint32_t sectors = (size - total) / ISO_SECTOR_SIZE;
            if (sectors > 32U) sectors = 32U;

            uint32_t generation = 0;
            bool cached = true;
            for (uint32_t i = 0; i < sectors; ++i) {
                if (!iso_cache_lookup(lba + i, out + total + i * ISO_SECTOR_SIZE,
                                      &generation)) {cached = false;break;}
            }
            if (cached) {total += sectors * ISO_SECTOR_SIZE;continue;}
            if (g_iso_bulk_reads && sectors > 1U &&
                block_read(g_iso_device, lba, (uint8_t)sectors,
                           out + total)) {
                for (uint32_t i = 0; i < sectors; ++i)
                    iso_cache_publish(lba + i, out + total + i * ISO_SECTOR_SIZE, generation);
                total += sectors * ISO_SECTOR_SIZE;
                continue;
            }

            if (g_iso_bulk_reads && sectors > 1U) {
                /* VMware can expose a perfectly readable ISO but reject or
                   time out on a large ATAPI READ(12). Once detected, keep
                   this mounted medium on the conservative one-sector path. */
                kprintf("[ISO9660] READ(12) multi-sector fallo; usando 1 sector por lectura\n");
                g_iso_bulk_reads = false;
            }

            for (uint32_t i = 0U; i < sectors; i++) {
                if (!iso_read_sector(lba + i,
                                     out + total + i * ISO_SECTOR_SIZE))
                    return false;
            }
            total += sectors * ISO_SECTOR_SIZE;
            continue;
        }

        if (chunk > size - total) chunk = size - total;
        if (!iso_read_sector(lba, sector)) return false;
        kmemcpy(out + total, sector + within, chunk);
        total += chunk;
    }
    *bytes_read = total;
    return true;
}

bool iso9660_builtin_init(void) {
    static const iso9660_driver_ops_t ops = {
        iso_impl_mount_default,
        iso_impl_is_mounted,
        iso_impl_unmount,
        iso_impl_media_signature,
        iso_impl_resolve,
        iso_impl_list,
        iso_impl_read_at
    };

    /*
     * En un Live CD esta misma implementacion ya esta integrada en el kernel
     * y montada antes de cargar los modulos residentes.  ISO9660.DVR contiene
     * otra copia de estas variables estaticas; si reemplazara las operaciones
     * activas, su g_iso_device aun estaria a NULL y el resto del CD dejaria de
     * ser legible (incluido VESA.DVR).  Mantener la instancia que ya esta
     * montada hace que el modulo externo sea idempotente.
     */
    if (iso9660_is_mounted()) return true;
    return iso9660_register_driver(&ops);
}

const bk_driver_module_t *bleskernos_driver_query(void) {
    static const bk_driver_module_t module = {
        BK_DRIVER_ABI_VERSION,
        sizeof(bk_driver_module_t),
        1U, 0U,
        "iso9660",
        "Sistema de archivos para CD-ROM ISO9660",
        iso9660_builtin_init,
        NULL
    };
    return &module;
}
