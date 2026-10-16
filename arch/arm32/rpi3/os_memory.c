#include "physmem.h"
#include "uart.h"
#include "../../../kernel/include/memory.h"
#include "../../../kernel/include/task.h"

#define RPI3_EARLY_HEAP_BYTES (32U * 1024U * 1024U)
#define RPI3_HEAP_ALIGN 8U

typedef struct arm_heap_block {
    uint32_t magic;
    uint32_t size;
    bool free;
    uint8_t reserved[3];
    uint32_t owner_process_id;
    struct arm_heap_block *next;
    struct arm_heap_block *prev;
} arm_heap_block_t;

#define ARM_HEAP_MAGIC 0xA32B1E50U

static uint8_t *g_heap_base;
static uint32_t g_heap_bytes;
static arm_heap_block_t *g_heap_head;

/* Stage 10 made the ARM port preemptive, but this bootstrap heap still used
 * the original single-threaded linked-list allocator.  A timer switch between
 * block_split()/block_merge_next() pointer stores lets another task enter
 * kmalloc/kfree and corrupt next/prev.  On the current ARM32 port only CPU0
 * schedules kernel work, so the correct lightweight lock is the scheduler
 * preemption guard.  It is deliberately nestable: task creation already
 * enters a preemption critical section before allocating its stack. */
static void heap_lock(void) { task_preempt_disable(); }
static void heap_unlock(void) { task_preempt_enable(); }

static bool g_heap_faulted;

static bool heap_header_in_range(const arm_heap_block_t *block) {
    uintptr_t begin = (uintptr_t)g_heap_base;
    uintptr_t end = begin + g_heap_bytes;
    uintptr_t p = (uintptr_t)block;
    return block && p >= begin && p + sizeof(*block) >= p &&
           p + sizeof(*block) <= end;
}

static bool heap_block_sane(const arm_heap_block_t *block,
                            const arm_heap_block_t *previous) {
    uintptr_t heap_end;
    uintptr_t payload;
    uintptr_t block_end;
    if (!heap_header_in_range(block)) return false;
    if (block->magic != ARM_HEAP_MAGIC || block->prev != previous) return false;
    heap_end = (uintptr_t)g_heap_base + g_heap_bytes;
    payload = (uintptr_t)(block + 1);
    block_end = payload + block->size;
    if (block_end < payload || block_end > heap_end) return false;
    if (block->next) {
        if (!heap_header_in_range(block->next)) return false;
        /* Blocks are split in increasing address order.  Requiring a strictly
         * increasing next pointer also turns an accidental list cycle into a
         * recoverable allocator failure instead of a kernel-wide busy loop. */
        if ((uintptr_t)block->next <= (uintptr_t)block) return false;
        if (block->next->prev != block) return false;
    }
    return true;
}

static void heap_report_fault_once(void) {
    if (g_heap_faulted) return;
    g_heap_faulted = true;
    rpi3_uart_puts("[BLES:MM] HEAP CORRUPTION detected; allocator disabled\n");
}

static uint32_t align_up(uint32_t value, uint32_t align) {
    return (value + align - 1U) & ~(align - 1U);
}

static void block_split(arm_heap_block_t *block, uint32_t wanted) {
    uint32_t remain;
    arm_heap_block_t *tail;
    if (!block || block->size <= wanted) return;
    remain = block->size - wanted;
    if (remain <= sizeof(*tail) + RPI3_HEAP_ALIGN) return;
    tail = (arm_heap_block_t *)((uint8_t *)(block + 1) + wanted);
    tail->magic = ARM_HEAP_MAGIC;
    tail->size = remain - (uint32_t)sizeof(*tail);
    tail->free = true;
    tail->owner_process_id = 0U;
    tail->next = block->next;
    tail->prev = block;
    if (tail->next) tail->next->prev = tail;
    block->next = tail;
    block->size = wanted;
}

static void block_merge_next(arm_heap_block_t *block) {
    arm_heap_block_t *next;
    if (!block || !block->free) return;
    next = block->next;
    if (!next || next->magic != ARM_HEAP_MAGIC || !next->free) return;
    block->size += (uint32_t)sizeof(*next) + next->size;
    block->next = next->next;
    if (block->next) block->next->prev = block;
}

