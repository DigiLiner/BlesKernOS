#ifndef BLESKERNOS_HEAP_STATISTICS_H
#define BLESKERNOS_HEAP_STATISTICS_H
/* Statistics for the caller's POSIX allocator, not total system RAM. */
typedef struct {
    unsigned int size, live_bytes, reserved_bytes, allocations, free_bytes;
} bk_posix_heap_statistics_t;
int bk_posix_heap_statistics(bk_posix_heap_statistics_t *);
#endif
