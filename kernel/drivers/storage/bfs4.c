#include "../../include/bfs4.h"
#include "../../include/memory.h"
#include "../../include/pit.h"
#include "../../include/klock.h"
#include "../../include/rtc.h"
#include "../../include/vga.h"

#define BFS4_VERSION 0x00020000U
#define BFS4_INODE_SIZE 256U
#define BFS4_EXTENTS 8U
#define BFS4_INDIRECT_EXTENTS (BFS4_BLOCK_SIZE / sizeof(bfs4_extent_t))
#define BFS4_HASH_SIZE 32768U
#define BFS4_MODE_FILE 0100000U
#define BFS4_MODE_DIR 0040000U
#define BFS4_MODE_LINK 0120000U
#define BFS4_CLEAN 0x434c454eU
#define BFS4_DIRTY 0x44495254U

typedef struct __attribute__((packed)) {
    uint32_t magic, version, block_size, total_blocks, free_blocks;
    uint32_t block_bitmap_start, block_bitmap_blocks;
    uint32_t inode_bitmap_start, inode_bitmap_blocks;
    uint32_t inode_table_start, inode_table_blocks;
    uint32_t inode_count, root_inode, data_start, state, generation;
    uint64_t created;
    char label[32];
    uint32_t crc;
} bfs4_super_t;

typedef struct __attribute__((packed)) { uint32_t start, count; } bfs4_extent_t;
typedef struct __attribute__((packed)) {
    uint32_t id, parent;
    uint64_t size;
    uint16_t mode, flags;
    uint64_t created, modified, accessed;
    uint32_t generation, extent_count;
    bfs4_extent_t extents[BFS4_EXTENTS];
    uint32_t indirect;
    char name[128];
    uint32_t crc, reserved;
} bfs4_inode_t;
typedef char bfs4_inode_size_check[(sizeof(bfs4_inode_t) == BFS4_INODE_SIZE) ? 1 : -1];
typedef char bfs4_super_size_check[(sizeof(bfs4_super_t) == 108U) ? 1 : -1];

typedef struct {
    block_device_t *device;
    uint32_t partition_lba, start_lba;
    bfs4_super_t super;
    bfs4_super_t persisted_super;
    bool persisted_super_valid;
    uint8_t *block_bitmap, *inode_bitmap;
    bfs4_inode_t *inodes;
    bfs4_extent_t **indirect_tables;
    uint32_t *hash_heads, *hash_next;
    uint32_t *child_heads, *child_next;
    uint32_t allocation_hint, inode_hint;
    uint32_t swap_inode;
    bool ready;
    /* A failed write may leave disk and cached metadata different. Never
     * checkpoint that mount as clean; a fresh mount must validate the disk. */
    bool write_failed;
    uint32_t tx_depth, journal_sequence, journal_epoch, journal_count;
    uint32_t journal_targets[120];
    bool journal_ready, journal_active;
} bfs4_volume_t;

/* The boot volume is the only BFS volume mounted in this revision. Other
 * disks still use FAT; a second BFS mount must not alias these caches. */
static bfs4_volume_t g_bfs[4];
/* Native kernel callers and disk administration do not all hold the VFS
 * domain. Serialize every public entry, with a sleeping recursive mutex so
 * nested check/rename/cleanup calls and SMP never interleave transactions. */
static kmutex_t g_bfs_lock=KMUTEX_INITIALIZER;
static void bfs4_guard_leave(kmutex_t **lock) { kmutex_unlock(*lock); }
#define BFS4_GUARD kmutex_t *bfs4_guard __attribute__((cleanup(bfs4_guard_leave)))=&g_bfs_lock; kmutex_lock(bfs4_guard)


static bool bfs4_defrag_allows_clean(bfs4_volume_t *v);
static void bfs4_defrag_forget(bfs4_volume_t *v);
static bool bfs4_defrag_before_mutation(bfs4_volume_t *v,uint32_t inode);
static void bfs4_defrag_invalidate(bfs4_volume_t *v,uint32_t first,uint32_t count);

static bool bfs4_leap(uint32_t year) {
    return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
}

static uint64_t bfs4_now(const bfs4_volume_t *v) {
    static const uint16_t before_month[12] =
        {0,31,59,90,120,151,181,212,243,273,304,334};
    rtc_datetime_t now;
    uint64_t days = 0;
    if (!rtc_get_datetime(&now) || now.date.year < 1970U ||
        now.date.month < 1U || now.date.month > 12U || !now.date.day)
        return v->super.created + pit_get_uptime_seconds();
    for (uint32_t year = 1970U; year < now.date.year; ++year)
        days += bfs4_leap(year) ? 366U : 365U;
    days += before_month[now.date.month - 1U] + now.date.day - 1U;
    if (now.date.month > 2U && bfs4_leap(now.date.year)) ++days;
    return days * 86400U + now.time.hour * 3600U +
           now.time.minute * 60U + now.time.second;
}

typedef struct {
    block_device_t *device;
    uint32_t start_lba, block, age;
    bool valid;
    uint8_t data[BFS4_BLOCK_SIZE];
} bfs4_cache_entry_t;
static bfs4_cache_entry_t *g_cache;
static uint32_t g_cache_slots, g_cache_age, g_cache_generation;
static kspinlock_t g_cache_lock = KSPINLOCK_INITIALIZER;
/* Serialized by g_bfs_lock. Small demand-page reads can share one disk
 * command without exposing the speculative bytes to the caller. */
static uint8_t *g_read_ahead;
#define BFS4_READ_AHEAD_BLOCKS 16U

static void bfs4_cache_init(void) {
    if (g_cache) return;
    uint32_t ram = mm_total_bytes();
    uint32_t slots = ram >= 128U * 1024U * 1024U ? 2048U :
                     ram >= 64U * 1024U * 1024U ? 512U :
                     ram >= 48U * 1024U * 1024U ? 256U : 64U;
    bfs4_cache_entry_t *created = kzalloc(slots * sizeof(*created));
    if (!created) return;
    (void)mm_set_allocation_owner(created, 0U);
    uint32_t irq = kspin_lock_irqsave(&g_cache_lock);
    if (!g_cache) { g_cache = created; g_cache_slots = slots; created = NULL; }
    kspin_unlock_irqrestore(&g_cache_lock, irq);
    if (created) kfree(created);
    kprintf("[BFS4:CACHE] slots=%u bytes=%u\n",g_cache_slots,
            g_cache_slots*(uint32_t)sizeof(*g_cache));
}

static uint32_t bfs4_cache_base(const bfs4_volume_t *v, uint32_t block) {
    return ((block ^ v->start_lba ^ (uint32_t)((uintptr_t)v->device >> 4U)) &
            (g_cache_slots / 4U - 1U)) * 4U;
}

static bool bfs4_cache_lookup(bfs4_volume_t *v, uint32_t block, void *target) {
    if (!g_cache) return false;
    bool found = false;
    uint32_t irq = kspin_lock_irqsave(&g_cache_lock);
    uint32_t base = bfs4_cache_base(v, block);
    for (uint32_t i = 0; i < 4U; ++i) {
        bfs4_cache_entry_t *e = &g_cache[base + i];
        if (e->valid && e->device == v->device &&
            e->start_lba == v->start_lba && e->block == block) {
            kmemcpy(target, e->data, BFS4_BLOCK_SIZE);
            e->age = ++g_cache_age;
            found = true;
            break;
        }
    }
    kspin_unlock_irqrestore(&g_cache_lock, irq);
    return found;
}

static uint32_t bfs4_cache_epoch(void) {
    uint32_t irq = kspin_lock_irqsave(&g_cache_lock);
    uint32_t epoch = g_cache_generation;
    kspin_unlock_irqrestore(&g_cache_lock, irq);
    return epoch;
}

static void bfs4_cache_publish(bfs4_volume_t *v, uint32_t block,
                               const void *data, uint32_t epoch) {
    if (!g_cache) return;
    uint32_t irq = kspin_lock_irqsave(&g_cache_lock);
    if (g_cache_generation == epoch) {
        uint32_t base = bfs4_cache_base(v, block);
        bfs4_cache_entry_t *victim = &g_cache[base];
        for (uint32_t i = 0; i < 4U; ++i) {
            bfs4_cache_entry_t *e = &g_cache[base + i];
            if (!e->valid) { victim = e; break; }
            if (e->device == v->device && e->start_lba == v->start_lba &&
                e->block == block) { victim = e; break; }
            if (e->age < victim->age) victim = e;
        }
        victim->device = v->device;
        victim->start_lba = v->start_lba;
        victim->block = block;
        victim->age = ++g_cache_age;
        kmemcpy(victim->data, data, BFS4_BLOCK_SIZE);
        victim->valid = true;
    }
    kspin_unlock_irqrestore(&g_cache_lock, irq);
}

static void bfs4_cache_invalidate(bfs4_volume_t *v, uint32_t first,
                                  uint32_t count) {
    if (!g_cache) return;
    uint32_t irq = kspin_lock_irqsave(&g_cache_lock);
    ++g_cache_generation;
    for (uint32_t block = first; block < first + count; ++block) {
        uint32_t base = bfs4_cache_base(v, block);
        for (uint32_t i = 0; i < 4U; ++i) {
            bfs4_cache_entry_t *e = &g_cache[base + i];
            if (e->valid && e->device == v->device &&
                e->start_lba == v->start_lba && e->block == block)
                e->valid = false;
        }
    }
    kspin_unlock_irqrestore(&g_cache_lock, irq);
}

