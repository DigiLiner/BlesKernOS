#ifdef BK_LBS_HZIP
#include "resources.h"
#endif
#include "hyperzip.h"

#define ZIP_LOCAL_SIGNATURE   0x04034B50U
#define ZIP_CENTRAL_SIGNATURE 0x02014B50U
#define ZIP_EOCD_SIGNATURE    0x06054B50U
#define ZIP_EOCD_MIN_SIZE     22U
#define ZIP_EOCD_MAX_SEARCH   (65535U + ZIP_EOCD_MIN_SIZE)

static uint16_t hz_read16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t hz_read32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t hz_strlen_local(const char *text) {
    uint32_t n = 0;
    if (!text) return 0;
    while (text[n]) ++n;
    return n;
}

static void hz_zero(void *pointer, uint32_t size) {
    uint8_t *p = (uint8_t *)pointer;
    uint32_t i;
    for (i = 0; i < size; ++i) p[i] = 0;
}

static void hz_copy(void *destination, const void *source, uint32_t size) {
    uint8_t *d = (uint8_t *)destination;
    const uint8_t *s = (const uint8_t *)source;
    uint32_t i;
    for (i = 0; i < size; ++i) d[i] = s[i];
}

static int hz_copy_string(char *destination, uint32_t capacity,
                          const uint8_t *source, uint32_t length) {
    uint32_t i;
    if (!destination || !capacity) return 0;
    if (length >= capacity) return 0;
    for (i = 0; i < length; ++i) {
        if (source[i] == 0U) return 0;
        destination[i] = (char)source[i];
    }
    destination[length] = '\0';
    return 1;
}

static int hz_join_path(char *output, uint32_t capacity,
                        const char *root, const char *relative) {
    uint32_t pos = 0;
    uint32_t i;
    uint32_t root_len;
    if (!output || !capacity || !root || !relative) return 0;
    root_len = hz_strlen_local(root);
    if (root_len + 2U >= capacity) return 0;
    for (i = 0; i < root_len; ++i) output[pos++] = root[i];
    if (pos && output[pos - 1] != '/' && output[pos - 1] != '\\') output[pos++] = '/';
    i = 0;
    while (relative[i]) {
        if (pos + 1U >= capacity) return 0;
        output[pos++] = relative[i++];
    }
    output[pos] = '\0';
    return 1;
}

static int hz_sanitize_relative(const char *input, char *output,
                                uint32_t capacity, bool keep_directory_slash) {
    uint32_t in = 0;
    uint32_t out = 0;
    bool segment_start = true;

    if (!input || !output || capacity < 2U) return 0;
    if (input[0] == '/' || input[0] == '\\') return 0;
    if (((input[0] >= 'A' && input[0] <= 'Z') ||
         (input[0] >= 'a' && input[0] <= 'z')) && input[1] == ':') return 0;

    while (input[in]) {
        char c = input[in++];
        if (c == '\\') c = '/';
        if (c == '/') {
            if (segment_start) continue;
            if (out + 1U >= capacity) return 0;
            output[out++] = '/';
            segment_start = true;
            continue;
        }
        if (segment_start) {
            if (c == '.') {
                char next = input[in];
                if (next == '.' && (input[in + 1] == '/' ||
                                    input[in + 1] == '\\' ||
                                    input[in + 1] == '\0'))
                    return 0;
                if (next == '/' || next == '\\' || next == '\0') {
                    while (input[in] == '/' || input[in] == '\\') ++in;
                    continue;
                }
            }
            segment_start = false;
        }
        if (c == ':') return 0;
        if (out + 1U >= capacity) return 0;
        output[out++] = c;
    }

    while (out && output[out - 1] == '/' && !keep_directory_slash) --out;
    if (!out) return 0;
    output[out] = '\0';
    return 1;
}

static void hz_mkdir_parents(const char *path, bool include_last) {
    char temp[BK_PATH_MAX];
    uint32_t len;
    uint32_t i;
    if (!path) return;
    len = hz_strlen_local(path);
    if (!len || len >= sizeof(temp)) return;
    for (i = 0; i <= len; ++i) temp[i] = path[i];
    for (i = 1; i < len; ++i) {
        if (temp[i] == '/' || temp[i] == '\\') {
            char old = temp[i];
            temp[i] = '\0';
            if (temp[0]) (void)bk_file_mkdir(temp);
            temp[i] = old;
        }
    }
    if (include_last) (void)bk_file_mkdir(temp);
}

