#ifndef W3_PROFILE_H
#define W3_PROFILE_H

#include "types.h"

/* Diagnostic-only W3 boot profiler. IDs below 512 are raw Linux i386
 * syscall numbers; the remaining IDs are nested kernel operations. */
#define W3P_LIMIT 540U
#define W3P_VFS_NORMALIZE 512U
#define W3P_VFS_OPEN      513U
#define W3P_VFS_READ      514U
#define W3P_VFS_WRITE     515U
#define W3P_VFS_FLUSH     516U
#define W3P_VFS_CLOSE     517U
#define W3P_VFS_STAT      518U
#define W3P_FAT_RESOLVE   519U
#define W3P_FAT_DIR_FIND  520U
#define W3P_FAT_SLOT_FIND 521U
#define W3P_FAT_FREE_RUN  522U
#define W3P_FAT_COMMIT    523U
#define W3P_FAT_SECTOR_R  524U
#define W3P_FAT_SECTOR_W  525U
#define W3P_BLOCK_READ    526U
#define W3P_BLOCK_WRITE   527U
#define W3P_AHCI_CMD      528U
#define W3P_AHCI_FLUSH    529U
#define W3P_IPC_YIELD     530U
#define W3P_IPC_SLEEP     531U
#define W3P_IPC_READ      532U
#define W3P_IPC_WRITE     533U
#define W3P_IPC_GC        534U

#ifdef W3_PROFILE
static inline uint64_t w3p_clock(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
void w3p_reset(void);
void w3p_record(uint32_t id, uint64_t started, uint32_t bytes);
void w3p_dump(void);
typedef struct { uint32_t id, bytes; uint64_t started; } w3p_guard_t;
static inline void w3p_guard_end(w3p_guard_t *guard) {
    w3p_record(guard->id, guard->started, guard->bytes);
}
#define W3P_SCOPE(category, amount) \
    w3p_guard_t w3p_guard __attribute__((cleanup(w3p_guard_end))) = \
        {(category), (amount), w3p_clock()}
#else
static inline uint64_t w3p_clock(void) { return 0U; }
static inline void w3p_reset(void) { }
static inline void w3p_record(uint32_t id, uint64_t started, uint32_t bytes) {
    (void)id; (void)started; (void)bytes;
}
static inline void w3p_dump(void) { }
#define W3P_SCOPE(category, amount) do { (void)(category); (void)(amount); } while (0)
#endif

#endif