static uint32_t bfs4_crc(const void *data, uint32_t length) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t value = 0xffffffffU;
    for (uint32_t i = 0; i < length; ++i) {
        value ^= p[i];
        for (uint32_t j = 0; j < 8; ++j)
            value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return ~value;
}


static bool bfs4_read_super(block_device_t *dev, uint32_t start,
                            bfs4_super_t *super_out, uint32_t *crc_out) {
    uint8_t raw[BFS4_BLOCK_SIZE];

    if (!dev || !super_out || !crc_out) return false;
    for (uint32_t attempt = 1U; attempt <= 2U; attempt++) {
        bfs4_super_t super;
        uint32_t stored_crc;
        uint32_t actual_crc;
        bool valid;

        if (!block_read(dev, start, 8U, raw)) {
            kprintf("[BFS4] superblock: lectura fallo dev=%s lba=%u intento=%u\n",
                    dev->name, start, attempt);
            continue;
        }
        kmemcpy(&super, raw, sizeof(super));
        stored_crc = super.crc;
        super.crc = 0U;
        actual_crc = bfs4_crc(&super, sizeof(super));
        valid = super.magic == BFS4_MAGIC &&
                super.version == BFS4_VERSION &&
                super.block_size == BFS4_BLOCK_SIZE &&
                actual_crc == stored_crc &&
                super.total_blocks &&
                super.total_blocks <= (dev->sector_count - start) / 8U &&
                super.inode_count >= 2U && super.inode_count <= 65536U &&
                (uint64_t)super.inode_table_blocks * BFS4_BLOCK_SIZE >=
                    (uint64_t)super.inode_count * BFS4_INODE_SIZE &&
                super.data_start > super.inode_table_start &&
                super.data_start < super.total_blocks &&
                (uint64_t)super.block_bitmap_start + super.block_bitmap_blocks <=
                    super.inode_bitmap_start &&
                (uint64_t)super.inode_bitmap_start + super.inode_bitmap_blocks <=
                    super.inode_table_start &&
                (uint64_t)super.inode_table_start + super.inode_table_blocks <=
                    super.data_start &&
                super.block_bitmap_start>=1U &&
                super.block_bitmap_blocks==(super.total_blocks+32767U)/32768U &&
                super.inode_bitmap_blocks==(super.inode_count+32767U)/32768U &&
                super.inode_table_blocks==(super.inode_count+15U)/16U &&
                super.free_blocks<=super.total_blocks-super.data_start &&
                super.root_inode == 1U &&
                (super.state == BFS4_CLEAN || super.state == BFS4_DIRTY);
        if (valid) {
            *super_out = super;
            *crc_out = stored_crc;
            if (attempt > 1U)
                kprintf("[BFS4] superblock valido en reintento dev=%s\n",
                        dev->name);
            return true;
        }

        kprintf("[BFS4] superblock invalido dev=%s intento=%u "
                "magic=%x ver=%x block=%u state=%x crc=%x/%x\n",
                dev->name, attempt, super.magic, super.version,
                super.block_size, super.state, stored_crc, actual_crc);
    }
    return false;
}

#include "bfs4_journal.inc"

static bool bfs4_bit(const uint8_t *map, uint32_t index) {
    return (map[index >> 3] & (1U << (index & 7U))) != 0;
}

static void bfs4_set_bit(uint8_t *map, uint32_t index, bool value) {
    uint8_t mask = (uint8_t)(1U << (index & 7U));
    if (value) map[index >> 3] |= mask;
    else map[index >> 3] &= (uint8_t)~mask;
}

static bool bfs4_read_block(bfs4_volume_t *v, uint32_t block, void *data) {
    if (block >= v->super.total_blocks) return false;
    return block_read(v->device, v->start_lba + block * 8U, 8U, data);
}

static bool bfs4_write_block(bfs4_volume_t *v, uint32_t block, const void *data) {
    if (block >= v->super.total_blocks || v->device->read_only) return false;
    bool ok = (block >= v->super.data_start ||
               bfs4_journal_save(v, v->start_lba + block * 8U, 8U)) &&
              block_write(v->device, v->start_lba + block * 8U, 8U, data);
    if (!ok) v->write_failed = true;
    return ok;
}

static bool bfs4_load_blocks(bfs4_volume_t *v, uint32_t first,
                             uint32_t count, void *target) {
    for (uint32_t done = 0; done < count;) {
        uint32_t run = count - done;
        if (run > 31U) run = 31U;
        if (!block_read(v->device, v->start_lba + (first + done) * 8U,
                        (uint8_t)(run * 8U),
                        (uint8_t *)target + done * BFS4_BLOCK_SIZE)) return false;
        done += run;
    }
    return true;
}

static bool bfs4_write_super(bfs4_volume_t *v) {
    uint8_t raw[BFS4_BLOCK_SIZE];
    kmemset(raw, 0, sizeof(raw));
    v->super.crc = 0;
    v->super.crc = bfs4_crc(&v->super, sizeof(v->super));
    /* In-place data/timestamp updates often leave allocation counters and
     * every other superblock byte unchanged. Do not journal and flush that
     * identical checkpoint again. Compare bytes, not just their CRC. */
    if(!v->write_failed && v->persisted_super_valid &&
       !kmemcmp(&v->super,&v->persisted_super,sizeof(v->super)))return true;
    kmemcpy(raw, &v->super, sizeof(v->super));
    bool ok=bfs4_write_block(v, 0, raw);
    if(ok){v->persisted_super=v->super;v->persisted_super_valid=true;}
    else v->persisted_super_valid=false;
    return ok;
}

static bool bfs4_begin_change(bfs4_volume_t *v) {
    if (v->super.state == BFS4_DIRTY) return true;
    if (v->super.state != BFS4_CLEAN) return false;
    v->super.state = BFS4_DIRTY;
    if (bfs4_write_super(v)) return true;
    v->super.state = BFS4_CLEAN;
    return false;
}

static bool bfs4_write_bitmap_byte(bfs4_volume_t *v, bool inode, uint32_t index) {
    if (!bfs4_begin_change(v)) return false;
    uint32_t block = index / (BFS4_BLOCK_SIZE * 8U);
    uint8_t *map = inode ? v->inode_bitmap : v->block_bitmap;
    uint32_t start = inode ? v->super.inode_bitmap_start : v->super.block_bitmap_start;
    return bfs4_write_block(v, start + block, map + block * BFS4_BLOCK_SIZE);
}

static bool bfs4_write_inode(bfs4_volume_t *v, uint32_t number) {
    bfs4_inode_t *inode;
    uint32_t offset, lba;
    if (!number || number >= v->super.inode_count) return false;
    if (!bfs4_begin_change(v)) return false;
    inode = &v->inodes[number];
    inode->crc = 0;
    inode->crc = bfs4_crc(inode, sizeof(*inode));
    offset = number * BFS4_INODE_SIZE;
    lba = v->start_lba + v->super.inode_table_start * 8U + offset / 512U;
    bool ok = bfs4_journal_save(v, lba, 1U) && block_write(v->device, lba, 1U,
                          (uint8_t *)v->inodes + (offset & ~511U));
    if (!ok) v->write_failed = true;
    return ok;
}

static uint8_t bfs4_fold(uint8_t ch) {
    return ch >= 'A' && ch <= 'Z' ? (uint8_t)(ch + 32U) : ch;
}

static bool bfs4_equal(const char *a, const char *b) {
    while (*a && *b && bfs4_fold((uint8_t)*a) == bfs4_fold((uint8_t)*b)) {
        ++a; ++b;
    }
    return !*a && !*b;
}

static uint32_t bfs4_hash(uint32_t parent, const char *name) {
    uint32_t h = 2166136261U ^ parent;
    while (*name) h = (h ^ bfs4_fold((uint8_t)*name++)) * 16777619U;
    return h & (BFS4_HASH_SIZE - 1U);
}

static void bfs4_hash_insert(bfs4_volume_t *v, uint32_t number) {
    bfs4_inode_t *inode = &v->inodes[number];
    uint32_t slot = bfs4_hash(inode->parent, inode->name);
    v->hash_next[number] = v->hash_heads[slot];
    v->hash_heads[slot] = number;
    v->child_next[number] = v->child_heads[inode->parent];
    v->child_heads[inode->parent] = number;
}

static void bfs4_hash_remove(bfs4_volume_t *v, uint32_t number) {
    bfs4_inode_t *inode = &v->inodes[number];
    uint32_t *link = &v->hash_heads[bfs4_hash(inode->parent, inode->name)];
    while (*link) {
        if (*link == number) { *link = v->hash_next[number]; break; }
        link = &v->hash_next[*link];
    }
    v->hash_next[number] = 0;
    link = &v->child_heads[inode->parent];
    while (*link) {
        if (*link == number) { *link = v->child_next[number]; break; }
        link = &v->child_next[*link];
    }
    v->child_next[number] = 0;
}

static uint32_t bfs4_find(bfs4_volume_t *v, uint32_t parent, const char *name) {
    uint32_t slot = bfs4_hash(parent, name);
    for (uint32_t number = v->hash_heads[slot]; number; number = v->hash_next[number]) {
        bfs4_inode_t *inode = &v->inodes[number];
        if (inode->parent == parent && bfs4_equal(inode->name, name)) return number;
    }
    return 0;
}