static int hz_find_eocd(const uint8_t *data, uint32_t size, uint32_t *position) {
    uint32_t start;
    uint32_t pos;
    if (!data || size < ZIP_EOCD_MIN_SIZE || !position) return 0;
    start = size > ZIP_EOCD_MAX_SEARCH ? size - ZIP_EOCD_MAX_SEARCH : 0U;
    pos = size - ZIP_EOCD_MIN_SIZE;
    for (;;) {
        if (hz_read32(data + pos) == ZIP_EOCD_SIGNATURE) {
            uint16_t comment = hz_read16(data + pos + 20U);
            if (pos + ZIP_EOCD_MIN_SIZE + comment <= size) {
                *position = pos;
                return 1;
            }
        }
        if (pos == start) break;
        --pos;
    }
    return 0;
}

static int hz_entry_payload(const hzip_archive_t *archive,
                            const hzip_entry_t *entry,
                            const uint8_t **payload) {
    const uint8_t *data;
    uint32_t offset;
    uint16_t name_len;
    uint16_t extra_len;
    if (!archive || !entry || !payload || !archive->file_data) return HZIP_ERR_ARGUMENT;
    data = (const uint8_t *)archive->file_data;
    offset = entry->local_header_offset;
    if (offset > archive->file_size || archive->file_size - offset < 30U)
        return HZIP_ERR_CORRUPT;
    if (hz_read32(data + offset) != ZIP_LOCAL_SIGNATURE) return HZIP_ERR_CORRUPT;
    name_len = hz_read16(data + offset + 26U);
    extra_len = hz_read16(data + offset + 28U);
    offset += 30U;
    if (offset > archive->file_size ||
        (uint32_t)name_len + (uint32_t)extra_len > archive->file_size - offset)
        return HZIP_ERR_CORRUPT;
    offset += (uint32_t)name_len + (uint32_t)extra_len;
    if (entry->compressed_size > archive->file_size - offset) return HZIP_ERR_CORRUPT;
    *payload = data + offset;
    return HZIP_OK;
}

void hzip_archive_init(hzip_archive_t *archive) {
    if (archive) hz_zero(archive, (uint32_t)sizeof(*archive));
}

void hzip_archive_close(hzip_archive_t *archive) {
    if (!archive) return;
#ifdef BK_LBS_HZIP
    if (archive->lbs_registered) bk_lbs_resource_release();
#endif
    if (archive->entries) bk_sys_free(archive->entries);
    if (archive->file_data) bk_sys_free(archive->file_data);
    hzip_archive_init(archive);
}

