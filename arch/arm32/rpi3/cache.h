#ifndef BLES_ARCH_ARM32_RPI3_CACHE_H
#define BLES_ARCH_ARM32_RPI3_CACHE_H

#include "../../../kernel/include/types.h"

/* Cortex-A53 L1 data cache uses 64-byte lines. These helpers operate by MVA
   to the point of coherency and are used for ARM<->VideoCore shared buffers. */
void rpi3_cache_clean_range(const void *addr, uint32_t bytes);
void rpi3_cache_invalidate_range(const void *addr, uint32_t bytes);
void rpi3_cache_clean_invalidate_range(const void *addr, uint32_t bytes);
/* Publish freshly generated/relocated code before entering ARM USR mode. */
void rpi3_cache_sync_code(const void *addr, uint32_t bytes);

#endif
