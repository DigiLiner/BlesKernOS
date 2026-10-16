#include "../include/paging.h"
#include "../include/pager.h"
#include "../include/memory.h"
#include "../include/smp.h"
#include "../include/klock.h"
#include "../include/vga.h"
#include "../include/syscall.h"
#include "../include/task.h"

#define PAGE_PAGED         0x100U /* pager-owned private anonymous page */
#define PAGE_PRESENT       0x001U
#define PAGE_WRITABLE      0x002U
#define PAGE_USER          0x004U
#define PAGE_USER_NONE     0x200U /* reserva de usuario PROT_NONE */
#define PAGE_PRIVATE_PTE   0x400U /* esta PTE reemplaza la vista heredada */
#define PAGE_COPY_ON_WRITE 0x800U /* writable private view still using file pages */
#define PAGE_WRITE_THROUGH 0x008U
#define PAGE_CACHE_DISABLE 0x010U
#define PAGE_4M            0x080U
#define PAGE_FRAME_4K      0xFFFFF000U
#define PAGE_FRAME_4M      0xFFC00000U
#define CR0_WP             0x00010000U
#define CR0_PG             0x80000000U
#define CR4_PSE            0x00000010U
#define CPUID_PSE          0x00000008U

/* Most of the identity map stays in 4 MiB pages. PDEs containing guard
 * pages need a supervisor-only split table outside the user-visible heap. */
/* Keep the early boot tables in supervisor BSS. Larger systems allocate
 * additional supervisor-only tables from the VM frame pool as needed; a
 * static 64-table array would consume the 4 MiB profile's remaining heap. */
#define PAGING_SPLIT_STATIC_TABLES 8U
#define PAGING_SPLIT_TABLES 64U
#define PAGING_PROCESS_MAX TASK_MAX
#define PAGING_PRIVATE_TABLE_MAX 512U
/* Wine's DOS allocator maps pages 1..15 before replacing the whole DOS
 * reservation. Keep page zero guarded; all other low pages can have private
 * backing, subject to the live-kernel/MMIO checks in the VM layer. */
#define PAGING_USER_EXACT_START 0x00001000U

typedef struct {
    uint32_t process_id;
    uint32_t borrowed_from;
    uint32_t *directory;
    uint32_t *table[PAGING_PRIVATE_TABLE_MAX];
    uint16_t pde_index[PAGING_PRIVATE_TABLE_MAX];
    uint16_t table_count;
    uint32_t user_vm_hint;
} paging_process_space_t;

static uint32_t kernel_page_directory[1024] __attribute__((aligned(4096)));
static uint32_t low_page_table[1024] __attribute__((aligned(4096)));
static uint32_t split_page_tables[PAGING_SPLIT_STATIC_TABLES][1024]
    __attribute__((aligned(4096)));
static uint16_t split_owner[PAGING_SPLIT_TABLES];
static uint32_t *split_dynamic_tables[PAGING_SPLIT_TABLES - PAGING_SPLIT_STATIC_TABLES];
static bool paging_enabled;
static volatile uint32_t g_tlb_generation = 1U;
static volatile uint32_t g_tlb_seen[SMP_MAX_CPUS];
static kspinlock_t g_paging_lock = KSPINLOCK_INITIALIZER;
/* Keep process-space metadata in supervisor BSS. Storing this in the legacy
 * PAGE_USER heap would let Ring 3 overwrite page-directory pointers. */
static paging_process_space_t g_process_spaces[PAGING_PROCESS_MAX];
static volatile uint32_t g_active_process[SMP_MAX_CPUS];

/* Compute the exclusive page end before rounding the start down. Reject
 * both byte-range and page-rounding overflow; never accept an empty loop. */
static bool paging_range_bounds(uint32_t address, uint32_t length,
                                uint32_t *first, uint32_t *end) {
    if (!length || length > 0xFFFFFFFFU - address) return false;
    uint32_t raw_end = address + length;
    if (raw_end > 0xFFFFFFFFU - (PAGING_PAGE_SIZE - 1U)) return false;
    *first = address & PAGE_FRAME_4K;
    *end = (raw_end + PAGING_PAGE_SIZE - 1U) & PAGE_FRAME_4K;
    return *end > *first;
}

static bool paging_cpuid_available(void) {
    uint32_t before, toggled, after;
    __asm__ volatile ("pushfl; popl %0" : "=r"(before));
    toggled = before ^ (1U << 21);
    __asm__ volatile ("pushl %0; popfl" : : "r"(toggled) : "cc");
    __asm__ volatile ("pushfl; popl %0" : "=r"(after));
    __asm__ volatile ("pushl %0; popfl" : : "r"(before) : "cc");
    return ((after ^ before) & (1U << 21)) != 0U;
}

static uint32_t paging_cpu(void) {
    uint32_t cpu = smp_cpu_index();
    return cpu < SMP_MAX_CPUS ? cpu : 0U;
}

static void paging_reload_cr3(void) {
    uint32_t cr3;
    __asm__ volatile ("movl %%cr3,%0" : "=r"(cr3));
    __asm__ volatile ("movl %0,%%cr3" : : "r"(cr3) : "memory");
}

static uint32_t *paging_current_directory(void) {
    uint32_t cr3;
    __asm__ volatile ("movl %%cr3,%0" : "=r"(cr3));
    return (uint32_t *)(uintptr_t)(cr3 & PAGE_FRAME_4K);
}

static bool paging_alloc_private_page(uint32_t **page_out) {
    uint32_t physical = 0U;
    if (!page_out || !mm_vm_frame_alloc(1U, &physical)) return false;
    *page_out = (uint32_t *)(uintptr_t)physical;
    kmemset(*page_out, 0, PAGING_PAGE_SIZE);
    return true;
}

static void paging_free_private_page(uint32_t *page) {
    if (page) mm_vm_frame_free((uint32_t)(uintptr_t)page, 1U);
}

static paging_process_space_t *paging_find_raw_space(uint32_t process_id) {
    if(!process_id) return NULL;
    for(uint32_t i=0U;i<PAGING_PROCESS_MAX;i++)
        if(g_process_spaces[i].process_id==process_id)
            return &g_process_spaces[i];
    return NULL;
}

static paging_process_space_t *paging_find_space(uint32_t process_id) {
    paging_process_space_t *s=paging_find_raw_space(process_id);
    for(unsigned n=0;s && s->borrowed_from && n<PAGING_PROCESS_MAX;n++)
        s=paging_find_raw_space(s->borrowed_from);
    return s;
}

/* Count explicit process PTEs, never the inherited identity map. RSS is
 * deduplicated by physical frame, including aliases at different addresses.
 * A diagnostic allocation is released before returning; callers sample the
 * global physical total before this transient workspace is allocated. */
bool paging_memory_pages(uint32_t pid,uint32_t *mapped,uint32_t *resident,
                         uint32_t *writable,uint32_t *cow,uint32_t *tables) {
    *mapped=*resident=*writable=*cow=*tables=0;
    uint8_t *seen=kzalloc(0x100000U/8U);
    if(!seen)return false;
    uint32_t flags=kspin_lock_irqsave(&g_paging_lock);
    paging_process_space_t *space=paging_find_space(pid);
    if(space) {
        *tables=space->table_count+1U;
        for(uint32_t t=0;t<space->table_count;t++)for(uint32_t i=0;i<1024;i++) {
            uint32_t pte=space->table[t][i];
            if(!(pte&PAGE_PRIVATE_PTE))continue;
            (*mapped)++;
            if(!(pte&PAGE_PRESENT))continue;
            uint32_t frame=pte>>12U;
            if(seen[frame>>3U]&(1U<<(frame&7U)))continue;
            seen[frame>>3U]|=1U<<(frame&7U);
            (*resident)++;
            if(pte&PAGE_WRITABLE)(*writable)++;
            if(pte&PAGE_COPY_ON_WRITE)(*cow)++;
        }
    }
    kspin_unlock_irqrestore(&g_paging_lock,flags);
    kfree(seen);return true;
}