int hzip_archive_open(hzip_archive_t *archive, const char *path) {
    void *buffer = NULL;
    uint32_t size = 0;
    const uint8_t *data;
    uint32_t eocd;
    uint16_t disk_number;
    uint16_t central_disk;
    uint16_t entries_disk;
    uint16_t entries_total;
    uint32_t central_size;
    uint32_t central_offset;
    uint32_t central_end;
    uint32_t position;
    uint32_t i;
    int result = HZIP_OK;

    if (!archive || !path || !path[0]) return HZIP_ERR_ARGUMENT;
    hzip_archive_close(archive);
    if (!bk_file_read_all(path, &buffer, &size) || !buffer) return HZIP_ERR_READ;
    data = (const uint8_t *)buffer;
    if (!hz_find_eocd(data, size, &eocd)) {
        bk_sys_free(buffer);
        return HZIP_ERR_NOT_ZIP;
    }

    disk_number = hz_read16(data + eocd + 4U);
    central_disk = hz_read16(data + eocd + 6U);
    entries_disk = hz_read16(data + eocd + 8U);
    entries_total = hz_read16(data + eocd + 10U);
    central_size = hz_read32(data + eocd + 12U);
    central_offset = hz_read32(data + eocd + 16U);

    if (disk_number || central_disk || entries_disk != entries_total) {
        result = HZIP_ERR_MULTIDISK;
        goto fail;
    }
    if (entries_total == 0xFFFFU || central_size == 0xFFFFFFFFU ||
        central_offset == 0xFFFFFFFFU) {
        result = HZIP_ERR_ZIP64;
        goto fail;
    }
    if (entries_total > HZIP_MAX_ENTRIES) {
        result = HZIP_ERR_TOO_MANY_ENTRIES;
        goto fail;
    }
    if (central_offset > size || central_size > size - central_offset) {
        result = HZIP_ERR_CORRUPT;
        goto fail;
    }
    central_end = central_offset + central_size;

    archive->entries = entries_total
        ? (hzip_entry_t *)bk_sys_alloc((uint32_t)entries_total * (uint32_t)sizeof(hzip_entry_t))
        : NULL;
    if (archive->entries)
        hz_zero(archive->entries, (uint32_t)entries_total * (uint32_t)sizeof(hzip_entry_t));
    if (entries_total && !archive->entries) {
        result = HZIP_ERR_MEMORY;
        goto fail;
    }
    archive->file_data = buffer;
    archive->file_size = size;
    archive->entry_count = entries_total;
    buffer = NULL;
    bk_string_copy_n(archive->source_path, path, sizeof(archive->source_path));
    archive->source_path[sizeof(archive->source_path) - 1U] = '\0';

    position = central_offset;
    for (i = 0; i < entries_total; ++i) {
        hzip_entry_t *entry = &archive->entries[i];
        uint16_t name_len;
        uint16_t extra_len;
        uint16_t comment_len;
        uint32_t record_size;
        if (position > size || position > central_end ||
            size - position < 46U || central_end - position < 46U ||
            hz_read32((const uint8_t *)archive->file_data + position) != ZIP_CENTRAL_SIGNATURE) {
            result = HZIP_ERR_CORRUPT;
            goto fail_archive;
        }
        data = (const uint8_t *)archive->file_data + position;
        entry->flags = hz_read16(data + 8U);
        entry->method = hz_read16(data + 10U);
        entry->dos_time = hz_read16(data + 12U);
        entry->dos_date = hz_read16(data + 14U);
        entry->crc32 = hz_read32(data + 16U);
        entry->compressed_size = hz_read32(data + 20U);
        entry->uncompressed_size = hz_read32(data + 24U);
        name_len = hz_read16(data + 28U);
        extra_len = hz_read16(data + 30U);
        comment_len = hz_read16(data + 32U);
        entry->external_attributes = hz_read32(data + 38U);
        entry->local_header_offset = hz_read32(data + 42U);

        if (entry->compressed_size == 0xFFFFFFFFU ||
            entry->uncompressed_size == 0xFFFFFFFFU ||
            entry->local_header_offset == 0xFFFFFFFFU) {
            result = HZIP_ERR_ZIP64;
            goto fail_archive;
        }
        record_size = 46U + (uint32_t)name_len + (uint32_t)extra_len + (uint32_t)comment_len;
        if (record_size > size - position || record_size > central_end - position ||
            name_len == 0U ||
            !hz_copy_string(entry->name, sizeof(entry->name), data + 46U, name_len)) {
            result = HZIP_ERR_CORRUPT;
            goto fail_archive;
        }
        entry->directory = entry->name[name_len - 1U] == '/' || entry->name[name_len - 1U] == '\\';
        archive->total_uncompressed += entry->uncompressed_size;
        archive->total_compressed += entry->compressed_size;
        position += record_size;
    }
#ifdef BK_LBS_HZIP
    archive->lbs_registered = 1;
    bk_lbs_resource_acquire();
#endif
    return HZIP_OK;

fail_archive:
    hzip_archive_close(archive);
    return result;
fail:
    if (buffer) bk_sys_free(buffer);
    return result;
}

const hzip_entry_t *hzip_archive_entry(const hzip_archive_t *archive,
                                       uint32_t index) {
    if (!archive || !archive->entries || index >= archive->entry_count) return NULL;
    return &archive->entries[index];
}

