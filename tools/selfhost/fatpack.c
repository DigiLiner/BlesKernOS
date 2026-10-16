/*
 * fatpack.c - small, dependency-free FAT32 image builder for BlesKernOS.
 *
 * It deliberately implements the operations used by the OS build instead of
 * depending on mkfs.fat/mtools: create an image, create directories, copy a
 * file (with VFAT long names), write raw boot payloads and patch a boot sector.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SECTOR 512U
#define FAT_EOC 0x0ffffff8U
#define ATTR_DIR 0x10U
#define ATTR_ARCHIVE 0x20U
#define ATTR_LFN 0x0fU

typedef struct {
    FILE *file;
    uint32_t total_sectors, reserved, fat_size, data_start, clusters;
    uint32_t root_cluster;
    uint8_t fats, sectors_per_cluster;
} fat_t;

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static int seek_abs(FILE *f, uint64_t offset) {
    if (offset > 0x7fffffffULL) return -1;
    return fseek(f, (long)offset, SEEK_SET);
}
static int sector_read(fat_t *fs, uint32_t sector, void *buffer) {
    return seek_abs(fs->file, (uint64_t)sector * SECTOR) == 0 &&
           fread(buffer, 1, SECTOR, fs->file) == SECTOR ? 0 : -1;
}
static int sector_write(fat_t *fs, uint32_t sector, const void *buffer) {
    return seek_abs(fs->file, (uint64_t)sector * SECTOR) == 0 &&
           fwrite(buffer, 1, SECTOR, fs->file) == SECTOR ? 0 : -1;
}
static uint32_t cluster_sector(const fat_t *fs, uint32_t cluster) {
    return fs->data_start + (cluster - 2U) * fs->sectors_per_cluster;
}
static uint32_t fat_get(fat_t *fs, uint32_t cluster) {
    uint8_t sector[SECTOR];
    uint32_t offset = cluster * 4U;
    if (sector_read(fs, fs->reserved + offset / SECTOR, sector)) return FAT_EOC;
    return rd32(sector + offset % SECTOR) & 0x0fffffffU;
}
static int fat_set(fat_t *fs, uint32_t cluster, uint32_t value) {
    uint8_t sector[SECTOR];
    uint32_t offset = cluster * 4U;
    for (uint32_t n = 0; n < fs->fats; n++) {
        uint32_t at = fs->reserved + n * fs->fat_size + offset / SECTOR;
        if (sector_read(fs, at, sector)) return -1;
        wr32(sector + offset % SECTOR, value & 0x0fffffffU);
        if (sector_write(fs, at, sector)) return -1;
    }
    return 0;
}
static int zero_cluster(fat_t *fs, uint32_t cluster) {
    uint8_t zero[SECTOR] = {0};
    uint32_t first = cluster_sector(fs, cluster);
    for (uint32_t n = 0; n < fs->sectors_per_cluster; n++)
        if (sector_write(fs, first + n, zero)) return -1;
    return 0;
}
static uint32_t alloc_cluster(fat_t *fs) {
    for (uint32_t cluster = 2; cluster < fs->clusters + 2U; cluster++) {
        if (fat_get(fs, cluster) == 0U) {
            if (fat_set(fs, cluster, 0x0fffffffU) || zero_cluster(fs, cluster))
                return 0;
            return cluster;
        }
    }
    return 0;
}
static int open_fs(const char *path, fat_t *fs) {
    uint8_t boot[SECTOR];
    memset(fs, 0, sizeof(*fs));
    fs->file = fopen(path, "r+b");
    if (!fs->file || fread(boot, 1, sizeof(boot), fs->file) != sizeof(boot) ||
        boot[510] != 0x55 || boot[511] != 0xaa || rd16(boot + 11) != SECTOR)
        return -1;
    fs->sectors_per_cluster = boot[13];
    fs->reserved = rd16(boot + 14);
    fs->fats = boot[16];
    fs->total_sectors = rd32(boot + 32);
    fs->fat_size = rd32(boot + 36);
    fs->root_cluster = rd32(boot + 44);
    fs->data_start = fs->reserved + fs->fats * fs->fat_size;
    fs->clusters = (fs->total_sectors - fs->data_start) /
                   fs->sectors_per_cluster;
    return fs->sectors_per_cluster && fs->fat_size ? 0 : -1;
}
static void close_fs(fat_t *fs) {
    if (fs->file) fclose(fs->file);
    fs->file = NULL;
}

static int create_image(const char *path, uint32_t size_kb,
                        uint32_t payload_reserved) {
    fat_t fs;
    uint8_t sector[SECTOR];
    uint32_t total = size_kb * 2U, reserved = payload_reserved + 3U;
    uint32_t fat_size = 1U, previous;
    if (size_kb < 34000U || reserved < 32U || reserved >= total / 2U) {
        fprintf(stderr, "fatpack: FAT32 necesita >= 34000 KiB y reserva valida\n");
        return 2;
    }
    do {
        uint32_t clusters;
        previous = fat_size;
        clusters = total - reserved - 2U * fat_size;
        fat_size = ((clusters + 2U) * 4U + SECTOR - 1U) / SECTOR;
    } while (fat_size != previous);
    memset(&fs, 0, sizeof(fs));
    fs.file = fopen(path, "w+b");
    if (!fs.file) { perror(path); return 2; }
    if (seek_abs(fs.file, (uint64_t)total * SECTOR - 1U) || fputc(0, fs.file) == EOF) {
        perror(path); close_fs(&fs); return 2;
    }
    fs.total_sectors = total; fs.reserved = reserved; fs.fats = 2;
    fs.fat_size = fat_size; fs.sectors_per_cluster = 1; fs.root_cluster = 2;
    fs.data_start = reserved + 2U * fat_size;
    fs.clusters = total - fs.data_start;

    memset(sector, 0, sizeof(sector));
    sector[0] = 0xeb; sector[1] = 0x58; sector[2] = 0x90;
    memcpy(sector + 3, "BLESFAT ", 8);
    wr16(sector + 11, SECTOR); sector[13] = 1; wr16(sector + 14, (uint16_t)reserved);
    sector[16] = 2; wr16(sector + 17, 0); wr16(sector + 19, 0);
    sector[21] = 0xf8; wr16(sector + 22, 0); wr16(sector + 24, 63);
    wr16(sector + 26, 16); wr32(sector + 28, 0); wr32(sector + 32, total);
    wr32(sector + 36, fat_size); wr16(sector + 40, 0); wr16(sector + 42, 0);
    wr32(sector + 44, 2); wr16(sector + 48, (uint16_t)payload_reserved);
    wr16(sector + 50, (uint16_t)(payload_reserved + 1U));
    sector[64] = 0x80; sector[66] = 0x29; wr32(sector + 67, 0x090b1e55U);
    memcpy(sector + 71, "BLESKERNOS ", 11); memcpy(sector + 82, "FAT32   ", 8);
    sector[510] = 0x55; sector[511] = 0xaa;
    if (sector_write(&fs, 0, sector) ||
        sector_write(&fs, payload_reserved + 1U, sector)) goto io_error;

    memset(sector, 0, sizeof(sector));
    wr32(sector + 0, 0x41615252U); wr32(sector + 484, 0x61417272U);
    wr32(sector + 488, fs.clusters - 1U); wr32(sector + 492, 3U);
    wr32(sector + 508, 0xaa550000U);
    if (sector_write(&fs, payload_reserved, sector) ||
        sector_write(&fs, payload_reserved + 2U, sector)) goto io_error;
    memset(sector, 0, sizeof(sector));
    for (uint32_t n = 0; n < 2U * fat_size; n++)
        if (sector_write(&fs, reserved + n, sector)) goto io_error;
    if (fat_set(&fs, 0, 0x0ffffff8U) || fat_set(&fs, 1, 0x0fffffffU) ||
        fat_set(&fs, 2, 0x0fffffffU) || zero_cluster(&fs, 2)) goto io_error;
    close_fs(&fs);
    printf("fatpack: FAT32 %u KiB, %u sectores reservados, FAT=%u\n",
           size_kb, reserved, fat_size);
    return 0;
io_error:
    perror(path); close_fs(&fs); return 2;
}

static int short_name(const char *name, uint8_t out[11], int force_alias) {
    const char *dot = strrchr(name, '.');
    size_t base = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    size_t ext = dot && dot != name ? strlen(dot + 1) : 0U;
    int valid = base >= 1U && base <= 8U && ext <= 3U;
    memset(out, ' ', 11);
    for (size_t i = 0; i < base; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || strchr("$%'-_@~`!(){}^#&", c))) valid = 0;
    }
    if (valid && !force_alias) {
        for (size_t i = 0; i < base; i++) out[i] = (uint8_t)toupper((unsigned char)name[i]);
        for (size_t i = 0; i < ext; i++) out[8 + i] = (uint8_t)toupper((unsigned char)dot[1 + i]);
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < base && n < 6U; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c)) out[n++] = (uint8_t)toupper(c);
    }
    while (n < 2U) out[n++] = '_';
    out[6] = '~'; out[7] = '1';
    for (size_t i = 0; i < ext && i < 3U; i++) {
        unsigned char c = (unsigned char)dot[1 + i];
        out[8 + i] = isalnum(c) ? (uint8_t)toupper(c) : '_';
    }
    return 1;
}
static int entry_matches(const uint8_t *entry, const char *component) {
    uint8_t wanted[11];
    short_name(component, wanted, 0);
    return entry[0] != 0x00 && entry[0] != 0xe5 && entry[11] != ATTR_LFN &&
           memcmp(entry, wanted, 11) == 0;
}
static uint32_t find_child(fat_t *fs, uint32_t directory, const char *name,
                           uint8_t *attributes) {
    uint8_t sector[SECTOR];
    uint32_t cluster = directory;
    while (cluster >= 2U && cluster < FAT_EOC) {
        uint32_t first = cluster_sector(fs, cluster);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (sector_read(fs, first + s, sector)) return 0;
            for (uint32_t off = 0; off < SECTOR; off += 32U) {
                if (sector[off] == 0x00) return 0;
                if (entry_matches(sector + off, name)) {
                    if (attributes) *attributes = sector[off + 11];
                    return ((uint32_t)rd16(sector + off + 20) << 16) |
                           rd16(sector + off + 26);
                }
            }
        }
        cluster = fat_get(fs, cluster);
    }
    return 0;
}

static int find_child_entry(fat_t *fs, uint32_t directory, const char *name,
                            uint32_t *entry_sector, uint32_t *entry_offset,
                            uint32_t *first_cluster, uint8_t *attributes) {
    uint8_t sector[SECTOR];
    uint32_t cluster = directory;
    while (cluster >= 2U && cluster < FAT_EOC) {
        uint32_t first = cluster_sector(fs, cluster);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (sector_read(fs, first + s, sector)) return -1;
            for (uint32_t off = 0; off < SECTOR; off += 32U) {
                if (sector[off] == 0x00) return 0;
                if (!entry_matches(sector + off, name)) continue;
                if (entry_sector) *entry_sector = first + s;
                if (entry_offset) *entry_offset = off;
                if (attributes) *attributes = sector[off + 11];
                if (first_cluster)
                    *first_cluster = ((uint32_t)rd16(sector + off + 20) << 16) |
                                     rd16(sector + off + 26);
                return 1;
            }
        }
        cluster = fat_get(fs, cluster);
    }
    return 0;
}

static int free_chain(fat_t *fs, uint32_t cluster) {
    while (cluster >= 2U && cluster < FAT_EOC) {
        uint32_t next = fat_get(fs, cluster);
        if (fat_set(fs, cluster, 0U)) return -1;
        cluster = next;
    }
    return 0;
}
static uint32_t resolve_directory(fat_t *fs, const char *path,
                                  char *leaf, size_t leaf_size) {
    char copy[512], *scan, *part;
    uint32_t cluster = fs->root_cluster;
    if (!path || strlen(path) >= sizeof(copy)) return 0;
    strcpy(copy, path); scan = copy;
    while (*scan == '/' || *scan == '\\') scan++;
    part = scan;
    for (;;) {
        char *end = part;
        uint8_t attr = 0;
        while (*end && *end != '/' && *end != '\\') end++;
        if (!*end) {
            if (!*part || strlen(part) >= leaf_size) return 0;
            strcpy(leaf, part); return cluster;
        }
        *end = 0;
        if (*part) {
            cluster = find_child(fs, cluster, part, &attr);
            if (!cluster || !(attr & ATTR_DIR)) return 0;
        }
        part = end + 1;
        while (*part == '/' || *part == '\\') part++;
    }
}
static uint8_t lfn_checksum(const uint8_t shortname[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1U) << 7) + (sum >> 1) + shortname[i]);
    return sum;
}
static void lfn_entry(uint8_t out[32], const char *name, uint32_t index,
                      uint32_t count, uint8_t checksum) {
    static const uint8_t positions[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    size_t length = strlen(name); uint32_t start = (index - 1U) * 13U;
    memset(out, 0xff, 32); out[0] = (uint8_t)index;
    if (index == count) out[0] |= 0x40U;
    out[11] = ATTR_LFN; out[12] = 0; out[13] = checksum; wr16(out + 26, 0);
    for (uint32_t n = 0; n < 13U; n++) {
        uint32_t at = start + n; uint16_t value = 0xffffU;
        if (at < length) value = (uint8_t)name[at];
        else if (at == length) value = 0;
        wr16(out + positions[n], value);
    }
}
static int write_dir_entries(fat_t *fs, uint32_t directory,
                             const char *name, const uint8_t shortname[11],
                             uint8_t attr, uint32_t first_cluster,
                             uint32_t size) {
    uint32_t long_count = short_name(name, (uint8_t[11]){0}, 0) ?
                          ((uint32_t)strlen(name) + 12U) / 13U : 0U;
    uint32_t needed = long_count + 1U, cluster = directory, run = 0;
    uint32_t run_cluster = 0, run_sector = 0, run_offset = 0;
    uint8_t sector[SECTOR], entry[32], checksum = lfn_checksum(shortname);
    for (;;) {
        uint32_t first = cluster_sector(fs, cluster);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (sector_read(fs, first + s, sector)) return -1;
            for (uint32_t off = 0; off < SECTOR; off += 32U) {
                if (sector[off] == 0 || sector[off] == 0xe5) {
                    if (!run) { run_cluster = cluster; run_sector = s; run_offset = off; }
                    if (++run >= needed) goto found;
                } else run = 0;
            }
        }
        {
            uint32_t next = fat_get(fs, cluster);
            if (next >= FAT_EOC) {
                next = alloc_cluster(fs); if (!next || fat_set(fs, cluster, next)) return -1;
            }
            cluster = next;
        }
    }
found:
    cluster = run_cluster;
    uint32_t s = run_sector, off = run_offset;
    for (uint32_t sequence = long_count; sequence > 0; sequence--) {
        if (sector_read(fs, cluster_sector(fs, cluster) + s, sector)) return -1;
        lfn_entry(entry, name, sequence, long_count, checksum);
        memcpy(sector + off, entry, 32);
        if (sector_write(fs, cluster_sector(fs, cluster) + s, sector)) return -1;
        off += 32U;
        if (off == SECTOR) { off = 0; if (++s == fs->sectors_per_cluster) { s = 0; cluster = fat_get(fs, cluster); } }
    }
    if (sector_read(fs, cluster_sector(fs, cluster) + s, sector)) return -1;
    memset(entry, 0, sizeof(entry)); memcpy(entry, shortname, 11); entry[11] = attr;
    wr16(entry + 20, (uint16_t)(first_cluster >> 16)); wr16(entry + 26, (uint16_t)first_cluster);
    wr32(entry + 28, size); memcpy(sector + off, entry, 32);
    return sector_write(fs, cluster_sector(fs, cluster) + s, sector);
}
static uint32_t make_directory_child(fat_t *fs, uint32_t parent,
                                     const char *leaf) {
    uint8_t name[11], attr;
    uint32_t cluster = find_child(fs, parent, leaf, &attr);
    if (cluster) return (attr & ATTR_DIR) ? cluster : 0U;
    cluster = alloc_cluster(fs);
    if (!cluster) return 0U;
    short_name(leaf, name, 0);
    if (write_dir_entries(fs, parent, leaf, name, ATTR_DIR, cluster, 0))
        return 0U;
    {
        uint8_t sector[SECTOR] = {0}, dot[11] = {'.',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};
        uint8_t dotdot[11] = {'.','.',' ',' ',' ',' ',' ',' ',' ',' ',' '};
        memcpy(sector, dot, 11); sector[11] = ATTR_DIR; wr16(sector + 20, (uint16_t)(cluster >> 16)); wr16(sector + 26, (uint16_t)cluster);
        memcpy(sector + 32, dotdot, 11); sector[43] = ATTR_DIR; wr16(sector + 52, (uint16_t)(parent >> 16)); wr16(sector + 58, (uint16_t)parent);
        if (sector_write(fs, cluster_sector(fs, cluster), sector)) return 0U;
    }
    return cluster;
}

static int make_directory(const char *image, const char *path) {
    fat_t fs; char copy[512], *part; uint32_t parent;
    if (open_fs(image, &fs)) { fprintf(stderr, "fatpack: imagen FAT32 invalida\n"); return 2; }
    if (!path || strlen(path) >= sizeof(copy)) { close_fs(&fs); return 2; }
    strcpy(copy, path); part = copy;
    while (*part == '/' || *part == '\\') part++;
    parent = fs.root_cluster;
    while (*part) {
        char *end = part;
        char separator;
        while (*end && *end != '/' && *end != '\\') end++;
        separator = *end;
        if (separator) *end = 0;
        parent = make_directory_child(&fs, parent, part);
        if (!parent) { close_fs(&fs); return 2; }
        if (!separator) break;
        part = end + 1;
        while (*part == '/' || *part == '\\') part++;
    }
    close_fs(&fs); return 0;
}
static int add_file(const char *image, const char *source, const char *target) {
    fat_t fs; FILE *input; char leaf[260]; uint8_t name[11], buffer[SECTOR];
    uint32_t parent, first = 0, previous = 0, size = 0;
    uint32_t old_cluster = 0, entry_sector = 0, entry_offset = 0;
    int existing;
    input = fopen(source, "rb");
    if (!input) { perror(source); return 2; }
    if (open_fs(image, &fs)) { fclose(input); return 2; }
    parent = resolve_directory(&fs, target, leaf, sizeof(leaf));
    if (!parent) { fprintf(stderr, "fatpack: directorio padre inexistente: %s\n", target); goto fail; }
    existing = find_child_entry(&fs, parent, leaf, &entry_sector, &entry_offset,
                                &old_cluster, NULL);
    if (existing < 0 || (existing && free_chain(&fs, old_cluster))) goto fail;
    for (;;) {
        size_t got = fread(buffer, 1, sizeof(buffer), input);
        if (!got) break;
        uint32_t cluster = alloc_cluster(&fs);
        if (!cluster || (previous && fat_set(&fs, previous, cluster))) goto fail;
        if (!first) first = cluster;
        previous = cluster;
        if (got < sizeof(buffer)) memset(buffer + got, 0, sizeof(buffer) - got);
        if (sector_write(&fs, cluster_sector(&fs, cluster), buffer)) goto fail;
        size += (uint32_t)got;
    }
    short_name(leaf, name, 0);
    if (existing) {
        if (sector_read(&fs, entry_sector, buffer)) goto fail;
        buffer[entry_offset + 11] = ATTR_ARCHIVE;
        wr16(buffer + entry_offset + 20, (uint16_t)(first >> 16));
        wr16(buffer + entry_offset + 26, (uint16_t)first);
        wr32(buffer + entry_offset + 28, size);
        if (sector_write(&fs, entry_sector, buffer)) goto fail;
    } else if (write_dir_entries(&fs, parent, leaf, name, ATTR_ARCHIVE,
                                 first, size)) goto fail;
    fclose(input); close_fs(&fs); return 0;
fail:
    fclose(input); close_fs(&fs); return 2;
}

static int import_tree(const char *image, const char *source,
                       const char *target) {
    DIR *directory = opendir(source);
    struct dirent *entry;
    if (!directory) { perror(source); return 2; }
    if (make_directory(image, target)) { closedir(directory); return 2; }
    while ((entry = readdir(directory)) != NULL) {
        char source_path[1024], target_path[1024];
        struct stat info;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (snprintf(source_path, sizeof(source_path), "%s/%s", source,
                     entry->d_name) >= (int)sizeof(source_path) ||
            snprintf(target_path, sizeof(target_path), "%s/%s", target,
                     entry->d_name) >= (int)sizeof(target_path)) {
            closedir(directory); return 2;
        }
        if (stat(source_path, &info)) { perror(source_path); closedir(directory); return 2; }
        if (S_ISDIR(info.st_mode)) {
            if (import_tree(image, source_path, target_path)) {
                closedir(directory); return 2;
            }
        } else if (S_ISREG(info.st_mode) &&
                   add_file(image, source_path, target_path)) {
            closedir(directory); return 2;
        }
    }
    closedir(directory); return 0;
}

static int ends_with_ci(const char *name, const char *suffix) {
    size_t name_length = strlen(name), suffix_length = strlen(suffix);
    if (suffix_length > name_length) return 0;
    name += name_length - suffix_length;
    while (*suffix) {
        if (tolower((unsigned char)*name++) !=
            tolower((unsigned char)*suffix++)) return 0;
    }
    return 1;
}

static int import_flat(const char *image, const char *source,
                       const char *target, const char *old_suffix,
                       const char *new_suffix) {
    DIR *directory = opendir(source);
    struct dirent *entry;
    if (!directory) { perror(source); return 2; }
    if (make_directory(image, target)) { closedir(directory); return 2; }
    while ((entry = readdir(directory)) != NULL) {
        char source_path[1024], target_path[1024], output_name[512];
        struct stat info;
        size_t stem;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            !ends_with_ci(entry->d_name, old_suffix)) continue;
        if (snprintf(source_path, sizeof(source_path), "%s/%s", source,
                     entry->d_name) >= (int)sizeof(source_path) ||
            stat(source_path, &info) || !S_ISREG(info.st_mode)) continue;
        stem = strlen(entry->d_name) - strlen(old_suffix);
        if (stem + strlen(new_suffix) + 1U > sizeof(output_name)) {
            closedir(directory); return 2;
        }
        memcpy(output_name, entry->d_name, stem);
        strcpy(output_name + stem, new_suffix);
        if (snprintf(target_path, sizeof(target_path), "%s/%s", target,
                     output_name) >= (int)sizeof(target_path) ||
            add_file(image, source_path, target_path)) {
            closedir(directory); return 2;
        }
    }
    closedir(directory); return 0;
}
static int raw_write(const char *image, uint32_t lba, const char *source,
                     uint32_t maximum) {
    fat_t fs; FILE *input; uint8_t buffer[SECTOR]; uint32_t count = 0;
    memset(&fs, 0, sizeof(fs)); fs.file = fopen(image, "r+b"); input = fopen(source, "rb");
    if (!fs.file || !input) { perror(!fs.file ? image : source); if (input) fclose(input); close_fs(&fs); return 2; }
    while (!feof(input)) {
        size_t got = fread(buffer, 1, sizeof(buffer), input); if (!got) break;
        if (++count > maximum) { fprintf(stderr, "fatpack: payload excede %u sectores\n", maximum); fclose(input); close_fs(&fs); return 2; }
        if (got < sizeof(buffer)) memset(buffer + got, 0, sizeof(buffer) - got);
        if (sector_write(&fs, lba + count - 1U, buffer)) { fclose(input); close_fs(&fs); return 2; }
    }
    fclose(input); close_fs(&fs); return 0;
}
static int patch_boot(const char *image, const char *source) {
    fat_t fs; FILE *input; uint8_t patch[SECTOR], boot[SECTOR]; uint16_t backup;
    input = fopen(source, "rb");
    if (!input || fread(patch, 1, sizeof(patch), input) != sizeof(patch)) { if (input) fclose(input); return 2; }
    fclose(input);
    if (open_fs(image, &fs)) return 2;
    if (sector_read(&fs, 0, boot)) { close_fs(&fs); return 2; }
    backup = rd16(boot + 50); memcpy(boot, patch, 3); memcpy(boot + 90, patch + 90, 422);
    if (sector_write(&fs, 0, boot) || sector_write(&fs, backup, boot)) { close_fs(&fs); return 2; }
    close_fs(&fs); return 0;
}
static uint32_t number(const char *text) {
    char *end; unsigned long value = strtoul(text, &end, 0);
    if (!text[0] || *end || value > 0xffffffffUL) { fprintf(stderr, "fatpack: numero invalido: %s\n", text); exit(2); }
    return (uint32_t)value;
}
static void usage(void) {
    puts("fatpack - empaquetador FAT32 de BlesKernOS\n"
         "  fatpack create IMAGEN SIZE_KB [RESERVA_PAYLOAD]\n"
         "  fatpack mkdir IMAGEN /RUTA\n"
         "  fatpack add IMAGEN ARCHIVO /DESTINO\n"
         "  fatpack import IMAGEN DIRECTORIO /DESTINO\n"
         "  fatpack importflat IMAGEN DIRECTORIO /DESTINO EXT_ORIG EXT_NUEVA\n"
         "  fatpack raw IMAGEN LBA ARCHIVO MAX_SECTORES\n"
         "  fatpack boot IMAGEN BOOTSECTOR.BIN");
}
int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) { usage(); return argc < 2 ? 2 : 0; }
    if (!strcmp(argv[1], "create") && (argc == 5 || argc == 4))
        return create_image(argv[2], number(argv[3]), argc == 5 ? number(argv[4]) : 2057U);
    if (!strcmp(argv[1], "mkdir") && argc == 4) return make_directory(argv[2], argv[3]);
    if (!strcmp(argv[1], "add") && argc == 5) return add_file(argv[2], argv[3], argv[4]);
    if (!strcmp(argv[1], "import") && argc == 5) return import_tree(argv[2], argv[3], argv[4]);
    if (!strcmp(argv[1], "importflat") && argc == 7)
        return import_flat(argv[2], argv[3], argv[4], argv[5], argv[6]);
    if (!strcmp(argv[1], "raw") && argc == 6) return raw_write(argv[2], number(argv[3]), argv[4], number(argv[5]));
    if (!strcmp(argv[1], "boot") && argc == 4) return patch_boot(argv[2], argv[3]);
    usage(); return 2;
}