static bfs4_volume_t *bfs4_volume(const fat_fs_t *fs) {
    if (!fs || fs->type != FAT_TYPE_BFS4) return NULL;
    for (uint32_t i = 0; i < 4U; ++i)
        if (g_bfs[i].ready && fs->device == g_bfs[i].device &&
            fs->volume_lba == g_bfs[i].partition_lba) return &g_bfs[i];
    return NULL;
}

static void bfs4_release(bfs4_volume_t *v) {
    if (v->indirect_tables) {
        for (uint32_t i = 0; i < v->super.inode_count; ++i)
            if (v->indirect_tables[i]) kfree(v->indirect_tables[i]);
        kfree(v->indirect_tables);
    }
    if (v->block_bitmap) kfree(v->block_bitmap);
    if (v->inode_bitmap) kfree(v->inode_bitmap);
    if (v->inodes) kfree(v->inodes);
    if (v->hash_heads) kfree(v->hash_heads);
    if (v->hash_next) kfree(v->hash_next);
    if (v->child_heads) kfree(v->child_heads);
    if (v->child_next) kfree(v->child_next);
    kmemset(v, 0, sizeof(*v));
}

bool bfs4_format(block_device_t *dev, const char *label) {
    BFS4_GUARD;
    uint8_t block[BFS4_BLOCK_SIZE], sector[512];
    bfs4_super_t super;
    bfs4_inode_t root;
    uint32_t total, inodes, data_start;
    for(uint32_t i=0;i<4U;i++)
        if(g_bfs[i].device==dev && g_bfs[i].swap_inode)return false;
    if (!dev || dev->sector_size != 512U || dev->read_only || !dev->write ||
        dev->sector_count <= BFS4_BOOT_SECTORS + 16384U) return false;
    total = (dev->sector_count - BFS4_BOOT_SECTORS) / 8U;
    inodes = total >= 8192U ? 16384U : 4096U;
    kmemset(&super, 0, sizeof(super));
    super.magic = BFS4_MAGIC;
    super.version = BFS4_VERSION;
    super.block_size = BFS4_BLOCK_SIZE;
    super.total_blocks = total;
    super.block_bitmap_start = 1U;
    super.block_bitmap_blocks = (total + 32767U) / 32768U;
    super.inode_bitmap_start = super.block_bitmap_start + super.block_bitmap_blocks;
    super.inode_bitmap_blocks = (inodes + 32767U) / 32768U;
    super.inode_table_start = super.inode_bitmap_start + super.inode_bitmap_blocks;
    super.inode_table_blocks = inodes * BFS4_INODE_SIZE / BFS4_BLOCK_SIZE;
    data_start = super.inode_table_start + super.inode_table_blocks;
    if (data_start >= total) return false;
    super.data_start = data_start;
    super.free_blocks = total - data_start;
    super.inode_count = inodes;
    super.root_inode = 1U;
    super.state = BFS4_CLEAN;
    super.generation = 1U;
    if (label) kstrncpy(super.label, label, sizeof(super.label) - 1U);
    for (uint32_t i = 0; i < 4U; ++i)
        if (g_bfs[i].ready && g_bfs[i].device == dev) bfs4_release(&g_bfs[i]);
    if (!bfs4_journal_format(dev)) return false;
    /* Write only the BFS region. The BIOS boot loader lives in the first
     * 4096 sectors and is installed separately by the installer. */
    /* Zero metadata in runs of eight BFS blocks (32 KiB). Each
     * block_write() is still durable: only the number of commands and
     * cache flushes changes, not the filesystem layout or ordering.
     * Low-memory fallback retains the original 4 KiB writes. */
    uint8_t *zero_run = (uint8_t *)kmalloc(8U * BFS4_BLOCK_SIZE);
    uint32_t zero_batch = zero_run ? 8U : 1U;
    bool zero_ok = true;
    kmemset(block, 0, sizeof(block));
    if (zero_run) kmemset(zero_run, 0, zero_batch * BFS4_BLOCK_SIZE);
    for (uint32_t i = 1U; i < data_start;) {
        uint32_t run = data_start - i;
        if (run > zero_batch) run = zero_batch;
        if (!block_write(dev, BFS4_BOOT_SECTORS + i * 8U,
                         (uint8_t)(run * 8U), zero_run ? zero_run : block)) {
            /* Old IDE implementations may reject a 64-sector command.
             * Retry this run as ordinary 4 KiB blocks before failing. */
            zero_ok = run > 1U;
            for (uint32_t j = 0; zero_ok && j < run; ++j)
                if (!block_write(dev, BFS4_BOOT_SECTORS + (i + j) * 8U,
                                 8U, block)) zero_ok = false;
            if (!zero_ok) break;
        }
        i += run;
    }
    if (zero_run) kfree(zero_run);
    if (!zero_ok) return false;
    for (uint32_t i = 0U; i < data_start; ++i) bfs4_set_bit(block, i, true);
    if (!block_write(dev, BFS4_BOOT_SECTORS + super.block_bitmap_start * 8U,
                     8U, block)) return false;
    kmemset(block, 0, sizeof(block));
    bfs4_set_bit(block, 0U, true);
    bfs4_set_bit(block, 1U, true);
    if (!block_write(dev, BFS4_BOOT_SECTORS + super.inode_bitmap_start * 8U,
                     8U, block)) return false;
    kmemset(&root, 0, sizeof(root));
    root.id = root.parent = 1U;
    root.mode = BFS4_MODE_DIR | 0755U;
    root.generation = 1U;
    root.name[0] = '/';
    root.crc = bfs4_crc(&root, sizeof(root));
    kmemset(sector, 0, sizeof(sector));
    kmemcpy(sector + BFS4_INODE_SIZE, &root, sizeof(root));
    if (!block_write(dev, BFS4_BOOT_SECTORS + super.inode_table_start * 8U,
                     1U, sector)) return false;
    super.crc = bfs4_crc(&super, sizeof(super));
    kmemset(block, 0, sizeof(block));
    kmemcpy(block, &super, sizeof(super));
    return block_write(dev, BFS4_BOOT_SECTORS, 8U, block);
}

bool bfs4_mount(fat_fs_t *fs, block_device_t *dev, uint32_t partition_lba) {
    BFS4_GUARD;
    bfs4_super_t super;
    bfs4_volume_t *v = NULL;
    uint32_t crc;
    uint32_t start = partition_lba + BFS4_BOOT_SECTORS;
    bool live=false;
    for(uint32_t i=0;i<4U;i++)
        if(g_bfs[i].ready && g_bfs[i].device==dev &&
           g_bfs[i].partition_lba==partition_lba) {
            if(g_bfs[i].write_failed)return false;
            live=true;
        }
    if (!fs || !dev || dev->sector_size != 512U ||
        start < partition_lba || start + 8U < start ||
        start + 8U > dev->sector_count ||
        (!live && !bfs4_journal_recover(dev, partition_lba)) ||
        !bfs4_read_super(dev, start, &super, &crc)) return false;
    for (uint32_t i = 0; i < 4U; ++i)
        if (g_bfs[i].ready && g_bfs[i].device == dev &&
            g_bfs[i].partition_lba == partition_lba) { v = &g_bfs[i]; break; }
    if (!v) {
        for (uint32_t i = 0; i < 4U; ++i)
            if (!g_bfs[i].ready && !g_bfs[i].device) { v = &g_bfs[i]; break; }
        if (!v) return false;
        v->device = dev;
        v->partition_lba = partition_lba;
        v->start_lba = start;
        v->super = super;
        v->super.crc = crc;
        if (!bfs4_journal_load(v)) { bfs4_release(v); return false; }
        v->block_bitmap = kmalloc(super.block_bitmap_blocks * BFS4_BLOCK_SIZE);
        v->inode_bitmap = kmalloc(super.inode_bitmap_blocks * BFS4_BLOCK_SIZE);
        v->inodes = kmalloc(super.inode_table_blocks * BFS4_BLOCK_SIZE);
        v->indirect_tables = kzalloc(super.inode_count * sizeof(bfs4_extent_t *));
        v->hash_heads = kzalloc(BFS4_HASH_SIZE * sizeof(uint32_t));
        v->hash_next = kzalloc(super.inode_count * sizeof(uint32_t));
        v->child_heads = kzalloc(super.inode_count * sizeof(uint32_t));
        v->child_next = kzalloc(super.inode_count * sizeof(uint32_t));
        if (!v->block_bitmap || !v->inode_bitmap || !v->inodes || !v->indirect_tables ||
            !v->hash_heads || !v->hash_next || !v->child_heads ||
            !v->child_next) { bfs4_release(v); return false; }
        if (!bfs4_load_blocks(v, super.block_bitmap_start,
                              super.block_bitmap_blocks, v->block_bitmap) ||
            !bfs4_load_blocks(v, super.inode_bitmap_start,
                              super.inode_bitmap_blocks, v->inode_bitmap) ||
            !bfs4_load_blocks(v, super.inode_table_start,
                              super.inode_table_blocks, v->inodes)) {
            bfs4_release(v); return false;
        }
        for (uint32_t i = 1; i < super.inode_count; ++i) {
            if (!bfs4_bit(v->inode_bitmap, i)) continue;
            bfs4_inode_t *inode = &v->inodes[i];
            uint32_t saved = inode->crc;
            inode->crc = 0;
            bool valid = inode->id == i && bfs4_crc(inode, sizeof(*inode)) == saved &&
                         inode->extent_count <= BFS4_EXTENTS + BFS4_INDIRECT_EXTENTS &&
                         (inode->extent_count <= BFS4_EXTENTS || inode->indirect) &&
                         inode->parent > 0U && inode->parent < super.inode_count &&
                         inode->name[127] == 0;
            inode->crc = saved;
            if (!valid) { bfs4_release(v); return false; }
            if (i > 1U) bfs4_hash_insert(v, i);
        }
        if (!bfs4_bit(v->inode_bitmap, 1U) ||
            (v->inodes[1].mode & 0170000U) != BFS4_MODE_DIR) {
            bfs4_release(v); return false;
        }
        for (uint32_t i = 2U; i < super.inode_count; ++i) {
            if (!bfs4_bit(v->inode_bitmap, i)) continue;
            uint32_t parent = v->inodes[i].parent;
            if (!bfs4_bit(v->inode_bitmap, parent) ||
                (v->inodes[parent].mode & 0170000U) != BFS4_MODE_DIR) {
                bfs4_release(v); return false;
            }
        }
        v->allocation_hint = super.data_start;
        v->inode_hint = 2U;
        v->ready = true;
    }
    kmemset(fs, 0, sizeof(*fs));
    fs->device = dev;
    fs->volume_lba = partition_lba;
    fs->type = FAT_TYPE_BFS4;
    fs->bytes_per_sector = 512U;
    fs->sectors_per_cluster = 8U;
    fs->total_sectors = super.total_blocks * 8U;
    fs->total_clusters = super.total_blocks - super.data_start;
    fs->root_dir_cluster = 1U;
    kmemcpy(fs->volume_label, super.label, 11U);
    kmemcpy(fs->fs_name, "BFS4", 5U);
    bfs4_cache_init();
    /* Reopening a live pinned volume must never discard its metadata or pin
     * because a recovery scan cannot allocate scratch memory under pressure. */
    if (!live && !v->swap_inode) {
        fat_check_report_t report;
        if (!bfs4_check(fs, &report)) {
            bfs4_release(v);
            return false;
        }
        /* Only the final free-space checkpoint is stale. The full scan has
         * validated references and bitmaps; recomputing a counter changes
         * no allocation, extent or file contents. */
        if (v->super.state==BFS4_DIRTY && report.errors == 1U && report.free_clusters != v->super.free_blocks) {
            v->super.free_blocks = report.free_clusters;
            if (!bfs4_check(fs, &report)) { bfs4_release(v); return false; }
        }
        if (report.errors) { bfs4_release(v); return false; }
        if (!dev->read_only && v->super.state==BFS4_DIRTY) {
            v->super.state = BFS4_CLEAN;
            if (!bfs4_write_super(v)) { bfs4_release(v); return false; }
        }
    }
    return true;
}