int hzip_read_entry(const hzip_archive_t *archive, uint32_t index,
                    void **buffer, uint32_t *size) {
    const hzip_entry_t *entry;
    const uint8_t *payload;
    uint8_t *output;
    uint32_t written = 0;
    int result;

    if (buffer) *buffer = NULL;
    if (size) *size = 0;
    if (!archive || !buffer || !size || index >= archive->entry_count)
        return HZIP_ERR_ARGUMENT;
    entry = &archive->entries[index];
    if (entry->directory) {
        output = (uint8_t *)bk_sys_alloc(1U);
        if (!output) return HZIP_ERR_MEMORY;
        *buffer = output;
        *size = 0;
        return HZIP_OK;
    }
    if (entry->flags & HZIP_FLAG_ENCRYPTED) return HZIP_ERR_ENCRYPTED;
    if (entry->method != 0U && entry->method != 8U) return HZIP_ERR_METHOD;
    result = hz_entry_payload(archive, entry, &payload);
    if (result != HZIP_OK) return result;

    output = (uint8_t *)bk_sys_alloc(entry->uncompressed_size ? entry->uncompressed_size : 1U);
    if (!output) return HZIP_ERR_MEMORY;
    if (entry->method == 0U) {
        if (entry->compressed_size != entry->uncompressed_size) {
            bk_sys_free(output);
            return HZIP_ERR_CORRUPT;
        }
        hz_copy(output, payload, entry->uncompressed_size);
        written = entry->uncompressed_size;
    } else {
        int inflate_result = hz_inflate_raw(payload, entry->compressed_size,
                                            output, entry->uncompressed_size,
                                            &written);
        if (inflate_result != HZ_INFLATE_OK || written != entry->uncompressed_size) {
            bk_sys_free(output);
            return HZIP_ERR_INFLATE;
        }
    }
    if (hz_crc32(output, written) != entry->crc32) {
        bk_sys_free(output);
        return HZIP_ERR_CRC;
    }
    *buffer = output;
    *size = written;
    return HZIP_OK;
}

int hzip_test_entry(const hzip_archive_t *archive, uint32_t index) {
    void *buffer;
    uint32_t size;
    int result;
    if (!archive || index >= archive->entry_count) return HZIP_ERR_ARGUMENT;
    if (archive->entries[index].directory) return HZIP_OK;
    result = hzip_read_entry(archive, index, &buffer, &size);
    (void)size;
    if (result == HZIP_OK) bk_sys_free(buffer);
    return result;
}

int hzip_test_archive(const hzip_archive_t *archive,
                      hzip_progress_callback_t progress, void *context,
                      uint32_t *tested, uint32_t *failed) {
    uint32_t ok = 0;
    uint32_t bad = 0;
    uint32_t i;
    int first_error = HZIP_OK;
    if (tested) *tested = 0;
    if (failed) *failed = 0;
    if (!archive) return HZIP_ERR_ARGUMENT;
    for (i = 0; i < archive->entry_count; ++i) {
        int result;
        if (progress) progress(i, archive->entry_count, &archive->entries[i], context);
        if (archive->entries[i].directory) {
            ++ok;
            continue;
        }
        result = hzip_test_entry(archive, i);
        if (result == HZIP_OK) ++ok;
        else {
            ++bad;
            if (first_error == HZIP_OK) first_error = result;
        }
        bk_sys_sleep_ms(0);
    }
    if (progress) progress(archive->entry_count, archive->entry_count, NULL, context);
    if (tested) *tested = ok;
    if (failed) *failed = bad;
    return bad ? first_error : HZIP_OK;
}

int hzip_extract_entry(const hzip_archive_t *archive, uint32_t index,
                       const char *destination_root) {
    const hzip_entry_t *entry;
    char relative[BK_PATH_MAX];
    char path[BK_PATH_MAX];
    void *buffer = NULL;
    uint32_t size = 0;
    int result;

    if (!archive || index >= archive->entry_count || !destination_root || !destination_root[0])
        return HZIP_ERR_ARGUMENT;
    entry = &archive->entries[index];
    if (!hz_sanitize_relative(entry->name, relative, sizeof(relative), entry->directory))
        return HZIP_ERR_PATH;
    if (!hz_join_path(path, sizeof(path), destination_root, relative)) return HZIP_ERR_PATH;

    if (entry->directory) {
        hz_mkdir_parents(path, true);
        return HZIP_OK;
    }
    hz_mkdir_parents(path, false);
    result = hzip_read_entry(archive, index, &buffer, &size);
    if (result != HZIP_OK) return result;
    if (!bk_file_write_all(path, buffer, size)) result = HZIP_ERR_WRITE;
    bk_sys_free(buffer);
    return result;
}

