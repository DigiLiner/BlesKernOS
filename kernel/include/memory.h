#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

/* Kernel globals fit below 4 MiB. Small native desktops reuse the physical
 * Windows image window; larger machines retain it for fixed-base programs. */
#define HEAP_START          0x00400000U
#define PE_FIXED_VIEW_START 0x00400000U
#define HEAP_LOW_START      0x00400000U
#define HEAP_ALLOC_START    0x02000000U
#define PE_FIXED_VIEW_END   0x02000000U
#define HEAP_DEFAULT_SIZE 0x04000000U
#define HEAP_LIMIT_MAX    0x60000000U
#define HEAP_END          (mm_heap_end())
#define HEAP_SIZE         (mm_heap_size())
#define HEAP_MAGIC  0xB1E5C0DE
#define MEMORY_DISPLAY_MB_THRESHOLD (64U * 1024U * 1024U)

typedef struct heap_block {
    uint32_t magic;
    size_t size;
    bool free;
    uint8_t subsystem; /* uses existing padding: the i386 header remains 24 bytes */
    uint32_t owner_process_id;
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

typedef struct {
    size_t total_bytes;
    size_t used_bytes;
    size_t free_bytes;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t used_blocks;
} heap_info_t;

typedef struct {
    size_t total_bytes;
    size_t used_bytes;
    size_t free_bytes;
    size_t reserved_bytes;
} system_memory_info_t;

/* Copy the bootloader E820 map out of the first physical page before
 * paging installs the null-page guard. Must run exactly once during early
 * BSP boot, while physical address 0x500 is still readable. */
void mm_boot_snapshot(void);
/* Cached legacy boot data from the first physical page. These accessors are
 * safe after the null-page guard is enabled; before the snapshot they fall
 * back to the firmware buffer only while paging is still disabled. */
uint16_t mm_boot_equipment_word(void);
uint16_t mm_boot_ebda_segment(void);
uint16_t mm_boot_conventional_kb(void);
uint8_t mm_boot_vesa_read8(uint32_t offset);
uint16_t mm_boot_vesa_read16(uint32_t offset);
uint32_t mm_boot_vesa_read32(uint32_t offset);
void mm_init(void);
uint32_t mm_heap_start(void);
uint32_t mm_heap_end(void);
uint32_t mm_physical_top(void);
/* Read-only firmware tables can occupy E820 reserved RAM above the heap. */
bool mm_firmware_range_valid(uint32_t address,uint32_t length);
uint32_t mm_total_bytes(void);
size_t mm_heap_size(void);

/* VM-1 physical-frame reserve. These pages are carved from the top of the
 * contiguous RAM region before the general heap is created, so their identity
 * addresses are supervisor-only and safe as backing for per-process mappings.
 * The allocator currently returns contiguous runs because paging_process_map()
 * consumes contiguous physical backing. */
bool mm_vm_frame_alloc(uint32_t page_count, uint32_t *physical_out);
void mm_vm_frame_free(uint32_t physical_address, uint32_t page_count);
uint32_t mm_vm_frame_total_pages(void);
uint32_t mm_vm_frame_free_pages(void);
/* Addresses still dereferenced by CPL0 under a process CR3. */
bool mm_kernel_identity_overlaps(uint32_t address, uint32_t length);
/* Reserve identity-mapped device registers against process MAP_FIXED. */
bool mm_register_kernel_mmio(uint32_t address, uint32_t length);

/* Page-aligned stacks with non-present guard pages. `allocation_out` is an
 * opaque token required by mm_free_guarded_stack(); callers use the returned
 * address as the usable stack base. */
void *mm_alloc_guarded_stack(uint32_t requested_size, bool user_stack,
                             void **allocation_out,
                             uint32_t *usable_size_out);
void mm_free_guarded_stack(void *allocation, void *stack,
                           uint32_t usable_size);

void *kmalloc(size_t size);
void *kzalloc(size_t size);
bool mm_pin_address_allocation(void *ptr);
void mm_unpin_address_allocation(void *ptr);
void kfree(void *ptr);
/* Native API entry points: ownership check and mutation share the heap lock. */
uint32_t mm_user_heap_available(uint32_t address,uint32_t process_id);
bool mm_user_heap_payload(uint32_t address,uint32_t length,uint32_t process_id);
bool mm_user_heap_pin(uint32_t address,uint32_t length,uint32_t process_id,void **token);
/* Called only through the trusted service boundary; records live in BSS. */
bool mm_user_publish(void *ptr,uint32_t process_id);
bool mm_user_subsystem(void *ptr,uint8_t subsystem,uint32_t process_id);
bool mm_user_free(void *ptr,uint32_t process_id);
void *mm_user_realloc(void *ptr,size_t size,uint32_t process_id);
void *krealloc(void *ptr, size_t new_size);
size_t mm_allocation_size(const void *ptr);
void mm_get_info(heap_info_t *info);
void mm_get_system_info(system_memory_info_t *info);
/* Unique allocated physical pages, including heap headers/page slack.
 * Excludes the fixed boot reservation; suitable for baseline deltas. */
void mm_resident_pages(uint32_t *heap_pages, uint32_t *vm_pages);
size_t mm_get_process_usage(uint32_t process_id);
void mm_get_process_usage_batch(const uint32_t *process_ids, size_t *usage,
                                uint32_t count);
enum {MM_SUBSYSTEM_OTHER,MM_SUBSYSTEM_GUI,MM_SUBSYSTEM_VFS,MM_SUBSYSTEM_VM,
      MM_SUBSYSTEM_PROCESS,MM_SUBSYSTEM_AUDIO,MM_SUBSYSTEM_NETWORK,MM_SUBSYSTEM_DRIVER,
      MM_SUBSYSTEM_COUNT};
typedef struct {uint32_t bytes,blocks;} mm_subsystem_usage_t;
bool mm_set_allocation_subsystem(void *ptr,uint8_t subsystem);
void mm_get_subsystem_usage(mm_subsystem_usage_t *out);
bool mm_set_allocation_owner(void *ptr, uint32_t process_id);
void mm_swap_process_owners(uint32_t first,uint32_t second);
size_t mm_release_process(uint32_t process_id);
void mm_dump(void);
void *kmemset(void *dst, int c, size_t n);
void *kmemcpy(void *dst, const void *src, size_t n);
int kmemcmp(const void *a, const void *b, size_t n);
size_t kstrlen(const char *s);
int kstrcmp(const char *a, const char *b);
int kstrncmp(const char *a, const char *b, size_t n);
char *kstrcpy(char *dst, const char *src);
char *kstrncpy(char *dst, const char *src, size_t n);
char *kstrcat(char *dst, const char *src);

#endif
