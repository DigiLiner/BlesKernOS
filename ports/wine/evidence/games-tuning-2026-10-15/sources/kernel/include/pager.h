#ifndef BK_PAGER_H
#define BK_PAGER_H
#include "paging.h"
#define VM_MAP_PAGED 0x40000000U
typedef struct {
    uint32_t virtual_pages,resident_pages,swapped_pages,faults,evictions,failures;
    uint32_t anonymous_pages,file_private_pages,file_clean_pages,borrowed_pages;
} pager_stats_t;
bool pager_frame_alloc(uint32_t pages,uint32_t *physical);
bool pager_map(uint32_t pid,uint32_t requested,uint32_t length,uint32_t prot,bool exact,
               uint32_t *address,paging_range_skip_t skip,void *context);
/* The caller retains ownership of the shared, resident source frames. */
bool pager_adopt_cow(uint32_t pid,uint32_t address,uint32_t length,uint32_t source);
/* File references are inode handles, never descriptor-table slots. VFS domain
 * precedes the pager mutex for file I/O and reference destruction. */
struct vfs_page_reference;
struct vfs_shared_mapping;
bool pager_map_file(uint32_t pid,uint32_t requested,uint32_t length,uint32_t prot,bool exact,
                    uint32_t *address,paging_range_skip_t skip,void *context,
                    struct vfs_page_reference *reference,uint32_t offset);
bool pager_file_promote(struct vfs_page_reference *reference,struct vfs_shared_mapping *shared);
void pager_file_written(struct vfs_page_reference *reference,uint32_t offset,const void *data,uint32_t length);
void pager_file_cache_perf(uint32_t *entries,uint32_t *bytes,uint32_t *released);
bool pager_protect(uint32_t pid,uint32_t address,uint32_t length,uint32_t prot);
/* Protect all live pager pages in a mixed VMA span. The caller preflights
 * ownership and supplies exact pager coverage; holes are left untouched. */
bool pager_protect_mapped(uint32_t pid,uint32_t address,uint32_t length,
                          uint32_t prot,uint32_t expected_bytes);
/* Drop contents while retaining a protected virtual reservation (brk shrink). */
bool pager_discard(uint32_t pid,uint32_t address,uint32_t length);
/* MADV_DONTNEED: discard private contents, keeping mapping permissions. */
bool pager_reset(uint32_t pid,uint32_t address,uint32_t length);
bool pager_fault(uint32_t pid,uint32_t address,bool write);
bool pager_fault_is_bus(uint32_t pid,uint32_t address,bool write);
bool pager_unmap(uint32_t pid,uint32_t address,uint32_t length);
bool pager_pin(uint32_t pid,uint32_t address,uint32_t length,bool write);
/* Aligned futex word: release cannot block after publishing a sleeping task. */
bool pager_pin_word(uint32_t pid,uint32_t address,void **token);
void pager_release_word(void *token);
void pager_unpin(uint32_t pid,uint32_t address,uint32_t length);
void pager_destroy(uint32_t pid);
bool pager_clone(uint32_t parent,uint32_t child,uint32_t address,uint32_t length);
void pager_swap_ids(uint32_t first,uint32_t second);
bool pager_swapoff(void);
void pager_stats(uint32_t pid,pager_stats_t *out);
#endif
