#ifndef BLESKERNOS_ARCHIVE_LBS_H
#define BLESKERNOS_ARCHIVE_LBS_H
#include <bleskernos_lbs.h>
#include <bleskernos_api.h>
#define BK_ARCHIVE_MAX_BYTES (16U * 1024U * 1024U)
/* BKZ1 RLE. Returned buffers belong to the caller: release with bk_sys_free.
 * Input is borrowed for the call. Invalid/truncated/trailing data is rejected.
 * On error output is NULL and output_size is zero. */
int bk_archive_info(const void *data, uint32_t size, uint32_t *unpacked_size);
int bk_archive_pack(const void *data, uint32_t size, void **output, uint32_t *output_size);
int bk_archive_unpack(const void *data, uint32_t size, void **output, uint32_t *output_size);
#endif
