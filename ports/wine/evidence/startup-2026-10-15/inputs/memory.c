#include "../include/types.h"
#include "../include/memory.h"
#include "../include/vga.h"
#include "../include/task.h"
#include "../include/klock.h"
#include "../include/paging.h"

static heap_block_t *heap_head = NULL;
static heap_block_t *heap_exec_reservation = NULL;
static uint32_t system_total_bytes = HEAP_DEFAULT_SIZE;
static uint32_t system_reserved_bytes = 0;
static uint32_t system_physical_top = HEAP_ALLOC_START + HEAP_DEFAULT_SIZE;
static uint32_t heap_start_address = HEAP_ALLOC_START;
static uint32_t heap_end_address = HEAP_ALLOC_START + HEAP_DEFAULT_SIZE;


#define ALIGN4(x) (((x) + 3) & ~3)
#define E820_MAP_ADDR     0x00000500U
#define E820_ENTRY_ADDR   (E820_MAP_ADDR + 4U)
#define E820_MAX_ENTRIES  20U
#define BDA_EBDA_SEGMENT_ADDR   0x0000040EU
#define BDA_EQUIPMENT_WORD_ADDR 0x00000410U
#define BDA_CONVENTIONAL_KB_ADDR 0x00000413U
#define VESA_BOOTINFO_ADDR      0x00000700U
#define VESA_BOOTINFO_SIZE      17U
#define U32_MAX_VALUE     0xFFFFFFFFU
#define MM_PAGE_SIZE      0x1000U
/* Traditional Linux/i386 ET_EXEC images occupy 0x08048000.  On machines
 * whose identity-mapped heap reaches that VA, keep the surrounding window
 * out of kmalloc so a private process mapping never hides live kernel data. */
#define MM_LINUX_EXEC_RESERVE_START 0x08000000U
#define MM_LINUX_EXEC_RESERVE_END   0x0A000000U

/* Wine must be able to install non-relocatable Win32 images at their preferred
 * base. Office 97's EXCEL.EXE/WINWORD.EXE use 0x30000000 and have stripped
 * base relocations. On large-RAM machines BlesKernOS' identity-addressed VM
 * frame pool used to occupy that same linear range, so MAP_FIXED_NOREPLACE
 * quite correctly saw a kernel-owned collision and Wine returned
 * STATUS_CONFLICTING_ADDRESSES.
 *
 * Keep a modest physical hole unused by the kernel. Per-process page tables may
 * then safely shadow its supervisor identity mapping with Wine's user PTEs.
 * 64 MiB leaves room for the main image and neighbouring fixed-base modules
 * while preserving almost all of the large VM frame pool. */
#define MM_WIN32_IMAGE_RESERVE_START 0x30000000U
#define MM_WIN32_IMAGE_RESERVE_END   0x34000000U

/* VM-1 reserves physical pages for process mappings and eager fork copies.
 * At 1 GiB, 512 MiB ran out while Wine started services, even though the
 * legacy heap still had ample room. Keep a quarter of the span for kmalloc
 * on large machines and give the remaining three quarters to process pages. */
#define MM_VM_POOL_MIN_RAM      (15U * 1024U * 1024U)
#define MM_VM_POOL_MIN_BYTES    (8U * 1024U * 1024U)
#define MM_VM_POOL_MAX_BYTES    (1024U * 1024U * 1024U)
#define MM_VM_POOL_MAX_PAGES    (MM_VM_POOL_MAX_BYTES / MM_PAGE_SIZE)
#define MM_VM_POOL_BITMAP_WORDS ((MM_VM_POOL_MAX_PAGES + 31U) / 32U)

typedef struct {
    uint32_t base_low;
    uint32_t base_high;
    uint32_t length_low;
    uint32_t length_high;
    uint32_t type;
} PACKED e820_entry_t;

/* Stage 2 publishes E820 at physical 0x500, which belongs to the page that
 * Phase 1 intentionally makes non-present. Keep a supervisor-only snapshot
 * in kernel BSS before enabling paging; no post-paging code may dereference
 * the bootloader buffer directly. */
static e820_entry_t g_boot_e820[E820_MAX_ENTRIES];
static uint32_t g_boot_e820_count;
static bool g_boot_e820_ready;
static uint8_t g_boot_vesa_info[VESA_BOOTINFO_SIZE];
static uint16_t g_boot_ebda_segment;
static uint16_t g_boot_equipment_word;
static uint16_t g_boot_conventional_kb;
static bool g_boot_legacy_ready;


/* The heap metadata is shared by every CPU. This is a short raw critical
 * section, not a kernel-wide gate: render, VFS and GUI code on other CPUs may
 * continue until they actually need an allocation. */
static kspinlock_t g_mm_heap_lock = KSPINLOCK_INITIALIZER;
static kspinlock_t g_mm_vm_frame_lock = KSPINLOCK_INITIALIZER;
static uint32_t g_vm_frame_start;
static uint32_t g_vm_frame_end;
static uint32_t g_vm_frame_pages;
static uint32_t g_vm_frame_used;
static uint32_t g_vm_frame_reserved_pages;
static uint32_t g_vm_frame_bitmap[MM_VM_POOL_BITMAP_WORDS];

static uint32_t mm_lock(void) {
    return kspin_lock_irqsave(&g_mm_heap_lock);
}

static void mm_unlock(uint32_t flags) {
    kspin_unlock_irqrestore(&g_mm_heap_lock, flags);
}

static bool mm_vm_bit_test(uint32_t page) {
    return (g_vm_frame_bitmap[page >> 5U] & (1U << (page & 31U))) != 0U;
}

static void mm_vm_bit_set(uint32_t page) {
    g_vm_frame_bitmap[page >> 5U] |= 1U << (page & 31U);
}

static void mm_vm_bit_clear(uint32_t page) {
    g_vm_frame_bitmap[page >> 5U] &= ~(1U << (page & 31U));
}

static bool mm_win32_image_reserved(uint32_t address) {
    return address >= MM_WIN32_IMAGE_RESERVE_START &&
           address < MM_WIN32_IMAGE_RESERVE_END;
}

/* The VM pool is represented by one contiguous bitmap. Mark the Win32 image
 * window permanently busy when physical RAM/pool crosses it, effectively
 * turning the bitmap into two allocatable extents without redesigning the
 * frame allocator. Those pages are intentionally never returned as backing. */
static void mm_vm_reserve_win32_image_window(void) {
    uint32_t first, last;
    if (!g_vm_frame_pages || g_vm_frame_start >= g_vm_frame_end) return;
    first = g_vm_frame_start > MM_WIN32_IMAGE_RESERVE_START
        ? g_vm_frame_start : MM_WIN32_IMAGE_RESERVE_START;
    last = g_vm_frame_end < MM_WIN32_IMAGE_RESERVE_END
        ? g_vm_frame_end : MM_WIN32_IMAGE_RESERVE_END;
    first = (first + MM_PAGE_SIZE - 1U) & ~(MM_PAGE_SIZE - 1U);
    last &= ~(MM_PAGE_SIZE - 1U);
    if (first >= last) return;

    for (uint32_t address = first; address < last; address += MM_PAGE_SIZE) {
        uint32_t page = (address - g_vm_frame_start) / MM_PAGE_SIZE;
        if (page >= g_vm_frame_pages || mm_vm_bit_test(page)) continue;
        mm_vm_bit_set(page);
        g_vm_frame_used++;
        g_vm_frame_reserved_pages++;
    }
}

static bool mm_block_valid_locked(const heap_block_t *block) {
    uintptr_t address;
    uintptr_t payload_end;

    if (!block) return false;
    address = (uintptr_t)block;
    if (address < heap_start_address ||
        address > (uintptr_t)heap_end_address - sizeof(*block)) return false;
    if (block->magic != HEAP_MAGIC ||
        block->size > (size_t)((uintptr_t)heap_end_address - address -
                              sizeof(*block))) return false;
    payload_end = address + sizeof(*block) + block->size;
    if (block->next) {
        if ((uintptr_t)block->next != payload_end ||
            payload_end > (uintptr_t)heap_end_address - sizeof(*block) ||
            block->next->prev != block) return false;
    } else if (payload_end != (uintptr_t)heap_end_address) {
        return false;
    }
    return true;
}