void mm_init(void) {
    uint32_t pages = (RPI3_EARLY_HEAP_BYTES + RPI3_PAGE_SIZE - 1U) / RPI3_PAGE_SIZE;
    g_heap_base = (uint8_t *)rpi3_phys_alloc_pages(pages);
    g_heap_bytes = g_heap_base ? pages * RPI3_PAGE_SIZE : 0U;
    g_heap_faulted = false;
    g_heap_head = (arm_heap_block_t *)g_heap_base;
    if (!g_heap_head || g_heap_bytes <= sizeof(*g_heap_head)) {
        g_heap_head = (arm_heap_block_t *)0;
        return;
    }
    g_heap_head->magic = ARM_HEAP_MAGIC;
    g_heap_head->size = g_heap_bytes - (uint32_t)sizeof(*g_heap_head);
    g_heap_head->free = true;
    g_heap_head->owner_process_id = 0U;
    g_heap_head->next = (arm_heap_block_t *)0;
    g_heap_head->prev = (arm_heap_block_t *)0;
}

uint32_t mm_heap_start(void) { return (uint32_t)(uintptr_t)g_heap_base; }
uint32_t mm_heap_end(void) { return mm_heap_start() + g_heap_bytes; }
uint32_t mm_physical_top(void) { return rpi3_phys_limit(); }
uint32_t mm_total_bytes(void) { return rpi3_phys_limit(); }
size_t mm_heap_size(void) { return g_heap_bytes; }

void *kmalloc(size_t size) {
    arm_heap_block_t *block;
    arm_heap_block_t *previous = NULL;
    uint32_t wanted;
    void *result = (void *)0;
    if (!size || !g_heap_head || g_heap_faulted) return (void *)0;
    wanted = align_up((uint32_t)size, RPI3_HEAP_ALIGN);
    if (wanted < size) return (void *)0;

    heap_lock();
    for (block = g_heap_head; block; previous = block, block = block->next) {
        if (!heap_block_sane(block, previous)) {
            heap_report_fault_once();
            result = (void *)0;
            break;
        }
        if (!block->free || block->size < wanted) continue;
        block_split(block, wanted);
        block->free = false;
        block->owner_process_id = task_current_process_id();
        result = (void *)(block + 1);
        break;
    }
    heap_unlock();
    return result;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) kmemset(p, 0, size);
    return p;
}

void kfree(void *ptr) {
    arm_heap_block_t *block;
    if (!ptr || g_heap_faulted) return;
    heap_lock();
    block = ((arm_heap_block_t *)ptr) - 1;
    if (!heap_header_in_range(block) || block->magic != ARM_HEAP_MAGIC) {
        heap_report_fault_once();
        heap_unlock();
        return;
    }
    /* A repeated free used to merge the same header a second time and could
       splice live blocks out of the list.  Treat it as a rejected request;
       the allocator remains usable and the offending program can fail
       without taking the complete desktop down. */
    if (block->free) {
        rpi3_uart_puts("[BLES:MM] WARN double free rejected ptr=");
        rpi3_uart_puthex((uint32_t)(uintptr_t)ptr);
        rpi3_uart_puts("\n");
        heap_unlock();
        return;
    }
    block->free = true;
    block->owner_process_id = 0U;
    block_merge_next(block);
    if (block->prev && block->prev->free) {
        block = block->prev;
        block_merge_next(block);
    }
    heap_unlock();
}

size_t mm_allocation_size(const void *ptr) {
    const arm_heap_block_t *block;
    size_t result = 0U;
    if (!ptr || g_heap_faulted) return 0U;
    heap_lock();
    block = ((const arm_heap_block_t *)ptr) - 1;
    if (heap_header_in_range(block) && block->magic == ARM_HEAP_MAGIC &&
        !block->free) result = block->size;
    heap_unlock();
    return result;
}