static bool bfs4_lookup(bfs4_volume_t *v, const char *path, uint32_t *result) {
    uint32_t current = 1U;
    if (!path || path[0] != '/') return false;
    while (*path == '/') ++path;
    while (*path) {
        char part[128];
        uint32_t n = 0;
        while (*path && *path != '/') {
            if (n >= 127U) return false;
            part[n++] = *path++;
        }
        part[n] = 0;
        while (*path == '/') ++path;
        if (n == 1U && part[0] == '.') continue;
        if (n == 2U && part[0] == '.' && part[1] == '.') {
            current = v->inodes[current].parent;
            continue;
        }
        if ((v->inodes[current].mode & 0170000U) != BFS4_MODE_DIR) return false;
        current = bfs4_find(v, current, part);
        if (!current) return false;
    }
    *result = current;
    return true;
}

static bool bfs4_parent(bfs4_volume_t *v, const char *path,
                        uint32_t *parent, char *name) {
    char parent_path[260];
    uint32_t length = 0, slash = 0;
    if (!path || path[0] != '/') return false;
    while (path[length]) {
        if (length >= 259U) return false;
        if (path[length] == '/') slash = length;
        ++length;
    }
    if (!length || slash == length - 1U || length - slash > 128U) return false;
    kmemcpy(name, path + slash + 1U, length - slash);
    if (slash == 0U) parent_path[0] = '/', parent_path[1] = 0;
    else { kmemcpy(parent_path, path, slash); parent_path[slash] = 0; }
    return bfs4_lookup(v, parent_path, parent) &&
           (v->inodes[*parent].mode & 0170000U) == BFS4_MODE_DIR;
}

static void bfs4_entry(const bfs4_inode_t *inode, fat_dir_entry_t *entry) {
    kmemset(entry, 0, sizeof(*entry));
    kstrncpy(entry->name, inode->name, FAT_MAX_NAME - 1U);
    kstrncpy(entry->short_name, inode->name, sizeof(entry->short_name) - 1U);
    entry->first_cluster = inode->id;
    entry->size = inode->size > 0xffffffffULL ? 0xffffffffU : (uint32_t)inode->size;
    entry->is_directory = (inode->mode & 0170000U) == BFS4_MODE_DIR;
    entry->attributes = entry->is_directory ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
}

bool bfs4_resolve(const fat_fs_t *fs, const char *path, fat_dir_entry_t *entry) {
    BFS4_GUARD;
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t number;
    if (!v || !entry || !bfs4_lookup(v, path, &number)) return false;
    bfs4_entry(&v->inodes[number], entry);
    return true;
}

bool bfs4_list(const fat_fs_t *fs, const fat_dir_entry_t *dir,
               fat_dir_entry_t *entries, fat_dir_metadata_t *metadata,
               uint32_t capacity, uint32_t *count) {
    BFS4_GUARD;
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t parent = dir ? dir->first_cluster : 1U, found = 0;
    if (!v || !entries || !count || parent >= v->super.inode_count ||
        (v->inodes[parent].mode & 0170000U) != BFS4_MODE_DIR) return false;
    for (uint32_t i = v->child_heads[parent]; i && found < capacity;
         i = v->child_next[i]) {
        bfs4_entry(&v->inodes[i], &entries[found]);
        if (metadata) {
            metadata[found].position = i;
            metadata[found].mode = v->inodes[i].mode;
        }
        ++found;
    }
    *count = found;
    return true;
}

static bfs4_extent_t *bfs4_indirect_table(bfs4_volume_t *v,
                                           bfs4_inode_t *inode) {
    if (!inode->indirect || inode->id >= v->super.inode_count) return NULL;
    bfs4_extent_t *table = v->indirect_tables[inode->id];
    if (table) return table;
    table = kmalloc(BFS4_BLOCK_SIZE);
    if (!table) return NULL;
    if (!bfs4_read_block(v, inode->indirect, table)) {
        kfree(table);
        return NULL;
    }
    (void)mm_set_allocation_owner(table, 0U);
    v->indirect_tables[inode->id] = table;
    return table;
}

static bfs4_extent_t *bfs4_extent(bfs4_volume_t *v, bfs4_inode_t *inode,
                                  uint32_t index) {
    if (index < BFS4_EXTENTS) return &inode->extents[index];
    if (index >= BFS4_EXTENTS + BFS4_INDIRECT_EXTENTS) return NULL;
    bfs4_extent_t *table = bfs4_indirect_table(v, inode);
    return table ? &table[index - BFS4_EXTENTS] : NULL;
}

static bool bfs4_flush_indirect(bfs4_volume_t *v, bfs4_inode_t *inode) {
    bfs4_extent_t *table = bfs4_indirect_table(v, inode);
    if (!table || !bfs4_begin_change(v)) return false;
    bfs4_cache_invalidate(v, inode->indirect, 1U);
    return bfs4_journal_save(v, v->start_lba + inode->indirect * 8U, 8U) &&
           bfs4_write_block(v, inode->indirect, table);
}

static uint32_t bfs4_map(bfs4_volume_t *v, bfs4_inode_t *inode,
                         uint32_t logical) {
    for (uint32_t i = 0; i < inode->extent_count; ++i) {
        bfs4_extent_t *extent = bfs4_extent(v, inode, i);
        if (!extent) return 0;
        if (logical < extent->count) return extent->start + logical;
        logical -= extent->count;
    }
    return 0;
}