static heap_block_t *mm_find_block_locked(const void *ptr) {
    heap_block_t *block;
    uintptr_t target;
    uint32_t limit;

    if (!ptr) return NULL;
    target = (uintptr_t)ptr;
    if (target < heap_start_address + sizeof(heap_block_t) ||
        target >= (uintptr_t)heap_end_address) return NULL;
    limit = (uint32_t)(mm_heap_size() / sizeof(heap_block_t)) + 1U;
    block = heap_head;
    while (block && limit--) {
        if (!mm_block_valid_locked(block)) return NULL;
        if ((uintptr_t)block + sizeof(*block) == target) return block;
        block = block->next;
    }
    return NULL;
}

static void mm_dump_locked(void);

static uint8_t mm_boot_read8(uint32_t address) {
    uint8_t value;
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)address;
    __asm__ volatile ("movb (%1), %0" : "=r"(value) : "r"(p) : "memory");
    return value;
}

static uint16_t mm_boot_read16(uint32_t address) {
    uint16_t value;
    volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)address;
    __asm__ volatile ("movw (%1), %0" : "=r"(value) : "r"(p) : "memory");
    return value;
}

static uint32_t mm_boot_read32(uint32_t address) {
    uint32_t value;
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)address;
    __asm__ volatile ("movl (%1), %0" : "=r"(value) : "r"(p) : "memory");
    return value;
}

static void mm_read_e820_entry(uint32_t index, e820_entry_t *entry) {
    uint32_t offset;
    if (!entry) return;
    if (g_boot_e820_ready) {
        if (index < g_boot_e820_count) *entry = g_boot_e820[index];
        else kmemset(entry, 0, sizeof(*entry));
        return;
    }
    offset = E820_ENTRY_ADDR + (index * (uint32_t)sizeof(e820_entry_t));
    entry->base_low = mm_boot_read32(offset);
    entry->base_high = mm_boot_read32(offset + 4U);
    entry->length_low = mm_boot_read32(offset + 8U);
    entry->length_high = mm_boot_read32(offset + 12U);
    entry->type = mm_boot_read32(offset + 16U);
}

void mm_boot_snapshot(void) {
    uint32_t count;

    if (g_boot_e820_ready && g_boot_legacy_ready) return;

    /* All three structures live in physical page zero. Copy every piece of
     * firmware/bootloader state used by the kernel before paging makes that
     * page non-present. Preserving only E820 was insufficient: graphics still
     * consulted 0x700 and the BIOS equipment word at 0x410. */
    count = mm_boot_read16(E820_MAP_ADDR);
    if (count > E820_MAX_ENTRIES) count = E820_MAX_ENTRIES;
    for (uint32_t i = 0U; i < count; i++)
        mm_read_e820_entry(i, &g_boot_e820[i]);
    g_boot_e820_count = count;

    g_boot_ebda_segment = mm_boot_read16(BDA_EBDA_SEGMENT_ADDR);
    g_boot_equipment_word = mm_boot_read16(BDA_EQUIPMENT_WORD_ADDR);
    g_boot_conventional_kb = mm_boot_read16(BDA_CONVENTIONAL_KB_ADDR);
    for (uint32_t i = 0U; i < VESA_BOOTINFO_SIZE; i++)
        g_boot_vesa_info[i] = mm_boot_read8(VESA_BOOTINFO_ADDR + i);

    /* Publish readiness last so no future SMP reader can observe a partially
     * copied snapshot. BSP boot is still single-CPU here, but the ordering is
     * part of the interface. */
    __asm__ volatile ("" : : : "memory");
    g_boot_legacy_ready = true;
    g_boot_e820_ready = true;
}

uint16_t mm_boot_ebda_segment(void) {
    if (g_boot_legacy_ready) return g_boot_ebda_segment;
    if (paging_is_enabled()) return 0U;
    return mm_boot_read16(BDA_EBDA_SEGMENT_ADDR);
}

uint16_t mm_boot_equipment_word(void) {
    if (g_boot_legacy_ready) return g_boot_equipment_word;
    if (paging_is_enabled()) return 0U;
    return mm_boot_read16(BDA_EQUIPMENT_WORD_ADDR);
}

uint16_t mm_boot_conventional_kb(void) {
    if (g_boot_legacy_ready) return g_boot_conventional_kb;
    if (paging_is_enabled()) return 0U;
    return mm_boot_read16(BDA_CONVENTIONAL_KB_ADDR);
}

uint8_t mm_boot_vesa_read8(uint32_t offset) {
    if (offset >= VESA_BOOTINFO_SIZE) return 0U;
    if (g_boot_legacy_ready) return g_boot_vesa_info[offset];
    if (paging_is_enabled()) return 0U;
    return mm_boot_read8(VESA_BOOTINFO_ADDR + offset);
}

uint16_t mm_boot_vesa_read16(uint32_t offset) {
    uint16_t value;
    if (offset > VESA_BOOTINFO_SIZE - 2U) return 0U;
    value = (uint16_t)mm_boot_vesa_read8(offset);
    value |= (uint16_t)((uint16_t)mm_boot_vesa_read8(offset + 1U) << 8);
    return value;
}

uint32_t mm_boot_vesa_read32(uint32_t offset) {
    uint32_t value;
    if (offset > VESA_BOOTINFO_SIZE - 4U) return 0U;
    value = (uint32_t)mm_boot_vesa_read8(offset);
    value |= (uint32_t)mm_boot_vesa_read8(offset + 1U) << 8;
    value |= (uint32_t)mm_boot_vesa_read8(offset + 2U) << 16;
    value |= (uint32_t)mm_boot_vesa_read8(offset + 3U) << 24;
    return value;
}

static uint32_t mm_add_clamped(uint32_t left, uint32_t right) {
    if (U32_MAX_VALUE - left < right) return U32_MAX_VALUE;
    return left + right;
}

static uint32_t mm_display_size_value(uint32_t bytes, const char **unit) {
    if (bytes > MEMORY_DISPLAY_MB_THRESHOLD) {
        if (unit) *unit = "MB";
        return (bytes + ((1024U * 1024U) - 1U)) / (1024U * 1024U);
    }
    if (unit) *unit = "KB";
    return (bytes + 1023U) / 1024U;
}

static uint32_t mm_align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static bool mm_entry_range(const e820_entry_t *entry, uint32_t *start_out,
                           uint32_t *end_out) {
    uint32_t start;
    uint32_t length;
    uint32_t end;

    if (!entry || entry->type != 1U || entry->base_high != 0U) return false;
    start = entry->base_low;
    length = entry->length_low;
    if (!length) return false;
    if (entry->length_high != 0U || start + length < start)
        end = U32_MAX_VALUE;
    else
        end = start + length;
    if (start_out) *start_out = start;
    if (end_out) *end_out = end;
    return end > start;
}

static uint32_t mm_contiguous_usable_end(uint32_t address, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        e820_entry_t entry;
        uint32_t start;
        uint32_t end;
        mm_read_e820_entry(i, &entry);
        if (!mm_entry_range(&entry, &start, &end)) continue;
        if (address >= start && address < end) return end;
    }
    return 0U;
}

