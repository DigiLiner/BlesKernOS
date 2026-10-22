#ifndef BLESKERNOS_LBS_STATS_H
#define BLESKERNOS_LBS_STATS_H
/* Virtual image accounting for this process, not unique physical RAM. */
typedef struct {
    unsigned int size, modules, image_bytes, references, pinned;
} bk_dl_statistics_t;
int bk_dl_statistics(bk_dl_statistics_t *out);
#endif

/* Virtual mappings in this process, NOT physical RAM saved. Clean read-only
 * file pages can share frames with another process mapping the same inode. */
#ifndef BLESKERNOS_LBS_MAPPING_STATISTICS_H
#define BLESKERNOS_LBS_MAPPING_STATISTICS_H
typedef struct {
    unsigned int size, modules, virtual_bytes, file_readonly_bytes, private_bytes;
} bk_dl_mapping_statistics_t;
int bk_dl_mapping_statistics(bk_dl_mapping_statistics_t *out);
#endif