bool bfs4_read(const fat_fs_t *fs, const fat_dir_entry_t *entry,
               uint32_t offset, void *buffer, uint32_t length, uint32_t *done) {
    BFS4_GUARD;
    bfs4_volume_t *v = bfs4_volume(fs);
    uint8_t scratch[BFS4_BLOCK_SIZE];
    uint32_t number, total = 0;
    if (!v || !entry || !buffer || !done) return false;
    *done = 0;
    number = entry->first_cluster;
    if(number && number==v->swap_inode)return false;
    if (!number || number >= v->super.inode_count ||
        !bfs4_bit(v->inode_bitmap, number)) return false;
    bfs4_inode_t *inode = &v->inodes[number];
    if ((inode->mode & 0170000U) == BFS4_MODE_DIR) return false;
    if (offset >= inode->size) return true;
    if (length > inode->size - offset) length = (uint32_t)(inode->size - offset);
    while (total < length) {
        uint32_t position = offset + total;
        uint32_t block = bfs4_map(v, inode, position / BFS4_BLOCK_SIZE);
        uint32_t local = position & (BFS4_BLOCK_SIZE - 1U);
        uint32_t chunk = BFS4_BLOCK_SIZE - local;
        if (chunk > length - total) chunk = length - total;
        if (!block) return false;
        if (local == 0U && chunk == BFS4_BLOCK_SIZE) {
            uint32_t run = 1U;
            while (run < 31U && length - total >= (run + 1U) * BFS4_BLOCK_SIZE &&
                   bfs4_map(v, inode, position / BFS4_BLOCK_SIZE + run) == block + run)
                ++run;
            bool cached = true;
            for (uint32_t i = 0; i < run; ++i)
                if (!bfs4_cache_lookup(v, block + i,
                                       (uint8_t *)buffer + total + i * BFS4_BLOCK_SIZE)) {
                    cached = false;
                    break;
                }
            if (!cached) {
                uint32_t epoch = bfs4_cache_epoch();
                uint32_t fetch = run;
                void *target = (uint8_t *)buffer + total;
                if (g_cache && mm_total_bytes() >= 128U * 1024U * 1024U) {
                    while (fetch < BFS4_READ_AHEAD_BLOCKS &&
                           (uint64_t)position / BFS4_BLOCK_SIZE + fetch <
                               inode->size / BFS4_BLOCK_SIZE &&
                           bfs4_map(v, inode, position / BFS4_BLOCK_SIZE + fetch) == block + fetch)
                        ++fetch;
                    if (fetch > run && !g_read_ahead) {
                        g_read_ahead = kmalloc(BFS4_READ_AHEAD_BLOCKS * BFS4_BLOCK_SIZE);
                        if (g_read_ahead) (void)mm_set_allocation_owner(g_read_ahead, 0U);
                    }
                    if (fetch > run && g_read_ahead) target = g_read_ahead;
                    else fetch = run;
                }
                bool ok = block_read(v->device, v->start_lba + block * 8U,
                                     (uint8_t)(fetch * 8U), target);
                /* A failure in the speculative tail must not turn a readable
                 * requested page into an I/O error. OOM also keeps the old path. */
                if (!ok && fetch > run) {
                    fetch = run; target = (uint8_t *)buffer + total;
                    ok = block_read(v->device, v->start_lba + block * 8U,
                                (uint8_t)(run * 8U),
                                target);
                }
                if (!ok) return false;
                if (target == g_read_ahead)
                    kmemcpy((uint8_t *)buffer + total, target, run * BFS4_BLOCK_SIZE);
                for (uint32_t i = 0; i < fetch; ++i)
                    bfs4_cache_publish(v, block + i,
                        (uint8_t *)target + i * BFS4_BLOCK_SIZE, epoch);
            }
            total += run * BFS4_BLOCK_SIZE;
            continue;
        } else {
            if (!bfs4_cache_lookup(v, block, scratch)) {
                uint32_t epoch = bfs4_cache_epoch();
                if (!bfs4_read_block(v, block, scratch)) return false;
                bfs4_cache_publish(v, block, scratch, epoch);
            }
            kmemcpy((uint8_t *)buffer + total, scratch + local, chunk);
        }
        total += chunk;
    }
    *done = total;
    return true;
}

static uint32_t bfs4_allocate_blocks(bfs4_volume_t *v, uint32_t count) {
    uint32_t begin = v->allocation_hint;
    if (!count || count > v->super.free_blocks) return 0;
    for (uint32_t pass = 0; pass < 2U; ++pass) {
        uint32_t low = pass ? v->super.data_start : begin;
        uint32_t high = pass ? begin : v->super.total_blocks;
        uint32_t run = 0;
        for (uint32_t i = low; i < high; ++i) {
            if (bfs4_bit(v->block_bitmap, i)) { run = 0; continue; }
            if (++run == count) {
                uint32_t first = i + 1U - count;
                for (uint32_t j = first; j <= i; ++j) bfs4_set_bit(v->block_bitmap, j, true);
                for (uint32_t j = first / 32768U; j <= i / 32768U; ++j)
                    if (!bfs4_write_bitmap_byte(v, false, j * 32768U)) {
                        for (uint32_t k = first; k <= i; ++k) bfs4_set_bit(v->block_bitmap, k, false);
                        return 0;
                    }
                v->super.free_blocks -= count;
                v->allocation_hint = i + 1U < v->super.total_blocks ? i + 1U : v->super.data_start;
                return first;
            }
        }
    }
    return 0;
}

static bool bfs4_free_extent(bfs4_volume_t *v, uint32_t first, uint32_t count) {
    if(first<v->super.data_start || first>=v->super.total_blocks ||
       !count || count>v->super.total_blocks-first)return false;
    for(uint32_t i=first;i<first+count;i++)
        if(!bfs4_bit(v->block_bitmap,i))return false;
    for (uint32_t i = first; i < first + count; ++i) bfs4_set_bit(v->block_bitmap, i, false);
    for (uint32_t j = first / 32768U; j <= (first + count - 1U) / 32768U; ++j)
        if (!bfs4_write_bitmap_byte(v, false, j * 32768U)) return false;
    v->super.free_blocks += count;
    if (first < v->allocation_hint) v->allocation_hint = first;
    return true;
}

/* Buffer addresses are relative to this write, never to the whole file. */
static bool bfs4_write_data(bfs4_volume_t *v, bfs4_inode_t *inode,
                            const void *data, uint32_t start, uint32_t length) {
    uint8_t scratch[BFS4_BLOCK_SIZE];
    const uint8_t *p = data;
    uint32_t done = 0;
    if (!length) return true;
    if (!bfs4_begin_change(v)) return false;
    while (done < length) {
        uint32_t position = start + done;
        uint32_t block = bfs4_map(v, inode, position / BFS4_BLOCK_SIZE);
        uint32_t local = position & (BFS4_BLOCK_SIZE - 1U);
        uint32_t count = BFS4_BLOCK_SIZE - local;
        if (count > length - done) count = length - done;
        if (!block) return false;
        if (!local && count == BFS4_BLOCK_SIZE && p) {
            uint32_t run = 1U;
            while (run < 31U && length - done >= (run + 1U) * BFS4_BLOCK_SIZE &&
                   bfs4_map(v, inode, position / BFS4_BLOCK_SIZE + run) == block + run)
                ++run;
            bfs4_defrag_invalidate(v,block,run);
            bfs4_cache_invalidate(v, block, run);
            if (!block_write(v->device, v->start_lba + block * 8U,
                             (uint8_t)(run * 8U), p + done)) {
                v->write_failed = true;
                return false;
            }
            done += run * BFS4_BLOCK_SIZE;
        } else {
            if ((local || count != BFS4_BLOCK_SIZE) &&
                !bfs4_read_block(v, block, scratch)) return false;
            if (p) kmemcpy(scratch + local, p + done, count);
            else kmemset(scratch + local, 0, count);
            bfs4_defrag_invalidate(v,block,1U);
            bfs4_cache_invalidate(v, block, 1U);
            if (!bfs4_write_block(v, block, scratch)) return false;
            done += count;
        }
    }
    return true;
}

static uint32_t bfs4_alloc_inode(bfs4_volume_t *v) {
    for (uint32_t pass = 0; pass < 2U; ++pass) {
        uint32_t low = pass ? 2U : v->inode_hint;
        uint32_t high = pass ? v->inode_hint : v->super.inode_count;
        for (uint32_t i = low; i < high; ++i) {
            if (bfs4_bit(v->inode_bitmap, i)) continue;
            bfs4_set_bit(v->inode_bitmap, i, true);
            if (!bfs4_write_bitmap_byte(v, true, i)) {
                bfs4_set_bit(v->inode_bitmap, i, false);
                return 0;
            }
            v->inode_hint = i + 1U < v->super.inode_count ? i + 1U : 2U;
            return i;
        }
    }
    return 0;
}

static uint32_t bfs4_create(bfs4_volume_t *v, uint32_t parent,
                            const char *name, uint16_t mode) {
    uint32_t number = bfs4_alloc_inode(v);
    if (!number) return 0;
    bfs4_inode_t *inode = &v->inodes[number];
    kmemset(inode, 0, sizeof(*inode));
    inode->id = number; inode->parent = parent; inode->mode = mode;
    inode->created = bfs4_now(v);
    inode->modified = inode->accessed = inode->created;
    inode->generation = v->super.generation++;
    kstrncpy(inode->name, name, sizeof(inode->name) - 1U);
    if (!bfs4_write_inode(v, number)) return 0;
    bfs4_hash_insert(v, number);
    if (!bfs4_write_super(v)) return 0;
    return number;
}

