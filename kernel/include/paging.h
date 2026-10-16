#ifndef PAGING_H
#define PAGING_H

#include "types.h"

#define PAGING_PAGE_SIZE 4096U

/* VM-1 keeps process-private anonymous mappings away from the historical
 * identity-mapped heap and below the signed-pointer boundary used by several
 * i386 syscall paths. The underlying kernel mapping stays supervisor-only. */
#define PAGING_USER_VM_START 0x60000000U
#define PAGING_USER_VM_END   0x78000000U

bool paging_process_borrow(uint32_t parent, uint32_t child);
bool paging_init(void);
bool paging_is_enabled(void);
uint32_t paging_fault_address(void);

/* Complete the boot-time user mapping once mm_init() knows the real heap. */
bool paging_configure_user_layout(uint32_t user_start, uint32_t user_end);

/* Page-aware validation used by the syscall/user-copy layer. */
bool paging_user_range_ok(const void *pointer, uint32_t length,
                          bool require_write);
/* Permission validation without demanding residency or detaching COW.
 * Only copy/pin helpers may dereference a range accepted by this check. */
bool paging_user_range_permitted(const void *pointer,uint32_t length,bool write);
bool paging_page_present(uint32_t address);
bool paging_page_user(uint32_t address);

/* Runtime permission changes used for real stack guard pages. All addresses
 * and lengths are rounded to 4 KiB pages. */
bool paging_set_range(uint32_t address, uint32_t length, bool present,
                      bool user, bool writable);

/* Transitional per-process address spaces used by fixed-address Linux ELF.
 * Kernel/heap identity mappings remain shared; explicitly mapped PDEs are
 * private to the process and may point at different physical backing. */
bool paging_process_create(uint32_t process_id);
void paging_process_destroy(uint32_t process_id);
/* Hold a live address space across user copies. Retirement fails while a CPU,
 * borrower or copy still references it; once retired no new access is allowed. */
bool paging_process_pin(uint32_t process_id);
void paging_process_unpin(uint32_t process_id);
bool paging_process_has_borrowers(uint32_t process_id);
bool paging_process_retire(uint32_t process_id);
bool paging_process_map(uint32_t process_id, uint32_t virtual_address,
                        uint32_t physical_address, uint32_t length,
                        bool user, bool writable);
/* Allocate a free virtual range in the process-private user arena and map a
 * contiguous physical run there. requested is an advisory mmap-style hint. */
bool paging_process_map_auto(uint32_t process_id, uint32_t requested,
                             uint32_t physical_address, uint32_t length,
                             bool writable, uint32_t *virtual_out);
/* Return zero when a candidate is allowed, otherwise the end of an owned
 * metadata-only interval to skip. Called under the paging lock: no locking
 * or page-table operations are permitted in the callback. */
typedef uint32_t (*paging_range_skip_t)(void *context, uint32_t address,
                                      uint32_t length);
bool paging_process_map_auto_filtered(uint32_t process_id, uint32_t requested,
    uint32_t physical_address, uint32_t length, bool writable,
    uint32_t *virtual_out, paging_range_skip_t skip, void *context);
/* Reserve an exact user virtual range without committing physical frames.
 * Used by Linux MAP_FIXED_NOREPLACE + PROT_NONE reservations (Wine). */
bool paging_process_range_free_exact(uint32_t process_id,
                                     uint32_t virtual_address,
                                     uint32_t length);
bool paging_process_reserve_exact(uint32_t process_id,
                                  uint32_t virtual_address,
                                  uint32_t length);
/* Remove process-private PTEs and restore the supervisor identity PTEs
 * underneath them. Exact Wine mappings may live outside PAGING_USER_VM_*;
 * physical pages are owned/freed by the caller. */
bool paging_process_unmap(uint32_t process_id, uint32_t virtual_address,
                          uint32_t length);
bool paging_process_protect(uint32_t process_id, uint32_t virtual_address,
                            uint32_t length, bool writable);
bool paging_process_access(uint32_t process_id, uint32_t virtual_address,
                            uint32_t length, bool accessible, bool writable);
bool paging_process_cow_access(uint32_t process_id, uint32_t address,
                               uint32_t length, bool accessible, bool writable);
bool paging_cow_write_pending(uint32_t address);
bool paging_process_cow_detach(uint32_t process_id, uint32_t address,
                               uint32_t length, uint32_t backing);
void paging_switch_process(uint32_t process_id);
uint32_t paging_current_process(void);
uint32_t paging_user_physical(uint32_t address);
/* Mark one high physical MMIO window uncached in the identity map. Call
 * during device discovery, before user process address spaces are created. */
bool paging_kernel_mmio_uncached(uint32_t physical_address, uint32_t length);
bool paging_process_copy(uint32_t parent,uint32_t child,uint32_t address,
                         uint32_t backing,uint32_t length);
bool paging_process_share(uint32_t parent,uint32_t child,uint32_t address,uint32_t length);
bool paging_process_write(uint32_t process,uint32_t address,const void *data,uint32_t length);
void paging_swap_process_ids(uint32_t first,uint32_t second);

/* Shared page tables need a TLB generation handoff on every CPU. The timer
 * scheduler calls paging_sync_cpu() before inspecting task stacks. */
void paging_sync_cpu(void);
uint32_t paging_tlb_generation(void);

/* If a CPU faults on a page that is present in the current page tables but
 * still has an old non-present TLB entry, refresh CR3 and retry once. */
bool paging_recover_stale_fault(uint32_t address);

bool paging_pager_reserve(uint32_t process_id,uint32_t requested,uint32_t length,
    bool accessible,bool writable,bool exact,uint32_t *virtual_out,
    paging_range_skip_t skip,void *context);
uint32_t paging_pager_entry(uint32_t pid,uint32_t address);
bool paging_pager_replace(uint32_t pid,uint32_t address,uint32_t frame,bool present);
bool paging_pager_replace_cow(uint32_t pid,uint32_t address,uint32_t frame,bool present,bool cow);
bool paging_pager_adopt(uint32_t pid,uint32_t address,uint32_t length);
uint32_t paging_process_owner(uint32_t pid);
bool paging_memory_pages(uint32_t pid,uint32_t *mapped,uint32_t *resident,
                         uint32_t *writable,uint32_t *cow,uint32_t *tables);
#endif
