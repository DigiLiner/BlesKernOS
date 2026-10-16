#ifndef WINE_POLICY_H
#define WINE_POLICY_H

#include "memory.h"

/* Use installed E820 RAM, never heap capacity, free RAM or swap capacity. */
static inline bool wine_memory_allowed(void) {
    return mm_total_bytes() > 128U * 1024U * 1024U;
}

#endif