bool paging_process_borrow(uint32_t parent,uint32_t child) {
    if (!parent || !child || parent == child) return false;
    uint32_t flags=kspin_lock_irqsave(&g_paging_lock);
    paging_process_space_t *src=paging_find_space(parent),*dst=paging_find_raw_space(child);
    if(src && !dst) for(unsigned i=0;i<PAGING_PROCESS_MAX;i++)
        if(!g_process_spaces[i].process_id) {dst=&g_process_spaces[i];break;}
    bool ok=src && dst && !dst->directory && !dst->borrowed_from;
    if(ok) {dst->process_id=child;dst->borrowed_from=src->process_id;}
    kspin_unlock_irqrestore(&g_paging_lock,flags);
    return ok;
}

static bool paging_space_private_pde(const paging_process_space_t *space,
                                     uint32_t pde_index) {
    if(!space) return false;
    for(uint32_t i=0U;i<space->table_count;i++)
        if(space->pde_index[i]==pde_index) return true;
    return false;
}

static uint32_t paging_kernel_entry(uint32_t address) {
    uint32_t pde = kernel_page_directory[address >> 22U];
    if (!(pde & PAGE_PRESENT)) return 0U;
    if (pde & PAGE_4M) {
        uint32_t physical = (pde & PAGE_FRAME_4M) +
                            (address & 0x003FF000U);
        uint32_t flags = pde & (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
                                PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE);
        return (physical & PAGE_FRAME_4K) | flags;
    }
    return ((uint32_t *)(uintptr_t)(pde & PAGE_FRAME_4K))
        [(address >> 12U) & 1023U];
}

static uint32_t paging_space_entry(const paging_process_space_t *space,
                                   uint32_t address) {
    uint32_t pde;
    if (!space || !space->directory) return 0U;
    pde = space->directory[address >> 22U];
    if (!(pde & PAGE_PRESENT)) return 0U;
    if (pde & PAGE_4M) {
        uint32_t physical = (pde & PAGE_FRAME_4M) +
                            (address & 0x003FF000U);
        uint32_t flags = pde & (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
                                PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE);
        return (physical & PAGE_FRAME_4K) | flags;
    }
    return ((uint32_t *)(uintptr_t)(pde & PAGE_FRAME_4K))
        [(address >> 12U) & 1023U];
}

static bool paging_space_user_range_free(const paging_process_space_t *space,
                                         uint32_t address, uint32_t length) {
    uint32_t end;
    if (!space || !length || address < PAGING_USER_VM_START ||
        address + length < address || address + length > PAGING_USER_VM_END)
        return false;
    end = address + length;
    for (uint32_t page = address; page < end; page += PAGING_PAGE_SIZE) {
        uint32_t entry = paging_space_entry(space, page);
        /* The arena starts as supervisor identity memory. Only a PAGE_USER PTE
         * represents an occupied process-private virtual address. */
        if (entry & (PAGE_USER_NONE|PAGE_PAGED)) return false;
        if ((entry & (PAGE_PRESENT | PAGE_USER)) ==
            (PAGE_PRESENT | PAGE_USER)) return false;
    }
    return true;
}

/* Exact mappings are independent of the automatic allocation arena. Wine's
 * shared user-data page at 0x7ffe0000 is above PAGING_USER_VM_END but below
 * the signed-pointer boundary. No extra physical RAM is reserved by allowing
 * that address: pager faults still allocate backed frames normally.
 * Low supervisor identity mappings are free
 * from the process point of view until a user/private PTE overrides them. */
static bool paging_space_exact_range_free(const paging_process_space_t *space,
                                          uint32_t address, uint32_t length) {
    uint32_t end;
    if (!space || !length || (address & (PAGING_PAGE_SIZE - 1U)) ||
        address < PAGING_USER_EXACT_START || address + length < address ||
        address + length > 0x80000000U) return false;
    end = address + length;
    for (uint32_t page = address; page < end; page += PAGING_PAGE_SIZE) {
        uint32_t entry = paging_space_entry(space, page);
        /* A process directory inherits the kernel's transitional Ring-3
         * windows (the native PE fixed view and legacy heap). Those entries
         * are not allocations owned by this Linux process and may be
         * shadowed by a private PTE. Only PAGE_PRIVATE_PTE entries represent
         * address-space ownership for MAP_FIXED_NOREPLACE. */
        if (!(entry & PAGE_PRIVATE_PTE)) continue;
        if (entry & (PAGE_USER_NONE|PAGE_PAGED)) return false;
        if ((entry & (PAGE_PRESENT | PAGE_USER)) ==
            (PAGE_PRESENT | PAGE_USER)) return false;
    }
    return true;
}

static uint32_t paging_space_find_user_range(paging_process_space_t *space,
                                             uint32_t requested,
                                             uint32_t length,
                                             paging_range_skip_t skip,
                                             void *context) {
    uint32_t rounded;
    uint32_t start;
    if (!space || !length || length > PAGING_USER_VM_END - PAGING_USER_VM_START ||
        length > 0xFFFFFFFFU - (PAGING_PAGE_SIZE - 1U)) return 0U;
    rounded = (length + PAGING_PAGE_SIZE - 1U) & PAGE_FRAME_4K;

    if (requested && requested <= 0xFFFFFFFFU - (PAGING_PAGE_SIZE - 1U)) {
        uint32_t hint = (requested + PAGING_PAGE_SIZE - 1U) & PAGE_FRAME_4K;
        if (hint >= PAGING_USER_VM_START && hint < PAGING_USER_VM_END &&
            hint + rounded >= hint && hint + rounded <= PAGING_USER_VM_END &&
            (!skip || !skip(context, hint, rounded)) &&
            paging_space_user_range_free(space, hint, rounded))
            return hint;
    }

    start = space->user_vm_hint;
    if (start < PAGING_USER_VM_START || start >= PAGING_USER_VM_END)
        start = PAGING_USER_VM_START;
    start = (start + PAGING_PAGE_SIZE - 1U) & PAGE_FRAME_4K;

    /* Continue from the last mapping first, then wrap so munmap holes can be
     * reused without a separate virtual-address allocator. */
    for (uint32_t pass = 0U; pass < 2U; pass++) {
        uint32_t candidate = pass ? PAGING_USER_VM_START : start;
        uint32_t limit = pass ? start : PAGING_USER_VM_END;
        while (candidate < limit && candidate + rounded >= candidate &&
               candidate + rounded <= limit) {
            uint32_t next = skip ? skip(context, candidate, rounded) : 0U;
            if (!next && paging_space_user_range_free(space, candidate, rounded))
                return candidate;
            if (next > candidate) {
                if (next > 0xffffffffU - (PAGING_PAGE_SIZE - 1U)) break;
                candidate = (next + PAGING_PAGE_SIZE - 1U) & PAGE_FRAME_4K;
            } else candidate += PAGING_PAGE_SIZE;
        }
    }
    return 0U;
}

static void paging_invlpg(uint32_t address) {
    __asm__ volatile ("invlpg (%0)" : : "r"(address) : "memory");
}

static uint32_t paging_publish_change(void) {
    uint32_t generation;
    __asm__ volatile ("lock incl %0" : "+m"(g_tlb_generation) : :
                      "memory", "cc");
    generation = g_tlb_generation;
    paging_reload_cr3();
    g_tlb_seen[paging_cpu()] = generation;
    return generation;
}

/* A process-private page-table edit only needs to invalidate CPUs that are
 * executing that process right now.  The previous code sent an IPI to every
 * online CPU and then waited for all of them, even if an AP was running an
 * unrelated process (or sitting in a long IRQ-off kernel section). Wine does
 * thousands of MAP_FIXED/munmap/mprotect operations, so those unnecessary
 * waits showed up as repeated "shootdown TLB incompleto" warnings and real
 * startup latency.
 *
 * A CPU that switches into the target after this snapshot is safe:
 * paging_switch_process() reloads CR3 and records the current generation.
 * process_id==0 keeps the old global behavior for kernel/shared mappings. */
static uint32_t paging_shootdown_mask(uint32_t process_id) {
    uint32_t mask = 0U;
    uint32_t self;
    uint32_t online;

    if (!smp_scheduler_started()) return 0U;
    self = paging_cpu();
    online = smp_cpu_count();
    if (online > 32U) online = 32U;
    uint32_t flags = kspin_lock_irqsave(&g_paging_lock);
    paging_process_space_t *target = paging_find_space(process_id);
    for (uint32_t cpu = 0U; cpu < online; cpu++) {
        if (cpu == self || !smp_cpu_online(cpu)) continue;
        /* CLONE_VM/vfork borrowers use the same CR3 under different IDs. */
        if (!process_id || g_active_process[cpu] == process_id ||
            (target && paging_find_space(g_active_process[cpu]) == target))
            mask |= 1U << cpu;
    }
    kspin_unlock_irqrestore(&g_paging_lock, flags);
    return mask;
}