void *krealloc(void *ptr, size_t new_size) {
    void *grown;
    size_t old_size;
    if (!ptr) return kmalloc(new_size);
    if (!new_size) { kfree(ptr); return (void *)0; }
    old_size = mm_allocation_size(ptr);
    if (!old_size) return (void *)0;
    if (new_size <= old_size) return ptr;
    grown = kmalloc(new_size);
    if (!grown) return (void *)0;
    kmemcpy(grown, ptr, old_size);
    kfree(ptr);
    return grown;
}

void mm_get_info(heap_info_t *info) {
    arm_heap_block_t *block;
    arm_heap_block_t *previous = NULL;
    if (!info) return;
    kmemset(info, 0, sizeof(*info));
    heap_lock();
    info->total_bytes = g_heap_bytes;
    for (block = g_heap_head; block; previous = block, block = block->next) {
        if (!heap_block_sane(block, previous)) {
            heap_report_fault_once();
            break;
        }
        info->total_blocks++;
        if (block->free) {
            info->free_blocks++;
            info->free_bytes += block->size;
        } else {
            info->used_blocks++;
            info->used_bytes += block->size;
        }
    }
    heap_unlock();
}

void mm_get_system_info(system_memory_info_t *info) {
    heap_info_t heap;
    if (!info) return;
    mm_get_info(&heap);
    info->total_bytes = rpi3_phys_limit();
    info->used_bytes = heap.used_bytes;
    info->free_bytes = rpi3_phys_free_bytes() + heap.free_bytes;
    info->reserved_bytes = rpi3_phys_base();
}

size_t mm_get_process_usage(uint32_t process_id) {
    arm_heap_block_t *block;
    arm_heap_block_t *previous = NULL;
    size_t usage = 0U;
    if (!process_id || g_heap_faulted) return 0U;
    heap_lock();
    for (block = g_heap_head; block; previous = block, block = block->next) {
        if (!heap_block_sane(block, previous)) { heap_report_fault_once(); break; }
        if (!block->free && block->owner_process_id == process_id)
            usage += block->size;
    }
    heap_unlock();
    return usage;
}
void mm_get_process_usage_batch(const uint32_t *process_ids,
                                size_t *usage, uint32_t count) {
    arm_heap_block_t *block;
    arm_heap_block_t *previous = NULL;
    if (!usage || !process_ids) return;
    for (uint32_t i = 0U; i < count; ++i) usage[i] = 0U;
    if (g_heap_faulted) return;
    heap_lock();
    for (block = g_heap_head; block; previous = block, block = block->next) {
        if (!heap_block_sane(block, previous)) { heap_report_fault_once(); break; }
        if (block->free || !block->owner_process_id) continue;
        for (uint32_t i = 0U; i < count; ++i) {
            if (process_ids[i] == block->owner_process_id) {
                usage[i] += block->size;
                break;
            }
        }
    }
    heap_unlock();
}
bool mm_set_allocation_owner(void *ptr, uint32_t process_id) {
    arm_heap_block_t *block;
    bool result = false;
    if (!ptr || g_heap_faulted) return false;
    heap_lock();
    block = ((arm_heap_block_t *)ptr) - 1;
    if (heap_header_in_range(block) && block->magic == ARM_HEAP_MAGIC &&
        !block->free) {
        block->owner_process_id = process_id;
        result = true;
    }
    heap_unlock();
    return result;
}
size_t mm_release_process(uint32_t process_id) {
    arm_heap_block_t *block;
    arm_heap_block_t *previous = NULL;
    size_t released = 0U;
    if (!process_id || g_heap_faulted) return 0U;
    heap_lock();
    for (block = g_heap_head; block; previous = block, block = block->next) {
        if (!heap_block_sane(block, previous)) { heap_report_fault_once(); break; }
        if (block->free || block->owner_process_id != process_id) continue;
        released += block->size;
        block->free = true;
        block->owner_process_id = 0U;
    }
    if (!g_heap_faulted) {
        block = g_heap_head;
        while (block && block->next) {
            if (!heap_block_sane(block, block->prev) ||
                !heap_block_sane(block->next, block)) {
                heap_report_fault_once();
                break;
            }
            if (block->free && block->next->free) {
                block_merge_next(block);
                continue;
            }
            block = block->next;
        }
    }
    heap_unlock();
    return released;
}
void mm_dump(void) { }
void mm_boot_snapshot(void) { }
uint16_t mm_boot_equipment_word(void) { return 0U; }
uint16_t mm_boot_ebda_segment(void) { return 0U; }
uint16_t mm_boot_conventional_kb(void) { return 0U; }
uint8_t mm_boot_vesa_read8(uint32_t offset UNUSED) { return 0U; }
uint16_t mm_boot_vesa_read16(uint32_t offset UNUSED) { return 0U; }
uint32_t mm_boot_vesa_read32(uint32_t offset UNUSED) { return 0U; }