static bool bfs4_store(fat_fs_t *fs, const char *path, const void *data,
                       uint32_t offset, uint32_t length, uint32_t size,
                       uint32_t expected_size) {
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t number, parent, needed, owned = 0, new_start = 0;
    char name[128];
    if (!v || (length && !data) || offset > size || length > size - offset ||
        !bfs4_parent(v, path, &parent, name)) return false;
    number = bfs4_find(v, parent, name);
    if (expected_size != 0xffffffffU &&
        (!number || v->inodes[number].size != expected_size)) return false;
    if (!number) number = bfs4_create(v, parent, name, BFS4_MODE_FILE | 0644U);
    if (!number) return false;
    if (!bfs4_defrag_before_mutation(v,number)) return false;
    bfs4_inode_t *inode = &v->inodes[number];
    if ((inode->mode & 0170000U) == BFS4_MODE_DIR) return false;
    for (uint32_t i = 0; i < inode->extent_count; ++i) {
        bfs4_extent_t *extent = bfs4_extent(v, inode, i);
        if (!extent) return false;
        owned += extent->count;
    }
    needed = size / BFS4_BLOCK_SIZE + (size % BFS4_BLOCK_SIZE != 0U);
    if (needed > owned) {
        uint32_t extra = needed - owned;
        new_start = bfs4_allocate_blocks(v, extra);
        if (!new_start) return false;
        bfs4_extent_t *last = inode->extent_count
            ? bfs4_extent(v, inode, inode->extent_count - 1U) : NULL;
        if (inode->extent_count && !last) return false;
        if (last && last->start + last->count == new_start) {
            last->count += extra;
        } else if (inode->extent_count < BFS4_EXTENTS + BFS4_INDIRECT_EXTENTS) {
            if (inode->extent_count == BFS4_EXTENTS && !inode->indirect) {
                uint32_t indirect_block = bfs4_allocate_blocks(v, 1U);
                bfs4_extent_t *table = kzalloc(BFS4_BLOCK_SIZE);
                if (!indirect_block || !table) {
                    if (indirect_block) (void)bfs4_free_extent(v, indirect_block, 1U);
                    if (table) kfree(table);
                    (void)bfs4_free_extent(v, new_start, extra);
                    return false;
                }
                (void)mm_set_allocation_owner(table, 0U);
                inode->indirect = indirect_block;
                v->indirect_tables[number] = table;
            }
            bfs4_extent_t *next = bfs4_extent(v, inode, inode->extent_count);
            if (!next) {
                (void)bfs4_free_extent(v, new_start, extra);
                return false;
            }
            next->start = new_start;
            next->count = extra;
            ++inode->extent_count;
        } else {
            (void)bfs4_free_extent(v, new_start, extra);
            return false;
        }
        if (inode->indirect && !bfs4_flush_indirect(v, inode)) return false;
    }
    /* Publish newly reserved extents with the old EOF before data I/O. A
     * failed transfer must not leave allocated blocks unreachable on disk. */
    if (needed > owned &&
        (!bfs4_write_inode(v, number) || !bfs4_write_super(v))) return false;
    uint32_t old_size = (uint32_t)inode->size;
    uint32_t zero_end = length ? offset : size;
    bool transferred = zero_end <= old_size ||
        bfs4_write_data(v, inode, NULL, old_size, zero_end - old_size);
    if (transferred && length)
        transferred = bfs4_write_data(v, inode, data, offset, length);
    /* On a failed transfer retain the old EOF and release reservations made
     * for this operation. In-place writes may have changed an old prefix. */
    uint32_t retained = transferred ? needed : owned;
    if (needed > owned) owned = needed;
    while (owned > retained) {
        bfs4_extent_t *last = bfs4_extent(v, inode, inode->extent_count - 1U);
        if (!last) return false;
        uint32_t release = owned - retained;
        if (release > last->count) release = last->count;
        if (!bfs4_free_extent(v, last->start + last->count - release, release)) return false;
        last->count -= release; owned -= release;
        if (inode->extent_count > BFS4_EXTENTS && !bfs4_flush_indirect(v, inode)) return false;
        if (!last->count) inode->extent_count--;
    }
    if(inode->indirect && inode->extent_count<=BFS4_EXTENTS) {
        uint32_t old_indirect=inode->indirect;
        if(!bfs4_free_extent(v,old_indirect,1U))return false;
        if(v->indirect_tables[number])kfree(v->indirect_tables[number]);
        v->indirect_tables[number]=NULL;inode->indirect=0;
    }
    if (transferred) inode->size = size;
    inode->modified = bfs4_now(v);
    bool persisted = bfs4_write_inode(v, number) && bfs4_write_super(v);
    return transferred && persisted;
}

static bool bfs4_write_impl(fat_fs_t *fs, const char *path, const void *data, uint32_t size) {
    return bfs4_store(fs, path, data, 0U, size, size, 0xffffffffU);
}

static bool bfs4_append_impl(fat_fs_t *fs, const char *path, const void *data,
                 uint32_t old_size, uint32_t new_size) {
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t number;
    if (!v || !data || !bfs4_lookup(v, path, &number) ||
        v->inodes[number].size != old_size || new_size < old_size) return false;
    /* The coherent VFS cache contains the original prefix. Rewriting the
     * boundary block preserves its old bytes, then only new blocks go out. */
    uint32_t start = (old_size / BFS4_BLOCK_SIZE) * BFS4_BLOCK_SIZE;
    return bfs4_store(fs, path, (const uint8_t *)data + start,
                      start, new_size - start, new_size, old_size);
}

static bool bfs4_write_at_impl(fat_fs_t *fs, const char *path, uint32_t offset,
                   const void *data, uint32_t length) {
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t number;
    if (!v || !data || length > 0xffffffffU - offset ||
        !bfs4_lookup(v, path, &number)) return false;
    if (!length) return true;
    uint32_t size = (uint32_t)v->inodes[number].size;
    uint32_t end = offset + length;
    if (end > size) size = end;
    return bfs4_store(fs, path, data, offset, length, size, 0xffffffffU);
}

static bool bfs4_resize_impl(fat_fs_t *fs, const char *path, uint32_t size) {
    return bfs4_store(fs, path, NULL, 0U, 0U, size, 0xffffffffU);
}

static bool bfs4_mkdir_impl(fat_fs_t *fs, const char *path) {
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t parent;
    char name[128];
    if (!v || !bfs4_parent(v, path, &parent, name)) return false;
    if (bfs4_find(v, parent, name)) return true;
    return bfs4_create(v, parent, name, BFS4_MODE_DIR | 0755U) != 0;
}

static bool bfs4_remove_impl(fat_fs_t *fs, const char *path) {
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t number;
    if (!v || !bfs4_lookup(v, path, &number) || number == 1U) return false;
    if (!bfs4_defrag_before_mutation(v,number)) return false;
    bfs4_inode_t *inode = &v->inodes[number];
    if ((inode->mode & 0170000U) == BFS4_MODE_DIR && v->child_heads[number])
        return false;
    bfs4_hash_remove(v, number);
    for (uint32_t i = 0; i < inode->extent_count; ++i) {
        bfs4_extent_t *extent = bfs4_extent(v, inode, i);
        if (!extent || !bfs4_free_extent(v, extent->start, extent->count)) return false;
    }
    if (inode->indirect) {
        if (!bfs4_free_extent(v, inode->indirect, 1U)) return false;
        if (v->indirect_tables[number]) kfree(v->indirect_tables[number]);
        v->indirect_tables[number] = NULL;
    }
    kmemset(inode, 0, sizeof(*inode));
    if (!bfs4_write_inode(v, number)) return false;
    bfs4_set_bit(v->inode_bitmap, number, false);
    if (!bfs4_write_bitmap_byte(v, true, number)) return false;
    if (number < v->inode_hint) v->inode_hint = number;
    return bfs4_write_super(v);
}

static bool bfs4_rename_impl(fat_fs_t *fs, const char *old_path, const char *new_path) {
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t number, parent, replaced;
    char name[128];
    if (!v || !bfs4_lookup(v, old_path, &number) || number == 1U ||
        !bfs4_parent(v, new_path, &parent, name)) return false;
    if (!bfs4_defrag_before_mutation(v,number)) return false;
    /* Keep the boot-time path stable as well as its physical blocks. */
    for(uint32_t ancestor=v->swap_inode;ancestor && ancestor!=1U;
        ancestor=v->inodes[ancestor].parent)
        if(ancestor==number)return false;
    if ((v->inodes[number].mode & 0170000U) == BFS4_MODE_DIR) {
        for (uint32_t current = parent; current != 1U; current = v->inodes[current].parent)
            if (current == number) return false;
    }
    replaced = bfs4_find(v, parent, name);
    if (replaced == number) return true;
    if(replaced && ((v->inodes[number].mode&0170000U)==BFS4_MODE_DIR)!=
       ((v->inodes[replaced].mode&0170000U)==BFS4_MODE_DIR))return false;
    if (replaced && !bfs4_remove(fs, new_path)) return false;
    bfs4_hash_remove(v, number);
    v->inodes[number].parent = parent;
    kmemset(v->inodes[number].name, 0, sizeof(v->inodes[number].name));
    kstrncpy(v->inodes[number].name, name, sizeof(v->inodes[number].name) - 1U);
    if (!bfs4_write_inode(v, number)) return false;
    bfs4_hash_insert(v, number);
    return true;
}

bool bfs4_inode(const fat_fs_t *fs, const char *path, uint64_t *inode) {
    BFS4_GUARD;
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t number;
    if (!v || !inode || !bfs4_lookup(v, path, &number)) return false;
    *inode = number;
    return true;
}

bool bfs4_get_mode(const fat_fs_t *fs, const char *path, uint16_t *mode) {
    BFS4_GUARD;
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t number;
    if (!v || !mode || !bfs4_lookup(v, path, &number)) return false;
    *mode = v->inodes[number].mode;
    return true;
}

static bool bfs4_set_mode_impl(fat_fs_t *fs, const char *path, uint16_t mode) {
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t number;
    if (!v || !bfs4_lookup(v, path, &number)) return false;
    if (!bfs4_defrag_before_mutation(v,number)) return false;
    if (mode & 0170000U) v->inodes[number].mode = mode;
    else v->inodes[number].mode = (v->inodes[number].mode & 0170000U) | (mode & 07777U);
    return bfs4_write_inode(v, number);
}

