#ifndef BLESKERNOS_HYPERZIP_LIBRARY_H
#define BLESKERNOS_HYPERZIP_LIBRARY_H

#include <bleskernos_api.h>
#include <bleskernos_inflate_lbs.h>

#define HZIP_VERSION_MAJOR 1U
#define HZIP_VERSION_MINOR 0U
#define HZIP_MAX_ENTRIES 8192U
#define HZIP_NAME_MAX BK_PATH_MAX

#define HZIP_FLAG_ENCRYPTED 0x0001U
#define HZIP_FLAG_DATA_DESCRIPTOR 0x0008U
#define HZIP_FLAG_UTF8 0x0800U

typedef enum {
    HZIP_OK = 0,
    HZIP_ERR_ARGUMENT = -1,
    HZIP_ERR_READ = -2,
    HZIP_ERR_NOT_ZIP = -3,
    HZIP_ERR_MULTIDISK = -4,
    HZIP_ERR_ZIP64 = -5,
    HZIP_ERR_TOO_MANY_ENTRIES = -6,
    HZIP_ERR_CORRUPT = -7,
    HZIP_ERR_MEMORY = -8,
    HZIP_ERR_ENCRYPTED = -9,
    HZIP_ERR_METHOD = -10,
    HZIP_ERR_INFLATE = -11,
    HZIP_ERR_CRC = -12,
    HZIP_ERR_PATH = -13,
    HZIP_ERR_WRITE = -14
} hzip_result_t;

typedef struct {
    char name[HZIP_NAME_MAX];
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t crc32;
    uint32_t local_header_offset;
    uint16_t method;
    uint16_t flags;
    uint16_t dos_time;
    uint16_t dos_date;
    uint32_t external_attributes;
    bool directory;
} hzip_entry_t;

typedef struct {
    uint32_t lbs_registered; /* Internal resource registration; initialize before use. */
    void *file_data;
    uint32_t file_size;
    hzip_entry_t *entries;
    uint32_t entry_count;
    uint64_t total_uncompressed;
    uint64_t total_compressed;
    char source_path[HZIP_NAME_MAX];
} hzip_archive_t;

typedef void (*hzip_progress_callback_t)(uint32_t current, uint32_t total,
                                         const hzip_entry_t *entry,
                                         void *context);

void hzip_archive_init(hzip_archive_t *archive);
void hzip_archive_close(hzip_archive_t *archive);
int hzip_archive_open(hzip_archive_t *archive, const char *path);

const hzip_entry_t *hzip_archive_entry(const hzip_archive_t *archive,
                                       uint32_t index);

int hzip_read_entry(const hzip_archive_t *archive, uint32_t index,
                    void **buffer, uint32_t *size);
int hzip_test_entry(const hzip_archive_t *archive, uint32_t index);
int hzip_test_archive(const hzip_archive_t *archive,
                      hzip_progress_callback_t progress, void *context,
                      uint32_t *tested, uint32_t *failed);

int hzip_extract_entry(const hzip_archive_t *archive, uint32_t index,
                       const char *destination_root);
int hzip_extract_archive(const hzip_archive_t *archive,
                         const char *destination_root,
                         const uint8_t *selection,
                         hzip_progress_callback_t progress, void *context,
                         uint32_t *extracted, uint32_t *failed);

const char *hzip_result_string(int result);
uint32_t hzip_entry_ratio_percent(const hzip_entry_t *entry);
void hzip_entry_datetime(const hzip_entry_t *entry,
                         uint16_t *year, uint8_t *month, uint8_t *day,
                         uint8_t *hour, uint8_t *minute, uint8_t *second);

#endif