static uint32_t paging_kick_generation(uint32_t process_id) {
    uint32_t mask = paging_shootdown_mask(process_id);
    /* El IPI de replanificacion entra por task_schedule_internal(), cuyo
     * primer paso es paging_sync_cpu(). */
    if (mask) smp_reschedule_mask(mask);
    return mask;
}

static bool paging_wait_generation(uint32_t generation, uint32_t mask,
                                   uint32_t process_id) {
    uint32_t spins = 2000000U;

    if (!smp_scheduler_started() || !mask) return true;
    while (spins--) {
        bool done = true;
        for (uint32_t cpu = 0U; cpu < 32U; cpu++) {
            if (!(mask & (1U << cpu))) continue;
            if (!smp_cpu_online(cpu)) continue;
            if (g_tlb_seen[cpu] < generation) {
                done = false;
                break;
            }
        }
        if (done) return true;
        __asm__ volatile ("pause");
    }
    /* A stalled CPU running the affected address space is worth reporting;
     * unrelated CPUs are no longer part of this wait. */
    static uint32_t last_reported_generation;
    if (!last_reported_generation ||
        generation - last_reported_generation >= 1024U) {
        kprintf("[PAGING] aviso: shootdown TLB incompleto gen=%u pid=%u mask=%x\n",
                generation, process_id, mask);
        last_reported_generation = generation;
    }
    return false;
}

static uint32_t *paging_split_pde_locked(uint32_t pde_index) {
    uint32_t pde;
    uint32_t base;
    uint32_t flags;
    uint32_t *table = NULL;

    if (pde_index >= 1024U) return NULL;
    pde = kernel_page_directory[pde_index];
    if ((pde & PAGE_PRESENT) && !(pde & PAGE_4M))
        return (uint32_t *)(uintptr_t)(pde & PAGE_FRAME_4K);

    for (uint32_t i = 0U; i < PAGING_SPLIT_TABLES; i++)
        if (split_owner[i] == (uint16_t)(pde_index + 1U))
            return i < PAGING_SPLIT_STATIC_TABLES ? split_page_tables[i]
                : split_dynamic_tables[i - PAGING_SPLIT_STATIC_TABLES];
    for (uint32_t i = 0U; i < PAGING_SPLIT_TABLES; i++) {
        if (split_owner[i]) continue;
        if (i < PAGING_SPLIT_STATIC_TABLES) {
            table = split_page_tables[i];
        } else {
            uint32_t physical;
            if (!mm_vm_frame_alloc(1U, &physical)) return NULL;
            table = (uint32_t *)(uintptr_t)physical;
            split_dynamic_tables[i - PAGING_SPLIT_STATIC_TABLES] = table;
        }
        split_owner[i] = (uint16_t)(pde_index + 1U);
        break;
    }
    if (!table) return NULL;

    base = pde & PAGE_FRAME_4M;
    flags = pde & (PAGE_WRITABLE | PAGE_USER |
                   PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE);
    for (uint32_t i = 0U; i < 1024U; i++)
        table[i] = (base + i * PAGING_PAGE_SIZE) |
                   PAGE_PRESENT | flags;
    kernel_page_directory[pde_index] =
        ((uint32_t)(uintptr_t)table & PAGE_FRAME_4K) |
        PAGE_PRESENT | PAGE_WRITABLE | (flags & PAGE_USER);
    return table;
}

static uint32_t paging_entry_flags(uint32_t address, bool *large_out) {
    uint32_t *directory=paging_current_directory();
    uint32_t pde = directory[address >> 22U];
    if (!(pde & PAGE_PRESENT)) {
        if (large_out) *large_out = false;
        return 0U;
    }
    if (pde & PAGE_4M) {
        if (large_out) *large_out = true;
        return pde;
    }
    if (large_out) *large_out = false;
    uint32_t pte = ((uint32_t *)(uintptr_t)(pde & PAGE_FRAME_4K))
        [(address >> 12U) & 1023U];
    /* IA-32 permissions are the intersection of PDE and PTE permissions. */
    if (!(pde & PAGE_USER)) pte &= ~PAGE_USER;
    if (!(pde & PAGE_WRITABLE)) pte &= ~(PAGE_WRITABLE | PAGE_COPY_ON_WRITE);
    return pte;
}