bool bfs4_get_times(const fat_fs_t *fs, const char *path,
                    uint32_t *access, uint32_t *modify) {
    BFS4_GUARD;
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t number;
    if (!v || !bfs4_lookup(v, path, &number)) return false;
    if (access) *access = (uint32_t)v->inodes[number].accessed;
    if (modify) *modify = (uint32_t)v->inodes[number].modified;
    return true;
}

static bool bfs4_set_times_impl(fat_fs_t *fs, const char *path,
                    bool set_access, uint64_t access,
                    bool set_modify, uint64_t modify) {
    bfs4_volume_t *v = bfs4_volume(fs);
    uint32_t number;
    if (!v || !bfs4_lookup(v, path, &number)) return false;
    if (!bfs4_defrag_before_mutation(v,number)) return false;
    if (set_access) v->inodes[number].accessed = access;
    if (set_modify) v->inodes[number].modified = modify;
    return bfs4_write_inode(v, number);
}

bool bfs4_space(const fat_fs_t *fs, uint64_t *total, uint64_t *free) {
    BFS4_GUARD;
    bfs4_volume_t *v = bfs4_volume(fs);
    if (!v || !total || !free) return false;
    *total = (uint64_t)(v->super.total_blocks - v->super.data_start) * BFS4_BLOCK_SIZE;
    *free = (uint64_t)v->super.free_blocks * BFS4_BLOCK_SIZE;
    return true;
}

static bool bfs4_sync_impl(const fat_fs_t *fs) {
    bfs4_volume_t *v = bfs4_volume(fs);
    /* Data, bitmap sectors and inode sectors are written through. The
     * superblock is the final checkpoint for free-space accounting. */
    if (!v) return false;
    if (v->write_failed) { v->super.state = BFS4_DIRTY; return false; }
    if (bfs4_defrag_allows_clean(v)) {
        fat_check_report_t report;
        if (!bfs4_check(fs, &report) || report.errors) {
            v->super.state = BFS4_DIRTY;
            (void)bfs4_write_super(v);
            return false;
        }
    }
    v->super.state = bfs4_defrag_allows_clean(v) ? BFS4_CLEAN : BFS4_DIRTY;
    if (bfs4_write_super(v)) return true;
    v->super.state = BFS4_DIRTY;
    return false;
}

bool bfs4_unmount(const fat_fs_t *fs) {
    BFS4_GUARD;
    bfs4_volume_t *v = bfs4_volume(fs);
    if (!v || v->swap_inode) return false;
    (void)bfs4_defrag_cancel(fs,0);
    if (!bfs4_sync(fs)) return false;
    bfs4_defrag_forget(v);
    bfs4_release(v);
    return true;
}

bool bfs4_check(const fat_fs_t *fs, fat_check_report_t *report) {
    BFS4_GUARD;
    bfs4_volume_t *v = bfs4_volume(fs);
    uint8_t *seen;
    if (!v || !report || (v->write_failed && v->journal_active)) return false;
    kmemset(report, 0, sizeof(*report));
    seen = kzalloc(v->super.block_bitmap_blocks * BFS4_BLOCK_SIZE);
    if (!seen) return false;
    if(!bfs4_bit(v->inode_bitmap,0) || !bfs4_bit(v->inode_bitmap,1) ||
       v->inodes[1].parent!=1U || !bfs4_equal(v->inodes[1].name,"/"))
        ++report->directory_errors;
    report->boot_sector_valid = true;
    report->fat_copies_match = true;
    report->backup_boot_matches = true;
    for (uint32_t block = 0; block < v->super.data_start; ++block)
        bfs4_set_bit(seen, block, true);
    for (uint32_t number = 1U; number < v->super.inode_count; ++number) {
        if (!bfs4_bit(v->inode_bitmap, number)) continue;
        bfs4_inode_t *inode = &v->inodes[number];
        uint64_t mapped = 0;
        uint32_t saved_crc=inode->crc;
        inode->crc=0;
        if(inode->id!=number || bfs4_crc(inode,sizeof(*inode))!=saved_crc ||
           inode->name[127] || !inode->name[0] ||
           ((inode->mode&0170000U)!=BFS4_MODE_FILE &&
            (inode->mode&0170000U)!=BFS4_MODE_DIR &&
            (inode->mode&0170000U)!=BFS4_MODE_LINK))++report->invalid_chains;
        inode->crc=saved_crc;
        /* Bound parent walking and reject cycles and duplicate directory names. */
        uint32_t ancestor=number,depth=0;
        while(ancestor!=1U && depth++<v->super.inode_count) {
            if(!ancestor || ancestor>=v->super.inode_count ||
               !bfs4_bit(v->inode_bitmap,ancestor))break;
            ancestor=v->inodes[ancestor].parent;
        }
        if(ancestor!=1U)++report->directory_errors;
        if(number!=1U && bfs4_find(v,inode->parent,inode->name)!=number)
            ++report->directory_errors;
        if (number != 1U && (inode->parent >= v->super.inode_count ||
            !bfs4_bit(v->inode_bitmap, inode->parent) ||
            (v->inodes[inode->parent].mode & 0170000U) != BFS4_MODE_DIR))
            ++report->directory_errors;
        if ((inode->mode & 0170000U) == BFS4_MODE_DIR) ++report->directories;
        else ++report->files;
        if (inode->extent_count > 1U) {
            ++report->fragmented_files;
            report->total_fragments += inode->extent_count;
        }
        if (inode->indirect) {
            if (inode->indirect < v->super.data_start ||
                inode->indirect >= v->super.total_blocks ||
                !bfs4_bit(v->block_bitmap, inode->indirect) ||
                bfs4_bit(seen, inode->indirect)) ++report->invalid_chains;
            else bfs4_set_bit(seen, inode->indirect, true);
        }
        for (uint32_t i = 0; i < inode->extent_count; ++i) {
            bfs4_extent_t *extent = bfs4_extent(v, inode, i);
            if (!extent) { ++report->invalid_chains; break; }
            uint32_t first = extent->start;
            uint32_t count = extent->count;
            if (!count || first < v->super.data_start ||
                first >= v->super.total_blocks ||
                count > v->super.total_blocks - first) {
                ++report->invalid_chains;
                continue;
            }
            mapped += count;
            for (uint32_t block = first; block < first + count; ++block) {
                if (!bfs4_bit(v->block_bitmap, block)) ++report->invalid_chains;
                if (bfs4_bit(seen, block)) ++report->crosslinked_clusters;
                bfs4_set_bit(seen, block, true);
            }
        }
        if (inode->size / BFS4_BLOCK_SIZE +
            (inode->size % BFS4_BLOCK_SIZE != 0U) != mapped)
            ++report->size_mismatches;
    }
    for (uint32_t block = 0; block < v->super.total_blocks; ++block) {
        if (bfs4_bit(v->block_bitmap, block)) {
            if (!bfs4_bit(seen, block)) ++report->lost_clusters;
            if (block >= v->super.data_start) ++report->allocated_clusters;
            else ++report->reserved_clusters;
        } else if (block >= v->super.data_start) ++report->free_clusters;
        else ++report->invalid_chains;
    }
    if (report->free_clusters != v->super.free_blocks) ++report->errors;
    report->errors += report->lost_clusters + report->crosslinked_clusters +
        report->invalid_chains + report->size_mismatches + report->directory_errors;
    report->referenced_clusters = report->allocated_clusters - report->lost_clusters;
    report->completed = true;
    kfree(seen);
    return true;
}

bool bfs4_swap_pin(const fat_fs_t *fs,const char *path,
                   block_device_t **device,uint32_t *lba,uint32_t *sectors) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);uint32_t number;
    if(!v || !device || !lba || !sectors || v->swap_inode || v->write_failed ||
       v->device->read_only || !v->device->write ||
       v->device->type!=BLOCK_DEVICE_ATA ||
       !bfs4_lookup(v,path,&number))return false;
    bfs4_inode_t *inode=&v->inodes[number];
    if((inode->mode&0170000U)!=BFS4_MODE_FILE || inode->extent_count!=1U ||
       inode->indirect || inode->size<8192U || inode->size>0xffffffffU ||
       (inode->size&4095U) || inode->extents[0].count!=inode->size/4096U ||
       !bfs4_defrag_allows_clean(v))return false;
    fat_check_report_t report;
    if(!bfs4_check(fs,&report) || report.errors)return false;
    uint64_t start=(uint64_t)v->start_lba+(uint64_t)inode->extents[0].start*8U;
    uint64_t length=inode->size/512U;
    if(start>=v->device->sector_count || length>v->device->sector_count-start)return false;
    bfs4_cache_invalidate(v,inode->extents[0].start,inode->extents[0].count);
    v->swap_inode=number;
    *device=v->device;*lba=(uint32_t)start;*sectors=(uint32_t)length;
    return true;
}
void bfs4_swap_unpin(const fat_fs_t *fs) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);if(v)v->swap_inode=0;
}

#define bfs4_defrag_step bfs4_defrag_step_impl
#define bfs4_defrag_cancel bfs4_defrag_cancel_impl
#include "bfs4_defrag.inc"
#undef bfs4_defrag_step
#undef bfs4_defrag_cancel

/* A file relocation holds its undo checkpoint over bounded copy steps. A
 * foreground writer cancels that reservation and checkpoints it before its
 * own operation; unrelated successful writes are never rolled back with it. */