void *mm_alloc_guarded_stack(uint32_t requested_size, bool user_stack UNUSED,
                             void **allocation_out,
                             uint32_t *usable_size_out) {
    void *p = kmalloc(requested_size);
    if (allocation_out) *allocation_out = p;
    if (usable_size_out) *usable_size_out = p ? requested_size : 0U;
    return p;
}
void mm_free_guarded_stack(void *allocation UNUSED, void *stack,
                           uint32_t usable_size UNUSED) { kfree(stack); }

void *kmemset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memset(void *dst, int c, size_t n) { return kmemset(dst, c, n); }
void *memcpy(void *dst, const void *src, size_t n) { return kmemcpy(dst, src, n); }

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || !n) return dst;
    if (d < s) while (n--) *d++ = *s++;
    else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *kmemcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

int kmemcmp(const void *a, const void *b, size_t n) {
    const uint8_t *aa = (const uint8_t *)a;
    const uint8_t *bb = (const uint8_t *)b;
    while (n--) {
        if (*aa != *bb) return (int)*aa - (int)*bb;
        aa++; bb++;
    }
    return 0;
}

size_t kstrlen(const char *s) { size_t n = 0U; if (!s) return 0U; while (s[n]) n++; return n; }
size_t strlen(const char *s) { return kstrlen(s); }
int kstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
int kstrncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
char *kstrcpy(char *dst, const char *src) {
    char *out = dst;
    while ((*dst++ = *src++) != '\0') { }
    return out;
}
char *kstrncpy(char *dst, const char *src, size_t n) {
    char *out = dst;
    size_t i = 0U;
    while (i < n && src[i]) { dst[i] = src[i]; i++; }
    while (i < n) dst[i++] = '\0';
    return out;
}
char *strncpy(char *dst, const char *src, size_t n) {
    return kstrncpy(dst, src, n);
}
char *kstrcat(char *dst, const char *src) {
    char *out = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++) != '\0') { }
    return out;
}

/* Clang may lower aligned struct copies to the ARM EABI helpers even in a
 * freestanding build. Keep these tiny wrappers in the architecture runtime
 * instead of linking compiler-rt/libgcc into the kernel bootstrap. */
void __aeabi_memcpy(void *dst, const void *src, size_t n) { (void)kmemcpy(dst, src, n); }
void __aeabi_memcpy4(void *dst, const void *src, size_t n) { (void)kmemcpy(dst, src, n); }
void __aeabi_memcpy8(void *dst, const void *src, size_t n) { (void)kmemcpy(dst, src, n); }
void __aeabi_memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d <= s) while (n--) *d++ = *s++;
    else { d += n; s += n; while (n--) *--d = *--s; }
}
void __aeabi_memset(void *dst, size_t n, int c) { (void)kmemset(dst, c, n); }
void __aeabi_memset4(void *dst, size_t n, int c) { (void)kmemset(dst, c, n); }
void __aeabi_memset8(void *dst, size_t n, int c) { (void)kmemset(dst, c, n); }
void __aeabi_memclr(void *dst, size_t n) { (void)kmemset(dst, 0, n); }
void __aeabi_memclr4(void *dst, size_t n) { (void)kmemset(dst, 0, n); }
void __aeabi_memclr8(void *dst, size_t n) { (void)kmemset(dst, 0, n); }