bool paging_init(void) {
    uint32_t eax = 1U, ebx, ecx, edx;
    uint32_t cr0, cr4;

    if (!paging_cpuid_available()) {
        kprintf("[PAGING] CPU sin CPUID; paging no activado\n");
        return false;
    }

    __asm__ volatile ("cpuid"
        : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    (void)ebx; (void)ecx;
    if ((edx & CPUID_PSE) == 0U) {
        kprintf("[PAGING] CPU sin paginas de 4 MiB; paging no activado\n");
        return false;
    }

    for (uint32_t i = 0U; i < 1024U; i++)
        kernel_page_directory[i] = (i << 22U) | PAGE_PRESENT |
            PAGE_WRITABLE | PAGE_4M;

    /* Page zero is deliberately absent. Everything else below 4 MiB is
     * supervisor-only: kernel image, GDT/IDT, AP trampoline and static SMP
     * state can no longer be overwritten directly from Ring 3. */
    low_page_table[0] = 0U;
    for (uint32_t i = 1U; i < 1024U; i++)
        low_page_table[i] = i * PAGING_PAGE_SIZE |
            PAGE_PRESENT | PAGE_WRITABLE;
    kernel_page_directory[0] =
        ((uint32_t)(uintptr_t)low_page_table & PAGE_FRAME_4K) |
        PAGE_PRESENT | PAGE_WRITABLE;

    kmemset(split_owner, 0, sizeof(split_owner));
    kmemset(split_dynamic_tables, 0, sizeof(split_dynamic_tables));
    kmemset((void *)g_tlb_seen, 0, sizeof(g_tlb_seen));
    g_tlb_seen[0] = g_tlb_generation;

    /* 0xFEC00000-0xFEFFFFFF contains IOAPIC/LAPIC MMIO. It remains
     * supervisor-only and uncached. */
    kernel_page_directory[0xFEC00000U >> 22U] |=
        PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE;

    /* Any Stage-2-confirmed VESA LFB is PCI MMIO, never ordinary WB RAM.
     * Mark its 4 MiB identity PDEs uncached before the first splash paint,
     * including QEMU's high 32-bit framebuffer. Bounds mirror gfx.c. */
    if (mm_boot_vesa_read32(0U) == 0x31495547U) {
        uint32_t fb = mm_boot_vesa_read32(4U);
        uint32_t w = mm_boot_vesa_read16(8U);
        uint32_t h = mm_boot_vesa_read16(10U);
        uint32_t stride = mm_boot_vesa_read16(12U);
        uint32_t bpp = mm_boot_vesa_read8(14U);
        uint32_t mode = mm_boot_vesa_read16(15U);
        uint32_t unit = bpp / 8U;
        if (mode && (bpp == 16U || bpp == 32U) &&
            w >= 640U && w <= 1920U && h >= 400U && h <= 1200U &&
            stride >= w * unit && stride <= 8192U &&
            fb >= 0x40000000U && fb < 0xFE000000U &&
            h * stride <= 16U * 1024U * 1024U &&
            h * stride <= 0xFE000000U - fb) {
            uint32_t first = fb >> 22U;
            uint32_t last = (fb + h * stride - 1U) >> 22U;
            for (uint32_t pde = first; pde <= last; ++pde)
                kernel_page_directory[pde] |=
                    PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE;
        }
    }

    __asm__ volatile ("movl %%cr4,%0" : "=r"(cr4));
    cr4 |= CR4_PSE;
    __asm__ volatile ("movl %0,%%cr4" : : "r"(cr4) : "memory");
    __asm__ volatile ("movl %0,%%cr3" : :
        "r"((uint32_t)(uintptr_t)kernel_page_directory) : "memory");
    __asm__ volatile ("movl %%cr0,%0" : "=r"(cr0));
    cr0 |= CR0_PG | CR0_WP;
    __asm__ volatile ("movl %0,%%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile ("jmp 1f\n1:" : : : "memory");
    paging_enabled = true;
    kprintf("[PAGING] activo: kernel supervisor, pagina cero guard, CR0.WP\n");
    return true;
}

bool paging_kernel_mmio_uncached(uint32_t physical_address, uint32_t length) {
    /* PCI MMIO BARs sit above RAM in our 32-bit identity map. Covering the
     * entire 4 MiB PDE also keeps every register in the BAR uncached without
     * spending one of the scarce split page tables. */
    if (!paging_enabled || !length || physical_address < mm_physical_top() ||
        physical_address + length < physical_address ||
        (physical_address >> 22U) !=
        ((physical_address + length - 1U) >> 22U)) return false;
    uint32_t index = physical_address >> 22U;
    uint32_t flags = kspin_lock_irqsave(&g_paging_lock);
    if (!(kernel_page_directory[index] & PAGE_4M)) {
        kspin_unlock_irqrestore(&g_paging_lock, flags);
        return false;
    }
    if (!mm_register_kernel_mmio(physical_address, length)) {
        kspin_unlock_irqrestore(&g_paging_lock, flags);
        return false;
    }
    kernel_page_directory[index] |= PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE;
    __asm__ volatile ("movl %%cr3,%%eax; movl %%eax,%%cr3" : : : "eax", "memory");
    kspin_unlock_irqrestore(&g_paging_lock, flags);
    return true;
}

bool paging_configure_user_layout(uint32_t user_start, uint32_t user_end) {
    if (!paging_enabled || user_end <= user_start) return false;
    /* PE fixed view and managed heap are the only flat Ring-3 ranges in this
     * transitional address space. The operation also handles a partial final
     * PDE using the supervisor-only split-table pool. */
    if (!paging_set_range(PE_FIXED_VIEW_START,
                          PE_FIXED_VIEW_END - PE_FIXED_VIEW_START,
                          true, true, true)) return false;
    if (!paging_set_range(user_start, user_end - user_start,
                          true, true, true)) return false;
    if (paging_page_present(0U) || paging_page_user(0x00100000U) ||
        !paging_page_user(PE_FIXED_VIEW_START) ||
        !paging_page_user(user_start)) {
        kprintf("[PAGING] ERROR: autoprueba supervisor/user fallo\n");
        return false;
    }
    kmemset(g_process_spaces, 0, sizeof(g_process_spaces));
    kmemset((void *)g_active_process, 0, sizeof(g_active_process));
    kprintf("[PAGING] Ring 3 permitido: %x-%x y heap %x-%x\n",
            PE_FIXED_VIEW_START, PE_FIXED_VIEW_END,
            user_start, user_end);
    return true;
}

bool paging_set_range(uint32_t address, uint32_t length, bool present,
                      bool user, bool writable) {
    uint32_t first;
    uint32_t end;
    uint32_t flags;
    uint32_t lock_flags;
    uint32_t generation;

    if (!paging_enabled || !paging_range_bounds(address, length, &first, &end))
        return false;
    flags = (present ? PAGE_PRESENT : 0U) |
            (writable ? PAGE_WRITABLE : 0U) |
            (user ? PAGE_USER : 0U);

    lock_flags = kspin_lock_irqsave(&g_paging_lock);
    for (uint32_t page = first; page < end;) {
        uint32_t pde_index = page >> 22U;
        uint32_t old_pde = kernel_page_directory[pde_index];
        uint32_t remaining = end - page;

        /* Preserve the compact 4 MiB identity mapping whenever the request
         * covers a complete, still-large PDE. This is essential for marking
         * a 120 MiB heap user-accessible without consuming one 4 KiB page
         * table for every 4 MiB. Guard pages and partial final ranges alone
         * force a split. A PDE that is already split is never collapsed,
         * because another stack guard may live in the same 4 MiB region. */
        if ((old_pde & PAGE_4M) &&
            (page & 0x003FFFFFU) == 0U &&
            remaining >= 0x00400000U) {
            uint32_t physical = old_pde & PAGE_FRAME_4M;
            uint32_t cache_flags = old_pde &
                (PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE);
            kernel_page_directory[pde_index] = physical | cache_flags |
                PAGE_4M | flags;
            for(uint32_t s=0U;s<PAGING_PROCESS_MAX;s++) {
                paging_process_space_t *space=&g_process_spaces[s];
                if(space->borrowed_from) continue;
                if(space->process_id &&
                   !paging_space_private_pde(space,pde_index))
                    space->directory[pde_index]=kernel_page_directory[pde_index];
                else if (space->process_id) {
                    uint32_t *private_table=(uint32_t *)(space->directory[pde_index]&PAGE_FRAME_4K);
                    for (uint32_t i=0;i<1024;i++)
                        if (!(private_table[i]&PAGE_PRIVATE_PTE))
                            private_table[i]=(physical+i*4096)|cache_flags|flags;
                }
            }
            page += 0x00400000U;
            continue;
        }

        {
            uint32_t *table = paging_split_pde_locked(pde_index);
            uint32_t pte_index;
            uint32_t physical;
            uint32_t cache_flags;
            if (!table) {
                kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
                kprintf("[PAGING] sin tablas para dividir PDE %u addr=%x\n",
                        pde_index, page);
                return false;
            }
            pte_index = (page >> 12U) & 1023U;
            physical = table[pte_index] & PAGE_FRAME_4K;
            cache_flags = table[pte_index] &
                (PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE);
            table[pte_index] = physical | cache_flags | flags;
            if (user) kernel_page_directory[pde_index] |= PAGE_USER;
            for(uint32_t s=0U;s<PAGING_PROCESS_MAX;s++) {
                paging_process_space_t *space=&g_process_spaces[s];
                if(space->borrowed_from) continue;
                if(space->process_id &&
                   !paging_space_private_pde(space,pde_index))
                    space->directory[pde_index]=kernel_page_directory[pde_index];
                else if (space->process_id) {
                    uint32_t *private_table=(uint32_t *)(space->directory[pde_index]&PAGE_FRAME_4K);
                    if (!(private_table[pte_index]&PAGE_PRIVATE_PTE))
                        private_table[pte_index]=table[pte_index];
                }
            }
            paging_invlpg(page);
            page += PAGING_PAGE_SIZE;
        }
    }
    generation = paging_publish_change();
    kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
    {
        uint32_t mask = paging_kick_generation(0U);
        paging_wait_generation(generation, mask, 0U);
    }
    return true;
}

bool paging_page_present(uint32_t address) {
    return (paging_entry_flags(address, NULL) & PAGE_PRESENT) != 0U;
}

bool paging_page_user(uint32_t address) {
    bool large;
    uint32_t flags = paging_entry_flags(address, &large);
    (void)large;
    return (flags & (PAGE_PRESENT | PAGE_USER)) ==
           (PAGE_PRESENT | PAGE_USER);
}

bool paging_user_range_permitted(const void *pointer,uint32_t length,bool write) {
    uint32_t start=(uint32_t)(uintptr_t)pointer;
    if(!pointer)return length==0;
    uint32_t end=start+(length?length:1U)-1U;
    if(end<start)return false;
    for(uint32_t page=start&~4095U;;page+=4096U) {
        uint32_t flags=paging_entry_flags(page,NULL);
        if(!(flags&PAGE_USER) || !(flags&(PAGE_PRESENT|PAGE_PAGED)))return false;
        if(write && !(flags&(PAGE_WRITABLE|PAGE_COPY_ON_WRITE)))return false;
        if(page>=(end&~4095U))return true;
    }
}

bool paging_user_range_ok(const void *pointer, uint32_t length,
                          bool require_write) {
    uint32_t start = (uint32_t)(uintptr_t)pointer;
    uint32_t end;
    uint32_t page;

    if (!pointer) return length == 0U;
    if (!length) return paging_page_user(start);
    end = start + length - 1U;
    if (end < start) return false;
    page = start & ~(PAGING_PAGE_SIZE - 1U);
    for (;;) {
        uint32_t flags = paging_entry_flags(page, NULL);
        if ((flags & (PAGE_PRESENT | PAGE_USER)) !=
            (PAGE_PRESENT | PAGE_USER)) return false;
        if (require_write && !(flags & PAGE_WRITABLE)) {
            if (!(flags & PAGE_COPY_ON_WRITE) || !syscall_linux_cow_fault(page)) return false;
        }
        if (page >= (end & ~(PAGING_PAGE_SIZE - 1U))) break;
        page += PAGING_PAGE_SIZE;
    }
    return true;
}

void paging_sync_cpu(void) {
    uint32_t cpu;
    uint32_t generation;
    if (!paging_enabled) return;
    cpu = paging_cpu();
    generation = g_tlb_generation;
    if (g_tlb_seen[cpu] == generation) return;
    paging_reload_cr3();
    g_tlb_seen[cpu] = generation;
}

uint32_t paging_tlb_generation(void) { return g_tlb_generation; }

bool paging_recover_stale_fault(uint32_t address) {
    uint32_t cpu = paging_cpu();
    if (!paging_enabled || !paging_page_present(address) ||
        g_tlb_seen[cpu] == g_tlb_generation) return false;
    paging_sync_cpu();
    kprintf("[PAGING] TLB obsoleto reparado cpu=%u addr=%x gen=%u\n",
            cpu, address, g_tlb_generation);
    return true;
}

static uint32_t *paging_private_table_locked(paging_process_space_t *space,
                                             uint32_t pde_index);

bool paging_process_create(uint32_t process_id) {
    paging_process_space_t *space=NULL;
    uint32_t flags;
    if(!paging_enabled || !process_id) return false;
    flags=kspin_lock_irqsave(&g_paging_lock);
    if(paging_find_space(process_id)) {
        kspin_unlock_irqrestore(&g_paging_lock,flags);
        return true;
    }
    for(uint32_t i=0U;i<PAGING_PROCESS_MAX;i++)
        if(!g_process_spaces[i].process_id) { space=&g_process_spaces[i]; break; }
    if(!space) {
        kspin_unlock_irqrestore(&g_paging_lock,flags);
        return false;
    }
    kmemset(space,0,sizeof(*space));
    if(!paging_alloc_private_page(&space->directory)) {
        kmemset(space,0,sizeof(*space));
        kspin_unlock_irqrestore(&g_paging_lock,flags);
        return false;
    }
    kmemcpy(space->directory,kernel_page_directory,PAGING_PAGE_SIZE);

    /* WINE-HEAP-METADATA-GUARD:
     *
     * Linux ET_DYN/PIE images, their interpreter, stacks and TLS currently
     * live in kmalloc-backed identity addresses below PAGING_USER_VM_START.
     * Therefore the whole heap cannot be made supervisor-only without making
     * legitimate user code non-executable (the v1 isolation patch did exactly
     * that and faulted at ld-linux entry with PF error 0x5).
     *
     * Protect only the page containing the allocator's root metadata.  The
     * corruption seen under Wine zeroed the heap_block_t at mm_heap_start()
     * (at HEAP_ALLOC_START).  A private 4 KiB table preserves PAGE_USER for
     * every neighbouring identity page while this one inherited alias becomes
     * supervisor-only.  PAGE_PRIVATE_PTE prevents later global paging_set_range
     * updates from accidentally publishing the guard again.  An explicit Linux
     * mmap at this virtual address must be rejected by the syscall layer:
     * CPL0 also runs under this CR3, so replacing the alias hides the real
     * allocator header from the kernel even if physical memory is intact.
     */
    space->process_id=process_id;
    space->user_vm_hint=PAGING_USER_VM_START;
    {
        uint32_t guard = mm_heap_start() & PAGE_FRAME_4K;
        uint32_t *table = paging_private_table_locked(space, guard >> 22U);
        if (!table) {
            uint32_t *directory = space->directory;
            kmemset(space, 0, sizeof(*space));
            kspin_unlock_irqrestore(&g_paging_lock, flags);
            paging_free_private_page(directory);
            return false;
        }
        table[(guard >> 12U) & 1023U] =
            (table[(guard >> 12U) & 1023U] & ~PAGE_USER) | PAGE_PRIVATE_PTE;
    }
    kspin_unlock_irqrestore(&g_paging_lock,flags);
    return true;
}

static uint32_t *paging_private_table_locked(paging_process_space_t *space,
                                             uint32_t pde_index) {
    uint32_t pde;
    uint32_t *table;

    for(uint32_t i=0U;i<space->table_count;i++)
        if(space->pde_index[i]==pde_index) return space->table[i];
    if(space->table_count>=PAGING_PRIVATE_TABLE_MAX) return NULL;
    if(!paging_alloc_private_page(&table)) return NULL;
    pde=space->directory[pde_index];
    if((pde&PAGE_PRESENT) && (pde&PAGE_4M)) {
        uint32_t base=pde&PAGE_FRAME_4M;
        uint32_t inherited=pde&(PAGE_WRITABLE|PAGE_USER|
                                PAGE_WRITE_THROUGH|PAGE_CACHE_DISABLE);
        for(uint32_t i=0U;i<1024U;i++)
            table[i]=(base+i*PAGING_PAGE_SIZE)|PAGE_PRESENT|inherited;
    } else if(pde&PAGE_PRESENT) {
        kmemcpy(table,(const void *)(uintptr_t)(pde&PAGE_FRAME_4K),
                PAGING_PAGE_SIZE);
    }
    uint32_t slot=space->table_count++;
    space->table[slot]=table;
    space->pde_index[slot]=(uint16_t)pde_index;
    space->directory[pde_index]=((uint32_t)(uintptr_t)table&PAGE_FRAME_4K)|
        PAGE_PRESENT|PAGE_WRITABLE|PAGE_USER;
    return table;
}

bool paging_process_map(uint32_t process_id,uint32_t virtual_address,
                        uint32_t physical_address,uint32_t length,
                        bool user,bool writable) {
    paging_process_space_t *space;
    uint32_t first,end,flags,lock_flags;
    if(!length || (virtual_address&(PAGING_PAGE_SIZE-1U)) ||
       (physical_address&(PAGING_PAGE_SIZE-1U)) ||
       !paging_range_bounds(virtual_address,length,&first,&end) ||
       end-first > 0xFFFFFFFFU-physical_address) return false;
    flags=PAGE_PRESENT|PAGE_PRIVATE_PTE|(user?PAGE_USER:PAGE_USER_NONE)|(writable?PAGE_WRITABLE:0U);
    lock_flags=kspin_lock_irqsave(&g_paging_lock);
    space=paging_find_space(process_id);
    if(!space) { kspin_unlock_irqrestore(&g_paging_lock,lock_flags); return false; }
    for(uint32_t index=first>>22U;index<=((end-1U)>>22U);index++) {
        if(!paging_private_table_locked(space,index)) {
            kspin_unlock_irqrestore(&g_paging_lock,lock_flags); return false;
        }
    }
    for(uint32_t page=first,physical=physical_address;page<end;
        page+=PAGING_PAGE_SIZE,physical+=PAGING_PAGE_SIZE) {
        uint32_t *table=paging_private_table_locked(space,page>>22U);
        if(!table) { kspin_unlock_irqrestore(&g_paging_lock,lock_flags); return false; }
        table[(page>>12U)&1023U]=(physical&PAGE_FRAME_4K)|flags;
    }
    uint32_t generation=paging_publish_change();
    kspin_unlock_irqrestore(&g_paging_lock,lock_flags);
    uint32_t mask=paging_kick_generation(process_id);
    paging_wait_generation(generation,mask,process_id);
    return true;
}

bool paging_process_map_auto(uint32_t process_id, uint32_t requested,
                             uint32_t physical_address, uint32_t length,
                             bool writable, uint32_t *virtual_out) {
    return paging_process_map_auto_filtered(process_id, requested,
        physical_address, length, writable, virtual_out, NULL, NULL);
}

bool paging_process_map_auto_filtered(uint32_t process_id, uint32_t requested,
    uint32_t physical_address, uint32_t length, bool writable,
    uint32_t *virtual_out, paging_range_skip_t skip, void *context) {
    paging_process_space_t *space;
    uint32_t address, end, rounded, flags, lock_flags;
    if (virtual_out) *virtual_out = 0U;
    if (!process_id || !length ||
        (physical_address & (PAGING_PAGE_SIZE - 1U)) ||
        length > 0xFFFFFFFFU - (PAGING_PAGE_SIZE - 1U)) return false;
    rounded = (length + PAGING_PAGE_SIZE - 1U) & PAGE_FRAME_4K;
    if (rounded > 0xFFFFFFFFU - physical_address) return false;
    if (!paging_process_create(process_id)) return false;

    lock_flags = kspin_lock_irqsave(&g_paging_lock);
    space = paging_find_space(process_id);
    if (!space) {
        kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
        return false;
    }
    address = paging_space_find_user_range(space, requested, rounded, skip, context);
    if (!address) {
        kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
        return false;
    }
    end = address + rounded;
    flags = PAGE_PRESENT | PAGE_USER | PAGE_PRIVATE_PTE | (writable ? PAGE_WRITABLE : 0U);
    for (uint32_t page = address, physical = physical_address; page < end;
         page += PAGING_PAGE_SIZE, physical += PAGING_PAGE_SIZE) {
        uint32_t *table = paging_private_table_locked(space, page >> 22U);
        if (!table) {
            for (uint32_t undo = address; undo < page; undo += PAGING_PAGE_SIZE) {
                uint32_t *undo_table = NULL;
                for (uint32_t i = 0U; i < space->table_count; i++)
                    if (space->pde_index[i] == (undo >> 22U)) {
                        undo_table = space->table[i];
                        break;
                    }
                if (undo_table)
                    undo_table[(undo >> 12U) & 1023U] =
                        paging_kernel_entry(undo) & ~PAGE_USER;
            }
            kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
            return false;
        }
        table[(page >> 12U) & 1023U] =
            (physical & PAGE_FRAME_4K) | flags;
    }
    space->user_vm_hint = end < PAGING_USER_VM_END ? end : PAGING_USER_VM_START;
    if (paging_current_directory() == space->directory) paging_reload_cr3();
    kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
    if (virtual_out) *virtual_out = address;
    return true;
}

bool paging_process_range_free_exact(uint32_t process_id,
                                     uint32_t virtual_address,
                                     uint32_t length) {
    paging_process_space_t *space;
    uint32_t rounded, lock_flags;
    bool free_range;
    if (!process_id || !length ||
        length > 0xFFFFFFFFU - (PAGING_PAGE_SIZE - 1U)) return false;
    rounded = (length + PAGING_PAGE_SIZE - 1U) & PAGE_FRAME_4K;
    if (!paging_process_create(process_id)) return false;
    lock_flags = kspin_lock_irqsave(&g_paging_lock);
    space = paging_find_space(process_id);
    free_range = paging_space_exact_range_free(space, virtual_address, rounded);
    kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
    return free_range;
}

bool paging_process_reserve_exact(uint32_t process_id,
                                  uint32_t virtual_address,
                                  uint32_t length) {
    paging_process_space_t *space;
    uint32_t end, rounded, lock_flags;
    if (!process_id || !length ||
        length > 0xFFFFFFFFU - (PAGING_PAGE_SIZE - 1U)) return false;
    rounded = (length + PAGING_PAGE_SIZE - 1U) & PAGE_FRAME_4K;
    if (!paging_process_create(process_id)) return false;

    lock_flags = kspin_lock_irqsave(&g_paging_lock);
    space = paging_find_space(process_id);
    if (!paging_space_exact_range_free(space, virtual_address, rounded)) {
        kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
        return false;
    }
    end = virtual_address + rounded;
    for (uint32_t page = virtual_address; page < end; page += PAGING_PAGE_SIZE) {
        uint32_t *table = paging_private_table_locked(space, page >> 22U);
        if (!table) {
            for (uint32_t undo = virtual_address; undo < page;
                 undo += PAGING_PAGE_SIZE) {
                uint32_t *undo_table = NULL;
                for (uint32_t i = 0U; i < space->table_count; i++)
                    if (space->pde_index[i] == (undo >> 22U)) {
                        undo_table = space->table[i];
                        break;
                    }
                if (undo_table)
                    undo_table[(undo >> 12U) & 1023U] =
                        paging_kernel_entry(undo) & ~PAGE_USER;
            }
            kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
            return false;
        }
        /* Deliberately non-present: PAGE_USER_NONE marks ownership/reservation
         * without consuming a physical frame. A later MAP_FIXED may replace
         * this PTE with committed backing. */
        table[(page >> 12U) & 1023U] = PAGE_USER_NONE | PAGE_PRIVATE_PTE;
    }
    if (paging_current_directory() == space->directory) paging_reload_cr3();
    kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
    return true;
}

bool paging_process_unmap(uint32_t process_id, uint32_t virtual_address,
                          uint32_t length) {
    paging_process_space_t *space;
    uint32_t raw_end, end, lock_flags;
    bool resident=false;
    if (!process_id || !length ||
        (virtual_address & (PAGING_PAGE_SIZE - 1U)) ||
        virtual_address + length < virtual_address ||
        length > 0xFFFFFFFFU - (PAGING_PAGE_SIZE - 1U)) return false;
    raw_end = virtual_address + length;
    if (raw_end > 0xFFFFFFFFU - (PAGING_PAGE_SIZE - 1U)) return false;
    end = (raw_end + PAGING_PAGE_SIZE - 1U) & PAGE_FRAME_4K;
    lock_flags = kspin_lock_irqsave(&g_paging_lock);
    space = paging_find_space(process_id);
    if (!space) {
        kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
        return false;
    }

    /* Validate the whole range first so a bad munmap cannot partially tear a
     * mapping down. */
    for (uint32_t page = virtual_address; page < end; page += PAGING_PAGE_SIZE) {
        uint32_t entry = paging_space_entry(space, page);
        if(entry & PAGE_PRESENT) resident=true;
        if (!(entry & PAGE_PRIVATE_PTE) ||
            (!(entry & (PAGE_USER_NONE|PAGE_PAGED)) && (entry & (PAGE_PRESENT | PAGE_USER)) !=
             (PAGE_PRESENT | PAGE_USER))) {
            kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
            return false;
        }
    }
    for (uint32_t page = virtual_address; page < end; page += PAGING_PAGE_SIZE) {
        uint32_t *table = NULL;
        for (uint32_t i = 0U; i < space->table_count; i++)
            if (space->pde_index[i] == (page >> 22U)) {
                table = space->table[i];
                break;
            }
        if (!table) {
            kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
            return false;
        }
        table[(page >> 12U) & 1023U] = paging_kernel_entry(page) & ~PAGE_USER;
        if (paging_current_directory() == space->directory) paging_invlpg(page);
    }
    /* Only mappings in the automatic mmap arena participate in its search
     * hint.  Wine also owns exact private mappings far above that arena. */
    if (virtual_address >= PAGING_USER_VM_START &&
        virtual_address < PAGING_USER_VM_END &&
        virtual_address < space->user_vm_hint)
        space->user_vm_hint = virtual_address;
    uint32_t generation=paging_publish_change();
    kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
    /* Pager translations were already revoked with an acknowledged shootdown.
     * Removing nonpresent reservations cannot leave a usable stale frame. */
    if(!resident)return true;
    {
        uint32_t mask = paging_kick_generation(process_id);
        return paging_wait_generation(generation, mask, process_id);
    }
    return true;
}

bool paging_process_protect(uint32_t process_id,uint32_t virtual_address,
                            uint32_t length,bool writable) {
    return paging_process_access(process_id,virtual_address,length,true,writable);
}

static bool paging_process_access_mode(uint32_t process_id,uint32_t virtual_address,
                            uint32_t length,bool accessible,bool writable,bool cow) {
    paging_process_space_t *space;
    uint32_t first,end,lock_flags;
    if(!paging_range_bounds(virtual_address,length,&first,&end)) return false;
    virtual_address=first;
    lock_flags=kspin_lock_irqsave(&g_paging_lock);
    space=paging_find_space(process_id);
    if(!space) { kspin_unlock_irqrestore(&g_paging_lock,lock_flags); return false; }
    for(uint32_t page=virtual_address;page<end;page+=PAGING_PAGE_SIZE) {
        uint32_t *table=NULL;
        for(uint32_t i=0U;i<space->table_count;i++)
            if(space->pde_index[i]==(page>>22U)) { table=space->table[i]; break; }
        uint32_t entry=table?table[(page>>12U)&1023U]:0U;
        /* A private table also contains inherited kernel PTEs. Table ownership
         * alone never authorizes granting Ring 3 access to those pages. */
        if(!(entry&PAGE_PRIVATE_PTE) || !(entry&(PAGE_PRESENT|PAGE_PAGED)) ||
           !(entry&(PAGE_USER|PAGE_USER_NONE))) {
            kspin_unlock_irqrestore(&g_paging_lock,lock_flags); return false;
        }
    }
    bool changed = false;
    for(uint32_t page=virtual_address;page<end;page+=PAGING_PAGE_SIZE) {
        uint32_t *table=NULL;
        for(uint32_t i=0;i<space->table_count;i++)
            if(space->pde_index[i]==(page>>22U)) {table=space->table[i];break;}
        uint32_t *pte=&table[(page>>12U)&1023U];
        uint32_t value = *pte &
            ~(PAGE_WRITABLE|PAGE_USER|PAGE_USER_NONE|PAGE_COPY_ON_WRITE);
        value |= PAGE_PRIVATE_PTE|(accessible?PAGE_USER:PAGE_USER_NONE);
        if(writable) value |= cow ? PAGE_COPY_ON_WRITE : PAGE_WRITABLE;
        if (value != *pte) { *pte = value; changed = true; }
    }
    /* Wine recommits an entire heap prefix while adding a few pages. An
     * unchanged PTE needs no TLB shootdown or cross-CPU acknowledgement. */
    if (!changed) {
        kspin_unlock_irqrestore(&g_paging_lock,lock_flags);
        return true;
    }
    uint32_t generation=paging_publish_change();
    kspin_unlock_irqrestore(&g_paging_lock,lock_flags);
    {
        uint32_t mask = paging_kick_generation(process_id);
        paging_wait_generation(generation, mask, process_id);
    }
    return true;
}

bool paging_process_access(uint32_t process_id,uint32_t address,uint32_t length,
                            bool accessible,bool writable) {
    return paging_process_access_mode(process_id,address,length,accessible,writable,false);
}

bool paging_process_cow_access(uint32_t process_id,uint32_t address,uint32_t length,
                                bool accessible,bool writable) {
    return paging_process_access_mode(process_id,address,length,accessible,writable,true);
}

bool paging_cow_write_pending(uint32_t address) {
    return (paging_entry_flags(address,NULL) & (PAGE_PRESENT|PAGE_USER|PAGE_COPY_ON_WRITE)) ==
        (PAGE_PRESENT|PAGE_USER|PAGE_COPY_ON_WRITE);
}

/* Replace a private view's backing while preserving each page's mprotect
 * permissions. A first write copies the whole view; readonly pages stay RO. */
bool paging_process_cow_detach(uint32_t process_id,uint32_t address,
                                uint32_t length,uint32_t backing) {
    if (!length || ((address|length|backing)&4095U) || address+length<address) return false;
    uint32_t lock=kspin_lock_irqsave(&g_paging_lock);
    paging_process_space_t *space=paging_find_space(process_id);
    if (!space) { kspin_unlock_irqrestore(&g_paging_lock,lock);return false; }
    for(uint32_t off=0;off<length;off+=4096U) {
        uint32_t pte=paging_space_entry(space,address+off);
        if (!(pte&PAGE_PRESENT) || !(pte&PAGE_PRIVATE_PTE)) {
            kspin_unlock_irqrestore(&g_paging_lock,lock);return false;
        }
    }
    for(uint32_t off=0;off<length;off+=4096U) {
        uint32_t *table=NULL;
        for(uint32_t i=0;i<space->table_count;i++)
            if(space->pde_index[i]==((address+off)>>22U)){table=space->table[i];break;}
        uint32_t *pte=&table[((address+off)>>12U)&1023U];
        uint32_t flags=*pte&~PAGE_FRAME_4K;
        if(flags&PAGE_COPY_ON_WRITE) flags|=PAGE_WRITABLE;
        *pte=(backing+off)|(flags&~PAGE_COPY_ON_WRITE);
    }
    uint32_t generation=paging_publish_change();
    kspin_unlock_irqrestore(&g_paging_lock,lock);
    {
        uint32_t mask = paging_kick_generation(process_id);
        paging_wait_generation(generation, mask, process_id);
    }
    return true;
}

void paging_switch_process(uint32_t process_id) {
    uint32_t *directory=kernel_page_directory;
    uint32_t cpu=paging_cpu();
    paging_process_space_t *space=paging_find_space(process_id);
    if(space) directory=space->directory;
    /* Capture before flushing. A concurrent later edit must remain unseen
     * so paging_sync_cpu() will flush it instead of acknowledging stale TLBs. */
    uint32_t generation=g_tlb_generation;
    if(paging_current_directory()!=directory || g_tlb_seen[cpu]!=generation)
        __asm__ volatile ("movl %0,%%cr3" : :
            "r"((uint32_t)(uintptr_t)directory) : "memory");
    g_active_process[cpu]=space?process_id:0U;
    g_tlb_seen[cpu]=generation;
}

uint32_t paging_user_physical(uint32_t address) {
    paging_process_space_t *space=paging_find_space(paging_current_process());
    uint32_t entry=space?paging_space_entry(space,address):paging_kernel_entry(address);
    return (entry&(PAGE_PRESENT|PAGE_USER))==(PAGE_PRESENT|PAGE_USER)
        ? (entry&PAGE_FRAME_4K)|(address&4095U) : 0;
}

/* Copia privada preservando permisos por pagina, tambien para PROT_NONE.
 * El hijo debe seguir suspendido hasta completar todas sus regiones. */
bool paging_process_copy(uint32_t parent,uint32_t child,uint32_t address,
                         uint32_t backing,uint32_t length) {
    if (!length || ((address|backing|length)&4095U) || address+length<address ||
        !paging_process_create(child)) return false;
    uint32_t lock=kspin_lock_irqsave(&g_paging_lock);
    paging_process_space_t *src=paging_find_space(parent),*dst=paging_find_space(child);
    bool ok=src && dst;
    for (uint32_t off=0;ok && off<length;off+=4096) {
        uint32_t pte=paging_space_entry(src,address+off);
        if (!(pte&PAGE_PRESENT) || !(pte&(PAGE_USER|PAGE_USER_NONE))) {ok=false;break;}
        uint32_t *table=paging_private_table_locked(dst,(address+off)>>22U);
        if (!table) {ok=false;break;}
        kmemcpy((void *)(backing+off),(void *)(pte&PAGE_FRAME_4K),4096);
        table[((address+off)>>12U)&1023U]=(backing+off)|(pte&0xfffU)|PAGE_PRIVATE_PTE;
    }
    kspin_unlock_irqrestore(&g_paging_lock,lock);return ok;
}

bool paging_process_share(uint32_t parent,uint32_t child,uint32_t address,uint32_t length) {
    if(!length || ((address|length)&4095) || address+length<address || !paging_process_create(child))return false;
    uint32_t lock=kspin_lock_irqsave(&g_paging_lock);
    paging_process_space_t *src=paging_find_space(parent),*dst=paging_find_space(child);
    bool ok=src && dst;
    for(uint32_t off=0;ok && off<length;off+=4096) {
        uint32_t pte=paging_space_entry(src,address+off);
        uint32_t *table=paging_private_table_locked(dst,(address+off)>>22);
        if(!table || !(pte&PAGE_PRESENT) || !(pte&(PAGE_USER|PAGE_USER_NONE))){ok=false;break;}
        table[((address+off)>>12)&1023]=pte|PAGE_PRIVATE_PTE;
    }
    kspin_unlock_irqrestore(&g_paging_lock,lock);return ok;
}

bool paging_process_write(uint32_t process,uint32_t address,const void *data,uint32_t length) {
    if (!length || address+length<address) return false;
    if(!pager_pin(process,address,length,true))return false;
    uint32_t lock=kspin_lock_irqsave(&g_paging_lock);
    paging_process_space_t *space=paging_find_space(process);
    bool ok=space!=NULL;
    for (uint32_t off=0;ok && off<length;) {
        uint32_t pte=paging_space_entry(space,address+off),n=4096-((address+off)&4095);
        if ((pte&(PAGE_PRESENT|PAGE_USER|PAGE_WRITABLE))!=7) {ok=false;break;}
        if(n>length-off)n=length-off;
        kmemcpy((void *)((pte&PAGE_FRAME_4K)|((address+off)&4095)),(const uint8_t *)data+off,n);
        off+=n;
    }
    kspin_unlock_irqrestore(&g_paging_lock,lock);
    pager_unpin(process,address,length);return ok;
}

void paging_swap_process_ids(uint32_t first,uint32_t second) {
    uint32_t flags=kspin_lock_irqsave(&g_paging_lock);
    paging_process_space_t *a=paging_find_raw_space(first),*b=paging_find_raw_space(second);
    if(a)a->process_id=second;
    if(b)b->process_id=first;
    for(uint32_t i=0;i<SMP_MAX_CPUS;i++) {
        if(g_active_process[i]==first) g_active_process[i]=second;
        else if(g_active_process[i]==second) g_active_process[i]=first;
    }
    kspin_unlock_irqrestore(&g_paging_lock,flags);
}

uint32_t paging_current_process(void) {
    uint32_t cpu=paging_cpu();
    return g_active_process[cpu];
}

void paging_process_destroy(uint32_t process_id) {
    paging_process_space_t *space;
    uint32_t *directory = NULL;
    uint32_t *tables[PAGING_PRIVATE_TABLE_MAX];
    uint32_t count=0U,flags;
    if(!process_id) return;
    flags=kspin_lock_irqsave(&g_paging_lock);
    space=paging_find_raw_space(process_id);
    if(space) {
        directory=space->directory;
        count=space->table_count;
        for(uint32_t i=0U;i<count;i++) tables[i]=space->table[i];
        kmemset(space,0,sizeof(*space));
    }
    kspin_unlock_irqrestore(&g_paging_lock,flags);
    for(uint32_t i=0U;i<count;i++) paging_free_private_page(tables[i]);
    paging_free_private_page(directory);
}

bool paging_is_enabled(void) { return paging_enabled; }

uint32_t paging_fault_address(void) {
    uint32_t address;
    __asm__ volatile ("movl %%cr2,%0" : "=r"(address));
    return address;
}

bool paging_pager_reserve(uint32_t process_id, uint32_t requested,
    uint32_t length, bool accessible, bool writable, bool exact,
    uint32_t *virtual_out, paging_range_skip_t skip, void *context) {
    paging_process_space_t *space;
    uint32_t address, end, rounded, flags, lock_flags;
    if (virtual_out) *virtual_out = 0U;
    if (!process_id || !length ||
        length > 0xFFFFFFFFU - (PAGING_PAGE_SIZE - 1U)) return false;
    rounded = (length + PAGING_PAGE_SIZE - 1U) & PAGE_FRAME_4K;
    if (!paging_process_create(process_id)) return false;

    lock_flags = kspin_lock_irqsave(&g_paging_lock);
    space = paging_find_space(process_id);
    if (!space) {
        kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
        return false;
    }
    address = exact ? (paging_space_exact_range_free(space,requested,rounded) ? requested : 0U)
                    : paging_space_find_user_range(space, requested, rounded, skip, context);
    if (!address) {
        kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
        return false;
    }
    end = address + rounded;
    flags = PAGE_PAGED | PAGE_PRIVATE_PTE | (accessible ? PAGE_USER : PAGE_USER_NONE) | (writable ? PAGE_WRITABLE : 0U);
    for (uint32_t page = address; page < end; page += PAGING_PAGE_SIZE) {
        uint32_t *table = paging_private_table_locked(space, page >> 22U);
        if (!table) {
            for (uint32_t undo = address; undo < page; undo += PAGING_PAGE_SIZE) {
                uint32_t *undo_table = NULL;
                for (uint32_t i = 0U; i < space->table_count; i++)
                    if (space->pde_index[i] == (undo >> 22U)) {
                        undo_table = space->table[i];
                        break;
                    }
                if (undo_table)
                    undo_table[(undo >> 12U) & 1023U] =
                        paging_kernel_entry(undo) & ~PAGE_USER;
            }
            kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
            return false;
        }
        table[(page >> 12U) & 1023U] =
            flags;
    }
    space->user_vm_hint = end < PAGING_USER_VM_END ? end : PAGING_USER_VM_START;
    if (paging_current_directory() == space->directory) paging_reload_cr3();
    kspin_unlock_irqrestore(&g_paging_lock, lock_flags);
    if (virtual_out) *virtual_out = address;
    return true;
}

/* Pager changes must acknowledge every affected CPU before a frame is reused.
 * On timeout restore the old PTE and retain the frame; an unacknowledged
 * translation must never point into another process's recycled page. */
uint32_t paging_pager_entry(uint32_t pid,uint32_t address) {
    uint32_t flags=kspin_lock_irqsave(&g_paging_lock);
    paging_process_space_t *space=paging_find_space(pid);
    uint32_t result=space?paging_space_entry(space,address):0U;
    kspin_unlock_irqrestore(&g_paging_lock,flags);return result;
}
bool paging_pager_adopt(uint32_t pid,uint32_t address,uint32_t length) {
    if(!length || ((address|length)&4095U) || address+length<address)return false;
    uint32_t flags=kspin_lock_irqsave(&g_paging_lock);
    paging_process_space_t *space=paging_find_space(pid);
    bool ok=space!=NULL;
    for(uint32_t off=0;ok && off<length;off+=4096U) {
        uint32_t entry=paging_space_entry(space,address+off);
        if(!(entry&PAGE_PRIVATE_PTE) || !(entry&PAGE_PRESENT))ok=false;
    }
    if(ok)for(uint32_t off=0;off<length;off+=4096U)
        for(uint32_t i=0;i<space->table_count;i++)
            if(space->pde_index[i]==((address+off)>>22U)) {
                space->table[i][((address+off)>>12U)&1023U]|=PAGE_PAGED;break;
            }
    /* PAGE_PAGED is software bookkeeping; access permissions did not change. */
    kspin_unlock_irqrestore(&g_paging_lock,flags);return ok;
}
bool paging_pager_replace(uint32_t pid,uint32_t address,uint32_t frame,bool present) {
    return paging_pager_replace_cow(pid,address,frame,present,false);
}
bool paging_pager_replace_cow(uint32_t pid,uint32_t address,uint32_t frame,bool present,bool cow) {
    uint32_t flags=kspin_lock_irqsave(&g_paging_lock);
    paging_process_space_t *space=paging_find_space(pid);
    uint32_t *pte=NULL;
    if(space)for(uint32_t i=0;i<space->table_count;i++)
        if(space->pde_index[i]==(address>>22))pte=&space->table[i][(address>>12)&1023U];
    if(!pte || !(*pte&PAGE_PAGED)){kspin_unlock_irqrestore(&g_paging_lock,flags);return false;}
    uint32_t old=*pte;
    uint32_t access=old&0xe06U;
    if(cow && (access&(PAGE_WRITABLE|PAGE_COPY_ON_WRITE)))
        access=(access&~PAGE_WRITABLE)|PAGE_COPY_ON_WRITE;
    else if(!cow && (access&PAGE_COPY_ON_WRITE))
        access=(access&~PAGE_COPY_ON_WRITE)|PAGE_WRITABLE;
    *pte=access|PAGE_PAGED|PAGE_PRIVATE_PTE|(present?(frame|PAGE_PRESENT):0U);
    uint32_t generation=paging_publish_change();
    kspin_unlock_irqrestore(&g_paging_lock,flags);
    uint32_t mask=paging_kick_generation(pid);
    /* Publishing a new frame does not release an old one. Keep it mapped
     * even if an AP is late; never expose then recycle that new frame. */
    if(present){(void)paging_wait_generation(generation,mask,pid);return true;}
    if(paging_wait_generation(generation,mask,pid))return true;
    flags=kspin_lock_irqsave(&g_paging_lock);*pte=old;
    generation=paging_publish_change();kspin_unlock_irqrestore(&g_paging_lock,flags);
    (void)paging_wait_generation(generation,paging_kick_generation(pid),pid);
    return false;
}

uint32_t paging_process_owner(uint32_t pid) {
    uint32_t flags=kspin_lock_irqsave(&g_paging_lock);
    paging_process_space_t *s=paging_find_space(pid);
    uint32_t owner=s?s->process_id:pid;
    kspin_unlock_irqrestore(&g_paging_lock,flags);return owner;
}