static void mm_detect_system_memory(void) {
    uint32_t count;
    uint32_t total = 0U;
    uint32_t physical_top = 0U;
    uint32_t candidate_start;
    uint32_t candidate_end;
    uint32_t heap_candidate_end;
    uint32_t vm_reserve = 0U;
    uint32_t vm_minimum;

    if (!g_boot_e820_ready) {
        /* Reading 0x500 after paging is enabled would fault before IDT setup
         * and therefore become a triple fault. Keep the conservative built-in
         * defaults rather than touching the now-unmapped null page. */
        if (paging_is_enabled()) {
            kprintf("[MM] ERROR: E820 no fue preservado antes de paging; "
                    "usando memoria por defecto\n");
            count = 0U;
        } else {
            mm_boot_snapshot();
            count = g_boot_e820_count;
        }
    } else {
        count = g_boot_e820_count;
    }
    for (uint32_t i = 0; i < count; i++) {
        e820_entry_t entry;
        uint32_t start;
        uint32_t end;
        uint32_t length;

        mm_read_e820_entry(i, &entry);
        if (!mm_entry_range(&entry, &start, &end)) continue;
        length = end - start;
        total = mm_add_clamped(total, length);
        if (end > physical_top) physical_top = end;
    }

    if (total != 0U) system_total_bytes = total;
    if (physical_top != 0U) system_physical_top = physical_top;
    if (system_physical_top > HEAP_LIMIT_MAX)
        system_physical_top = HEAP_LIMIT_MAX;
    system_physical_top &= ~(MM_PAGE_SIZE - 1U);
    kprintf("[MM] E820: entradas=%u util=%u KiB top=%u KiB\n",
            count, system_total_bytes / 1024U,
            system_physical_top / 1024U);
    /* Below 96 MiB prioritize the native desktop over an unused physical
     * fixed-image window. Private Linux mappings still reject heap overlap. */
    candidate_start = system_physical_top < 95U * 1024U * 1024U
        ? HEAP_LOW_START : HEAP_ALLOC_START;
    candidate_end = mm_contiguous_usable_end(candidate_start, count);

    if (!candidate_end) candidate_end = system_physical_top;
    if (candidate_end > HEAP_LIMIT_MAX) candidate_end = HEAP_LIMIT_MAX;
    candidate_start = mm_align_up(candidate_start, MM_PAGE_SIZE);
    candidate_end &= ~(MM_PAGE_SIZE - 1U);
    heap_candidate_end = candidate_end;

    g_vm_frame_start = 0U;
    g_vm_frame_end = 0U;
    g_vm_frame_pages = 0U;
    g_vm_frame_used = 0U;
    g_vm_frame_reserved_pages = 0U;
    kmemset(g_vm_frame_bitmap, 0, sizeof(g_vm_frame_bitmap));

    /* Carve the frame pool from the top of the contiguous E820 region. At
     * 1 GiB Wine's concurrent services exhaust a half-span VM pool during
     * fork; at that size reserve three quarters while retaining roughly
     * 250 MiB for the identity heap. Smaller machines retain the old split. */
    vm_minimum = system_physical_top < 31U * 1024U * 1024U
        ? 1024U * 1024U : (system_physical_top < 47U * 1024U * 1024U
        ? 2U * 1024U * 1024U : MM_VM_POOL_MIN_BYTES);
    if (system_physical_top >= MM_VM_POOL_MIN_RAM &&
        candidate_end > candidate_start + vm_minimum + MM_PAGE_SIZE) {
        uint32_t span = candidate_end - candidate_start;
        uint32_t divisor = system_physical_top >= (128U * 1024U * 1024U)
            ? 2U : 4U;
        vm_reserve = system_physical_top >= (768U * 1024U * 1024U)
            ? span - span / 4U : span / divisor;
        /* Native 32 MiB desktops use the heap for application images; only
         * page tables and explicit private mappings need this frame pool. */
        if (system_physical_top < 47U * 1024U * 1024U) vm_reserve = vm_minimum;
        if (vm_reserve < vm_minimum) vm_reserve = vm_minimum;
        if (vm_reserve > MM_VM_POOL_MAX_BYTES) vm_reserve = MM_VM_POOL_MAX_BYTES;
        vm_reserve &= ~(MM_PAGE_SIZE - 1U);

        /* 0x08000000-0x09ffffff is intentionally kept free as the Linux/i386
         * ET_EXEC virtual window. If the top-of-RAM pool would overlap that
         * hole, place the pool immediately below it instead. */
        if (candidate_end > MM_LINUX_EXEC_RESERVE_START &&
            candidate_end <= MM_LINUX_EXEC_RESERVE_END) {
            heap_candidate_end = MM_LINUX_EXEC_RESERVE_START;
            if (heap_candidate_end > candidate_start + vm_reserve) {
                g_vm_frame_end = heap_candidate_end;
                g_vm_frame_start = heap_candidate_end - vm_reserve;
            }
        } else {
            uint32_t proposed_start = candidate_end - vm_reserve;
            if (candidate_end > MM_LINUX_EXEC_RESERVE_END &&
                proposed_start < MM_LINUX_EXEC_RESERVE_END)
                proposed_start = MM_LINUX_EXEC_RESERVE_END;
            if (proposed_start > candidate_start + MM_PAGE_SIZE) {
                g_vm_frame_start = proposed_start;
                g_vm_frame_end = candidate_end;
            }
        }
        if (g_vm_frame_end > g_vm_frame_start &&
            g_vm_frame_end - g_vm_frame_start >= vm_minimum) {
            g_vm_frame_pages = (g_vm_frame_end - g_vm_frame_start) / MM_PAGE_SIZE;
            if (g_vm_frame_pages > MM_VM_POOL_MAX_PAGES)
                g_vm_frame_pages = MM_VM_POOL_MAX_PAGES;
            g_vm_frame_end = g_vm_frame_start + g_vm_frame_pages * MM_PAGE_SIZE;
            heap_candidate_end = g_vm_frame_start;
        } else {
            g_vm_frame_start = 0U;
            g_vm_frame_end = 0U;
            g_vm_frame_pages = 0U;
            vm_reserve = 0U;
        }
    }

    /* Large-memory configurations can place the physical VM pool across
     * 0x30000000. Keep that range free of all kernel backing so a process CR3
     * may replace the inherited identity PTEs with Wine's fixed image pages. */
    mm_vm_reserve_win32_image_window();

    /* Deje al menos una pagina para metadata y allocations pequeñas. */
    if (heap_candidate_end <= candidate_start + MM_PAGE_SIZE) {
        heap_start_address = candidate_start;
        heap_end_address = candidate_start;
        g_vm_frame_start = g_vm_frame_end = g_vm_frame_pages = 0U;
    } else {
        heap_start_address = candidate_start;
        heap_end_address = heap_candidate_end;
    }

    /* Free VM frames are available RAM, not permanently occupied memory.
     * Image-window bits remain reserved and are excluded from capacity. */
    uint32_t managed = (uint32_t)mm_heap_size() +
        (g_vm_frame_pages - g_vm_frame_reserved_pages) * MM_PAGE_SIZE;
    system_reserved_bytes = system_total_bytes > managed
        ? system_total_bytes - managed : 0U;
}

uint32_t mm_heap_start(void) { return heap_start_address; }
uint32_t mm_heap_end(void) { return heap_end_address; }
bool mm_firmware_range_valid(uint32_t address,uint32_t length) {
    if(!length || address<0x1000U || address+length<address)return false;
    uint32_t limit=(system_physical_top+0x3FFFFFU)&~0x3FFFFFU;
    if(address+length>limit)return false;
    for(uint32_t i=0;i<g_boot_e820_count;i++) {
        const e820_entry_t *e=&g_boot_e820[i];
        if(e->base_high || e->length_high || e->type<1U || e->type>4U)continue;
        if(address>=e->base_low && address-e->base_low<=e->length_low &&
           length<=e->length_low-(address-e->base_low))return true;
    }
    return false;
}

uint32_t mm_physical_top(void) { return system_physical_top; }
uint32_t mm_total_bytes(void) { return system_total_bytes; }


size_t mm_heap_size(void) {
    return heap_end_address > heap_start_address ? heap_end_address - heap_start_address : 0U;
}

bool mm_vm_frame_alloc(uint32_t page_count, uint32_t *physical_out) {
    uint32_t flags;
    uint32_t run = 0U;
    uint32_t run_start = 0U;
    if (physical_out) *physical_out = 0U;
    if (!physical_out || !page_count || page_count > g_vm_frame_pages)
        return false;

    flags = kspin_lock_irqsave(&g_mm_vm_frame_lock);
    if (page_count > g_vm_frame_pages - g_vm_frame_used) {
        kspin_unlock_irqrestore(&g_mm_vm_frame_lock, flags);
        return false;
    }
    for (uint32_t page = 0U; page < g_vm_frame_pages; page++) {
        if (!mm_vm_bit_test(page)) {
            if (!run) run_start = page;
            run++;
            if (run == page_count) {
                for (uint32_t i = 0U; i < page_count; i++)
                    mm_vm_bit_set(run_start + i);
                g_vm_frame_used += page_count;
                *physical_out = g_vm_frame_start + run_start * MM_PAGE_SIZE;
                kspin_unlock_irqrestore(&g_mm_vm_frame_lock, flags);
                kmemset((void *)(uintptr_t)*physical_out, 0,
                        page_count * MM_PAGE_SIZE);
                return true;
            }
        } else {
            run = 0U;
        }
    }
    kspin_unlock_irqrestore(&g_mm_vm_frame_lock, flags);
    return false;
}