int hzip_extract_archive(const hzip_archive_t *archive,
                         const char *destination_root,
                         const uint8_t *selection,
                         hzip_progress_callback_t progress, void *context,
                         uint32_t *extracted, uint32_t *failed) {
    uint32_t ok = 0;
    uint32_t bad = 0;
    uint32_t total = 0;
    uint32_t current = 0;
    uint32_t i;
    int first_error = HZIP_OK;
    if (extracted) *extracted = 0;
    if (failed) *failed = 0;
    if (!archive || !destination_root) return HZIP_ERR_ARGUMENT;

    for (i = 0; i < archive->entry_count; ++i)
        if (!selection || selection[i]) ++total;
    for (i = 0; i < archive->entry_count; ++i) {
        int result;
        if (selection && !selection[i]) continue;
        if (progress) progress(current, total, &archive->entries[i], context);
        result = hzip_extract_entry(archive, i, destination_root);
        if (result == HZIP_OK) ++ok;
        else {
            ++bad;
            if (first_error == HZIP_OK) first_error = result;
        }
        ++current;
        bk_sys_sleep_ms(0);
    }
    if (progress) progress(total, total, NULL, context);
    if (extracted) *extracted = ok;
    if (failed) *failed = bad;
    return bad ? first_error : HZIP_OK;
}

const char *hzip_result_string(int result) {
    switch (result) {
        case HZIP_OK: return "OK";
        case HZIP_ERR_ARGUMENT: return "invalid argument";
        case HZIP_ERR_READ: return "cannot read archive";
        case HZIP_ERR_NOT_ZIP: return "not a ZIP archive";
        case HZIP_ERR_MULTIDISK: return "multi-disk ZIP is not supported";
        case HZIP_ERR_ZIP64: return "ZIP64 is not supported";
        case HZIP_ERR_TOO_MANY_ENTRIES: return "too many entries";
        case HZIP_ERR_CORRUPT: return "corrupt ZIP structure";
        case HZIP_ERR_MEMORY: return "not enough memory";
        case HZIP_ERR_ENCRYPTED: return "encrypted entry";
        case HZIP_ERR_METHOD: return "unsupported compression method";
        case HZIP_ERR_INFLATE: return "DEFLATE stream error";
        case HZIP_ERR_CRC: return "CRC32 mismatch";
        case HZIP_ERR_PATH: return "unsafe or too long path";
        case HZIP_ERR_WRITE: return "cannot write extracted file";
        default: return "unknown ZIP error";
    }
}

uint32_t hzip_entry_ratio_percent(const hzip_entry_t *entry) {
    uint32_t saved;
    uint32_t percent = 0;
    uint64_t scaled_saved;
    uint64_t threshold;
    if (!entry || !entry->uncompressed_size) return 0;
    if (entry->compressed_size >= entry->uncompressed_size) return 0;
    saved = entry->uncompressed_size - entry->compressed_size;

    /* Avoid 64-bit division. BlesKernOS currently exports __divdi3 but not
     * __udivdi3 to ET_REL applications. The result is only 0..100, so a
     * bounded comparison loop is cheaper and has no runtime helper. */
    scaled_saved = (uint64_t)saved * 100U;
    threshold = entry->uncompressed_size;
    while (percent < 100U && scaled_saved >= threshold) {
        ++percent;
        threshold += entry->uncompressed_size;
    }
    return percent;
}

void hzip_entry_datetime(const hzip_entry_t *entry,
                         uint16_t *year, uint8_t *month, uint8_t *day,
                         uint8_t *hour, uint8_t *minute, uint8_t *second) {
    uint16_t date = entry ? entry->dos_date : 0;
    uint16_t time = entry ? entry->dos_time : 0;
    if (year) *year = (uint16_t)(1980U + ((date >> 9) & 0x7FU));
    if (month) *month = (uint8_t)((date >> 5) & 0x0FU);
    if (day) *day = (uint8_t)(date & 0x1FU);
    if (hour) *hour = (uint8_t)((time >> 11) & 0x1FU);
    if (minute) *minute = (uint8_t)((time >> 5) & 0x3FU);
    if (second) *second = (uint8_t)((time & 0x1FU) * 2U);
}