static bool bfs4_journal_prepare(bfs4_volume_t *v) {
    if(!v->journal_active || v->tx_depth)return true;
    if(g_defrag.volume!=v || !g_defrag.destination)return false;
    ++v->tx_depth;
    bool ok=bfs4_defrag_before_mutation(v,g_defrag.inode);
    --v->tx_depth;
    return bfs4_journal_finish(v,ok) && ok;
}
bool bfs4_defrag_step(const fat_fs_t *fs,uint32_t owner) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if(!v || v->write_failed || v!=g_defrag.volume ||
       owner!=g_defrag.info.owner || g_defrag.info.state!=BK_DEFRAG_RUNNING)return false;
    ++v->tx_depth;
    bool ok=bfs4_defrag_step_impl(fs,owner);
    --v->tx_depth;
    if(!ok && v->journal_active) {
        (void)bfs4_journal_finish(v,false);
        if(!v->write_failed) {
            g_defrag.destination=0;g_defrag.uncertain=false;
            bfs4_defrag_map();
        }
    } else if(!v->tx_depth && !g_defrag.destination && !g_defrag.uncertain)
        ok=bfs4_journal_finish(v,ok) && ok;
    return ok;
}
bool bfs4_defrag_cancel(const fat_fs_t *fs,uint32_t owner) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if(!v || v->write_failed)return false;
    ++v->tx_depth;
    bool ok=bfs4_defrag_cancel_impl(fs,owner);
    --v->tx_depth;
    if(!v->tx_depth && ok)ok=bfs4_journal_finish(v,true);
    return ok;
}


/* The BFS4 recursive mutex serializes these operations, including nested rename
 * replacement. Undo protects metadata atomicity; existing data overwritten
 * in place retains normal partial-write semantics on failed I/O. */
bool bfs4_write(fat_fs_t *fs, const char *path, const void *data, uint32_t size) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || v->write_failed || v->device->read_only) return false;
    if(!bfs4_journal_prepare(v))return false;
    ++v->tx_depth;
    bool ok=bfs4_write_impl(fs,path,data,size);
    --v->tx_depth;
    if (!v->tx_depth) ok=bfs4_journal_finish(v,ok) && ok;
    return ok;
}
bool bfs4_append(fat_fs_t *fs, const char *path, const void *data, uint32_t old_size, uint32_t new_size) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || v->write_failed || v->device->read_only) return false;
    if(!bfs4_journal_prepare(v))return false;
    ++v->tx_depth;
    bool ok=bfs4_append_impl(fs,path,data,old_size,new_size);
    --v->tx_depth;
    if (!v->tx_depth) ok=bfs4_journal_finish(v,ok) && ok;
    return ok;
}
bool bfs4_write_at(fat_fs_t *fs, const char *path, uint32_t offset, const void *data, uint32_t length) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || v->write_failed || v->device->read_only) return false;
    if(!bfs4_journal_prepare(v))return false;
    ++v->tx_depth;
    bool ok=bfs4_write_at_impl(fs,path,offset,data,length);
    --v->tx_depth;
    if (!v->tx_depth) ok=bfs4_journal_finish(v,ok) && ok;
    return ok;
}
bool bfs4_resize(fat_fs_t *fs, const char *path, uint32_t size) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || v->write_failed || v->device->read_only) return false;
    if(!bfs4_journal_prepare(v))return false;
    ++v->tx_depth;
    bool ok=bfs4_resize_impl(fs,path,size);
    --v->tx_depth;
    if (!v->tx_depth) ok=bfs4_journal_finish(v,ok) && ok;
    return ok;
}
bool bfs4_mkdir(fat_fs_t *fs, const char *path) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || v->write_failed || v->device->read_only) return false;
    if(!bfs4_journal_prepare(v))return false;
    ++v->tx_depth;
    bool ok=bfs4_mkdir_impl(fs,path);
    --v->tx_depth;
    if (!v->tx_depth) ok=bfs4_journal_finish(v,ok) && ok;
    return ok;
}
bool bfs4_remove(fat_fs_t *fs, const char *path) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || v->write_failed || v->device->read_only) return false;
    if(!bfs4_journal_prepare(v))return false;
    ++v->tx_depth;
    bool ok=bfs4_remove_impl(fs,path);
    --v->tx_depth;
    if (!v->tx_depth) ok=bfs4_journal_finish(v,ok) && ok;
    return ok;
}
bool bfs4_rename(fat_fs_t *fs, const char *old_path, const char *new_path) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || v->write_failed || v->device->read_only) return false;
    if(!bfs4_journal_prepare(v))return false;
    ++v->tx_depth;
    bool ok=bfs4_rename_impl(fs,old_path,new_path);
    --v->tx_depth;
    if (!v->tx_depth) ok=bfs4_journal_finish(v,ok) && ok;
    return ok;
}
bool bfs4_set_mode(fat_fs_t *fs, const char *path, uint16_t mode) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || v->write_failed || v->device->read_only) return false;
    if(!bfs4_journal_prepare(v))return false;
    ++v->tx_depth;
    bool ok=bfs4_set_mode_impl(fs,path,mode);
    --v->tx_depth;
    if (!v->tx_depth) ok=bfs4_journal_finish(v,ok) && ok;
    return ok;
}
bool bfs4_set_times(fat_fs_t *fs, const char *path, bool set_access, uint64_t access, bool set_modify, uint64_t modify) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || v->write_failed || v->device->read_only) return false;
    if(!bfs4_journal_prepare(v))return false;
    ++v->tx_depth;
    bool ok=bfs4_set_times_impl(fs,path,set_access,access,set_modify,modify);
    --v->tx_depth;
    if (!v->tx_depth) ok=bfs4_journal_finish(v,ok) && ok;
    return ok;
}
bool bfs4_sync(const fat_fs_t *fs) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);
    if (!v || v->write_failed || v->device->read_only) return false;
    if(v->journal_active && g_defrag.volume==v && g_defrag.destination) {
        ++v->tx_depth;
        bool ok=bfs4_sync_impl(fs);
        --v->tx_depth;
        return ok; /* reservation remains dirty, with its undo log live */
    }
    if(!bfs4_journal_prepare(v))return false;
    ++v->tx_depth;
    bool ok=bfs4_sync_impl(fs);
    --v->tx_depth;
    if (!v->tx_depth) ok=bfs4_journal_finish(v,ok) && ok;
    return ok;
}

static bool bfs4_journal_reload(bfs4_volume_t *v) {
    /* Recovery rewrites disk outside the normal superblock writer. */
    v->persisted_super_valid=false;
    bfs4_super_t super;uint32_t crc;
    if(!bfs4_read_super(v->device,v->start_lba,&super,&crc) ||
       super.total_blocks!=v->super.total_blocks ||
       super.inode_count!=v->super.inode_count)return false;
    v->super=super;v->super.crc=crc;
    if(!bfs4_load_blocks(v,super.block_bitmap_start,super.block_bitmap_blocks,v->block_bitmap) ||
       !bfs4_load_blocks(v,super.inode_bitmap_start,super.inode_bitmap_blocks,v->inode_bitmap) ||
       !bfs4_load_blocks(v,super.inode_table_start,super.inode_table_blocks,v->inodes))return false;
    for(uint32_t i=0;i<super.inode_count;i++) {
        if(v->indirect_tables[i])kfree(v->indirect_tables[i]);
        v->indirect_tables[i]=NULL;
    }
    kmemset(v->hash_heads,0,BFS4_HASH_SIZE*sizeof(uint32_t));
    kmemset(v->hash_next,0,super.inode_count*sizeof(uint32_t));
    kmemset(v->child_heads,0,super.inode_count*sizeof(uint32_t));
    kmemset(v->child_next,0,super.inode_count*sizeof(uint32_t));
    for(uint32_t i=2;i<super.inode_count;i++)
        if(bfs4_bit(v->inode_bitmap,i))bfs4_hash_insert(v,i);
    v->allocation_hint=super.data_start;v->inode_hint=2;
    return true;
}

/* Explicit repair changes only a proven stale free-space counter. Lost data,
 * missing allocations, crosslinks and invalid directories require inspection.
 * Never route BFS4 to the FAT chain-truncation repair routine. */
bool bfs4_repair(const fat_fs_t *fs,fat_repair_report_t *repair,fat_check_report_t *after) {
    BFS4_GUARD;
    bfs4_volume_t *v=bfs4_volume(fs);fat_check_report_t before;
    if(!repair || !after)return false;
    kmemset(repair,0,sizeof(*repair));kmemset(after,0,sizeof(*after));
    repair->attempted=true;
    if(!v || !bfs4_check(fs,&before)){repair->scan_incomplete=true;return false;}
    repair->errors_before=before.errors;*after=before;
    if(v->device->read_only || !v->device->write){repair->read_only=true;return false;}
    bool counter_only=before.errors==1U && before.free_clusters!=v->super.free_blocks;
    if(before.errors && !counter_only){repair->errors_after=before.errors;return false;}
    if(counter_only)v->super.free_blocks=before.free_clusters;
    if(!bfs4_sync(fs)){repair->write_errors=1;return false;}
    if(!bfs4_check(fs,after)){repair->scan_incomplete=true;return false;}
    repair->errors_after=after->errors;repair->completed=!after->errors;
    return repair->completed;
}