void mm_vm_frame_free(uint32_t physical_address, uint32_t page_count) {
    uint32_t flags;
    uint32_t first;
    if (!page_count || !g_vm_frame_pages ||
        physical_address < g_vm_frame_start ||
        physical_address >= g_vm_frame_end ||
        (physical_address & (MM_PAGE_SIZE - 1U))) return;
    first = (physical_address - g_vm_frame_start) / MM_PAGE_SIZE;
    if (first + page_count < first || first + page_count > g_vm_frame_pages)
        return;
    flags = kspin_lock_irqsave(&g_mm_vm_frame_lock);
    for (uint32_t i = 0U; i < page_count; i++) {
        uint32_t address = g_vm_frame_start + (first + i) * MM_PAGE_SIZE;
        /* Reserved image-window bits are permanent ownership markers, not
         * allocations. Never let an erroneous/free-overlapping range expose
         * them to the allocator again. */
        if (mm_win32_image_reserved(address)) continue;
        if (!mm_vm_bit_test(first + i)) continue;
        mm_vm_bit_clear(first + i);
        if (g_vm_frame_used) g_vm_frame_used--;
    }
    kspin_unlock_irqrestore(&g_mm_vm_frame_lock, flags);
}

uint32_t mm_vm_frame_total_pages(void) { return g_vm_frame_pages; }

static struct { uint32_t start, end; } g_kernel_mmio[8];
static uint32_t g_kernel_mmio_count;

bool mm_register_kernel_mmio(uint32_t address, uint32_t length) {
    uint32_t end = address + length;
    if (!length || end <= address || g_kernel_mmio_count >= 8U) return false;
    g_kernel_mmio[g_kernel_mmio_count].start = address;
    g_kernel_mmio[g_kernel_mmio_count].end = end;
    g_kernel_mmio_count++;
    return true;
}

bool mm_kernel_identity_overlaps(uint32_t address, uint32_t length) {
    uint32_t end = address + length;
    if (!length || end < address) return true;
    for (uint32_t i = 0; i < g_kernel_mmio_count; i++)
        if (address < g_kernel_mmio[i].end && g_kernel_mmio[i].start < end)
            return true;
    if (address < g_vm_frame_end && g_vm_frame_start < end &&
        g_vm_frame_start < g_vm_frame_end) {
        uint32_t first = address > g_vm_frame_start ? address : g_vm_frame_start;
        uint32_t last = end < g_vm_frame_end ? end : g_vm_frame_end;
        /* The Win32 image hole is physically unused by the VM allocator. Its
         * inherited identity PTE is therefore safe to shadow in one process.
         * Any overlap with the surrounding live frame pool remains forbidden. */
        if (first < MM_WIN32_IMAGE_RESERVE_START ||
            last > MM_WIN32_IMAGE_RESERVE_END) return true;
    }
    if (address >= heap_end_address || end <= heap_start_address) return false;
    /* The ET_EXEC hole is excluded from allocations, except for the heap
     * list header in its first page. Keep that header and the tail header
     * at RESERVE_END accessible to the allocator under every process CR3. */
    uint32_t first = address > heap_start_address ? address : heap_start_address;
    uint32_t last = end < heap_end_address ? end : heap_end_address;
    if (first >= last) return false;
    return first < MM_LINUX_EXEC_RESERVE_START + MM_PAGE_SIZE ||
           last > MM_LINUX_EXEC_RESERVE_END;
}

uint32_t mm_vm_frame_free_pages(void) {
    uint32_t flags = kspin_lock_irqsave(&g_mm_vm_frame_lock);
    uint32_t free_pages = g_vm_frame_pages >= g_vm_frame_used
        ? g_vm_frame_pages - g_vm_frame_used : 0U;
    kspin_unlock_irqrestore(&g_mm_vm_frame_lock, flags);
    return free_pages;
}

/* WINE-HEAP-ROOT-WRITE-TRACE v3
 *
 * Wine has repeatedly exposed the allocator root at heap_start_address with
 * magic/size zeroed.  The v2 per-process supervisor guard proved that a plain
 * CPL3 store is not required: the corruption still occurs without a user-mode
 * page fault.  Intercept the two bulk-write primitives used throughout the
 * kernel so the first overwrite can identify its caller.
 *
 * The header is restored after the write.  This is intentionally diagnostic:
 * it prevents one bad bulk copy/clear from destroying the allocator while
 * preserving the rest of the destination write, allowing wineboot to advance
 * far enough to reveal the responsible subsystem.  Direct scalar stores are
 * unaffected and will still be caught by kmalloc's existing corruption check.
 */
static uint32_t g_heap_root_write_reports = 0U;

static bool mm_heap_root_write_overlap(const void *dst, size_t n) {
    uint32_t start, end, root, root_end;
    if (!heap_head || !dst || !n || heap_head->magic != HEAP_MAGIC) return false;
    start = (uint32_t)(uintptr_t)dst;
    if (n > (size_t)(U32_MAX_VALUE - start)) end = U32_MAX_VALUE;
    else end = start + (uint32_t)n;
    root = (uint32_t)(uintptr_t)heap_head;
    root_end = root + (uint32_t)sizeof(heap_block_t);
    return start < root_end && root < end;
}

static void mm_heap_root_write_report(const char *op, const void *dst,
                                      size_t n, uint32_t caller) {
    uint32_t report = g_heap_root_write_reports++;
    if (report < 32U) {
        kprintf("[MM:ROOTWRITE] op=%s dst=%x len=%u caller=%x pid=%u hit=%u\n",
                op, (uint32_t)(uintptr_t)dst, (uint32_t)n, caller,
                task_current_process_id(), report + 1U);
    } else if (report == 32U) {
        kprintf("[MM:ROOTWRITE] se ocultan repeticiones posteriores\n");
    }
}

void *kmemset(void *dst, int c, size_t n) {
    void *original = dst;
    uint8_t *bytes = (uint8_t *)dst;
    uint8_t value = (uint8_t)c;
    size_t original_n = n;
    bool protect_root = mm_heap_root_write_overlap(dst, n);
    uint32_t root_magic = 0U, root_size = 0U, root_owner = 0U;
    bool root_free = false;
    heap_block_t *root_next = NULL, *root_prev = NULL;

    if (protect_root) {
        uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
        root_magic = heap_head->magic;
        root_size = (uint32_t)heap_head->size;
        root_free = heap_head->free;
        root_owner = heap_head->owner_process_id;
        root_next = heap_head->next;
        root_prev = heap_head->prev;
        mm_heap_root_write_report("memset", dst, original_n, caller);
    }

    while (n && ((uintptr_t)bytes & 3U)) {
        *bytes++ = value;
        n--;
    }
    if (n >= 32U) {
        uint32_t dword = (uint32_t)value * 0x01010101U;
        uint32_t count = (uint32_t)(n >> 2);
        uint32_t *out = (uint32_t *)(void *)bytes;
        __asm__ volatile ("cld; rep stosl"
                          : "+D"(out), "+c"(count)
                          : "a"(dword) : "memory");
        bytes = (uint8_t *)(void *)out;
        n &= 3U;
    }
    while (n--) *bytes++ = value;

    if (protect_root) {
        heap_head->magic = root_magic;
        heap_head->size = (size_t)root_size;
        heap_head->free = root_free;
        heap_head->owner_process_id = root_owner;
        heap_head->next = root_next;
        heap_head->prev = root_prev;
    }
    return original;
}

void *kmemcpy(void *dst, const void *src, size_t n) {
    void *original = dst;
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t original_n = n;
    bool protect_root = mm_heap_root_write_overlap(dst, n);
    uint32_t root_magic = 0U, root_size = 0U, root_owner = 0U;
    bool root_free = false;
    heap_block_t *root_next = NULL, *root_prev = NULL;

    if (protect_root) {
        uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
        root_magic = heap_head->magic;
        root_size = (uint32_t)heap_head->size;
        root_free = heap_head->free;
        root_owner = heap_head->owner_process_id;
        root_next = heap_head->next;
        root_prev = heap_head->prev;
        mm_heap_root_write_report("memcpy", dst, original_n, caller);
    }

    /* REP MOVSL es especialmente eficaz para superficies y caches grandes en
     * P6 (Pentium Pro/II/III). Para tamaños pequeños se evita su coste fijo. */
    if ((((uintptr_t)d | (uintptr_t)s) & 3U) == 0U && n >= 32U) {
        uint32_t count = (uint32_t)(n >> 2);
        uint32_t *out = (uint32_t *)(void *)d;
        const uint32_t *in = (const uint32_t *)(const void *)s;
        __asm__ volatile ("cld; rep movsl"
                          : "+D"(out), "+S"(in), "+c"(count)
                          : : "memory");
        d = (uint8_t *)(void *)out;
        s = (const uint8_t *)(const void *)in;
        n &= 3U;
    }
    while (n--) *d++ = *s++;

    if (protect_root) {
        heap_head->magic = root_magic;
        heap_head->size = (size_t)root_size;
        heap_head->free = root_free;
        heap_head->owner_process_id = root_owner;
        heap_head->next = root_next;
        heap_head->prev = root_prev;
    }
    return original;
}

int kmemcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return (int)(*pa - *pb);
        pa++;
        pb++;
    }
    return 0;
}

size_t kstrlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

int kstrcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int kstrncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && (*a == *b)) { a++; b++; }
    if (n == (size_t)-1) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *kstrcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *kstrncpy(char *dst, const char *src, size_t n) {
    size_t i=0;
    while(i<n && src[i]){dst[i]=src[i];i++;}
    while(i<n)dst[i++]='\0';
    return dst;
}

char *kstrcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) {}
    return dst;
}

void mm_init(void) {
    const char *unit;
    uint32_t ram_value;

    mm_detect_system_memory();
    heap_exec_reservation=NULL;
    heap_head = (heap_block_t *)(uintptr_t)heap_start_address;
    if (mm_heap_size() <= sizeof(heap_block_t)) {
        kprintf("[MM] ERROR: no hay region contigua para el heap\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    heap_head->magic = HEAP_MAGIC;
    heap_head->size = mm_heap_size() - sizeof(heap_block_t);
    heap_head->free = true;
    heap_head->owner_process_id = 0U;
    heap_head->next = NULL;
    heap_head->prev = NULL;
    if(heap_start_address+sizeof(heap_block_t)+16U<
           MM_LINUX_EXEC_RESERVE_START &&
       heap_end_address>MM_LINUX_EXEC_RESERVE_START+sizeof(heap_block_t)) {
        uint32_t reserved_end=heap_end_address<MM_LINUX_EXEC_RESERVE_END
            ? heap_end_address : MM_LINUX_EXEC_RESERVE_END;
        heap_block_t *reserved=(heap_block_t *)(uintptr_t)
            MM_LINUX_EXEC_RESERVE_START;
        heap_head->size=MM_LINUX_EXEC_RESERVE_START-heap_start_address-
            sizeof(heap_block_t);
        heap_head->next=reserved;
        reserved->magic=HEAP_MAGIC;
        reserved->size=reserved_end-MM_LINUX_EXEC_RESERVE_START-
            sizeof(heap_block_t);
        reserved->free=false;
        heap_exec_reservation=reserved;
        /* Unused identity space is unavailable capacity, not allocated
         * payload. The list header itself remains resident kernel data. */
        system_reserved_bytes+=reserved->size;
        reserved->owner_process_id=0U;
        reserved->prev=heap_head;
        reserved->next=NULL;
        if(reserved_end+sizeof(heap_block_t)+16U<heap_end_address) {
            heap_block_t *tail=(heap_block_t *)(uintptr_t)reserved_end;
            reserved->next=tail;
            tail->magic=HEAP_MAGIC;
            tail->size=heap_end_address-reserved_end-sizeof(heap_block_t);
            tail->free=true;
            tail->owner_process_id=0U;
            tail->prev=reserved;
            tail->next=NULL;
        }
        kprintf("  [MM] Ventana ET_EXEC reservada: %x - %x\n",
                MM_LINUX_EXEC_RESERVE_START,reserved_end);
    }
    ram_value = mm_display_size_value(system_total_bytes, &unit);
    kprintf("  [MM] RAM usable: %u %s\n", ram_value, unit);
    kprintf("  [MM] Heap dinamico: %x - %x (%u KB)\n", mm_heap_start(),
            mm_heap_end(), (uint32_t)(mm_heap_size() / 1024U));
    if (g_vm_frame_pages) {
        kprintf("  [MM] VM frames: %x - %x (%u KiB, %u paginas)\n",
                g_vm_frame_start, g_vm_frame_end,
                (g_vm_frame_pages * MM_PAGE_SIZE) / 1024U, g_vm_frame_pages);
        if (g_vm_frame_reserved_pages)
            kprintf("  [MM] Win32 fixed-image hole: %x - %x (%u KiB reservados)\n",
                    MM_WIN32_IMAGE_RESERVE_START, MM_WIN32_IMAGE_RESERVE_END,
                    (g_vm_frame_reserved_pages * MM_PAGE_SIZE) / 1024U);
    }
    if (mm_heap_start() >= PE_FIXED_VIEW_END)
        kprintf("  [MM] Vista PE fija: %x - %x\n",
                PE_FIXED_VIEW_START, PE_FIXED_VIEW_END);
    else
        kprintf("  [MM] Vista PE fija desactivada: heap reducido ocupa %x-%x\n",
                PE_FIXED_VIEW_START, PE_FIXED_VIEW_END);
    /* BLES_WINE_PREFERRED_VIEW_FIX_20260723 */
    if (paging_is_enabled() &&
        !paging_configure_user_layout(mm_heap_start(), mm_heap_end())) {
        kprintf("[PAGING] ERROR: no se pudo publicar el heap Ring 3\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
}

/* Optional resident caches can be discarded without allocating or waiting.
 * Calls happen outside the heap lock and may decline if their lock is busy. */
extern uint32_t bk_graphics_cache_reclaim(void) __attribute__((weak));
extern uint32_t gui_content_staging_reclaim(void) __attribute__((weak));
void *kmalloc(size_t size) {
    uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
    uint32_t flags;
    heap_block_t *blk;
    bool corrupt = false;
    bool reclaimed = false;

    if (!size || size > U32_MAX_VALUE - 3U) return NULL;
    size = ALIGN4(size);
retry:
    flags = mm_lock();

    blk = heap_head;
    while (blk) {
        uint32_t address = (uint32_t)(uintptr_t)blk;
        if (!mm_block_valid_locked(blk)) {
            kprintf("[MM] CORRUPCION heap bloque=%x magic=%x size=%u caller=%x\n",
                    address,
                    address >= heap_start_address &&
                    address <= heap_end_address - sizeof(heap_block_t)
                        ? blk->magic : 0U,
                    address >= heap_start_address &&
                    address <= heap_end_address - sizeof(heap_block_t)
                        ? (uint32_t)blk->size : 0U,
                    caller);
            corrupt = true;
            break;
        }
        if (blk->free && blk->size >= size) {
            if (blk->size >= size + sizeof(heap_block_t) + 16U) {
                uint8_t *raw = (uint8_t *)blk;
                heap_block_t *split = (heap_block_t *)(
                    raw + sizeof(heap_block_t) + size);
                split->magic = HEAP_MAGIC;
                split->size = blk->size - size - sizeof(heap_block_t);
                split->free = true;
                split->owner_process_id = 0U;
                split->next = blk->next;
                split->prev = blk;
                if (blk->next) blk->next->prev = split;
                blk->next = split;
                blk->size = size;
            }
            blk->free = false;
            blk->subsystem = 0;
            blk->owner_process_id = task_current_process_id();
            void *result = (void *)((uint8_t *)blk + sizeof(heap_block_t));
            mm_unlock(flags);
            return result;
        }
        blk = blk->next;
    }

    if(!corrupt && !reclaimed) {
        mm_unlock(flags);reclaimed=true;
        uint32_t recovered=0;
        if(bk_graphics_cache_reclaim)recovered+=bk_graphics_cache_reclaim();
        if(gui_content_staging_reclaim)recovered+=gui_content_staging_reclaim();
        if(recovered)goto retry;
        flags=mm_lock();
    }
    if (!corrupt) {
        kprintf("[MM] ERROR: sin memoria! pedido=%u caller=%x pid=%u\n",
                size, caller, task_current_process_id());
    }
    mm_dump_locked();
    mm_unlock(flags);
    return NULL;
}

void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) kmemset(ptr, 0, size);
    return ptr;
}

void *mm_alloc_guarded_stack(uint32_t requested_size, bool user_stack,
                             void **allocation_out,
                             uint32_t *usable_size_out) {
    uint32_t usable_size;
    uint32_t total_size;
    uint8_t *raw;
    uint8_t *stack;
    uint8_t *lower_guard;
    uint8_t *upper_guard;

    if (allocation_out) *allocation_out = NULL;
    if (usable_size_out) *usable_size_out = 0U;
    if (requested_size < 16384U) requested_size = 16384U;
    if (requested_size > 0xFFFFFFFFU - (PAGING_PAGE_SIZE - 1U))
        return NULL;
    usable_size = (requested_size + PAGING_PAGE_SIZE - 1U) &
                  ~(PAGING_PAGE_SIZE - 1U);
    if (usable_size > 0xFFFFFFFFU - PAGING_PAGE_SIZE * 3U) return NULL;
    total_size = usable_size + PAGING_PAGE_SIZE * 3U;
    raw = (uint8_t *)kmalloc(total_size);
    if (!raw) return NULL;
    (void)mm_set_allocation_owner(raw, 0U);
    (void)mm_set_allocation_subsystem(raw,MM_SUBSYSTEM_PROCESS);

    stack = (uint8_t *)(((uintptr_t)raw + PAGING_PAGE_SIZE * 2U - 1U) &
                        ~(uintptr_t)(PAGING_PAGE_SIZE - 1U));
    lower_guard = stack - PAGING_PAGE_SIZE;
    upper_guard = stack + usable_size;
    if (lower_guard < raw ||
        upper_guard + PAGING_PAGE_SIZE > raw + total_size) {
        kfree(raw);
        return NULL;
    }

    /* This allocator also backs zero-initialized IPC and VFS structures. */
    kmemset(stack, 0, usable_size);
    if (!paging_set_range((uint32_t)(uintptr_t)lower_guard,
                          PAGING_PAGE_SIZE, false, false, false) ||
        !paging_set_range((uint32_t)(uintptr_t)stack, usable_size,
                          true, user_stack, true) ||
        !paging_set_range((uint32_t)(uintptr_t)upper_guard,
                          PAGING_PAGE_SIZE, false, false, false)) {
        (void)paging_set_range((uint32_t)(uintptr_t)lower_guard,
            usable_size + PAGING_PAGE_SIZE * 2U, true, true, true);
        kfree(raw);
        return NULL;
    }

    if (allocation_out) *allocation_out = raw;
    if (usable_size_out) *usable_size_out = usable_size;
    return stack;
}

void mm_free_guarded_stack(void *allocation, void *stack,
                           uint32_t usable_size) {
    uint32_t rounded;
    uint32_t lower;
    if (!allocation || !stack || !usable_size) return;
    rounded = (usable_size + PAGING_PAGE_SIZE - 1U) &
              ~(PAGING_PAGE_SIZE - 1U);
    lower = (uint32_t)(uintptr_t)stack - PAGING_PAGE_SIZE;
    /* Return the pages to the heap's default Ring-3 mapping before another
     * allocation reuses them. The shared TLB generation prevents stale guard
     * entries on a CPU that previously ran this task. */
    (void)paging_set_range(lower, rounded + PAGING_PAGE_SIZE * 2U,
                           true, true, true);
    kfree(allocation);
}

/* Forked identity-addressed ELF/stack views must reserve their original heap
 * addresses until the last descendant stops mapping them. */
static struct { void *ptr; uint32_t refs; bool pending; } address_pins[128];

/* Supervisor capability records for allocations explicitly published by the
 * trusted GUI service. A shared owner=0 header alone never grants free/realloc
 * authority. Records disappear before an address can be reused. */
#define MM_USER_PUBLICATIONS 512U
static struct {void *pointer;uint32_t publisher;} user_publications[MM_USER_PUBLICATIONS];
static bool mm_user_manages_locked(heap_block_t *block,void *ptr,uint32_t pid) {
    if(!block || block->free || !pid)return false;
    if(block->owner_process_id==pid)return true;
    if(block->owner_process_id)return false;
    for(unsigned i=0;i<MM_USER_PUBLICATIONS;i++)
        if(user_publications[i].pointer==ptr && user_publications[i].publisher==pid)return true;
    return false;
}
static void mm_user_forget_locked(void *ptr) {
    for(unsigned i=0;i<MM_USER_PUBLICATIONS;i++)
        if(user_publications[i].pointer==ptr){user_publications[i].pointer=NULL;user_publications[i].publisher=0;break;}
}
bool mm_user_publish(void *ptr,uint32_t pid) {
    uint32_t flags=mm_lock();heap_block_t *block=mm_find_block_locked(ptr);
    bool ok=false;
    if(mm_user_manages_locked(block,ptr,pid)) {
        for(unsigned i=0;i<MM_USER_PUBLICATIONS;i++)
            if(user_publications[i].pointer==ptr && user_publications[i].publisher==pid){ok=true;break;}
        if(!ok && block->owner_process_id==pid)for(unsigned i=0;i<MM_USER_PUBLICATIONS;i++)
            if(!user_publications[i].pointer){user_publications[i].pointer=ptr;user_publications[i].publisher=pid;ok=true;break;}
        if(ok)block->owner_process_id=0;
    }
    mm_unlock(flags);return ok;
}
bool mm_user_subsystem(void *ptr,uint8_t subsystem,uint32_t pid) {
    if(subsystem>=MM_SUBSYSTEM_COUNT)return false;
    uint32_t flags=mm_lock();heap_block_t *block=mm_find_block_locked(ptr);
    bool ok=mm_user_manages_locked(block,ptr,pid);
    if(ok)block->subsystem=subsystem;
    mm_unlock(flags);return ok;
}

bool mm_pin_address_allocation(void *ptr) {
    uint32_t flags=mm_lock();
    heap_block_t *blk=mm_find_block_locked(ptr);
    int slot=-1;
    for(unsigned i=0;i<128;i++) {
        if(address_pins[i].ptr==ptr) {slot=i;break;}
        if(!address_pins[i].ptr && slot<0) slot=i;
    }
    bool ok=blk && !blk->free && slot>=0 && address_pins[slot].refs!=U32_MAX_VALUE;
    if(ok) {address_pins[slot].ptr=ptr;address_pins[slot].refs++;}
    mm_unlock(flags);return ok;
}

static bool mm_defer_pinned_free(void *ptr) {
    for(unsigned i=0;i<128;i++) if(address_pins[i].ptr==ptr) {
        address_pins[i].pending=true;return true;
    }
    return false;
}

void mm_unpin_address_allocation(void *ptr) {
    bool release=false;
    uint32_t flags=mm_lock();
    for(unsigned i=0;i<128;i++) if(address_pins[i].ptr==ptr) {
        if(!--address_pins[i].refs) {
            release=address_pins[i].pending;
            kmemset(&address_pins[i],0,sizeof(address_pins[i]));
        }
        break;
    }
    if(release) kfree(ptr);
    mm_unlock(flags);
}

void kfree(void *ptr) {
    uint32_t flags;
    heap_block_t *blk;

    if (!ptr) return;
    flags = mm_lock();
    blk = mm_find_block_locked(ptr);
    if (!blk) {
        kprintf("[MM] ERROR: free invalido ptr=%x caller=%x\n",
                (uint32_t)(uintptr_t)ptr,
                (uint32_t)(uintptr_t)__builtin_return_address(0));
        mm_unlock(flags);
        return;
    }
    if (blk->free) {
        mm_unlock(flags);
        return;
    }

    if(mm_defer_pinned_free(ptr)) {
        blk->owner_process_id=0U;mm_unlock(flags);return;
    }
    mm_user_forget_locked(ptr);
    blk->free = true;
    blk->owner_process_id = 0U;

    if (blk->next && mm_block_valid_locked(blk->next) && blk->next->free) {
        heap_block_t *next = blk->next;
        blk->size += sizeof(heap_block_t) + next->size;
        blk->next = next->next;
        if (blk->next) blk->next->prev = blk;
    }

    if (blk->prev && mm_block_valid_locked(blk->prev) && blk->prev->free) {
        heap_block_t *prev = blk->prev;
        prev->size += sizeof(heap_block_t) + blk->size;
        prev->next = blk->next;
        if (prev->next) prev->next->prev = prev;
    }
    mm_unlock(flags);
}

/* Legacy shared payloads remain accessible, but allocator headers, free
 * storage and another process's private allocations are never API buffers.
 * Explicit private VM mappings are checked separately by the API wrapper. */
uint32_t mm_user_heap_available(uint32_t address,uint32_t pid) {
    if(address<heap_start_address || address>=heap_end_address)return 4096U;
    uint32_t flags=mm_lock(),available=0;
    for(heap_block_t *block=heap_head;block && mm_block_valid_locked(block);block=block->next) {
        uint32_t start=(uint32_t)(uintptr_t)(block+1);
        if(!block->free && block!=heap_exec_reservation && address>=start &&
           address-start<block->size && (!block->owner_process_id || block->owner_process_id==pid)) {
            available=block->size-(address-start);break;
        }
    }
    mm_unlock(flags);return available;
}

bool mm_user_heap_payload(uint32_t address,uint32_t length,uint32_t pid) {
    if(!length)return true;
    if(length>U32_MAX_VALUE-address)return false;
    uint32_t end=address+length;
    if(address>=heap_end_address || end<=heap_start_address)return true;
    uint32_t flags=mm_lock();bool ok=false;
    for(heap_block_t *block=heap_head;block && mm_block_valid_locked(block);block=block->next) {
        uint32_t start=(uint32_t)(uintptr_t)(block+1);
        if(!block->free && block!=heap_exec_reservation && address>=start &&
           end<=start+block->size &&
           (!block->owner_process_id || block->owner_process_id==pid)){ok=true;break;}
    }
    mm_unlock(flags);return ok;
}

bool mm_user_heap_pin(uint32_t address,uint32_t length,uint32_t pid,void **token) {
    *token=NULL;
    if(!length)return true;
    if(length>U32_MAX_VALUE-address)return false;
    uint32_t end=address+length;
    if(address>=heap_end_address || end<=heap_start_address)return true;
    uint32_t flags=mm_lock();bool ok=false;
    for(heap_block_t *block=heap_head;block && mm_block_valid_locked(block);block=block->next) {
        uint32_t start=(uint32_t)(uintptr_t)(block+1);
        if(!block->free && block!=heap_exec_reservation && address>=start &&
           end<=start+block->size && (!block->owner_process_id || block->owner_process_id==pid)) {
            ok=mm_pin_address_allocation(block+1);
            if(ok)*token=block+1;
            break;
        }
    }
    mm_unlock(flags);return ok;
}

bool mm_user_free(void *ptr,uint32_t pid) {
    if(!ptr)return true;
    if(!pid)return false;
    uint32_t flags=mm_lock();heap_block_t *block=mm_find_block_locked(ptr);
    bool ok=mm_user_manages_locked(block,ptr,pid);
    if(ok)kfree(ptr);
    mm_unlock(flags);return ok;
}
void *mm_user_realloc(void *ptr,size_t size,uint32_t pid) {
    if(!pid || size>U32_MAX_VALUE-3U)return NULL;
    if(!ptr)return kmalloc(size);
    uint32_t flags=mm_lock();heap_block_t *block=mm_find_block_locked(ptr);
    if(!mm_user_manages_locked(block,ptr,pid)){mm_unlock(flags);return NULL;}
    if(!size){kfree(ptr);mm_unlock(flags);return NULL;}
    size=ALIGN4(size);
    size_t old_size=block->size;uint8_t subsystem=block->subsystem;
    bool published=block->owner_process_id==0;
    /* These paths cannot allocate or reclaim while the outer heap lock is held. */
    if(size<=old_size || (block->next && mm_block_valid_locked(block->next) &&
       block->next->free && block->next->size+sizeof(heap_block_t)>=size-old_size)) {
        void *result=krealloc(ptr,size);mm_unlock(flags);return result;
    }
    if(!mm_pin_address_allocation(ptr)){mm_unlock(flags);return NULL;}
    mm_unlock(flags);
    void *result=kmalloc(size);
    if(result && published && !mm_user_publish(result,pid)){kfree(result);result=NULL;}
    if(result){
        (void)mm_set_allocation_subsystem(result,subsystem);
        kmemcpy(result,ptr,old_size);
        /* Mark the old allocation pending while its pin still prevents reuse. */
        kfree(ptr);
    }
    mm_unpin_address_allocation(ptr);return result;
}

size_t mm_allocation_size(const void *ptr) {
    uint32_t flags;
    heap_block_t *block;
    size_t size = 0U;

    if (!ptr) return 0U;
    flags = mm_lock();
    block = mm_find_block_locked(ptr);
    if (block && !block->free) size = block->size;
    mm_unlock(flags);
    return size;
}

void *krealloc(void *ptr, size_t new_size) {
    uint32_t flags;
    heap_block_t *block;
    size_t old_size;
    uint32_t owner;
    uint8_t subsystem;
    void *new_ptr;

    if (!ptr) return kmalloc(new_size);
    if (!new_size) { kfree(ptr); return NULL; }
    if (new_size > U32_MAX_VALUE - 3U) return NULL;
    new_size = ALIGN4(new_size);

    flags = mm_lock();
    block = mm_find_block_locked(ptr);
    if (!block || block->free) {
        mm_unlock(flags);
        return NULL;
    }
    old_size = block->size;
    owner = block->owner_process_id;
    subsystem = block->subsystem;
    /* A fork descendant may still map the complete original allocation.
     * Neither reclaim its tail nor move/reuse its identity address. */
    bool pinned = false;
    for (unsigned i = 0U; i < 128U; i++)
        if (address_pins[i].ptr == ptr) { pinned = true; break; }
    if (pinned) {
        mm_unlock(flags);
        return old_size >= new_size ? ptr : NULL;
    }
    /* Reuse adjacent free RAM rather than requiring old + new allocations
     * simultaneously. All metadata updates remain under the heap lock. */
    if (old_size < new_size && block->next &&
        mm_block_valid_locked(block->next) && block->next->free &&
        block->next->size + sizeof(heap_block_t) >= new_size - old_size) {
        heap_block_t *next = block->next;
        block->size += sizeof(heap_block_t) + next->size;
        block->next = next->next;
        if (block->next) block->next->prev = block;
    }
    if (block->size >= new_size) {
        if (block->size - new_size >= sizeof(heap_block_t) + 16U) {
            heap_block_t *tail = (heap_block_t *)((uint8_t *)(block + 1) + new_size);
            tail->magic = HEAP_MAGIC;
            tail->size = block->size - new_size - sizeof(heap_block_t);
            tail->free = true;
            tail->owner_process_id = 0U;
            tail->prev = block;
            tail->next = block->next;
            if (tail->next) tail->next->prev = tail;
            block->next = tail;
            block->size = new_size;
            if (tail->next && mm_block_valid_locked(tail->next) && tail->next->free) {
                heap_block_t *next = tail->next;
                tail->size += sizeof(heap_block_t) + next->size;
                tail->next = next->next;
                if (tail->next) tail->next->prev = tail;
            }
        }
        mm_unlock(flags);
        return ptr;
    }
    mm_unlock(flags);

    new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    (void)mm_set_allocation_owner(new_ptr, owner);
    (void)mm_set_allocation_subsystem(new_ptr,subsystem);
    kmemcpy(new_ptr, ptr, old_size);
    kfree(ptr);
    return new_ptr;
}

void mm_get_info(heap_info_t *info) {
    uint32_t flags;
    heap_block_t *blk;
    size_t total = 0U;
    size_t used = 0U;
    uint32_t blocks = 0U;
    uint32_t free_blocks = 0U;
    uint32_t used_blocks = 0U;

    if (!info) return;
    flags = mm_lock();
    blk = heap_head;
    while (blk && mm_block_valid_locked(blk)) {
        blocks++;
        total += (blk==heap_exec_reservation?0U:blk->size) + sizeof(heap_block_t);
        if (blk->free) free_blocks++; else used_blocks++;
        /* Headers consume RAM even when the payload is free. */
        used += sizeof(heap_block_t);
        if (!blk->free && blk!=heap_exec_reservation) used += blk->size;
        blk = blk->next;
    }

    info->total_bytes = total;
    info->used_bytes = used;
    info->free_bytes = total >= used ? total - used : 0U;
    info->total_blocks = blocks;
    info->free_blocks = free_blocks;
    info->used_blocks = used_blocks;
    mm_unlock(flags);
}

bool mm_set_allocation_owner(void *ptr, uint32_t process_id) {
    uint32_t flags;
    heap_block_t *blk;
    bool result = false;

    if (!ptr) return false;
    flags = mm_lock();
    blk = mm_find_block_locked(ptr);
    if (blk && !blk->free) {
        if(process_id)mm_user_forget_locked(ptr);
        blk->owner_process_id = process_id;
        result = true;
    }
    mm_unlock(flags);
    return result;
}

bool mm_set_allocation_subsystem(void *ptr,uint8_t subsystem) {
    if(subsystem>=8U)return false;
    uint32_t flags=mm_lock();heap_block_t *block=mm_find_block_locked(ptr);
    bool ok=block && !block->free;
    if(ok)block->subsystem=subsystem;
    mm_unlock(flags);return ok;
}

void mm_swap_process_owners(uint32_t first,uint32_t second) {
    uint32_t flags=mm_lock();
    for (heap_block_t *blk=heap_head;blk;blk=blk->next) {
        if (blk->free) continue;
        if (blk->owner_process_id==first) blk->owner_process_id=second;
        else if (blk->owner_process_id==second) blk->owner_process_id=first;
    }
    mm_unlock(flags);
}

size_t mm_get_process_usage(uint32_t process_id) {
    uint32_t flags;
    heap_block_t *blk;
    size_t used = 0U;

    if (!process_id) return 0U;
    flags = mm_lock();
    blk = heap_head;
    while (blk && mm_block_valid_locked(blk)) {
        if (!blk->free && blk->owner_process_id == process_id)
            used += blk->size;
        blk = blk->next;
    }
    mm_unlock(flags);
    return used;
}

void mm_get_process_usage_batch(const uint32_t *process_ids, size_t *usage,
                                uint32_t count) {
    uint32_t flags;
    heap_block_t *blk;

    if (!process_ids || !usage || !count) return;
    for (uint32_t i = 0U; i < count; i++) usage[i] = 0U;

    /* Un solo recorrido del heap para todos los procesos. Process Manager
     * antes recorria la lista completa una vez por fila, manteniendo el lock
     * de memoria durante miles de comparaciones y frenando a 3D Plus. */
    flags = mm_lock();
    blk = heap_head;
    while (blk && mm_block_valid_locked(blk)) {
        if (!blk->free && blk->owner_process_id) {
            for (uint32_t i = 0U; i < count; i++) {
                if (process_ids[i] == blk->owner_process_id) {
                    usage[i] += blk->size;
                    break;
                }
            }
        }
        blk = blk->next;
    }
    mm_unlock(flags);
}

size_t mm_release_process(uint32_t process_id) {
    uint32_t flags;
    heap_block_t *blk;
    size_t released = 0U;

    if (!process_id) return 0U;
    flags = mm_lock();
    for (blk = heap_head; blk && mm_block_valid_locked(blk); blk = blk->next) {
        if (blk->free || blk->owner_process_id != process_id) continue;
        if(mm_defer_pinned_free((void *)(blk+1))) {blk->owner_process_id=0U;continue;}
        released += blk->size;
        mm_user_forget_locked((void *)(blk+1));
        blk->free = true;
        blk->owner_process_id = 0U;
    }
    blk = heap_head;
    while (blk && mm_block_valid_locked(blk) && blk->next) {
        if (!mm_block_valid_locked(blk->next)) break;
        if (blk->free && blk->next->free) {
            heap_block_t *next = blk->next;
            blk->size += sizeof(heap_block_t) + next->size;
            blk->next = next->next;
            if (blk->next) blk->next->prev = blk;
            continue;
        }
        blk = blk->next;
    }
    mm_unlock(flags);
    return released;
}

void mm_resident_pages(uint32_t *heap_pages, uint32_t *vm_pages) {
    uint32_t pages=0, last_end=0, flags=mm_lock();
    for(heap_block_t *b=heap_head;b && mm_block_valid_locked(b);b=b->next) {
        uint32_t first=(uint32_t)(uintptr_t)b/4096U;
        uint32_t bytes=b->free || b==heap_exec_reservation?0U:b->size;
        uint32_t end=((uint32_t)(uintptr_t)(b+1)+bytes+4095U)/4096U;
        if(first<last_end)first=last_end;
        if(end>first)pages+=end-first;
        if(end>last_end)last_end=end;
    }
    mm_unlock(flags);
    *heap_pages=pages;
    flags=kspin_lock_irqsave(&g_mm_vm_frame_lock);
    *vm_pages=g_vm_frame_used-g_vm_frame_reserved_pages;
    kspin_unlock_irqrestore(&g_mm_vm_frame_lock,flags);
}

void mm_get_system_info(system_memory_info_t *info) {
    heap_info_t heap;
    uint32_t used;

    if (!info) return;

    mm_get_info(&heap);
    uint32_t flags = kspin_lock_irqsave(&g_mm_vm_frame_lock);
    uint32_t vm_used = (g_vm_frame_used - g_vm_frame_reserved_pages) * MM_PAGE_SIZE;
    kspin_unlock_irqrestore(&g_mm_vm_frame_lock, flags);
    /* Capacity is physical installed E820 RAM; reserved is the portion
     * outside our addressable heap + VM frame pool, NOT used by tasks.
     * Report actual allocated pages separately so 3 GiB machines do not
     * appear to be wasting 1.5 GiB of allocations.  The four-field ABI is
     * unchanged: installed = managed-used + managed-free + reserved. */
    uint32_t managed = system_total_bytes >= system_reserved_bytes
        ? system_total_bytes - system_reserved_bytes : 0U;
    used = vm_used;
    if (U32_MAX_VALUE - used < heap.used_bytes) used = U32_MAX_VALUE;
    else used += (uint32_t)heap.used_bytes;
    if (used > managed) used = managed;

    info->total_bytes = system_total_bytes;
    info->reserved_bytes = system_reserved_bytes;
    info->used_bytes = used;
    info->free_bytes = managed - used;
}

static void mm_dump_locked(void) {
    heap_block_t *blk = heap_head;
    heap_block_t *tail[16];
    uint32_t count = 0U, tail_count = 0U, tail_next = 0U;
    size_t used = 0U, free_bytes = 0U, largest_free = 0U;

    kprintf("[MM] Resumen heap:\n");
    while (blk && count < 65536U) {
        uint32_t address = (uint32_t)(uintptr_t)blk;
        if (!mm_block_valid_locked(blk)) {
            uint32_t magic = 0U;
            uint32_t size = 0U;
            if (address >= heap_start_address &&
                address <= heap_end_address - sizeof(heap_block_t)) {
                magic = blk->magic;
                size = (uint32_t)blk->size;
            }
            kprintf("  CORRUPTO block=%x magic=%x size=%u tras=%u bloques\n",
                    address, magic, size, count);
            break;
        }
        if (blk->free) {
            free_bytes += blk->size;
            if (blk->size > largest_free) largest_free = blk->size;
        } else {
            used += blk->size;
        }
        tail[tail_next] = blk;
        tail_next = (tail_next + 1U) % 16U;
        if (tail_count < 16U) tail_count++;
        count++;
        blk = blk->next;
    }
    kprintf("  bloques=%u usados=%u libres=%u mayor_libre=%u\n",
            count, (uint32_t)used, (uint32_t)free_bytes,
            (uint32_t)largest_free);
    kprintf("  ultimos bloques:\n");
    for (uint32_t i = 0U; i < tail_count; i++) {
        uint32_t index = (tail_next + 16U - tail_count + i) % 16U;
        blk = tail[index];
        kprintf("  block %x: size=%u free=%u owner=%u magic=%x\n",
                (uint32_t)(uintptr_t)blk, (uint32_t)blk->size, blk->free,
                blk->owner_process_id, blk->magic);
    }
}

void mm_dump(void) {
    uint32_t flags = mm_lock();
    mm_dump_locked();
    mm_unlock(flags);
}

void mm_get_subsystem_usage(mm_subsystem_usage_t *out) {
    if(!out)return;
    uint32_t flags=mm_lock();kmemset(out,0,sizeof(*out)*MM_SUBSYSTEM_COUNT);
    for(heap_block_t *b=heap_head;b;b=b->next) {
        if(!mm_block_valid_locked(b))break;
        if(b->free || b==heap_exec_reservation)continue;
        uint32_t kind=b->subsystem<MM_SUBSYSTEM_COUNT?b->subsystem:0;
        out[kind].bytes+=b->size;out[kind].blocks++;
    }
    mm_unlock(flags);
}
