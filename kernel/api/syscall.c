#define SYS_API_MAX_CALLEE_CLEANUP 64U
#include "../include/pager.h"
#include "../include/syscall.h"
#include "../include/api.h"
#include "../include/task.h"
#include "../include/gdt.h"
#include "../include/klock.h"
#include "../include/kernel_domains.h"
#include "../include/pit.h"
#include "../include/rtc.h"
#include "../include/vga.h"
#include "../include/vfs.h"
#include "../include/memory.h"
#include "../include/paging.h"
#include "../include/usercopy.h"
#include "../include/elf_loader.h"
#include "../include/network.h"
#include "../include/w3_profile.h"
#include "../include/gfx3d.h"
#include "../include/sound.h"
#include "../../gui/gui.h"
#include "../../ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"

#define SYSCALL_WRITE_MAX       65536U
#define SYSCALL_FILE_IO_MAX     (32U * 1024U * 1024U)
#define SYSCALL_ALLOC_MAX       (64U * 1024U * 1024U)
#define SYSCALL_RESOURCE_SLOTS  TASK_MAX
/* A process table also contains sockets, pipes and duplicated handles.
 * Wine's server needs more entries than the global disk-file table. */
#define SYSCALL_FD_SLOTS        509U /* 512 including standard descriptors */
#define SYSCALL_ALLOC_INITIAL   128U
#define SYSCALL_ALLOC_LIMIT     8192U
#define LINUX_LDT_ENTRIES       8192U
#define LINUX_LDT_BYTES         (LINUX_LDT_ENTRIES * 8U)
/* Wine creates a dense VMA layout: PE/ELF images, reserve_area() holes,
 * thread stacks, font caches and MAP_FIXED_NOREPLACE probes all consume
 * entries here. 64 entries was enough for the early W1/W2 gate and 256 got
 * wineboot/services through W3, but real Office 97 workloads go further:
 * WINWORD.EXE reaches 256 live VMAs shortly after its first surface and every
 * subsequent mmap() then fails with ENOMEM. That presents as dead input and
 * apparently random application exits even though physical VM frames remain.
 *
 * Keep the common case small (the table still starts at 64 and grows on
 * demand), but leave enough metadata headroom for a long-lived Wine process.
 * Quake II with modern Gallium also exhausts 1024 descriptors during texture
 * loading. Allow up to 8192 entries (192 KiB), allocated only as needed;
 * small applications still start with a single metadata page. */
#define SYSCALL_VM_MAP_INITIAL  64U
#define SYSCALL_VM_MAP_LIMIT    8192U
#define SYSCALL_VM_PRIVATE_CACHE_MAX VFS_PRIVATE_IMAGE_MAX_BYTES
#define SYSCALL_LINUX_BRK_RESERVE (4U * 1024U * 1024U)
#define LINUX_IO_BASE 1000
#define LINUX_IO_PAIRS 512U
#define LINUX_IO_ENDPOINTS (2U * LINUX_IO_PAIRS)
#define LINUX_DEVICE_BASE (LINUX_IO_BASE + LINUX_IO_ENDPOINTS)
static int linux_device_open(unsigned kind, uint32_t flags);
#define SYS_API_KERNEL_STACK_RESERVE (16U * 1024U)

typedef struct {
    uint32_t backing;       /* supervisor identity physical run */
    uint32_t address;       /* process-private virtual base */
    uint32_t length;
    uint32_t protection;
    uint32_t flags;
    vfs_shared_mapping_t *shared;
} syscall_vm_map_t;

typedef struct {
    syscall_vm_map_t *maps;
    uint32_t map_capacity;
    uint32_t brk_backing;
    uint32_t brk_base;
    uint32_t brk_current;
    uint32_t brk_capacity;
} syscall_vm_state_t;

typedef struct {
    int fds[SYSCALL_FD_SLOTS];
    uint8_t flags[SYSCALL_FD_SLOTS+3];
    uint32_t open_flags[SYSCALL_FD_SLOTS+3];
} syscall_fd_table_t;

typedef struct {
    uint32_t process_id;
    int stdio_fds[3];
    int *fds;
    uint8_t *linux_fd_flags;
    uint32_t *linux_open_flags;
    uint16_t linux_umask;
    uint32_t lock_wait_owner;
    void **allocations;
    uint32_t allocation_capacity;
    /* VM bookkeeping is kernel metadata: keep it in the supervisor-only
     * static resource table, never in the legacy PAGE_USER heap. */
    syscall_vm_state_t vm;
    bool vm_initialized;
    uint32_t vm_borrowed_from;
    uint8_t *linux_ldt;
    uint32_t linux_ldt_highwater;
    uint32_t vm_perf_mmap_calls;
    uint32_t vm_perf_file_maps;
    uint32_t vm_perf_cached_file_maps;
    uint32_t vm_perf_cache_hits;
    uint32_t vm_perf_cache_misses;
    uint32_t vm_perf_fallback_reads;
    uint32_t vm_perf_file_io_kib;
    uint32_t vm_perf_file_io_ticks;
    int net_sockets[NET_SOCKET_MAX];
} syscall_resources_t;

static syscall_resources_t resources[SYSCALL_RESOURCE_SLOTS];

/* Cumulative VM counters survive short-lived Wine helper processes.  Per-PID
 * counters disappear when syscall_process_cleanup() releases a process, which
 * made the first-surface profiler miss most of wine-preloader/wineserver's
 * work.  The launcher samples these totals before spawn and subtracts them at
 * the first visible surface. */
typedef struct {
    uint32_t mmap_calls;
    uint32_t file_maps;
    uint32_t cached_file_maps;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t fallback_reads;
    uint32_t file_io_kib;
    uint32_t file_io_ticks;
} syscall_vm_perf_totals_t;
static syscall_vm_perf_totals_t g_vm_perf_totals;

/* Boot completion is a process-scoped handshake, never a persistent marker. */
static volatile uint32_t g_wine_boot_pid;
static volatile int32_t g_wine_boot_result;
void syscall_wine_boot_begin(uint32_t pid) {
    g_wine_boot_pid=pid;
    __sync_lock_test_and_set(&g_wine_boot_result,0);
}
int32_t syscall_wine_boot_result(void) {
    return __sync_val_compare_and_swap(&g_wine_boot_result,0,0);
}

/* VMA descriptors contain physical backing and trusted ownership/protection
 * state. The legacy heap is PAGE_USER; an owner tag there is not isolation.
 * Use the existing supervisor identity pool, just like private page tables.
 * Capacity is bounded by SYSCALL_VM_MAP_LIMIT before computing page counts. */
static uint32_t sys_vm_map_pages(uint32_t capacity) {
    return (capacity * sizeof(syscall_vm_map_t) + PAGING_PAGE_SIZE - 1U) /
        PAGING_PAGE_SIZE;
}

static bool sys_vm_maps_reserve(syscall_vm_state_t *vm, uint32_t wanted) {
    syscall_vm_map_t *grown;
    syscall_vm_map_t *old_maps;
    uint32_t physical;
    uint32_t old_capacity, new_capacity;
    if (!vm || wanted > SYSCALL_VM_MAP_LIMIT) return false;
    if (vm->map_capacity >= wanted) return true;
    old_capacity = vm->map_capacity;
    new_capacity = old_capacity ? old_capacity : SYSCALL_VM_MAP_INITIAL;
    while (new_capacity < wanted && new_capacity < SYSCALL_VM_MAP_LIMIT) {
        uint32_t next = new_capacity * 2U;
        new_capacity = next > SYSCALL_VM_MAP_LIMIT ? SYSCALL_VM_MAP_LIMIT : next;
    }
    if (new_capacity < wanted) return false;
    if (!mm_vm_frame_alloc(sys_vm_map_pages(new_capacity), &physical))
        return false; /* Preserve both descriptors and capacity on OOM. */
    grown = (syscall_vm_map_t *)(uintptr_t)physical;
    old_maps = vm->maps;
    if (old_capacity)
        kmemcpy(grown, old_maps, old_capacity * sizeof(syscall_vm_map_t));
    kmemset(grown + old_capacity, 0,
        (new_capacity - old_capacity) * sizeof(syscall_vm_map_t));
    vm->maps = grown;
    vm->map_capacity = new_capacity;
    if (old_maps)
        mm_vm_frame_free((uint32_t)(uintptr_t)old_maps,
                         sys_vm_map_pages(old_capacity));
    return true;
}

static void sys_vm_maps_release(syscall_vm_state_t *vm) {
    if (!vm) return;
    if (vm->maps)
        mm_vm_frame_free((uint32_t)(uintptr_t)vm->maps,
                         sys_vm_map_pages(vm->map_capacity));
    vm->maps = NULL;
    vm->map_capacity = 0U;
}

static syscall_vm_map_t *sys_vm_free_map_slot(syscall_vm_state_t *vm,
                                               bool grow) {
    uint32_t old_capacity;
    if (!vm) return NULL;
    for (uint32_t i = 0U; i < vm->map_capacity; i++)
        if (!vm->maps[i].address) return &vm->maps[i];
    if (!grow || vm->map_capacity >= SYSCALL_VM_MAP_LIMIT) return NULL;
    old_capacity = vm->map_capacity;
    if (!sys_vm_maps_reserve(vm, old_capacity ? old_capacity + 1U
                                              : SYSCALL_VM_MAP_INITIAL))
        return NULL;
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[VM:MAPS] grow %u -> %u slots\n", old_capacity, vm->map_capacity);
#endif
    return &vm->maps[old_capacity];
}

static bool sys_vm_ensure_free_slots(syscall_vm_state_t *vm, uint32_t needed) {
    uint32_t free_slots = 0U;
    uint32_t old_capacity;
    if (!vm) return false;
    for (uint32_t i = 0U; i < vm->map_capacity; i++)
        if (!vm->maps[i].address) free_slots++;
    if (free_slots >= needed) return true;
    old_capacity = vm->map_capacity;
    while (free_slots < needed && vm->map_capacity < SYSCALL_VM_MAP_LIMIT) {
        uint32_t target = vm->map_capacity ? vm->map_capacity * 2U
                                           : SYSCALL_VM_MAP_INITIAL;
        if (target > SYSCALL_VM_MAP_LIMIT) target = SYSCALL_VM_MAP_LIMIT;
        if (!sys_vm_maps_reserve(vm, target)) return false;
        free_slots += vm->map_capacity - old_capacity;
        old_capacity = vm->map_capacity;
    }
    return free_slots >= needed;
}
#include "../linux/i386_lock_state.inc"
static bool sys_raw_close(int fd);
static bool sys_raw_retain(int fd);
static int32_t sys_raw_read(int fd,void *buffer,uint32_t length);
static int32_t sys_raw_write(int fd,const void *buffer,uint32_t length);
typedef struct {
    uint32_t process_id;
    uint32_t dispatcher;
} syscall_signal_registration_t;
static syscall_signal_registration_t signal_registrations[TASK_MAX];

static syscall_signal_registration_t *signal_registration_for(
        uint32_t process_id, bool create) {
    syscall_signal_registration_t *free_slot = NULL;
    for (uint32_t i = 0U; i < TASK_MAX; i++) {
        if (signal_registrations[i].process_id == process_id)
            return &signal_registrations[i];
        if (!signal_registrations[i].process_id && !free_slot)
            free_slot = &signal_registrations[i];
    }
    if (!create || !free_slot) return NULL;
    free_slot->process_id = process_id;
    free_slot->dispatcher = 0U;
    return free_slot;
}

static int32_t allocation_free_slot(syscall_resources_t *resource) {
    void **grown;
    uint32_t old_capacity;
    uint32_t new_capacity;
    if (!resource) return -1;
    if (!resource->allocations) {
        resource->allocations = (void **)kzalloc(
            SYSCALL_ALLOC_INITIAL * sizeof(void *));
        if (!resource->allocations) return -1;
        resource->allocation_capacity = SYSCALL_ALLOC_INITIAL;
    }
    for (uint32_t i = 0; i < resource->allocation_capacity; i++)
        if (!resource->allocations[i]) return (int32_t)i;
    old_capacity = resource->allocation_capacity;
    if (old_capacity >= SYSCALL_ALLOC_LIMIT) return -1;
    new_capacity = old_capacity * 2U;
    if (new_capacity > SYSCALL_ALLOC_LIMIT)
        new_capacity = SYSCALL_ALLOC_LIMIT;
    grown = (void **)krealloc(resource->allocations,
                              new_capacity * sizeof(void *));
    if (!grown) return -1;
    kmemset(grown + old_capacity, 0,
            (new_capacity - old_capacity) * sizeof(void *));
    resource->allocations = grown;
    resource->allocation_capacity = new_capacity;
    return (int32_t)old_capacity;
}

static bool caller_is_user(const registers_t *regs) {
    return regs && (regs->cs & 3U) == 3U;
}

/* All user pointers are now checked against the actual page tables. Kernel
 * callers keep the historical direct-pointer path, while Ring-3 callers may
 * only touch present PAGE_USER mappings and must additionally have writable
 * permission for output buffers. */
static bool user_range_ok(const registers_t *regs, const void *pointer,
                          uint32_t length) {
    if (!caller_is_user(regs)) return pointer != NULL || length == 0U;
    return user_access_ok(pointer, length, false);
}

static bool user_write_range_ok(const registers_t *regs, void *pointer,
                                uint32_t length) {
    if (!caller_is_user(regs)) return pointer != NULL || length == 0U;
    return user_access_ok(pointer, length, true);
}

static bool user_mapping_range_ok(const registers_t *regs,const void *pointer,uint32_t length) {
    return caller_is_user(regs)?paging_user_range_permitted(pointer,length,false):pointer!=NULL || !length;
}
static bool user_mapping_write_ok(const registers_t *regs,void *pointer,uint32_t length) {
    return caller_is_user(regs)?paging_user_range_permitted(pointer,length,true):pointer!=NULL || !length;
}

static bool copy_user_string(const registers_t *regs, const char *source,
                             char *destination, uint32_t capacity) {
    if (!source || !destination || capacity < 2U) return false;
    if (caller_is_user(regs))
        return copy_string_from_user(destination, capacity, source);
    for (uint32_t i = 0U; i < capacity; i++) {
        destination[i] = source[i];
        if (!destination[i]) return true;
    }
    destination[capacity - 1U] = '\0';
    return false;
}

static syscall_resources_t *resource_for(uint32_t process_id, bool create) {
    syscall_resources_t *free_slot = NULL;

    for (uint32_t i = 0; i < SYSCALL_RESOURCE_SLOTS; i++) {
        if (resources[i].process_id == process_id) return &resources[i];
        if (!resources[i].process_id && !free_slot) free_slot = &resources[i];
    }
    if (!create || !free_slot) return NULL;
    kmemset(free_slot, 0, sizeof(*free_slot));
    /* Allocate descriptor tables only for live processes. The resource
     * metadata itself needs a slot for every process the scheduler admits. */
    syscall_fd_table_t *table = kmalloc(sizeof(*table));
    if (!table) return NULL;
    (void)mm_set_allocation_owner(table, 0U);
    kmemset(table, 0, sizeof(*table));
    free_slot->fds = table->fds;
    free_slot->linux_fd_flags = table->flags;
    free_slot->linux_open_flags = table->open_flags;
    free_slot->process_id = process_id;
    free_slot->linux_umask = 0022U;
    for (uint32_t i = 0; i < 3U; i++)
        free_slot->stdio_fds[i] = linux_device_open(i?1:0,i?1:0);
    free_slot->linux_open_flags[1]=free_slot->linux_open_flags[2]=1U;
    for (uint32_t i = 0; i < SYSCALL_FD_SLOTS; i++) free_slot->fds[i] = -1;
    for (uint32_t i = 0; i < NET_SOCKET_MAX; i++) free_slot->net_sockets[i] = -1;
    return free_slot;
}

void syscall_process_cleanup(uint32_t process_id) {
    kernel_domains_enter(KDOMAIN_VFS);
    libc_process_stdio_cleanup(process_id);
    kernel_domains_exit(KDOMAIN_VFS);
    syscall_resources_t *resource = resource_for(process_id, false);
    syscall_signal_registration_t *signal =
        signal_registration_for(process_id, false);
    if (signal) kmemset(signal, 0, sizeof(*signal));
    if (!resource) {pager_destroy(process_id);return;}
    kernel_domains_enter(KDOMAIN_VFS);
    linux_unlock_file(process_id,-1);
    for (uint32_t i = 0; i < 3U; i++) {
        if (resource->stdio_fds[i] >= 0) (void)sys_raw_close(resource->stdio_fds[i]);
    }
    for (uint32_t i = 0; i < SYSCALL_FD_SLOTS; i++) {
        if (resource->fds[i] >= 0) (void)sys_raw_close(resource->fds[i]);
    }
    for (uint32_t i = 0; i < resource->allocation_capacity; i++) {
        if (resource->allocations[i]) kfree(resource->allocations[i]);
    }
    if (resource->allocations) kfree(resource->allocations);
    if (resource->linux_ldt) kfree(resource->linux_ldt);
    if (resource->vm_initialized) {
        for (uint32_t i = 0; i < resource->vm.map_capacity; i++)
            if (resource->vm.maps[i].address) {
                syscall_vm_map_t *map = &resource->vm.maps[i];
                if(map->flags&VM_MAP_PAGED)
                    (void)pager_unmap(process_id,map->address,map->length);
                else (void)paging_process_unmap(process_id, map->address, map->length);
                if(map->shared)(void)vfs_shared_release(map->shared);
                else if(map->backing)
                    mm_vm_frame_free(map->backing, map->length / PAGING_PAGE_SIZE);
            }
        sys_vm_maps_release(&resource->vm);
        if (resource->vm.brk_capacity) {
            (void)pager_unmap(process_id,resource->vm.brk_base,
                             resource->vm.brk_capacity);
        }
    }
    pager_destroy(process_id);
    kernel_domains_exit(KDOMAIN_VFS);
    for (uint32_t i = 0; i < NET_SOCKET_MAX; i++) {
        if (resource->net_sockets[i] >= 0)
            network_socket_close(resource->net_sockets[i]);
    }
    kfree(resource->fds); /* first member of syscall_fd_table_t */
    kmemset(resource, 0, sizeof(*resource));
}

static int resource_fd(syscall_resources_t *resource, uint32_t user_fd) {
    uint32_t slot;
    if (!resource) return -1;
    if (user_fd < 3U) return resource->stdio_fds[user_fd];
    slot = user_fd - 3U;
    return slot < SYSCALL_FD_SLOTS ? resource->fds[slot] : -1;
}

void syscall_inherit_files(uint32_t parent_process,
                           uint32_t child_process) {
    syscall_resources_t *parent = resource_for(parent_process, false);
    syscall_resources_t *child;
    if (!parent || !child_process) return;
    child = resource_for(child_process, true);
    if (!child) return;
    kmemcpy(child->linux_fd_flags,parent->linux_fd_flags,SYSCALL_FD_SLOTS+3U);
    kmemcpy(child->linux_open_flags,parent->linux_open_flags,(SYSCALL_FD_SLOTS+3U)*sizeof(uint32_t));
    child->linux_umask = parent->linux_umask;
    for (uint32_t i = 0U; i < 3U; i++) {
        if(child->stdio_fds[i]>=0) sys_raw_close(child->stdio_fds[i]);
        child->stdio_fds[i]=-1;
        if (parent->stdio_fds[i] >= 0 && sys_raw_retain(parent->stdio_fds[i]))
            child->stdio_fds[i] = parent->stdio_fds[i];
    }
    for (uint32_t i = 0U; i < SYSCALL_FD_SLOTS; i++) {
        if (parent->fds[i] >= 0 && sys_raw_retain(parent->fds[i]))
            child->fds[i] = parent->fds[i];
    }
}

bool syscall_linux_borrow_resources(uint32_t parent_id,uint32_t child_id) {
    syscall_resources_t *parent=resource_for(parent_id,false),*child=resource_for(child_id,true);
    if(!parent || !child) return false;
    syscall_inherit_files(parent_id,child_id);
    if (parent->linux_ldt) {
        child->linux_ldt = (uint8_t *)kmalloc(LINUX_LDT_BYTES);
        if (!child->linux_ldt) return false;
        (void)mm_set_allocation_owner(child->linux_ldt, 0U);
        kmemcpy(child->linux_ldt, parent->linux_ldt, LINUX_LDT_BYTES);
        child->linux_ldt_highwater = parent->linux_ldt_highwater;
    }
    child->vm_borrowed_from=parent->vm_borrowed_from?parent->vm_borrowed_from:parent_id;
    return true;
}

bool syscall_linux_clone_resources(uint32_t parent_id,uint32_t child_id) {
    syscall_resources_t *parent=resource_for(parent_id,false),*child=resource_for(child_id,true);
    uint32_t vm_before=mm_vm_frame_free_pages();
#ifndef BLESK_WINE_VERBOSE_TRACE
    (void)vm_before;
#endif
    if (!parent || !child) return false;
    syscall_inherit_files(parent_id,child_id);
    if (parent->linux_ldt) {
        child->linux_ldt = (uint8_t *)kmalloc(LINUX_LDT_BYTES);
        if (!child->linux_ldt) return false;
        (void)mm_set_allocation_owner(child->linux_ldt, 0U);
        kmemcpy(child->linux_ldt, parent->linux_ldt, LINUX_LDT_BYTES);
        child->linux_ldt_highwater = parent->linux_ldt_highwater;
    }
    if (!parent->vm_initialized) return true;
    kmemset(&child->vm, 0, sizeof(child->vm));
    if (!sys_vm_maps_reserve(&child->vm, parent->vm.map_capacity)) return false;
    child->vm_initialized=true;
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[LINUX:FORKVM] parent=%u child=%u begin free=%u\n",
            parent_id,child_id,vm_before);
#endif

    /* Reproduce virtual-only PROT_NONE reservations exactly as the parent
     * represents them: metadata only, with no user PTEs.  sys_vm_mmap() keeps
     * Wine's MAP_NORESERVE/PROT_NONE holes exclusively in vm.maps[] so they
     * may coexist with independently managed ELF/stack mappings.
     *
     * Older fork code called paging_process_reserve_exact() here.  That made
     * the child's representation stricter than the parent's by installing
     * PAGE_USER_NONE PTEs across the whole VMA.  The child already has its
     * Linux stack and ELF images cloned before this function runs, so a large
     * Wine reservation could collide with those valid PTEs and turn fork()
     * into a false ENOMEM (Wine surfaced it as STATUS_NO_MEMORY/c0000017).
     * Backed/private mappings are copied in pass two below and install their
     * real PTEs independently. */
    for (uint32_t i=0;i<parent->vm.map_capacity;i++) {
        syscall_vm_map_t *src=&parent->vm.maps[i],*dst=&child->vm.maps[i];
        if (!src->address || src->backing || src->shared) continue;
        *dst=*src;
        if((src->flags&VM_MAP_PAGED) &&
           !pager_clone(parent_id,child_id,src->address,src->length))return false;
#ifdef BLESK_WINE_VERBOSE_TRACE
        kprintf("[LINUX:FORKVM] parent=%u child=%u reserve-meta slot=%u addr=%x len=%u\n",
                parent_id,child_id,i,src->address,src->length);
#endif
    }
    for (uint32_t i=0;i<parent->vm.map_capacity;i++) {
        syscall_vm_map_t *src=&parent->vm.maps[i],*dst=&child->vm.maps[i];
        if (!src->address || !src->backing) continue;
        if(src->shared) {
            *dst=*src;dst->shared->references++;
            bool cloned=(src->flags&VM_MAP_PAGED)
                ? pager_clone(parent_id,child_id,src->address,src->length)
                : paging_process_share(parent_id,child_id,src->address,src->length);
            if(!cloned) {
                kprintf("[LINUX:FORKVM] parent=%u child=%u fail=share slot=%u addr=%x len=%u free=%u\n",
                        parent_id,child_id,i,src->address,src->length,
                        mm_vm_frame_free_pages());
                return false;
            }
            continue;
        }
        uint32_t backing;
        uint32_t pages=src->length/4096;
        if (!pager_frame_alloc(pages,&backing)) {
            kprintf("[LINUX:FORKVM] parent=%u child=%u fail=alloc slot=%u pages=%u len=%u free=%u\n",
                    parent_id,child_id,i,pages,src->length,
                    mm_vm_frame_free_pages());
            return false;
        }
        *dst=*src;dst->backing=backing;
        if (!paging_process_copy(parent_id,child_id,src->address,backing,src->length)) {
            kprintf("[LINUX:FORKVM] parent=%u child=%u fail=copy slot=%u addr=%x pages=%u free=%u\n",
                    parent_id,child_id,i,src->address,pages,
                    mm_vm_frame_free_pages());
            return false;
        }
    }
    if (parent->vm.brk_capacity) {
        child->vm.brk_base=parent->vm.brk_base;
        child->vm.brk_current=parent->vm.brk_current;child->vm.brk_capacity=parent->vm.brk_capacity;
        if(!pager_clone(parent_id,child_id,parent->vm.brk_base,parent->vm.brk_capacity))return false;
    }
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[LINUX:FORKVM] parent=%u child=%u ok free_before=%u free_after=%u consumed=%u\n",
            parent_id,child_id,vm_before,mm_vm_frame_free_pages(),
            vm_before>=mm_vm_frame_free_pages()?vm_before-mm_vm_frame_free_pages():0U);
#endif
    return true;
}

static int resource_socket(syscall_resources_t *resource, uint32_t handle) {
    if (!resource || handle >= NET_SOCKET_MAX) return -1;
    return resource->net_sockets[handle];
}

static int32_t sys_net_socket(uint32_t type) {
    syscall_resources_t *resource =
        resource_for(task_current_process_id(), true);
    int raw_socket;
    if (!resource || type != NET_SOCKET_TCP) return -BK_EINVAL;
    for (uint32_t i = 0; i < NET_SOCKET_MAX; i++) {
        if (resource->net_sockets[i] >= 0) continue;
        raw_socket = network_socket_open((uint8_t)type);
        if (raw_socket < 0) return -BK_EMFILE;
        resource->net_sockets[i] = raw_socket;
        return (int32_t)i;
    }
    return -BK_EMFILE;
}

static int32_t sys_net_close(uint32_t handle) {
    syscall_resources_t *resource =
        resource_for(task_current_process_id(), false);
    int raw_socket = resource_socket(resource, handle);
    if (raw_socket < 0) return -BK_EBADF;
    network_socket_close(raw_socket);
    resource->net_sockets[handle] = -1;
    return 0;
}

static int32_t sys_open(const registers_t *regs, const char *user_path,
                        uint32_t flags) {
    char path[VFS_MAX_PATH];
    syscall_resources_t *resource;
    int raw_fd;

    if (!copy_user_string(regs, user_path, path, sizeof(path)))
        return -BK_EFAULT;
    if ((flags & VFS_O_RDWR) == 0U || (flags & VFS_O_RDWR) > VFS_O_RDWR)
        return -BK_EINVAL;
    resource = resource_for(task_current_process_id(), true);
    if (!resource) return -BK_EMFILE;
    for (uint32_t i = 0; i < SYSCALL_FD_SLOTS; i++) {
        if (resource->fds[i] >= 0) continue;
        raw_fd = vfs_open(path, flags);
        if (raw_fd < 0) return -BK_ENOENT;
        resource->fds[i] = raw_fd;
        resource->linux_fd_flags[i+3U]=0U;
        resource->linux_open_flags[i+3U]=(flags & VFS_O_RDWR)-1U;
        return (int32_t)(i + 3U);
    }
    return -BK_EMFILE;
}

static int32_t sys_read_resident(const registers_t *regs, uint32_t fd, void *buffer,
                        uint32_t length) {
    syscall_resources_t *resource;
    int raw_fd;
    int32_t result;
    if (length > SYSCALL_FILE_IO_MAX ||
        !user_write_range_ok(regs, buffer, length))
        return -BK_EFAULT;

    /* SYS_READ no toma KDOMAIN_VFS desde el dispatcher porque stdin de una
     * terminal puede bloquear esperando al usuario. Retener el dominio VFS
     * durante ese tiempo congelaria operaciones de archivos de otros tasks. */
    kernel_domains_enter(KDOMAIN_VFS);
    resource = resource_for(task_current_process_id(), false);
    raw_fd = resource_fd(resource, fd);
    if (fd == 0U && raw_fd < 0) {
        kernel_domains_exit(KDOMAIN_VFS);
        result = vga_console_input_read((char *)buffer, length);
        return result >= 0 ? result : -BK_EBADF;
    }
    if (raw_fd < 0) {
        kernel_domains_exit(KDOMAIN_VFS);
        return -BK_EBADF;
    }
    /* A regular-file read may span many pages. Wine's setup reads complete
     * placeholder DLLs in one call; rejecting >64 KiB silently omitted them
     * from the prefix. Keep the VFS transaction in bounded chunks, while a
     * pipe/device read returns after its first available chunk. */
    uint32_t done = 0U;
    do {
        uint32_t chunk = length - done;
        if (chunk > SYSCALL_WRITE_MAX) chunk = SYSCALL_WRITE_MAX;
        result = sys_raw_read(raw_fd, (uint8_t *)buffer + done, chunk);
        if (result < 0) { if (done) result = (int32_t)done; break; }
        uint32_t got = (uint32_t)result;
        done += got;
        result = (int32_t)done;
        if (got < chunk || raw_fd >= LINUX_IO_BASE) break;
    } while (done < length);
    kernel_domains_exit(KDOMAIN_VFS);
    return result;
}

static int32_t sys_write_resident(const registers_t *regs, uint32_t fd,
                         const void *buffer, uint32_t length) {
    syscall_resources_t *resource;
    int raw_fd;
    if (length > SYSCALL_FILE_IO_MAX || !user_range_ok(regs, buffer, length))
        return -BK_EFAULT;
    resource = resource_for(task_current_process_id(), false);
    raw_fd = resource_fd(resource, fd);
    if ((fd == 1U || fd == 2U) && raw_fd < 0) {
        const char *text = (const char *)buffer;
        for (uint32_t i = 0; i < length; i++) vga_putchar(text[i]);
        return (int32_t)length;
    }
    if (raw_fd < 0) return -BK_EBADF;
    uint32_t done = 0U;
    int32_t result = 0;
    do {
        uint32_t chunk = length - done;
        if (chunk > SYSCALL_WRITE_MAX) chunk = SYSCALL_WRITE_MAX;
        result = sys_raw_write(raw_fd, (const uint8_t *)buffer + done, chunk);
        if (result < 0) return done ? (int32_t)done : result;
        done += (uint32_t)result;
        if ((uint32_t)result < chunk || raw_fd >= LINUX_IO_BASE) break;
    } while (done < length);
    return (int32_t)done;
}

/* Pin each I/O span before taking controller locks. A large swapped buffer
 * must not require all its physical pages to be resident at once. */
static int32_t sys_paged_io(const registers_t *regs,uint32_t fd,void *buffer,
                            uint32_t length,bool read) {
    if(length>SYSCALL_FILE_IO_MAX || (uint32_t)(uintptr_t)buffer+length<(uint32_t)(uintptr_t)buffer)
        return -BK_EFAULT;
    if(!length)return read?sys_read_resident(regs,fd,buffer,0):sys_write_resident(regs,fd,buffer,0);
    uint32_t pid=task_current_process_id(),done=0;
    bool special=resource_fd(resource_for(pid,false),fd)>=LINUX_IO_BASE;
    while(done<length) {
        uint32_t address=(uint32_t)(uintptr_t)buffer+done;
        /* Pipes must see one complete <=PIPE_BUF write. Splitting at a
         * virtual page boundary would violate atomic nonblocking writes. */
        /* A page at a time turned a large BFS4 streaming write into hundreds
         * of independent durable metadata transactions. Bound resident I/O
         * to 64 KiB, matching the lower layer, instead of splitting every
         * otherwise contiguous user buffer at virtual page boundaries. */
        uint32_t chunk=SYSCALL_WRITE_MAX;
        if(chunk>length-done)chunk=length-done;
        if(!pager_pin(pid,address,chunk,read)) {
            /* Preserve progress up to an inaccessible page and low-RAM
             * operation when the complete batch cannot be made resident.
             * Pipe/socket writes keep their existing atomic span. */
            if(special)return done?(int32_t)done:-BK_EFAULT;
            chunk=4096U-(address&4095U);
            if(chunk>length-done)chunk=length-done;
            if(!pager_pin(pid,address,chunk,read))return done?(int32_t)done:-BK_EFAULT;
        }
        int32_t result=read?sys_read_resident(regs,fd,(void *)(uintptr_t)address,chunk)
                           :sys_write_resident(regs,fd,(void *)(uintptr_t)address,chunk);
        pager_unpin(pid,address,chunk);
        if(result<0)return done?(int32_t)done:result;
        done+=(uint32_t)result;
        syscall_resources_t *resource=resource_for(pid,false);
        if((uint32_t)result<chunk || resource_fd(resource,fd)>=LINUX_IO_BASE)break;
    }
    return (int32_t)done;
}
static int32_t sys_read(const registers_t *regs,uint32_t fd,void *buffer,uint32_t length) {
    return sys_paged_io(regs,fd,buffer,length,true);
}
static int32_t sys_write(const registers_t *regs,uint32_t fd,const void *buffer,uint32_t length) {
    return sys_paged_io(regs,fd,(void *)buffer,length,false);
}

static int32_t sys_close(uint32_t fd) {
    syscall_resources_t *resource =
        resource_for(task_current_process_id(), false);
    uint32_t slot;
    if (!resource) return -BK_EBADF;
    if (fd < 3U) {
        if (resource->stdio_fds[fd] < 0) return -BK_EBADF;
        linux_unlock_file(task_current_process_id(),resource->stdio_fds[fd]);
        if (!sys_raw_close(resource->stdio_fds[fd])) return -BK_EIO;
        resource->stdio_fds[fd] = -1;
        return 0;
    }
    slot = fd - 3U;
    if (slot >= SYSCALL_FD_SLOTS || resource->fds[slot] < 0)
        return -BK_EBADF;
    linux_unlock_file(task_current_process_id(),resource->fds[slot]);
    if (!sys_raw_close(resource->fds[slot])) return -BK_EIO;
    resource->fds[slot] = -1;
    return 0;
}

static int32_t sys_dup(uint32_t old_fd, int32_t requested_fd) {
    syscall_resources_t *resource =
        resource_for(task_current_process_id(), false);
    int raw_fd = resource_fd(resource, old_fd);
    int32_t target = requested_fd;
    int *destination;
    if (!resource || raw_fd < 0) return -BK_EBADF;
    if (target < 0) {
        for (uint32_t i=0U;i<SYSCALL_FD_SLOTS;i++)
            if (resource->fds[i] < 0) { target=(int32_t)(i+3U); break; }
        if (target < 0) return -BK_EMFILE;
    }
    if ((uint32_t)target >= SYSCALL_FD_SLOTS + 3U) return -BK_EBADF;
    if ((uint32_t)target == old_fd) return target;
    destination = target < 3 ? &resource->stdio_fds[target]
                             : &resource->fds[target-3];
    if(*destination>=0)linux_unlock_file(task_current_process_id(),*destination);
    if (*destination >= 0 && !sys_raw_close(*destination)) return -BK_EIO;
    if (!sys_raw_retain(raw_fd)) { *destination=-1; return -BK_EIO; }
    *destination=raw_fd;
    resource->linux_fd_flags[target]=0U;
    resource->linux_open_flags[target]=resource->linux_open_flags[old_fd];
    return target;
}

static int32_t sys_alloc(uint32_t size) {
    syscall_resources_t *resource;
    void *allocation;
    int32_t slot;
    if (!size || size > SYSCALL_ALLOC_MAX) return -BK_EINVAL;
    resource = resource_for(task_current_process_id(), true);
    if (!resource) return -BK_ENOMEM;
    slot = allocation_free_slot(resource);
    if (slot < 0) {
        kprintf("[SYSCALL] alloc slots agotados pid=%u size=%u max=%u\n",
                task_current_process_id(), size, SYSCALL_ALLOC_LIMIT);
        return -BK_ENOMEM;
    }
    allocation = kzalloc(size);
    if (!allocation) return -BK_ENOMEM;
    resource->allocations[(uint32_t)slot] = allocation;
    return (int32_t)(uintptr_t)allocation;
}

static int32_t sys_realloc(void *old_pointer, uint32_t size) {
    syscall_resources_t *resource =
        resource_for(task_current_process_id(), false);
    void *allocation;
    if (!old_pointer) return sys_alloc(size);
    if (!resource || !size || size > SYSCALL_ALLOC_MAX) return -BK_EINVAL;
    for (uint32_t i = 0; i < resource->allocation_capacity; i++) {
        if (resource->allocations[i] != old_pointer) continue;
        allocation = krealloc(old_pointer, size);
        if (!allocation) return -BK_ENOMEM;
        resource->allocations[i] = allocation;
        return (int32_t)(uintptr_t)allocation;
    }
    return -BK_EACCES;
}

static int32_t sys_free(void *pointer) {
    syscall_resources_t *resource =
        resource_for(task_current_process_id(), false);
    if (!pointer) return 0;
    if (!resource) return -BK_EACCES;
    for (uint32_t i = 0; i < resource->allocation_capacity; i++) {
        if (resource->allocations[i] != pointer) continue;
        kfree(pointer);
        resource->allocations[i] = NULL;
        return 0;
    }
    return -BK_EACCES;
}

static syscall_vm_state_t *sys_vm_state(syscall_resources_t *resource,
                                        bool create) {
    if (resource && resource->vm_borrowed_from)
        resource=resource_for(resource->vm_borrowed_from,false);
    if (!resource) return NULL;
    if (!resource->vm_initialized) {
        if (!create) return NULL;
        kmemset(&resource->vm, 0, sizeof(resource->vm));
        if (!sys_vm_maps_reserve(&resource->vm, SYSCALL_VM_MAP_INITIAL))
            return NULL;
        resource->vm_initialized = true;
    }
    return &resource->vm;
}

/* PROT_NONE Wine reservations can own virtual addresses without PTEs.
 * The page-table allocator alone must never put a libc heap or mmap inside
 * one: freeing that reservation would also destroy the unrelated allocation.
 * VM operations are serialized by KDOMAIN_VFS; this callback only reads
 * metadata and skips whole intervals, rather than probing each reserved page. */
static uint32_t sys_vm_auto_skip(void *context, uint32_t address, uint32_t length) {
    const syscall_vm_state_t *vm = context;
    uint32_t end = address + length, next = 0U;
    for (uint32_t i = 0U; i < vm->map_capacity; ++i) {
        const syscall_vm_map_t *m = &vm->maps[i];
        uint32_t limit = m->address + m->length;
        if (m->address && address < limit && m->address < end && limit > next)
            next = limit;
    }
    uint32_t brk_end = vm->brk_base + vm->brk_capacity;
    if (vm->brk_capacity && address < brk_end && vm->brk_base < end && brk_end > next)
        next = brk_end;
    return next;
}

static uint32_t sys_vm_brk(uint32_t requested) {
    uint32_t process_id = task_current_process_id();
    syscall_resources_t *resource = resource_for(process_id, true);
    syscall_vm_state_t *vm = sys_vm_state(resource, true);
    uint32_t address;

    if (!vm || !process_id) return 0U;
    if (!vm->brk_capacity) {
        if(!pager_map(process_id,0U,SYSCALL_LINUX_BRK_RESERVE,0U,false,
                      &address,sys_vm_auto_skip,vm))return 0U;
        vm->brk_base = address;
        vm->brk_current = address;
        vm->brk_capacity = SYSCALL_LINUX_BRK_RESERVE;
        /* A native process can reach brk before it has ever needed a private
         * CR3. Switching in CPL0 is safe because kernel mappings are shared. */
        paging_switch_process(process_id);
    }
    if (!requested) return vm->brk_current;
    if (requested < vm->brk_base ||
        requested - vm->brk_base > vm->brk_capacity)
        return vm->brk_current;
    uint32_t old_end=(vm->brk_current+4095U)&~4095U;
    uint32_t new_end=(requested+4095U)&~4095U;
    if(requested>vm->brk_current) {
        if(new_end>old_end && !pager_protect(process_id,old_end,new_end-old_end,3U))
            return vm->brk_current;
        /* Only the previously committed tail needs clearing. Fresh pages
         * remain absent and are zero-filled at first access. */
        uint32_t tail_end=requested<old_end?requested:old_end;
        if(tail_end>vm->brk_current) {
            uint32_t bytes=tail_end-vm->brk_current;
            if(!pager_pin(process_id,vm->brk_current,bytes,true)) {
                if(new_end>old_end)(void)pager_protect(process_id,old_end,new_end-old_end,0U);
                return vm->brk_current;
            }
            uint8_t zero[64]={0};bool ok=true;
            for(uint32_t pos=vm->brk_current;pos<tail_end && ok;) {
                uint32_t n=tail_end-pos;if(n>sizeof(zero))n=sizeof(zero);
                ok=paging_process_write(process_id,pos,zero,n);pos+=n;
            }
            pager_unpin(process_id,vm->brk_current,bytes);
            if(!ok) {
                if(new_end>old_end)(void)pager_protect(process_id,old_end,new_end-old_end,0U);
                return vm->brk_current;
            }
        }
    } else if(new_end<old_end &&
              !pager_discard(process_id,new_end,old_end-new_end)) {
        return vm->brk_current;
    }
    vm->brk_current = requested;
    return requested;
}

/* The bootstrap island at 0x00100000..0x0010ffff is discardable, allowing
 * Wine to map the DOS area. The resident image and the Stage-2 stack below
 * 4 MiB remain live; the separately placed BSS is also identity mapped in
 * every process CR3. A private user PTE over either range would hide kernel
 * data while servicing that process. Reject reservations too, so Wine can
 * search for a usable address before committing it with mprotect(). */
extern char __kernel_live_start[];
extern char __bss_start[], __bss_end[];
static bool sys_vm_hits_live_low_kernel(uint32_t address, uint32_t length) {
    uint32_t end;
    if (!length || address + length < address) return true;
    end = address + length;
    return (address < 0x00400000U &&
            (uint32_t)(uintptr_t)__kernel_live_start < end) ||
           (address < (uint32_t)(uintptr_t)__bss_end &&
            (uint32_t)(uintptr_t)__bss_start < end);
}

static uint32_t sys_vm_map_slots_used(const syscall_vm_state_t *vm) {
    uint32_t used = 0U;
    if (!vm) return 0U;
    for (uint32_t i = 0U; i < vm->map_capacity; i++)
        if (vm->maps[i].address) used++;
    return used;
}

static int32_t sys_vm_metadata_exhausted(uint32_t process_id,
                                         const syscall_vm_state_t *vm,
                                         uint32_t site) {
    kprintf("[VM:MAPS] pid=%u metadata agotada used=%u/%u site=%u\n",
            process_id, sys_vm_map_slots_used(vm),
            vm ? vm->map_capacity : 0U, site);
    return -BK_ENOMEM;
}

static bool sys_vm_range_overlaps(const syscall_vm_state_t *vm,
                                  uint32_t address, uint32_t length,
                                  uint32_t process_id) {
    uint32_t end;
    if (!vm || !length || address + length < address) return true;
    end = address + length;
    for (uint32_t i = 0U; i < vm->map_capacity; i++) {
        const syscall_vm_map_t *m = &vm->maps[i];
        uint32_t map_end;
        if (!m->address || !m->length) continue;
        map_end = m->address + m->length;
        if (map_end < m->address) return true;
        if (address < map_end && m->address < end) return true;
    }
    if (vm->brk_capacity) {
        uint32_t brk_end = vm->brk_base + vm->brk_capacity;
        if (brk_end < vm->brk_base ||
            (address < brk_end && vm->brk_base < end)) return true;
    }
    if (elf_linux_range_occupied(process_id, address, length)) return true;
    if (task_process_user_range_occupied(process_id, address, length)) return true;
    return false;
}

static bool sys_vm_virtual_reservations_cover(const syscall_vm_state_t *vm,
                                                uint32_t address,
                                                uint32_t length,
                                                bool *split_needed) {
    uint32_t end;
    uint32_t cursor;
    if (split_needed) *split_needed = false;
    if (!vm || !length || address + length < address) return false;
    end = address + length;
    cursor = address;

    /* MAP_FIXED may replace a range covered by several adjacent VMAs. Wine's
     * reserve_area() naturally creates exactly this shape while bisecting
     * around ELF/stack collisions. Treat the union of virtual-only private
     * reservations as one continuous covering interval. */
    while (cursor < end) {
        uint32_t next = cursor;
        for (uint32_t i = 0U; i < vm->map_capacity; i++) {
            const syscall_vm_map_t *m = &vm->maps[i];
            uint32_t map_end;
            if (!m->address || m->backing || m->shared || (m->flags&VM_MAP_PAGED) || m->protection ||
                (m->flags & 3U) != 2U) continue;
            map_end = m->address + m->length;
            if (map_end < m->address) return false;
            if (m->address <= cursor && cursor < map_end && map_end > next)
                next = map_end;
            if (split_needed && m->address < address && end < map_end)
                *split_needed = true;
        }
        if (next == cursor) return false;
        cursor = next > end ? end : next;
    }
    return true;
}

static bool sys_vm_carve_virtual_reservations(syscall_vm_state_t *vm,
                                               uint32_t address,
                                               uint32_t length,
                                               syscall_vm_map_t *commit_slot) {
    uint32_t end;
    if (!vm || !length || address + length < address) return false;
    end = address + length;

    for (uint32_t i = 0U; i < vm->map_capacity; i++) {
        syscall_vm_map_t *m = &vm->maps[i];
        syscall_vm_map_t old;
        uint32_t map_end;
        if (m == commit_slot || !m->address || m->backing || m->shared ||
            m->protection || (m->flags & 3U) != 2U) continue;
        map_end = m->address + m->length;
        if (map_end < m->address || address >= map_end || m->address >= end)
            continue;

        old = *m;
        if (address <= old.address && end >= map_end) {
            kmemset(m, 0, sizeof(*m));
            continue;
        }
        if (address <= old.address) {
            m->address = end;
            m->length = map_end - end;
            continue;
        }
        if (end >= map_end) {
            m->length = address - old.address;
            continue;
        }

        /* The committed range is strictly inside one reservation, so keep a
         * left and right metadata VMA around it. The caller preflights one
         * extra free slot for this case. */
        syscall_vm_map_t *right = NULL;
        for (uint32_t j = 0U; j < vm->map_capacity; j++) {
            if (&vm->maps[j] != commit_slot && !vm->maps[j].address) {
                right = &vm->maps[j];
                break;
            }
        }
        if (!right) return false;
        *right = old;
        right->address = end;
        right->length = map_end - end;
        m->length = address - old.address;
    }
    return true;
}

static int32_t sys_vm_munmap(uint32_t address, uint32_t length);

static int32_t sys_vm_mmap(uint32_t requested, uint32_t length,
                           uint32_t protection, uint32_t flags,
                           int32_t fd, uint32_t page_offset) {
    enum {
        LINUX_MAP_SHARED = 0x01U,
        LINUX_MAP_PRIVATE = 0x02U,
        LINUX_MAP_FIXED = 0x10U,
        LINUX_MAP_ANONYMOUS = 0x20U,
        LINUX_MAP_NORESERVE = 0x4000U,
        LINUX_MAP_FIXED_NOREPLACE = 0x100000U
    };
    uint32_t process_id = task_current_process_id();
    syscall_resources_t *resource;
    syscall_vm_state_t *vm;
    syscall_vm_map_t *mapping = NULL;
    syscall_vm_map_t *fixed_container = NULL;
    uint32_t rounded;
    uint32_t backing;
    uint32_t address;
    uint32_t pages;
    int mapped_raw_fd = -1;
    const char *mapped_path = NULL;
    uint32_t mapped_file_offset = 0U;
    bool anonymous = (flags & LINUX_MAP_ANONYMOUS) != 0U;
    bool fixed = (flags & LINUX_MAP_FIXED) != 0U;
    bool fixed_noreplace = (flags & LINUX_MAP_FIXED_NOREPLACE) != 0U;
    bool virtual_reservation = fixed_noreplace && anonymous && !protection;
    bool fixed_virtual_reservation = fixed && anonymous && !protection &&
                                     (flags & LINUX_MAP_NORESERVE) != 0U;
    bool metadata_reservation = virtual_reservation || fixed_virtual_reservation;
    bool fixed_virtual_cover = false;
    bool fixed_virtual_split = false;
    bool fixed_empty_range = false;
    bool fixed_cached_private_replace = false;

    /* SYSCALL_ALLOC_MAX protects mappings that can consume physical backing.
     * Wine's reserve_area() deliberately asks for much larger PROT_NONE
     * MAP_FIXED_NOREPLACE ranges; those are virtual address reservations only
     * and consume page-table metadata, not one frame per mapped page. */
    if (!process_id || !length ||
        (length > SYSCALL_ALLOC_MAX && !metadata_reservation) ||
        (protection & ~7U) || !(flags & 3U) || (flags & 3U)==3U)
        return -BK_EINVAL;
    if (flags & ~(3U|LINUX_MAP_FIXED|LINUX_MAP_ANONYMOUS|0x800U|0x1000U|
                  LINUX_MAP_NORESERVE|0x20000U|LINUX_MAP_FIXED_NOREPLACE))
        return -BK_ENOSYS;
    if (fixed && fixed_noreplace) return -BK_EINVAL;
    if ((fixed || fixed_noreplace) && !requested) return -BK_EACCES;
    if ((fixed || fixed_noreplace) &&
        (requested & (PAGING_PAGE_SIZE-1U))) return -BK_EINVAL;
    if (length > 0xFFFFFFFFU - (PAGING_PAGE_SIZE - 1U)) return -BK_ENOMEM;
    rounded = (length + PAGING_PAGE_SIZE - 1U) & ~(PAGING_PAGE_SIZE - 1U);
    pages = rounded / PAGING_PAGE_SIZE;
    if ((fixed || fixed_noreplace) &&
        (sys_vm_hits_live_low_kernel(requested, rounded) ||
         mm_kernel_identity_overlaps(requested, rounded)))
        return fixed_noreplace ? -BK_EEXIST : -BK_ENOMEM;
    resource = resource_for(process_id, true);
    if (!resource) return -BK_ENOMEM;
    vm = sys_vm_state(resource, true);
    if (!vm) return -BK_ENOMEM;
    resource->vm_perf_mmap_calls++;
    g_vm_perf_totals.mmap_calls++;
    if (!anonymous) {
        resource->vm_perf_file_maps++;
        g_vm_perf_totals.file_maps++;
    }

    /* Wine maps the same PE/DLL file several times (headers plus individual
     * sections). The old MAP_PRIVATE path allocated fresh frames and reread
     * every range synchronously from BFS4. That is correct but expensive.
     *
     * Prefer the VFS inode-backed file cache for ordinary MAP_PRIVATE views.
     * Cached pages are installed read-only/COW, so the first private write
     * detaches the view through syscall_linux_cow_fault(). If the cache cannot
     * represent a file/range, fall back to the proven eager private path. */
    bool private_file = !anonymous && (flags&3U)==LINUX_MAP_PRIVATE;
    if((flags&3U)==LINUX_MAP_SHARED || private_file) {
        bool file_cow = private_file;
        bool cache_range_ok = true;
        int raw=anonymous?-1:resource_fd(resource,fd);
        int32_t mode=anonymous?2:vfs_status_flags(raw);
        uint32_t offset=anonymous?0:page_offset*4096U;
        uint32_t file_rounded=0U;
        uint32_t file_size_u=0U;
        bool cache_hit=false;
        bool cache_try=anonymous;
        vfs_shared_mapping_t *shared=NULL;

        if((fixed || fixed_noreplace) && (!requested || requested+rounded<requested ||
           sys_vm_hits_live_low_kernel(requested,rounded))) return -BK_ENOMEM;
        if(fixed_noreplace && sys_vm_range_overlaps(vm,requested,rounded,process_id))
            return -BK_EEXIST;
        if(!anonymous && raw<0)return -BK_EBADF;
        if(!anonymous && (raw>=LINUX_IO_BASE || vfs_is_directory(raw)))return -19;
        if(!anonymous && (mode<0 || (mode&3U)==1U ||
           (!file_cow && (protection&2U) && (mode&3U)!=2U)))return -13;
        if(!anonymous && (page_offset>0xfffffU || offset+rounded<offset))return -BK_EINVAL;
        if(!anonymous) {
            int32_t file_size=vfs_size(raw);
            if(file_size<0)return -BK_EIO;
            file_size_u=(uint32_t)file_size;
            file_rounded=(file_size_u+4095U)&~4095U;
            cache_range_ok=offset+rounded>=offset && offset+rounded<=file_rounded;
            if(!cache_range_ok && !file_cow)return -BK_ENOSYS;
            cache_hit=vfs_has_shared_mapping(raw);
            /* Regular private views reserve PTEs and retain an inode, not a
             * full-file physical cache. The page fault loads a single page;
             * clean aliases share it and pressure can discard it without swap.
             * Existing MAP_SHARED objects retain their coherent backing path.
             * Keep legacy handling for unsupported beyond-EOF/orphan cases. */
            /* ELF loaders reserve a file view through the future BSS tail.
             * Pages beyond EOF need virtual metadata, not a contiguous eager
             * allocation. They fault with SIGBUS until replaced by BSS. */
            if(file_cow && !cache_hit) {
                vfs_page_reference_t *reference=vfs_page_reference_acquire(raw);
                if(reference) {
                    mapping=sys_vm_free_map_slot(vm,true);
                    if(!mapping){vfs_page_reference_release(reference);return -BK_ENOMEM;}
                    if(fixed) {
                        int32_t error=sys_vm_munmap(requested,rounded);
                        if(error){vfs_page_reference_release(reference);return error;}
                        mapping=sys_vm_free_map_slot(vm,true);
                    }
                    bool mapped=mapping && pager_map_file(process_id,requested,rounded,protection,
                        fixed || fixed_noreplace,&address,sys_vm_auto_skip,vm,reference,offset);
                    if(mapped) {
                        *mapping=(syscall_vm_map_t){0,address,rounded,protection,flags|VM_MAP_PAGED,NULL};
                        resource->vm_perf_cached_file_maps++;g_vm_perf_totals.cached_file_maps++;
                        paging_switch_process(process_id);return (int32_t)address;
                    }
                    vfs_page_reference_release(reference);
                }
            }
            if((flags&3U)==LINUX_MAP_SHARED)
                cache_try=cache_range_ok;
            else
                cache_try=cache_range_ok &&
                    file_size_u<=SYSCALL_VM_PRIVATE_CACHE_MAX &&
                    (cache_hit || vfs_shared_prefetch_room());
        }

        /* Anonymous MAP_SHARED requires shared backing. File-private mappings
         * use the cache only while it has a bounded file size and spare slots;
         * otherwise the legacy eager path remains the compatibility fallback. */
        if(cache_try) {
            uint32_t io_start=pit_get_ticks();
            uint32_t io_ticks;
            shared=file_cow?vfs_private_acquire(raw):vfs_shared_acquire(raw,rounded);
            io_ticks=pit_get_ticks()-io_start;
            resource->vm_perf_file_io_ticks += io_ticks;
            g_vm_perf_totals.file_io_ticks += io_ticks;
            if(!anonymous) {
                if(shared) {
                    resource->vm_perf_cached_file_maps++;
                    g_vm_perf_totals.cached_file_maps++;
                    if(cache_hit) {
                        resource->vm_perf_cache_hits++;
                        g_vm_perf_totals.cache_hits++;
                    } else {
                        uint32_t kib=(shared->size + 1023U) / 1024U;
                        resource->vm_perf_cache_misses++;
                        resource->vm_perf_file_io_kib += kib;
                        g_vm_perf_totals.cache_misses++;
                        g_vm_perf_totals.file_io_kib += kib;
                    }
                } else {
                    resource->vm_perf_cache_misses++;
                    g_vm_perf_totals.cache_misses++;
                }
            }
        }

        if(shared) {
            mapping = sys_vm_free_map_slot(vm, true);
            if(!mapping) {
                vfs_shared_release(shared);
                return sys_vm_metadata_exhausted(process_id, vm, 1U);
            }
            backing=shared->backing+offset;
            if(fixed) {
                int32_t error=sys_vm_munmap(requested,rounded);
                if(error) {vfs_shared_release(shared);return error;}
                mapping = sys_vm_free_map_slot(vm, true);
                if(!mapping) {
                    vfs_shared_release(shared);
                    return sys_vm_metadata_exhausted(process_id, vm, 2U);
                }
            }
            bool mapped;
            if(fixed || fixed_noreplace) {
                address=requested;
                mapped=paging_process_map(process_id,address,backing,rounded,
                    protection!=0,!file_cow && (protection&2U)!=0U);
            } else {
                mapped=paging_process_map_auto_filtered(process_id,requested,backing,rounded,
                    !file_cow && (protection&2U)!=0U,&address,sys_vm_auto_skip,vm);
            }
            if(!mapped) {
                vfs_shared_release(shared);return -BK_ENOMEM;
            }
            if((file_cow && !paging_process_cow_access(process_id,address,rounded,
                                                       protection!=0,(protection&2U)!=0U)) ||
               (!file_cow && !protection &&
                !paging_process_access(process_id,address,rounded,false,false))) {
                paging_process_unmap(process_id,address,rounded);
                vfs_shared_release(shared);return -BK_ENOMEM;
            }
            *mapping=(syscall_vm_map_t){backing,address,rounded,protection,
                flags|(!file_cow && (mode&3U)!=2U?0x80000000U:0U),shared};
            if(file_cow && pager_adopt_cow(process_id,address,rounded,backing))
                mapping->flags|=VM_MAP_PAGED;
            paging_switch_process(process_id);
            return (int32_t)address;
        }

        if(!file_cow) return -BK_ENOMEM;
        resource->vm_perf_fallback_reads++;
        g_vm_perf_totals.fallback_reads++;
    }

    if (fixed_virtual_reservation) {
        /* Linux MAP_FIXED is destructive: a new anonymous PROT_NONE mapping
         * replaces any mmap-owned mappings in the requested interval. Wine
         * exercises exactly this while reshaping its address-space holes: it
         * may first commit a small RW MAP_FIXED page, then immediately cover
         * the same page again with PROT_NONE|MAP_FIXED|MAP_NORESERVE.
         *
         * Keep BlesKernOS' cheap metadata-only representation for PROT_NONE,
         * but first tear down syscall-owned VMAs with the normal munmap path.
         * sys_vm_munmap() already refuses brk/ELF/task-owned ranges and unsafe
         * partial shared-map splits, so this gains Linux replacement semantics
         * without allowing MAP_FIXED to destroy unrelated kernel/loader state. */
        if (!requested) return -BK_EINVAL;
        int32_t replace_result = sys_vm_munmap(requested, rounded);
        if (replace_result) {
            kprintf("[VM:FIXED-NONE] pid=%u base=%x len=%x replace=%d\n",
                    process_id, requested, rounded, replace_result);
            return replace_result;
        }
        mapping = sys_vm_free_map_slot(vm, true);
        if (!mapping) return sys_vm_metadata_exhausted(process_id, vm, 3U);
        mapping->address = requested;
        mapping->length = rounded;
        mapping->protection = 0U;
        mapping->flags = flags;
        mapping->backing = 0U;
        mapping->shared = NULL;
#ifdef BLESK_WINE_VERBOSE_TRACE
        kprintf("[VM:FIXED-NONE] pid=%u base=%x len=%x metadata-only\n",
                process_id, requested, rounded);
#endif
        return (int32_t)requested;
    }

    /* A real MAP_FIXED mapping creates user PTEs.  Do not let those PTEs
     * shadow the live low kernel while BlesKernOS still shares one linear
     * address space between CPL0 and CPL3.  The Wine DOS mapping ends exactly
     * at 0x00110000 and therefore remains fully legal. */
    if ((fixed || fixed_noreplace) && !metadata_reservation &&
        sys_vm_hits_live_low_kernel(requested, rounded))
        return -BK_ENOMEM;

    if (fixed) {
        if (requested+rounded<requested) return -BK_ENOMEM;

        /* v14 can back MAP_PRIVATE file views with the inode cache. The ELF
         * dynamic loader commonly follows such a file mapping with an
         * anonymous MAP_FIXED for the zero-fill/BSS tail of the same segment.
         * Linux MAP_FIXED destructively replaces the overlapped pages. Before
         * v14 these file views had private backing and the fixed_container path
         * handled that pattern in-place; cached views have m->shared != NULL,
         * so the old test rejected the BSS commit with ENOSYS/ENOMEM and ld.so
         * reported "cannot map zero-fill pages".
         *
         * sys_vm_munmap() already protects brk/ELF/task-owned ranges and v14
         * taught it how to split cached private VMAs safely, so use it only
         * when this exact cached-private replacement case is present. */
        for (uint32_t i=0;i<vm->map_capacity;++i) {
            syscall_vm_map_t *m=&vm->maps[i];
            uint32_t map_end;
            if (!m->address || !m->length || (!m->shared && !(m->flags&VM_MAP_PAGED)) ||
                (m->flags&3U)!=LINUX_MAP_PRIVATE)
                continue;
            map_end=m->address+m->length;
            if (map_end<m->address) continue;
            if (requested<map_end && m->address<requested+rounded) {
                fixed_cached_private_replace=true;
                break;
            }
        }

        if (fixed_cached_private_replace) {
            int32_t replace_result=sys_vm_munmap(requested,rounded);
            if (replace_result) return replace_result;
            fixed_empty_range=true;
            if (!sys_vm_ensure_free_slots(vm,1U))
                return sys_vm_metadata_exhausted(process_id,vm,4U);
            mapping=sys_vm_free_map_slot(vm,false);
            if (!mapping)
                return sys_vm_metadata_exhausted(process_id,vm,4U);
        } else {
            /* A backed mapping can still be replaced in-place when one mapping
             * contains the entire request. Wine's virtual reservations are more
             * interesting: reserve_area() often leaves several adjacent VMAs and
             * later commits one MAP_FIXED range spanning all of them. Linux
             * permits that replacement, so do not require a single container. */
            for (uint32_t i=0;i<vm->map_capacity;++i) {
                syscall_vm_map_t *m=&vm->maps[i];
                if (m->address && m->backing && !m->shared &&
                    (m->flags&3U)==LINUX_MAP_PRIVATE &&
                    requested>=m->address && requested+rounded<=m->address+m->length) {
                    fixed_container=m;break;
                }
            }
            if (fixed_container) {
                mapping=fixed_container;
            } else {
                fixed_virtual_cover = sys_vm_virtual_reservations_cover(
                    vm, requested, rounded, &fixed_virtual_split);
            if (!fixed_virtual_cover) {
                /* Linux MAP_FIXED does not require a preceding PROT_NONE
                 * reservation.  Wine's loader sometimes commits a tiny view
                 * directly into an otherwise empty fixed address (notably
                 * 0x7f000000 during wineboot).  Returning ENOSYS here made
                 * Wine fall back to 0x00110000, which is BlesKernOS' live
                 * identity-mapped kernel window and therefore failed with
                 * ENOMEM.
                 *
                 * Full destructive MAP_FIXED replacement is still deferred:
                 * if Bles owns any VMA/ELF/brk/stack in the requested range,
                 * retain the conservative ENOSYS result.  For an actually
                 * empty range, however, installing fresh private PTEs is both
                 * safe and the Linux-compatible behavior Wine needs. */
                if (requested < 0x00010000U ||
                    sys_vm_range_overlaps(vm, requested, rounded, process_id))
                    return -BK_ENOSYS;
                fixed_empty_range = true;
            }
            /* One slot is the committed mapping. If the commit cuts a hole
             * inside a single reservation, carving also needs a right VMA.
             * Grow before taking a pointer because krealloc() may move maps. */
            uint32_t needed_slots =
                (fixed_virtual_cover && fixed_virtual_split) ? 2U : 1U;
            if (!sys_vm_ensure_free_slots(vm, needed_slots))
                return sys_vm_metadata_exhausted(process_id, vm, 4U);
            mapping = sys_vm_free_map_slot(vm, false);
            if (!mapping)
                return sys_vm_metadata_exhausted(process_id, vm, 4U);
            }
        }
    } else {
        mapping = sys_vm_free_map_slot(vm, true);
        if (!mapping) return sys_vm_metadata_exhausted(process_id, vm, 5U);
    }

    if (fixed_noreplace) {
        if (!requested) return -BK_EINVAL;
        /* Linux tests MAP_FIXED_NOREPLACE against process VMAs.  A Wine
         * PROT_NONE|MAP_NORESERVE reservation is intentionally metadata-only:
         * it may span addresses outside BlesKernOS' generic auto-mmap arena
         * (0x60000000..0x78000000) without creating or replacing any PTE.
         * Checking paging_process_range_free_exact() here made every address
         * above PAGING_USER_VM_END look occupied, so Wine recursively split
         * 0x7ffe0000..4GiB into 64-KiB probes until the W3 gate timed out.
         *
         * For a real, committed MAP_FIXED_NOREPLACE mapping, keep the page
         * table collision check; for an uncommitted VMA reservation, the VM
         * map/ELF/brk/stack metadata is the authoritative collision set. */
        if (sys_vm_range_overlaps(vm, requested, rounded, process_id))
            return -BK_EEXIST;
        if (!virtual_reservation &&
            !paging_process_range_free_exact(process_id, requested, rounded))
            return -BK_EEXIST;
        if (virtual_reservation) {
            mapping->address=requested;
            mapping->length=rounded;
            mapping->protection=0U;
            mapping->flags=flags;
            mapping->backing=0U;
            mapping->shared=NULL;
#ifdef BLESK_WINE_VERBOSE_TRACE
            kprintf("[VM:RESERVE] pid=%u base=%x len=%x metadata-only\n",
                    process_id, requested, rounded);
#endif
            return (int32_t)requested;
        }
    }

    if (anonymous && (flags&3U)==LINUX_MAP_PRIVATE &&
        (!fixed || fixed_empty_range || fixed_virtual_cover)) {
        if(!pager_map(process_id,requested,rounded,protection,fixed || fixed_noreplace,
                      &address,sys_vm_auto_skip,vm))return -BK_ENOMEM;
        if(fixed_virtual_cover && !sys_vm_carve_virtual_reservations(vm,requested,rounded,mapping)) {
            (void)pager_unmap(process_id,address,rounded);return -BK_ENOMEM;
        }
        *mapping=(syscall_vm_map_t){0,address,rounded,protection,flags|VM_MAP_PAGED,NULL};
        paging_switch_process(process_id);
        return (int32_t)address;
    }
    if (!pager_frame_alloc(pages, &backing)) {
        kprintf("[VM] mmap sin frames pid=%u pages=%u libres=%u\n",
                process_id, pages, mm_vm_frame_free_pages());
        return -BK_ENOMEM;
    }

    if (!anonymous) {
        int32_t old_position;
        int32_t read_result;
        mapped_raw_fd = resource_fd(resource, (uint32_t)fd);
        if (mapped_raw_fd < 0 || page_offset > 0xFFFFFFFFU / PAGING_PAGE_SIZE) {
            mm_vm_frame_free(backing, pages);
            return mapped_raw_fd < 0 ? -BK_EBADF : -BK_EINVAL;
        }
        mapped_file_offset = page_offset * PAGING_PAGE_SIZE;
        mapped_path = vfs_fd_path(mapped_raw_fd);
        old_position = vfs_tell(mapped_raw_fd);
        if (vfs_seek(mapped_raw_fd, (int32_t)mapped_file_offset, 0U) < 0) {
            mm_vm_frame_free(backing, pages);
            return -BK_EIO;
        }
        {
            uint32_t io_start=pit_get_ticks();
            uint32_t io_ticks;
            read_result = sys_raw_read(mapped_raw_fd, (void *)(uintptr_t)backing, length);
            io_ticks=pit_get_ticks()-io_start;
            resource->vm_perf_file_io_ticks += io_ticks;
            g_vm_perf_totals.file_io_ticks += io_ticks;
        }
        if (read_result > 0) {
            uint32_t kib=((uint32_t)read_result + 1023U) / 1024U;
            resource->vm_perf_file_io_kib += kib;
            g_vm_perf_totals.file_io_kib += kib;
        }
        if (old_position >= 0) (void)vfs_seek(mapped_raw_fd, old_position, 0U);
        if (read_result < 0) {
            mm_vm_frame_free(backing, pages);
            return -BK_EIO;
        }
    }

    if (fixed) {
        if (fixed_container && fixed_container->backing) {
            uint32_t destination=fixed_container->backing+requested-fixed_container->address;
            /* The content is prepared in temporary frames; a read error
             * above cannot alter the original reservation. */
            kmemcpy((void *)(uintptr_t)destination,(void *)(uintptr_t)backing,rounded);
            mm_vm_frame_free(backing,pages);
            if (!paging_process_access(process_id,requested,rounded,protection!=0,(protection&2U)!=0U))
                return -BK_ENOMEM;
            return (int32_t)requested;
        }
        /* Commit real pages either across metadata-only reservations or into
         * a genuinely empty fixed address. paging_process_map() installs the
         * exact private PTEs. Only the reservation-covered case needs VMA
         * carving afterwards. */
        if ((!fixed_virtual_cover && !fixed_empty_range) ||
            !paging_process_map(process_id,requested,backing,rounded,
                                protection!=0,(protection&2U)!=0U)) {
            mm_vm_frame_free(backing,pages);return -BK_ENOMEM;
        }
#ifdef BLESK_WINE_VERBOSE_TRACE
        if (fixed_empty_range)
            kprintf("[VM:FIXED-FREE] pid=%u base=%x len=%x prot=%x\n",
                    process_id,requested,rounded,protection);
#endif
        if (fixed_virtual_cover &&
            !sys_vm_carve_virtual_reservations(vm, requested, rounded, mapping)) {
            /* Preflight above guarantees metadata capacity, so this is only a
             * defensive fallback. Keep ownership of the committed backing in
             * the selected slot rather than leaking it. */
            mapping->backing=backing;
            mapping->address=requested;
            mapping->length=rounded;
            mapping->protection=protection;
            mapping->flags=flags;
            mapping->shared=NULL;
            paging_switch_process(process_id);
            return (int32_t)requested;
        }
        mapping->backing=backing;
        mapping->address=requested;
        mapping->length=rounded;
        mapping->protection=protection;
        mapping->flags=flags;
        mapping->shared=NULL;
        paging_switch_process(process_id);
        return (int32_t)requested;
    }

    if (fixed_noreplace) {
        if (!paging_process_map(process_id,requested,backing,rounded,
                                protection!=0,(protection&2U)!=0U)) {
            mm_vm_frame_free(backing,pages);return -BK_ENOMEM;
        }
        mapping->backing=backing;
        mapping->address=requested;
        mapping->length=rounded;
        mapping->protection=protection;
        mapping->flags=flags;
        mapping->shared=NULL;
        paging_switch_process(process_id);
        return (int32_t)requested;
    }

    if (!paging_process_map_auto_filtered(process_id, requested, backing, rounded,
                                 (protection & 2U) != 0U, &address,
                                 sys_vm_auto_skip, vm)) {
        mm_vm_frame_free(backing, pages);
        return -BK_ENOMEM;
    }
    if (!protection && !paging_process_access(process_id,address,rounded,false,false)) {
        paging_process_unmap(process_id,address,rounded);
        mm_vm_frame_free(backing,pages);return -BK_ENOMEM;
    }
    mapping->backing = backing;
    mapping->address = address;
    mapping->length = rounded;
    mapping->protection = protection;
    mapping->flags = flags;
    if (!anonymous) {
#ifdef BLESK_WINE_VERBOSE_TRACE
        kprintf("[VM:FILEMAP] pid=%u base=%x len=%x off=%x prot=%x flags=%x fd=%d path=%s\n",
                process_id, address, rounded, mapped_file_offset, protection, flags,
                mapped_raw_fd, mapped_path ? mapped_path : "?");
#endif
        syscall_linux_trace_observe_filemap(process_id, mapped_path);
    }
    paging_switch_process(process_id);
    return (int32_t)address;
}

static int32_t sys_vm_munmap(uint32_t address, uint32_t length) {
    uint32_t process_id = task_current_process_id();
    syscall_resources_t *resource = resource_for(process_id, false);
    syscall_vm_state_t *vm = sys_vm_state(resource, false);
    uint32_t rounded, end, free_slots = 0U, split_slots = 0U;

    /* Linux requires a page-aligned start and a non-zero length.  The length
     * itself need not be aligned; it is rounded up.  Most importantly,
     * munmap() is not an "exact mapping delete" operation: holes in the
     * requested interval are simply ignored and an entirely unmapped range
     * succeeds.  Wine uses that behaviour in check_vmsplit() with
     * munmap(0x80000000, 1). */
    if (!process_id || !length || (address & (PAGING_PAGE_SIZE - 1U)) ||
        length > 0xFFFFFFFFU - (PAGING_PAGE_SIZE - 1U))
        return -BK_EINVAL;
    rounded = (length + PAGING_PAGE_SIZE - 1U) & ~(PAGING_PAGE_SIZE - 1U);
    if (address + rounded < address) return -BK_EINVAL;
    end = address + rounded;

    /* No mmap bookkeeping means the interval is a hole from this subsystem's
     * point of view.  Linux reports success for that case. */
    if (!vm) return 0;

    /* brk/ELF/task-owned ranges are not represented by syscall_vm_map_t yet.
     * Do not claim that we tore one of those down until their ownership can be
     * split safely.  Normal holes, including Wine's vmsplit probe, still
     * return success below. */
    if (vm->brk_capacity) {
        uint32_t brk_end = vm->brk_base + vm->brk_capacity;
        if (brk_end >= vm->brk_base && address < brk_end && vm->brk_base < end)
            return -BK_ENOSYS;
    }
    if (elf_linux_range_occupied(process_id, address, rounded) ||
        task_process_user_range_occupied(process_id, address, rounded))
        return -BK_ENOSYS;

    /* Preflight before changing page tables. One free metadata slot is needed
     * whenever munmap punches a hole through the middle of one VMA. Cached
     * private file views may now be split too: each resulting VMA keeps one
     * reference to the same inode-backed cache object. */
    for (uint32_t i = 0U; i < vm->map_capacity; i++) {
        syscall_vm_map_t *m = &vm->maps[i];
        uint32_t map_end, cut_start, cut_end;
        if (!m->address || !m->length) { free_slots++; continue; }
        map_end = m->address + m->length;
        if (map_end < m->address || address >= map_end || m->address >= end)
            continue;
        cut_start = address > m->address ? address : m->address;
        cut_end = end < map_end ? end : map_end;
        if (m->address < cut_start && cut_end < map_end) split_slots++;
    }
    if (split_slots > free_slots &&
        !sys_vm_ensure_free_slots(vm, split_slots))
        return -BK_ENOMEM;

    /* Only true MAP_SHARED views may have modified the inode-backed cache and
     * therefore require writeback. MAP_PRIVATE cached views are COW and never
     * dirty the shared object. */
    for (uint32_t i = 0U; i < vm->map_capacity; i++) {
        syscall_vm_map_t *m = &vm->maps[i];
        uint32_t map_end;
        if (!m->address || !m->length || !m->shared || (m->flags&3U)!=1U) continue;
        map_end = m->address + m->length;
        if (map_end < m->address || address >= map_end || m->address >= end)
            continue;
        if (!vfs_shared_sync(m->shared)) return -BK_EIO;
    }

    for (uint32_t i = 0U; i < vm->map_capacity; i++) {
        syscall_vm_map_t *m = &vm->maps[i];
        syscall_vm_map_t old;
        uint32_t map_end, cut_start, cut_end, cut_length, backing_cut = 0U;
        if (!m->address || !m->length) continue;
        map_end = m->address + m->length;
        if (map_end < m->address || address >= map_end || m->address >= end)
            continue;

        old = *m;
        cut_start = address > old.address ? address : old.address;
        cut_end = end < map_end ? end : map_end;
        cut_length = cut_end - cut_start;

        if(old.flags&VM_MAP_PAGED) {
            if(!pager_unmap(process_id,cut_start,cut_length))return -BK_EIO;
        } else if (old.backing || old.shared) {
            if (!paging_process_unmap(process_id, cut_start, cut_length))
                return -BK_EIO;
            if (!old.shared) {
                backing_cut = old.backing + (cut_start - old.address);
                mm_vm_frame_free(backing_cut, cut_length / PAGING_PAGE_SIZE);
            }
        }

        if (cut_start == old.address && cut_end == map_end) {
            if (old.shared && !vfs_shared_release(old.shared)) return -BK_EIO;
            kmemset(m, 0, sizeof(*m));
            continue;
        }
        if (cut_start == old.address) {
            m->address = cut_end;
            m->length = map_end - cut_end;
            if (old.backing)
                m->backing = old.backing + (cut_end - old.address);
            continue;
        }
        if (cut_end == map_end) {
            m->length = cut_start - old.address;
            continue;
        }

        /* The unmapped interval is strictly inside this VMA.  Keep the left
         * side in-place and materialize the right side in a free slot. */
        syscall_vm_map_t *right = NULL;
        for (uint32_t j = 0U; j < vm->map_capacity; j++) {
            if (!vm->maps[j].address) { right = &vm->maps[j]; break; }
        }
        if (!right) return -BK_ENOMEM; /* preflight should make this impossible */
        *right = old;
        right->address = cut_end;
        right->length = map_end - cut_end;
        if (old.backing)
            right->backing = old.backing + (cut_end - old.address);
        if (old.shared) old.shared->references++;
        m->length = cut_start - old.address;
    }

    return 0;
}

/* A private mapping's futex identity is its address space and virtual word,
 * even without FUTEX_PRIVATE_FLAG. COW and swap can change its physical frame
 * while a waiter sleeps. Only true MAP_SHARED aliases use a physical key. */
static int32_t sys_vm_madvise(uint32_t address,uint32_t length,uint32_t advice) {
    if((address&4095U) || advice>4U || length>0xfffff000U ||
       (uint64_t)address+((length+4095U)&~4095U)>0xffffffffULL)return -BK_EINVAL;
    if(!length)return 0;
    uint32_t pid=task_current_process_id(),end=address+((length+4095U)&~4095U);
    if(advice!=4U && paging_user_range_permitted((const void *)(uintptr_t)address,end-address,false))return 0;
    syscall_resources_t *r=resource_for(pid,false);
    syscall_vm_state_t *vm=sys_vm_state(r,false);
    if(!vm)return -BK_ENOMEM;
    /* Validate VMAs without touching their contents, including PROT_NONE.
     * Advice 0..3 is a hint; advice 4 must actually release private pages. */
    for(unsigned pass=0;pass<(advice==4U?2U:1U);pass++) {
        uint32_t cursor=address;
        while(cursor<end) {
            uint32_t next=cursor;bool paged=false,shared=false,reservation=false;
            if(vm->brk_base && cursor>=vm->brk_base && cursor<((vm->brk_current+4095U)&~4095U)) {
                next=(vm->brk_current+4095U)&~4095U;paged=true;
            }
            for(uint32_t i=0;i<vm->map_capacity;i++) {
                syscall_vm_map_t *m=&vm->maps[i];uint32_t limit=m->address+m->length;
                if(m->address && m->address<=cursor && cursor<limit && limit>next) {
                    next=limit;paged=(m->flags&VM_MAP_PAGED)!=0U;
                    shared=(m->flags&3U)==1U;
                    reservation=!m->backing && !m->shared && !m->protection && !paged;
                }
            }
            if(next==cursor)return -BK_ENOMEM;
            if(next>end)next=end;
            if(advice==4U && !paged && !shared && !reservation)return -BK_EINVAL;
            if(pass && paged && !shared && !pager_reset(pid,cursor,next-cursor))return -BK_ENOMEM;
            cursor=next;
        }
    }
    return 0;
}

bool syscall_linux_futex_key(uint32_t address,bool private_key,uint32_t *key,uint32_t *scope) {
    uint32_t pid=task_current_process_id();
    *key=address;*scope=paging_process_owner(pid);
    if(private_key)return address!=0U;
    kernel_domains_enter(KDOMAIN_VFS);
    syscall_resources_t *r=resource_for(pid,false);
    if(r && r->vm_initialized)for(uint32_t i=0;i<r->vm.map_capacity;i++) {
        syscall_vm_map_t *m=&r->vm.maps[i];
        if(m->address && (m->flags&3U)==1U && address>=m->address &&
           address-m->address<m->length) {
            *key=paging_user_physical(address);*scope=0U;break;
        }
    }
    kernel_domains_exit(KDOMAIN_VFS);return *key!=0U;
}

static int32_t sys_vm_mprotect(uint32_t address, uint32_t length,
                               uint32_t protection) {
    uint32_t process_id = task_current_process_id();
    syscall_resources_t *resource = resource_for(process_id, false);
    syscall_vm_state_t *vm = sys_vm_state(resource, false);
    uint32_t end, rounded_end;
    if (!length || (protection & ~7U) || address + length < address)
        return -BK_EINVAL;
    if (address & (PAGING_PAGE_SIZE - 1U)) return -BK_EINVAL;
    end = address + length;
    rounded_end = (end + PAGING_PAGE_SIZE - 1U) &
                  ~(PAGING_PAGE_SIZE - 1U);
    if (rounded_end < end) return -BK_EINVAL;
    /* Reservations and direct ELF protection take different paths below;
     * neither may install/alter user PTEs over the live identity kernel. */
    if (sys_vm_hits_live_low_kernel(address, rounded_end - address))
        return -BK_ENOMEM;
    /* PIE/interpreter images currently have owned identity allocations in
     * the heap and need RELRO. Only the ELF loader may authorize those exact
     * ranges; an arbitrary reservation must not shadow allocator metadata,
     * kernel stacks, or the physical-frame pool used by CPL0. */
    if (mm_kernel_identity_overlaps(address, rounded_end - address))
        return elf_linux_protect(process_id, address,
            rounded_end - address, protection) ? 0 : -BK_ENOMEM;
    if (!vm) return elf_linux_protect(process_id,address,
        rounded_end-address,protection) ? 0 : -BK_ENOMEM;

    /* Linux accepts one mprotect over adjacent VMAs. Incremental Win32
     * commits commonly span an already-backed prefix and a reserved tail;
     * ELF RELRO can also span several cached file views. Preserve each
     * backing instead of replacing or zeroing the previously committed data.
     * Preflight coverage and shared-file permissions before changing pages. */
    uint32_t covered_bytes = 0U, intersections = 0U, paged_bytes = 0U;
    /* VMAs never overlap. Count their intersections in one pass, rather
     * than rescanning the entire table for every page-sized heap commit. */
    for (uint32_t i = 0; i < vm->map_capacity; ++i) {
        syscall_vm_map_t *m = &vm->maps[i];
        uint32_t limit = m->address + m->length;
        if (!m->address || limit <= m->address ||
            m->address >= rounded_end || limit <= address) continue;
        if (m->shared && (m->flags & 0x80000000U) && (protection & 2U))
            return -13;
        uint32_t first = m->address > address ? m->address : address;
        uint32_t last = limit < rounded_end ? limit : rounded_end;
        covered_bytes += last - first;
        if(m->flags&VM_MAP_PAGED)paged_bytes += last - first;
        intersections++;
    }
    if (covered_bytes == rounded_end - address && intersections > 1U) {
        /* Paged commits have no contiguous backing pointer. Protect them in
         * one region walk; treating them as reservations recursively scanned
         * the complete VMA table once for every old heap commit. */
        if(paged_bytes && !pager_protect_mapped(process_id,address,
                rounded_end-address,protection,paged_bytes))return -BK_ENOMEM;
        /* Backed VMAs already own their pages. Protect each intersection
         * directly: recursion used to repeat full-table coverage scans for
         * every old commit whenever Wine extended a texture allocation. */
        for (uint32_t i = 0; i < vm->map_capacity; ++i) {
            syscall_vm_map_t *m = &vm->maps[i];
            uint32_t limit = m->address + m->length;
            if (!m->address || m->address >= rounded_end || limit <= address)continue;
            if(m->flags&VM_MAP_PAGED){m->protection=protection;continue;}
            if(!m->backing)continue;
            uint32_t first = m->address > address ? m->address : address;
            uint32_t last = limit < rounded_end ? limit : rounded_end;
            bool ok = m->shared && (m->flags & 3U) == 2U
                ? paging_process_cow_access(process_id, first, last - first,
                    protection != 0U, (protection & 2U) != 0U)
                : paging_process_access(process_id, first, last - first,
                    protection != 0U, (protection & 2U) != 0U);
            if (!ok) return -BK_ENOMEM;
            m->protection = protection;
        }
        /* Commit only virtual reservations. A commit can grow/reallocate
         * maps; fetch the entry again on the next iteration. */
        for (uint32_t i = 0; i < vm->map_capacity; ++i) {
            syscall_vm_map_t *m = &vm->maps[i];
            uint32_t limit = m->address + m->length;
            if (!m->address || m->backing || (m->flags&VM_MAP_PAGED) ||
                m->address >= rounded_end || limit <= address) continue;
            uint32_t first = m->address > address ? m->address : address;
            uint32_t last = limit < rounded_end ? limit : rounded_end;
            int32_t result = sys_vm_mprotect(first, last - first, protection);
            if (result) return result;
        }
        return 0;
    }

    for(uint32_t i=0;i<vm->map_capacity;i++) {
        syscall_vm_map_t *m=&vm->maps[i];
        if((m->flags&VM_MAP_PAGED) && m->address<=address &&
           rounded_end<=m->address+m->length) {
            if(!pager_protect(process_id,address,rounded_end-address,protection))return -BK_ENOMEM;
            m->protection=protection;return 0;
        }
    }

    /* Promote a virtual anonymous reservation through the pager. Wine commits
     * large heaps/stacks with mprotect; eager contiguous frames here defeated
     * demand paging and forced fork to copy every committed byte again. */
    if (protection) {
        bool split_needed = false;
        uint32_t span = rounded_end - address;
        if (sys_vm_virtual_reservations_cover(vm, address, span,
                                               &split_needed)) {
            syscall_vm_map_t *commit = NULL;
            uint32_t free_slots = 0U;
            uint32_t inherited_flags = 0x22U; /* PRIVATE|ANONYMOUS fallback */
            uint32_t needed_slots = split_needed ? 2U : 1U;

            if (!sys_vm_ensure_free_slots(vm, needed_slots))
                return -BK_ENOMEM;
            for (uint32_t i = 0U; i < vm->map_capacity; i++) {
                syscall_vm_map_t *m = &vm->maps[i];
                uint32_t map_end;
                if (!m->address) {
                    free_slots++;
                    if (!commit) commit = m;
                    continue;
                }
                map_end = m->address + m->length;
                if (!m->backing && !m->shared && !m->protection &&
                    (m->flags & 3U) == 2U && m->address <= address &&
                    address < map_end)
                    inherited_flags = m->flags;
            }
            if (!commit || free_slots < (split_needed ? 2U : 1U))
                return -BK_ENOMEM;
            uint32_t mapped;
            if (!pager_map(process_id,address,span,protection,true,&mapped,NULL,NULL))
                return -BK_ENOMEM;
            if (!sys_vm_carve_virtual_reservations(vm, address, span, commit)) {
                (void)pager_unmap(process_id,address,span);
                return -BK_ENOMEM;
            }
            commit->backing = 0U;
            commit->address = address;
            commit->length = span;
            commit->protection = protection;
            commit->flags = inherited_flags | VM_MAP_PAGED;
            commit->shared = NULL;
            paging_switch_process(process_id);
#ifdef BLESK_WINE_VERBOSE_TRACE
            kprintf("[VM:MPROTECT] pid=%u base=%x len=%x prot=%x committed\n",
                    process_id, address, span, protection);
#endif
            return 0;
        }
    }

    for (uint32_t i = 0U; i < vm->map_capacity; i++) {
        syscall_vm_map_t *mapping = &vm->maps[i];
        uint32_t map_start = mapping->address;
        uint32_t map_end = map_start + mapping->length;
        if (mapping->address && !mapping->backing && !mapping->shared &&
            address >= map_start && rounded_end <= map_end) {
            /* Keeping an existing reservation at PROT_NONE is a no-op. */
            if (!protection) return 0;
            return -BK_ENOMEM;
        }
        if (mapping->backing && address >= map_start &&
            rounded_end <= map_end) {
            if(mapping->shared && (mapping->flags&0x80000000U) && (protection&2))return -13;
            if(mapping->shared && (mapping->flags&3U)==2U) {
                if(!paging_process_cow_access(process_id,address,rounded_end-address,
                                               protection!=0,(protection&2U)!=0))return -BK_ENOMEM;
                mapping->protection=protection;return 0;
            }
            if (!paging_process_access(process_id, address,
                                        rounded_end - address, protection!=0,
                                        (protection & 2U) != 0U))
                return -BK_ENOMEM;
            mapping->protection = protection;
            return 0;
        }
    }
    if (vm->brk_capacity && address >= vm->brk_base &&
        rounded_end <= vm->brk_base + vm->brk_capacity)
        return pager_protect(process_id, address,
                             rounded_end-address,protection)
            ? 0 : -BK_ENOMEM;
    return elf_linux_protect(process_id,address,
                              rounded_end-address,protection) ? 0 : -BK_ENOMEM;
}

bool syscall_linux_cow_fault(uint32_t address) {
    if (!paging_cow_write_pending(address)) return false;
    kernel_domains_enter(KDOMAIN_VFS);
    uint32_t pid=task_current_process_id();
    syscall_vm_state_t *vm=sys_vm_state(resource_for(pid,false),false);
    bool ok=false;
    if(vm)for(uint32_t i=0;i<vm->map_capacity;i++) {
        syscall_vm_map_t *m=&vm->maps[i];
        if(!m->address || address<m->address || address-m->address>=m->length)continue;
        if(m->flags&VM_MAP_PAGED){ok=pager_fault(pid,address,true);break;}
        if(!m->shared || (m->flags&3U)!=2U)break;
        if(!paging_cow_write_pending(address)) {ok=true;break;}
        uint32_t backing;
        if(!pager_frame_alloc(m->length/4096U,&backing))break;
        kmemcpy((void *)backing,(void *)m->backing,m->length);
        if(!paging_process_cow_detach(pid,m->address,m->length,backing)) {
            mm_vm_frame_free(backing,m->length/4096U);break;
        }
        vfs_shared_mapping_t *shared=m->shared;
        m->backing=backing;m->shared=NULL;
        (void)vfs_shared_release(shared);
        ok=true;break;
    }
    kernel_domains_exit(KDOMAIN_VFS);
    return ok;
}

typedef struct PACKED {
    uint64_t device;
    uint32_t pad0;
    uint32_t inode_legacy;
    uint32_t mode, links, uid, gid;
    uint64_t rdevice;
    uint32_t pad1;
    int64_t size;
    uint32_t block_size;
    uint64_t blocks;
    uint32_t access_time, access_nsec;
    uint32_t modify_time, modify_nsec;
    uint32_t change_time, change_nsec;
    uint64_t inode;
} linux_i386_stat64_t;

uint32_t syscall_linux_random_word(void) {
    static kspinlock_t random_lock=KSPINLOCK_INITIALIZER;
    static uint32_t state=0x6d2b79f5U, counter;
    uint32_t eax=1U,ebx,ecx,edx,value=0U;
    __asm__ volatile("cpuid"
        : "+a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx));
    if(ecx&(1U<<30)) for(uint32_t attempt=0U;attempt<10U;attempt++) {
        uint8_t ready;
        __asm__ volatile(".byte 0x0f,0xc7,0xf0; setc %1"
            : "=a"(value),"=qm"(ready));
        if(ready) return value;
    }
    /* Preserve state across words, callers and CPUs. Reseeding from the
     * same tick/stack address returned identical words throughout a random
     * buffer, giving Wine duplicate RPC UUIDs. Timing remains a fallback
     * seed, not a cryptographic entropy guarantee on machines without RDRAND. */
    uint32_t lo=0U,hi=0U;
    if(edx&(1U<<4))__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    uint32_t lock=kspin_lock_irqsave(&random_lock);
    value=state ^ (++counter*0x9E3779B9U) ^ lo ^ hi ^ pit_get_ticks();
    value^=value<<13; value^=value>>17; value^=value<<5;
    state=value?value:0x6d2b79f5U;
    kspin_unlock_irqrestore(&random_lock,lock);
    return value;
}
typedef char linux_i386_stat64_must_be_96_bytes[
    sizeof(linux_i386_stat64_t) == 96U ? 1 : -1];
static bool linux_unix_stat(const char *path,linux_i386_stat64_t *status);
static bool linux_io_stat(int raw,linux_i386_stat64_t *status);

typedef struct { int32_t seconds, microseconds; } linux_i386_timeval_t;
typedef struct { int32_t seconds, nanoseconds; } linux_i386_timespec_t;
typedef struct { int64_t seconds, nanoseconds; } linux_i386_timespec64_t;
typedef char linux_i386_timespec64_must_be_16_bytes[
    sizeof(linux_i386_timespec64_t) == 16U ? 1 : -1];
typedef struct { uint16_t rows, columns, x_pixels, y_pixels; }
    linux_i386_winsize_t;
typedef struct PACKED {
    uint32_t inode;
    uint32_t offset;
    uint16_t record_length;
    char name[256];
    uint16_t padding;
} linux_i386_kernel_dirent_t;
typedef char linux_i386_kernel_dirent_must_be_268_bytes[
    sizeof(linux_i386_kernel_dirent_t) == 268U ? 1 : -1];
typedef struct { char value[6][65]; } linux_i386_utsname_t;
typedef struct PACKED {
    int32_t uptime;
    uint32_t loads[3];
    uint32_t total_ram, free_ram, shared_ram, buffer_ram;
    uint32_t total_swap, free_swap;
    uint16_t processes, pad;
    uint32_t total_high, free_high, memory_unit;
    uint8_t reserved[8];
} linux_i386_sysinfo_t;
typedef char linux_i386_sysinfo_must_be_64_bytes[
    sizeof(linux_i386_sysinfo_t)==64U ? 1 : -1];
typedef struct { uint32_t current, maximum; } linux_i386_rlimit_t;

static bool linux_leap_year(uint32_t year) {
    return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

static uint32_t linux_realtime_seconds_uncached(void) {
    static const uint16_t before_month[12] =
        {0,31,59,90,120,151,181,212,243,273,304,334};
    rtc_datetime_t now;
    uint64_t days = 0U;
    if (!rtc_get_datetime(&now) || now.date.year < 1970U ||
        now.date.month < 1U || now.date.month > 12U || !now.date.day)
        return pit_get_frequency_hz()
            ? pit_get_ticks() / pit_get_frequency_hz() : 0U;
    for (uint32_t year=1970U; year<now.date.year; year++)
        days += linux_leap_year(year) ? 366U : 365U;
    days += before_month[now.date.month-1U] + now.date.day - 1U;
    if (now.date.month > 2U && linux_leap_year(now.date.year)) days++;
    return (uint32_t)(days*86400ULL + now.time.hour*3600U +
                      now.time.minute*60U + now.time.second);
}

/* Interpolate the wall clock with PIT ticks between CMOS samples. Reading
 * calendar registers once per tick made Wine's startup perform thousands of
 * slow port-I/O transactions. A one-second resync interval retains external
 * RTC adjustments. Ignore +/-1 calendar second of sampling phase: combining
 * fresh RTC seconds with an unrelated PIT fraction used to run backwards at
 * second boundaries. All realtime ABIs and absolute futex waits use this pair. */
static kspinlock_t g_linux_realtime_lock = KSPINLOCK_INITIALIZER;
static uint32_t g_linux_realtime_tick, g_linux_realtime_sample_tick;
static uint32_t g_linux_realtime_value, g_linux_realtime_fraction;
static uint32_t g_linux_realtime_hz;
static bool g_linux_realtime_valid;

static uint64_t linux_realtime_nanoseconds(void) {
    uint32_t irq = kspin_lock_irqsave(&g_linux_realtime_lock);
    uint32_t tick = pit_get_ticks(), hz = pit_get_frequency_hz();
    if (!g_linux_realtime_valid || !hz || hz != g_linux_realtime_hz) {
        g_linux_realtime_value = linux_realtime_seconds_uncached();
        g_linux_realtime_fraction = hz ? tick % hz : 0U;
        g_linux_realtime_sample_tick = tick;
        g_linux_realtime_hz = hz;
        g_linux_realtime_valid = true;
    } else {
        /* Unsigned subtraction handles a PIT wrap; the 64-bit sum also
         * permits a long interval without callers and loses no partial tick. */
        uint64_t elapsed = (uint64_t)(uint32_t)(tick - g_linux_realtime_tick)
                         + g_linux_realtime_fraction;
        g_linux_realtime_value += (uint32_t)(elapsed / hz);
        g_linux_realtime_fraction = (uint32_t)(elapsed % hz);
        if ((uint32_t)(tick - g_linux_realtime_sample_tick) >= hz) {
            uint32_t rtc = linux_realtime_seconds_uncached();
            int64_t difference = (int64_t)rtc - g_linux_realtime_value;
            if (difference < -1 || difference > 1)
                g_linux_realtime_value = rtc;
            g_linux_realtime_sample_tick = tick;
        }
    }
    g_linux_realtime_tick = tick;
    uint64_t value = (uint64_t)g_linux_realtime_value * 1000000000ULL;
    if (hz) value += (uint64_t)g_linux_realtime_fraction * 1000000000ULL / hz;
    kspin_unlock_irqrestore(&g_linux_realtime_lock, irq);
    return value;
}

static uint32_t linux_realtime_seconds(void) {
    return (uint32_t)(linux_realtime_nanoseconds() / 1000000000ULL);
}

static void linux_fill_stat64(linux_i386_stat64_t *status, uint32_t size,
                              bool directory) {
    uint32_t now = linux_realtime_seconds();
    kmemset(status, 0, sizeof(*status));
    status->device = 1U;
    status->inode_legacy = 1U;
    status->inode = 1U;
    status->mode = directory ? 0040755U : 0100644U;
    status->links = directory ? 2U : 1U;
    status->size = size;
    status->block_size = PAGING_PAGE_SIZE;
    status->blocks = (size + 511U) / 512U;
    status->access_time = status->modify_time = status->change_time = now;
}

static void linux_apply_vfs_times(linux_i386_stat64_t *status,
                                  const char *path) {
    uint32_t accessed, modified;
    if (path && vfs_get_times(path, &accessed, &modified)) {
        status->access_time = accessed;
        status->modify_time = modified;
        status->change_time = modified;
    }
}

static uint16_t linux_current_umask(void) {
    syscall_resources_t *resource =
        resource_for(task_current_process_id(), true);
    return resource ? (uint16_t)(resource->linux_umask & 0777U) : 0022U;
}

static uint16_t linux_creation_mode(uint32_t mode) {
    uint16_t mask = linux_current_umask();
    return (uint16_t)((mode & 07777U) & ~(uint32_t)mask);
}

static uint16_t linux_set_umask(uint32_t mask) {
    syscall_resources_t *resource =
        resource_for(task_current_process_id(), true);
    uint16_t old = resource ? resource->linux_umask : 0022U;
    if (resource) resource->linux_umask = (uint16_t)(mask & 0777U);
    return old;
}

/* Missing split-archive volumes are otherwise swallowed by Wine's file
 * lookup and surface only as InstallShield's generic transfer error -2.
 * Keep a bounded, deduplicated failure-only diagnostic, never successful I/O. */
static void linux_missing_installer_volume(const char *path) {
    const char *name=path;
    for(const char *p=path;*p;++p)if(*p=='/')name=p+1;
    if((name[0]!='d' && name[0]!='D') || (name[1]!='a' && name[1]!='A') ||
       (name[2]!='t' && name[2]!='T') || (name[3]!='a' && name[3]!='A') || name[4]!='.')return;
    const char *suffix=name+5;
    if(!*suffix)return;
    if(!((suffix[0]=='z' || suffix[0]=='Z') && !suffix[1])) {
        for(const char *p=suffix;*p;++p)if(*p<'0' || *p>'9')return;
    }
    static uint32_t seen[32],count;
    uint32_t hash=task_current_process_id()^2166136261U;
    for(const char *p=path;*p;++p)hash=(hash^(uint8_t)*p)*16777619U;
    for(unsigned i=0;i<count;++i)if(seen[i]==hash)return;
    if(count>=32)return;
    seen[count++]=hash;
    kprintf("[LINUX:INSTALL-VOLUME] missing pid=%u path=%s\n",task_current_process_id(),path);
}

/* Wine's directory-notification backend names a live directory descriptor
 * through /proc/self/fd/N, also appending child names and /.. to that link.
 * Resolve only our own live VFS descriptions; never alias an unlinked old
 * description to a replacement file that now occupies the same pathname. */
static int32_t linux_resolve_proc_fd(const char *path,char *full,bool *direct) {
    static const char prefix[]="/proc/self/fd/";
    if(direct)*direct=false;
    if(kstrncmp(path,prefix,sizeof(prefix)-1U)) {
        if(path!=full)kstrcpy(full,path);
        return 0;
    }
    const char *tail=path+sizeof(prefix)-1U;
    if(*tail<'0' || *tail>'9')return -BK_ENOENT;
    uint32_t fd=0;
    do {
        if(fd>(SYSCALL_FD_SLOTS+3U)/10U)return -BK_ENOENT;
        fd=fd*10U+(uint32_t)(*tail++-'0');
    }while(*tail>='0' && *tail<='9');
    if((*tail && *tail!='/') || fd>=SYSCALL_FD_SLOTS+3U)return -BK_ENOENT;
    int raw=resource_fd(resource_for(task_current_process_id(),false),fd);
    const char *base=raw>=0 && raw<LINUX_IO_BASE?vfs_fd_path(raw):NULL;
    if(!base)return -BK_ENOENT;
    uint64_t d,i,actual_d,actual_i;
    if(!vfs_fd_identity(raw,&d,&i) || !vfs_identity(base,&actual_d,&actual_i) ||
       d!=actual_d || i!=actual_i)return -BK_ENOENT;
    if(*tail && !vfs_is_directory(raw))return -20;
    if(kstrlen(base)+kstrlen(tail)>=VFS_MAX_PATH)return -36;
    char expanded[VFS_MAX_PATH];kstrcpy(expanded,base);kstrcat(expanded,tail);
    if(!vfs_normalize_nofollow(expanded,full))return -40;
    if(direct)*direct=!*tail;
    return 0;
}

static int32_t linux_stat_path(const char *path, void *user_status) {
    char resolved[VFS_MAX_PATH];
    int32_t result=linux_resolve_proc_fd(path,resolved,NULL);
    if(result<0)return result;
    path=resolved;
    vfs_dir_entry_t entry;
    linux_i386_stat64_t status;
    if(linux_unix_stat(path,&status))return copy_to_user(user_status,&status,sizeof(status))?0:-BK_EFAULT;
    if (!vfs_stat(path, &entry)) {linux_missing_installer_volume(path);return -BK_ENOENT;}
    linux_fill_stat64(&status, entry.size, entry.type == VFS_NODE_DIR);
    linux_apply_vfs_times(&status, path);
    uint16_t mode;
    if (vfs_get_mode(path, &mode))
        status.mode = (entry.type == VFS_NODE_DIR ? 0040000U : 0100000U) |
                      (uint32_t)(mode & 07777U);
    uint64_t device,inode;
    if (!vfs_identity(path,&device,&inode)) return -BK_EIO;
    status.device=device;status.inode=inode;
    status.inode_legacy=(uint32_t)status.inode;
    return copy_to_user(user_status, &status, sizeof(status)) ? 0 : -BK_EFAULT;
}
static int32_t linux_lstat_path(const char *path,void *user_status) {
    char target[VFS_MAX_PATH];int n=vfs_readlink(path,target,sizeof(target));
    if(n<0)return (n==-22 || n==-2)?linux_stat_path(path,user_status):n;
    linux_i386_stat64_t status;linux_fill_stat64(&status,n,false);status.mode=0120777;
    uint64_t device,inode;
    if(!vfs_identity_nofollow(path,&device,&inode))return -BK_EIO;
    status.device=device;status.inode=inode;
    status.inode_legacy=(uint32_t)status.inode;
    return copy_to_user(user_status,&status,sizeof(status))?0:-BK_EFAULT;
}
static int32_t sys_linux_stat64(registers_t *regs, const char *user_path,
                                void *user_status) {
    char path[VFS_MAX_PATH];
    if (!copy_user_string(regs,user_path,path,sizeof(path)) ||
        !user_write_range_ok(regs,user_status,sizeof(linux_i386_stat64_t))) return -BK_EFAULT;
    return linux_stat_path(path,user_status);
}

static int32_t sys_linux_fstat64(registers_t *regs, uint32_t user_fd,
                                 void *user_status) {
    syscall_resources_t *resource =
        resource_for(task_current_process_id(), false);
    linux_i386_stat64_t status;
    int raw_fd = resource_fd(resource, user_fd);
    int32_t size;
    if (!user_write_range_ok(regs, user_status, sizeof(status)))
        return -BK_EFAULT;
    if(raw_fd>=LINUX_IO_BASE) {
        if(!linux_io_stat(raw_fd,&status))return -BK_EBADF;
        return copy_to_user(user_status,&status,sizeof(status))?0:-BK_EFAULT;
    }
    if (user_fd < 3U && raw_fd < 0) size = 0;
    else {
        if (raw_fd < 0) return -BK_EBADF;
        size = vfs_size(raw_fd);
        if (size < 0) return -BK_EIO;
    }
    linux_fill_stat64(&status, (uint32_t)size, false);
    if (raw_fd>=0) {
        linux_apply_vfs_times(&status, vfs_fd_path(raw_fd));
        uint64_t device,inode;
        if (!vfs_fd_identity(raw_fd,&device,&inode)) return -BK_EIO;
        status.device=device;status.inode=inode;
        status.inode_legacy=(uint32_t)status.inode;
        bool directory = vfs_is_directory(raw_fd);
        uint16_t mode;
        if (vfs_fd_mode(raw_fd, &mode))
            status.mode = (directory ? 0040000U : 0100000U) |
                          (uint32_t)(mode & 07777U);
        else if (directory) status.mode=0040755U;
        if (directory) status.links=2;
    } else { status.mode=0020666;status.device=5;status.inode=user_fd+1; }
    return copy_to_user(user_status, &status, sizeof(status)) ? 0 : -BK_EFAULT;
}

static int32_t sys_linux_clock(registers_t *regs, uint32_t clock_id,
                               void *user_time, bool resolution) {
    linux_i386_timespec_t value;
    uint32_t hz = pit_get_frequency_hz();
    uint32_t ticks = pit_get_ticks();
    if (!user_write_range_ok(regs, user_time, sizeof(value))) return -BK_EFAULT;
    /* glibc 2.39 implements __time64() with CLOCK_REALTIME_COARSE (5).
     * It first tries clock_gettime64(403); ENOSYS there is valid and makes
     * glibc fall back to legacy i386 clock_gettime(265).  BlesKernOS used to
     * reject id 5 in that fallback with EINVAL, leaving Wine's timezone
     * initialization without a usable realtime value and eventually reaching
     * __mktime_internal(NULL).  Until a distinct coarse cache is worthwhile,
     * CLOCK_REALTIME_COARSE can use the same source as CLOCK_REALTIME. */
    if (clock_id != 0U && clock_id != 1U && clock_id != 4U &&
        clock_id != 5U && clock_id != 7U)
        return -BK_EINVAL;
    if (resolution) {
        value.seconds = 0;
        value.nanoseconds = hz ? (int32_t)(1000000000U / hz) : 10000000;
    } else if (clock_id == 0U || clock_id == 5U) {
        uint64_t realtime = linux_realtime_nanoseconds();
        value.seconds = (int32_t)(realtime / 1000000000ULL);
        value.nanoseconds = (int32_t)(realtime % 1000000000ULL);
    } else {
        value.seconds = hz ? (int32_t)(ticks / hz) : 0;
        value.nanoseconds = hz ? (int32_t)((ticks % hz) *
            (1000000000U / hz)) : 0;
    }
    return copy_to_user(user_time, &value, sizeof(value)) ? 0 : -BK_EFAULT;
}

/* Linux i386 gained the time64 clock syscalls at numbers 403/406.  Modern
 * glibc prefers them and only falls back to the legacy 32-bit ABI for some
 * clock ids.  Wine's wineserver also queries CLOCK_BOOTTIME (7), for which
 * an old-kernel EINVAL fallback can turn into a tight retry loop.  BlesKernOS
 * has no suspend accounting yet, so CLOCK_BOOTTIME is currently equivalent to
 * CLOCK_MONOTONIC; CLOCK_REALTIME_COARSE (5) uses the realtime source. */
static int32_t sys_linux_clock64(registers_t *regs, uint32_t clock_id,
                                 void *user_time, bool resolution) {
    linux_i386_timespec64_t value;
    uint32_t hz = pit_get_frequency_hz();
    uint32_t ticks = pit_get_ticks();

    if (!user_write_range_ok(regs, user_time, sizeof(value))) return -BK_EFAULT;
    if (clock_id != 0U && clock_id != 1U && clock_id != 4U &&
        clock_id != 5U && clock_id != 7U)
        return -BK_EINVAL;

    if (resolution) {
        value.seconds = 0;
        value.nanoseconds = hz ? (int64_t)(1000000000U / hz) : 10000000LL;
    } else if (clock_id == 0U || clock_id == 5U) {
        uint64_t realtime = linux_realtime_nanoseconds();
        value.seconds = (int64_t)(realtime / 1000000000ULL);
        value.nanoseconds = (int64_t)(realtime % 1000000000ULL);
    } else {
        value.seconds = hz ? (int64_t)(ticks / hz) : 0;
        value.nanoseconds = hz ? (int64_t)(ticks % hz) * 1000000000LL / hz : 0;
    }
    return copy_to_user(user_time, &value, sizeof(value)) ? 0 : -BK_EFAULT;
}


#include "../linux/i386_fs.inc"

/* Linux i386 modify_ldt(2).  Keep this separate from BlesKernOS' legacy
 * NE/Win16 GDT allocator: Wine expects genuine TI=1 selectors backed by a
 * per-mm LDT. */
typedef struct {
    uint32_t entry_number;
    uint32_t base_addr;
    uint32_t limit;
    uint32_t flags;
} linux_ldt_user_desc_t;

static void linux_ldt_encode(uint8_t *out, const linux_ldt_user_desc_t *d) {
    uint32_t base=d->base_addr, limit=d->limit;
    uint32_t contents=(d->flags >> 1U) & 3U;
    bool seg32=(d->flags & 0x01U)!=0U;
    bool read_exec_only=(d->flags & 0x08U)!=0U;
    bool pages=(d->flags & 0x10U)!=0U;
    bool not_present=(d->flags & 0x20U)!=0U;
    uint8_t type;
    if (contents == 2U || contents == 3U)
        type=(uint8_t)(0x08U | (read_exec_only ? 0U : 0x02U));
    else
        type=(uint8_t)((contents == 1U ? 0x04U : 0U) |
                       (read_exec_only ? 0U : 0x02U));
    out[0]=(uint8_t)limit; out[1]=(uint8_t)(limit>>8U);
    out[2]=(uint8_t)base; out[3]=(uint8_t)(base>>8U);
    out[4]=(uint8_t)(base>>16U);
    out[5]=(uint8_t)(0x70U | type | (not_present ? 0U : 0x80U));
    out[6]=(uint8_t)(((limit>>16U)&0x0fU) | (seg32?0x40U:0U) |
                     (pages?0x80U:0U));
    out[7]=(uint8_t)(base>>24U);
}

static bool linux_ldt_empty(const linux_ldt_user_desc_t *d, uint32_t func) {
    bool canonical = d->base_addr==0U && d->limit==0U &&
        (d->flags & 0x7fU)==0x28U; /* read_exec_only + not_present */
    return canonical || (func==1U && d->base_addr==0U && d->limit==0U);
}

static int32_t sys_linux_modify_ldt(registers_t *regs, uint32_t func,
                                    uint32_t user_ptr, uint32_t bytecount) {
    syscall_resources_t *r=resource_for(task_current_process_id(),true);
    if (!r) return -BK_ENOMEM;
    if (func==0U || func==2U) {
        uint32_t amount;
        if (!user_ptr && bytecount) return -BK_EFAULT;
        if (func==2U) {
            static const uint8_t zeros[128]={0};
            uint32_t left=bytecount;
            uint8_t *dst=(uint8_t *)(uintptr_t)user_ptr;
            while(left) {
                uint32_t n=left>sizeof(zeros)?sizeof(zeros):left;
                if(!copy_to_user(dst,zeros,n)) return -BK_EFAULT;
                dst+=n;left-=n;
            }
            return (int32_t)bytecount;
        }
        amount=bytecount;
        if(amount>LINUX_LDT_BYTES) amount=LINUX_LDT_BYTES;
        if(amount>r->linux_ldt_highwater) amount=r->linux_ldt_highwater;
        if(amount && (!r->linux_ldt || !copy_to_user((void *)(uintptr_t)user_ptr,
                                                     r->linux_ldt,amount)))
            return -BK_EFAULT;
        return (int32_t)amount;
    }
    if (func==1U || func==0x11U) {
        linux_ldt_user_desc_t d;
        uint8_t encoded[8];
        if (!user_ptr || bytecount!=sizeof(d)) return -BK_EINVAL;
        if (!copy_from_user(&d,(const void *)(uintptr_t)user_ptr,sizeof(d)))
            return -BK_EFAULT;
        if (d.entry_number>=LINUX_LDT_ENTRIES || (d.flags & ~0x7fU))
            return -BK_EINVAL;
        /* Linux rejects conforming code for func=1, and for 0x11 while it is
         * present.  32-bit BlesKernOS has no lm bit to accept. */
        if (((d.flags>>1U)&3U)==3U && (func==1U || !(d.flags&0x20U)))
            return -BK_EINVAL;
        if (!r->linux_ldt) {
            r->linux_ldt=(uint8_t *)kmalloc(LINUX_LDT_BYTES);
            if(!r->linux_ldt) return -BK_ENOMEM;
            (void)mm_set_allocation_owner(r->linux_ldt,0U);
            kmemset(r->linux_ldt,0,LINUX_LDT_BYTES);
        }
        if (linux_ldt_empty(&d,func)) kmemset(r->linux_ldt+d.entry_number*8U,0,8U);
        else {
            linux_ldt_encode(encoded,&d);
            kmemcpy(r->linux_ldt+d.entry_number*8U,encoded,8U);
        }
        if ((d.entry_number+1U)*8U>r->linux_ldt_highwater)
            r->linux_ldt_highwater=(d.entry_number+1U)*8U;
        gdt_activate_user_ldt(r->linux_ldt,LINUX_LDT_BYTES);
#ifdef BLESK_WINE_VERBOSE_TRACE
        kprintf("[LINUX:LDT] pid=%u func=%x entry=%u base=%x limit=%x flags=%x\n",
                task_current_process_id(),func,d.entry_number,d.base_addr,d.limit,d.flags);
#endif
        return 0;
    }
    return -BK_ENOSYS;
}

void syscall_linux_activate_ldt(uint32_t process_id) {
    syscall_resources_t *r=resource_for(process_id,false);
    if (r && r->linux_ldt) gdt_activate_user_ldt(r->linux_ldt,LINUX_LDT_BYTES);
    else gdt_disable_user_ldt();
}

static uint32_t syscall_domain_mask(uint32_t number) {
    switch (number) {
        case SYS_WRITE:
            /* stdout/stderr pueden estar redirigidos a un archivo mediante
             * dup2(), por lo que write ya no es exclusivamente consola. */
            return KDOMAIN_DRIVER | KDOMAIN_VFS;
        case SYS_OPEN:
        case SYS_CLOSE:
        case SYS_FILE_WRITE:
        case SYS_FILE_SEEK:
        case SYS_FILE_TELL:
        case SYS_FILE_SIZE:
        case SYS_FILE_STAT:
        case SYS_FILE_SYNC:
        case SYS_FILE_TRUNCATE:
        case SYS_FILE_DUP:
        case SYS_LINUX_FCNTL64:
        case SYS_LINUX_CLONE:
        case SYS_GETCWD:
        case SYS_CHDIR:
        case SYS_MKDIR:
        case SYS_UNLINK:
        case SYS_RENAME:
        case SYS_GETDENTS:
            return KDOMAIN_VFS;
        case SYS_ALLOC:
        case SYS_REALLOC:
        case SYS_FREE:
            return KDOMAIN_TASK;
        case SYS_SPAWN:
        case SYS_LINUX_EXECVE:
            /* Loading/relocating an ELF does not mutate desktop state.
             * Reaping and error dialogs take GUI locally when needed; holding
             * it across disk I/O stalls unrelated native windows. */
            return KDOMAIN_TASK | KDOMAIN_VFS;
        case SYS_LINUX_STAT64:
        case SYS_LINUX_FSTAT64:
        case SYS_LINUX_ACCESS:
            return KDOMAIN_VFS;
        case SYS_WAITPID:
        case SYS_KILL:
        case SYS_THREAD_CREATE:
        case SYS_THREAD_JOIN:
        case SYS_THREAD_DETACH:
        case SYS_THREAD_EXIT:
        case SYS_THREAD_TLS_SET:
        case SYS_THREAD_TLS_GET:
        case SYS_SIGNAL_REGISTER:
        case SYS_SIGNAL_SEND:
        case SYS_PERSONALITY:
        case SYS_VM_BRK:
        case SYS_VM_MPROTECT:
        case SYS_LINUX_FUTEX:
        case SYS_LINUX_SET_THREAD_AREA:
        case SYS_LINUX_GET_THREAD_AREA:
        case SYS_LINUX_UNAME:
        case SYS_LINUX_GETTIMEOFDAY:
        case SYS_LINUX_CLOCK_GETTIME:
        case SYS_LINUX_CLOCK_GETRES:
        case SYS_LINUX_IOCTL:
            return KDOMAIN_TASK | KDOMAIN_VFS;
        case SYS_LINUX_SET_TID_ADDRESS:
        case SYS_LINUX_SET_ROBUST_LIST:
        case SYS_LINUX_TIME:
        case SYS_LINUX_NANOSLEEP:
        case SYS_LINUX_SYSINFO:
        case SYS_LINUX_GETRLIMIT:
        case SYS_LINUX_PRLIMIT64:
        case SYS_LINUX_GETRANDOM:
        case SYS_LINUX_SCHED_GETAFFINITY:
        case SYS_LINUX_SCHED_SETAFFINITY:
            return KDOMAIN_TASK;
        case SYS_VM_MMAP:
        case SYS_VM_MUNMAP:
        case SYS_LINUX_MADVISE:
            return KDOMAIN_TASK | KDOMAIN_VFS;
        case SYS_NET_RESOLVE:
        case SYS_NET_SOCKET:
        case SYS_NET_CONNECT:
        case SYS_NET_SEND:
        case SYS_NET_RECV:
        case SYS_NET_CLOSE:
        case SYS_NET_HTTP_GET:
            return KDOMAIN_NET;
        default:
            return 0U;
    }
}

/* Wine may retain overlapping reservation records around committed views.
 * Report the address union, rather than adding those records twice. */
static bool sys_vm_memory_totals(const syscall_vm_state_t *vm,uint32_t *reserved,uint32_t *committed) {
    typedef struct {uint32_t start,end;} interval_t;
    interval_t *ranges=kmalloc((vm->map_capacity+1U)*sizeof(*ranges));
    if(!ranges)return false;
    for(uint32_t pass=0;pass<2;pass++) {
        uint32_t count=0;
        uint32_t brk_length=pass?vm->brk_current-vm->brk_base:vm->brk_capacity;
        if(brk_length)ranges[count++]=(interval_t){vm->brk_base,vm->brk_base+brk_length};
        for(uint32_t i=0;i<vm->map_capacity;i++) {
            const syscall_vm_map_t *m=&vm->maps[i];
            if(!m->address || (pass && !m->protection))continue;
            interval_t entry={m->address,m->address+m->length};
            uint32_t j=count++;
            while(j && ranges[j-1].start>entry.start){ranges[j]=ranges[j-1];j--;}
            ranges[j]=entry;
        }
        uint32_t bytes=0,end=0;
        for(uint32_t i=0;i<count;i++) {
            uint32_t start=ranges[i].start>end?ranges[i].start:end;
            if(ranges[i].end>start)bytes+=ranges[i].end-start;
            if(ranges[i].end>end)end=ranges[i].end;
        }
        if(pass)*committed=bytes;else *reserved=bytes;
    }
    kfree(ranges);return true;
}
#include "../linux/i386_ipc.inc"
#include "../linux/i386_lock.inc"
#include "../linux/i386_exec.inc"
#include "../linux/i386_translate.inc"
#include "../linux/i386_trace.h"

static registers_t *syscall_dispatch(registers_t *regs) {
    char path[VFS_MAX_PATH];
    char second_path[VFS_MAX_PATH];
    char thread_name[TASK_NAME_LEN];
    int32_t result;
    uint32_t domain_mask;

    if (!regs) return regs;
    if (caller_is_user(regs) && task_current_is_linux_abi() && !linux_i386_translate(regs))
        return regs;
    if (regs->eax == SYS_UPCALL_RETURN) {
        kernel_domains_enter(KDOMAIN_GUI | KDOMAIN_WINE);
        if (!task_finish_user_upcall(regs)) {
            regs->eax = (uint32_t)-BK_EINVAL;
        } else {
            (void)task_prepare_user_upcall(regs);
        }
        kernel_domains_exit(KDOMAIN_GUI | KDOMAIN_WINE);
        return regs;
    }
    if ((regs->eax == SYS_SLEEP || regs->eax == SYS_YIELD) &&
        task_prepare_user_upcall(regs)) return regs;
    domain_mask = syscall_domain_mask(regs->eax);
    kernel_domains_enter(domain_mask);
#define SYSCALL_RETURN(frame_value) do { \
        kernel_domains_exit(domain_mask); \
        return (frame_value); \
    } while (0)
    switch (regs->eax) {
        case SYS_EXIT:
            task_exit_from_interrupt((int32_t)regs->ebx);
            SYSCALL_RETURN(task_schedule_voluntary(regs));
        case SYS_WRITE:
            regs->eax = (uint32_t)sys_write(regs, regs->ebx,
                (const void *)(uintptr_t)regs->ecx, regs->edx);
            break;
        case SYS_GETPID: regs->eax = task_current_process_id(); break;
        case SYS_GETPPID: regs->eax = task_current_parent_pid(); break;
        case SYS_YIELD:
            regs->eax = 0;
            SYSCALL_RETURN(task_schedule_voluntary(regs));
        case SYS_SLEEP:
            task_sleep_from_interrupt(regs->ebx);
            SYSCALL_RETURN(task_schedule_voluntary(regs));
        case SYS_UPTIME_MS: {
            uint32_t hz = pit_get_frequency_hz();
            regs->eax = hz ? (pit_get_ticks() * 1000U) / hz : 0;
            break;
        }
        case SYS_TICKS: regs->eax = pit_get_ticks(); break;
        case SYS_ABI_VERSION: regs->eax = SYSCALL_ABI_VERSION; break;
        case SYS_PERSONALITY:
            if (regs->ebx > 1U)
                regs->eax = (uint32_t)-BK_EINVAL;
            else
                regs->eax = task_set_current_linux_abi(regs->ebx != 0U)
                    ? 0U : (uint32_t)-BK_EACCES;
            break;
        case SYS_VM_BRK:
            regs->eax = sys_vm_brk(regs->ebx);
            break;
        case SYS_VM_MMAP:
            regs->eax = (uint32_t)sys_vm_mmap(
                regs->ebx, regs->ecx, regs->edx, regs->esi,
                (int32_t)regs->edi, regs->ebp);
            break;
        case SYS_VM_MUNMAP:
            regs->eax = (uint32_t)sys_vm_munmap(regs->ebx, regs->ecx);
            break;
        case SYS_VM_MPROTECT:
            regs->eax = (uint32_t)sys_vm_mprotect(
                regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_LINUX_FUTEX: {
            static volatile uint32_t futex_error_reports;
            uint32_t operation = regs->ecx & 0x7FU;
            bool private_key=(regs->ecx&128U)!=0;
            uint32_t mask=(operation==9 || operation==10)?regs->ebp:~0U;
            if ((regs->ebx&3) || !mask || (regs->ecx&~0x1ffU) ||
                ((regs->ecx&256U) && operation!=9)) {
                if (__sync_fetch_and_add(&futex_error_reports, 1U) < 32U)
                    kprintf("[LINUX:FUTEX] INVALID pid=%u op=%u addr=%x val=%x flags=%x mask=%x\n",
                            task_current_process_id(), operation, regs->ebx,
                            regs->edx, regs->ecx, mask);
                regs->eax=(uint32_t)-BK_EINVAL;break;
            }
            if (operation == 0U || operation == 9U) { /* WAIT/WAIT_BITSET */
                uint32_t ticks=0x7fffffffU;
                if (regs->esi) {
                    linux_i386_timespec_t ts;
                    if (!copy_from_user(&ts,(void *)regs->esi,sizeof(ts))) {regs->eax=(uint32_t)-BK_EFAULT;break;}
                    if (ts.seconds<0 || ts.nanoseconds<0 || ts.nanoseconds>=1000000000) {regs->eax=(uint32_t)-BK_EINVAL;break;}
                    int64_t ns=(int64_t)ts.seconds*1000000000LL+ts.nanoseconds;
                    uint32_t hz=pit_get_frequency_hz(),now=pit_get_ticks();
                    if (operation==9) ns-=(regs->ecx&256U)
                        ? (int64_t)linux_realtime_nanoseconds()
                        : (int64_t)now*1000000000LL/hz;
                    uint64_t delay=ns>0?((uint64_t)ns*hz+999999999ULL)/1000000000ULL:0;
                    ticks=delay>0x7fffffffU?0x7fffffffU:(uint32_t)delay;
                }
                regs->eax=(uint32_t)-110; /* vencimiento; WAKE escribe cero */
                int32_t rc=task_linux_futex_wait(regs,regs->ebx,regs->edx,mask,private_key,ticks);
                if (!rc) SYSCALL_RETURN(task_schedule_voluntary(regs));
                if (rc < 0 && rc != -BK_EAGAIN && rc != -4 && rc != -110 &&
                    __sync_fetch_and_add(&futex_error_reports, 1U) < 32U)
                    kprintf("[LINUX:FUTEX] WAIT pid=%u op=%u addr=%x val=%x flags=%x rc=%d\n",
                            task_current_process_id(), operation, regs->ebx,
                            regs->edx, regs->ecx, rc);
                regs->eax=(uint32_t)rc;break;
            }
            if (operation == 1U || operation == 10U) { /* WAKE */
                int32_t rc = (int32_t)regs->edx < 0 ? -BK_EINVAL :
                    task_linux_futex_wake(regs->ebx,regs->edx,mask,private_key);
                if (rc < 0 && __sync_fetch_and_add(&futex_error_reports, 1U) < 32U)
                    kprintf("[LINUX:FUTEX] WAKE pid=%u op=%u addr=%x count=%d flags=%x rc=%d\n",
                            task_current_process_id(), operation, regs->ebx,
                            (int32_t)regs->edx, regs->ecx, rc);
                regs->eax=(uint32_t)rc;
                break;
            }
            if (__sync_fetch_and_add(&futex_error_reports, 1U) < 32U)
                kprintf("[LINUX:FUTEX] UNSUPPORTED pid=%u op=%u addr=%x flags=%x\n",
                        task_current_process_id(), operation, regs->ebx, regs->ecx);
            regs->eax = (uint32_t)-BK_ENOSYS;
            break;
        }
        case SYS_LINUX_CLONE: {
            enum {
                LINUX_CLONE_VM=0x00000100U,
                LINUX_CLONE_FS=0x00000200U,
                LINUX_CLONE_FILES=0x00000400U,
                LINUX_CLONE_SIGHAND=0x00000800U,
                LINUX_CLONE_VFORK=0x00004000U,
                LINUX_CLONE_THREAD=0x00010000U,
                LINUX_CLONE_SYSVSEM=0x00040000U,
                LINUX_CLONE_SETTLS=0x00080000U,
                LINUX_CLONE_PARENT_SETTID=0x00100000U,
                LINUX_CLONE_CHILD_CLEARTID=0x00200000U,
                LINUX_CLONE_DETACHED=0x00400000U,
                LINUX_CLONE_CHILD_SETTID=0x01000000U
            };
            const uint32_t supported = LINUX_CLONE_VM | LINUX_CLONE_FS |
                LINUX_CLONE_FILES | LINUX_CLONE_SIGHAND |
                LINUX_CLONE_THREAD | LINUX_CLONE_SYSVSEM |
                LINUX_CLONE_SETTLS | LINUX_CLONE_PARENT_SETTID |
                LINUX_CLONE_CHILD_CLEARTID | LINUX_CLONE_DETACHED |
                LINUX_CLONE_CHILD_SETTID;
            uint32_t clone_flags=regs->ebx;
            uint32_t tls_base=task_current_tls_base();
            int32_t tls_entry=(int32_t)GDT_USER_TLS_MIN_ENTRY;
            int32_t tid;

            bool thread=(clone_flags&LINUX_CLONE_THREAD)!=0;
            bool vfork=(clone_flags & (LINUX_CLONE_VM|LINUX_CLONE_VFORK)) ==
                (LINUX_CLONE_VM|LINUX_CLONE_VFORK);
            const uint32_t shared=LINUX_CLONE_VM|LINUX_CLONE_FS|LINUX_CLONE_FILES|LINUX_CLONE_SIGHAND;
            const uint32_t process_flags=LINUX_CLONE_SETTLS|LINUX_CLONE_PARENT_SETTID|
                LINUX_CLONE_CHILD_SETTID|LINUX_CLONE_CHILD_CLEARTID|0xffU|
                (vfork ? LINUX_CLONE_VM|LINUX_CLONE_VFORK : 0U);
            if ((thread && ((clone_flags&shared)!=shared || (clone_flags&0xff) ||
                            (clone_flags&~supported) || !regs->ecx)) ||
                (!thread && ((clone_flags&~process_flags) || (clone_flags&0xff)!=17)) ||
                (regs->ecx&3)) {
                regs->eax=(uint32_t)-BK_EINVAL;
                break;
            }
            if ((regs->ecx && !user_write_range_ok(regs,
                    (void *)(uintptr_t)(regs->ecx-4U),4U)) ||
                ((clone_flags & LINUX_CLONE_PARENT_SETTID) &&
                 !user_write_range_ok(regs,(void *)(uintptr_t)regs->edx,4U)) ||
                ((clone_flags & (LINUX_CLONE_CHILD_SETTID |
                                 LINUX_CLONE_CHILD_CLEARTID)) &&
                 !user_write_range_ok(regs,(void *)(uintptr_t)regs->edi,4U))) {
                regs->eax=(uint32_t)-BK_EFAULT;
                break;
            }
            if (clone_flags & LINUX_CLONE_SETTLS) {
                struct linux_user_desc {
                    int32_t entry_number;
                    uint32_t base_addr, limit, flags;
                } descriptor;
                if (!copy_from_user(&descriptor,(const void *)regs->esi,sizeof(descriptor))) {
                    regs->eax=(uint32_t)-BK_EFAULT;
                    break;
                }
                if (descriptor.entry_number < (int32_t)GDT_USER_TLS_MIN_ENTRY ||
                    descriptor.entry_number >= (int32_t)(GDT_USER_TLS_MIN_ENTRY +
                                                          GDT_USER_TLS_COUNT) ||
                    descriptor.limit != 0xfffffU || descriptor.flags != 0x51U) {
                    kprintf("[LINUX:CLONE] reject TLS entry=%d base=%x limit=%x flags=%x gs=%x\n",
                            descriptor.entry_number, descriptor.base_addr,
                            descriptor.limit, descriptor.flags, regs->gs);
                    regs->eax=(uint32_t)-BK_EINVAL;break;
                }
                tls_entry=descriptor.entry_number;
                tls_base=descriptor.base_addr;
            }
            if (!thread) {regs->eax=(uint32_t)task_linux_fork(regs,tls_base);break;}
            tid=task_clone_linux_thread(regs,regs->ecx,tls_base,tls_entry,
                                        clone_flags,regs->edx,regs->edi);
            if (tid < 0) {
                regs->eax=(uint32_t)-BK_EAGAIN;
                break;
            }
            regs->eax=(uint32_t)tid;
            break;
        }
        case SYS_LINUX_SET_THREAD_AREA: {
            struct linux_user_desc {
                int32_t entry_number;
                uint32_t base_addr, limit, flags;
            } descriptor;
            int32_t actual_entry;
            if (!copy_from_user(&descriptor,
                    (const void *)(uintptr_t)regs->ebx,
                    sizeof(descriptor))) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            if (descriptor.base_addr && !user_write_range_ok(regs,
                    (void *)(uintptr_t)descriptor.base_addr, 4U)) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            actual_entry = descriptor.entry_number;
            if (!task_linux_set_thread_area(descriptor.entry_number,
                                             descriptor.base_addr,
                                             &actual_entry)) {
                regs->eax = (uint32_t)-BK_ESRCH;
                break;
            }
            descriptor.entry_number = actual_entry;
            if (!copy_to_user((void *)(uintptr_t)regs->ebx, &descriptor,
                              sizeof(descriptor))) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            /* Linux set_thread_area() installs a descriptor only.  Userland
             * chooses whether that selector belongs in FS or GS.  Forcing
             * both here destroys glibc GS when Wine allocates its FS/TEB. */
            regs->eax = 0U;
            break;
        }
        case SYS_LINUX_GET_THREAD_AREA: {
            struct linux_user_desc {
                int32_t entry_number;
                uint32_t base_addr, limit, flags;
            } descriptor;
            if (!copy_from_user(&descriptor,
                    (const void *)(uintptr_t)regs->ebx,
                    sizeof(descriptor))) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            if (!task_linux_get_thread_area(descriptor.entry_number,
                                             &descriptor.base_addr)) {
                regs->eax = (uint32_t)-BK_EINVAL;
                break;
            }
            descriptor.limit = 0xFFFFFU;
            descriptor.flags = 0x51U; /* 32-bit, pages, present, usable */
            regs->eax = copy_to_user((void *)(uintptr_t)regs->ebx,
                    &descriptor, sizeof(descriptor))
                ? 0U : (uint32_t)-BK_EFAULT;
            break;
        }
        case SYS_LINUX_EXECVE:
            result=sys_linux_execve(regs);
            if (!result) SYSCALL_RETURN(task_schedule_voluntary(regs));
            regs->eax=(uint32_t)result;break;
        case SYS_LINUX_UNAME: {
            linux_i386_utsname_t name;
            if (!user_write_range_ok(regs, (void *)(uintptr_t)regs->ebx,
                                     sizeof(name))) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            kmemset(&name, 0, sizeof(name));
            kstrncpy(name.value[0], "BlesKernOS", 64U);
            kstrncpy(name.value[1], "bleskernos", 64U);
            kstrncpy(name.value[2], "0.9", 64U);
            kstrncpy(name.value[3], "Linux ABI 17 Ring 3", 64U);
            kstrncpy(name.value[4], "i686", 64U);
            kstrncpy(name.value[5], "(none)", 64U);
            regs->eax = copy_to_user((void *)(uintptr_t)regs->ebx,
                    &name, sizeof(name)) ? 0U : (uint32_t)-BK_EFAULT;
            break;
        }
        case SYS_LINUX_GETTIMEOFDAY: {
            linux_i386_timeval_t value;
            if (!user_write_range_ok(regs, (void *)(uintptr_t)regs->ebx,
                                     sizeof(value)) ||
                (regs->ecx && !user_write_range_ok(regs,
                    (void *)(uintptr_t)regs->ecx, 8U))) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            uint64_t realtime = linux_realtime_nanoseconds();
            value.seconds = (int32_t)(realtime / 1000000000ULL);
            value.microseconds = (int32_t)((realtime % 1000000000ULL) / 1000U);
            if (!copy_to_user((void *)(uintptr_t)regs->ebx,
                              &value, sizeof(value))) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            if (regs->ecx) {
                uint32_t timezone[2] = {0U, 0U};
                if (!copy_to_user((void *)(uintptr_t)regs->ecx,
                                  timezone, sizeof(timezone))) {
                    regs->eax = (uint32_t)-BK_EFAULT;
                    break;
                }
            }
            regs->eax = 0U;
            break;
        }
        case SYS_LINUX_CLOCK_GETTIME:
            regs->eax = (uint32_t)sys_linux_clock(regs, regs->ebx,
                (void *)(uintptr_t)regs->ecx, false);
            break;
        case SYS_LINUX_CLOCK_GETRES:
            regs->eax = (uint32_t)sys_linux_clock(regs, regs->ebx,
                (void *)(uintptr_t)regs->ecx, true);
            break;
        case SYS_LINUX_STAT64:
            regs->eax = (uint32_t)sys_linux_stat64(regs,
                (const char *)(uintptr_t)regs->ebx,
                (void *)(uintptr_t)regs->ecx);
            break;
        case SYS_LINUX_FSTAT64:
            regs->eax = (uint32_t)sys_linux_fstat64(regs, regs->ebx,
                (void *)(uintptr_t)regs->ecx);
            break;
        case SYS_LINUX_ACCESS: {
            vfs_dir_entry_t entry;
            if (regs->ecx & ~7U)
                regs->eax = (uint32_t)-BK_EINVAL;
            else if (!copy_user_string(regs,
                    (const char *)(uintptr_t)regs->ebx,
                    path, sizeof(path)))
                regs->eax = (uint32_t)-BK_EFAULT;
            else regs->eax = vfs_stat(path, &entry)
                ? 0U : (uint32_t)-BK_ENOENT;
            break;
        }
        case SYS_LINUX_IOCTL:
            if(regs->ecx==0x541bU) { /* FIONREAD on an inotify queue. */
                int raw=resource_fd(resource_for(task_current_process_id(),false),regs->ebx);
                linux_device *device=linux_device_for(raw);
                if(raw<0)regs->eax=(uint32_t)-BK_EBADF;
                else if(!device || device->kind!=6)regs->eax=(uint32_t)-BK_ENOTTY;
                else {
                    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
                    uint32_t bytes=device->inotify->used;
                    kspin_unlock_irqrestore(&linux_io_lock,lock);
                    regs->eax=copy_to_user((void *)(uintptr_t)regs->edx,&bytes,4)?0U:(uint32_t)-BK_EFAULT;
                }
            } else if (((regs->ecx>>8)&255U)=='P' || ((regs->ecx>>8)&255U)=='X') {
                syscall_resources_t *resource=resource_for(task_current_process_id(),false);
                linux_device *device=linux_device_for(resource_fd(resource,regs->ebx));
                regs->eax=device && (device->kind==4 || device->kind==5)
                    ? (uint32_t)linux_oss_ioctl(device,regs->ecx,regs->edx) : (uint32_t)-BK_ENOTTY;
            } else if ((regs->ecx >= BLES_GFX_OPEN && regs->ecx <= BLES_GFX_LAUNCH_BUSY) ||
                       regs->ecx == BLES_GFX_NATIVE_FULLSCREEN ||
                       regs->ecx == BLES_GFX_NATIVE_EXIT_REQUEST) {
                /* Native Gallium screensavers use two ioctls after the
                 * original 0xB100..0xB110 graphics range. Both are checked
                 * by bles_gfx_ioctl_pinned(), but must pass this dispatcher
                 * first. Otherwise their calls always return ENOTTY and
                 * a cancelled Jelly renderer keeps running forever. */
                syscall_resources_t *resource=resource_for(task_current_process_id(),false);
                int raw=resource_fd(resource,regs->ebx);
                linux_device *device=linux_device_for(raw);
                regs->eax=device && device->kind==3
                    ? (uint32_t)bles_gfx_ioctl(device,regs->ecx,regs->edx)
                    : (uint32_t)-BK_EBADF;
            } else if (regs->ecx == 0x5413U && regs->ebx < 3U) { /* TIOCGWINSZ */
                linux_i386_winsize_t window = {25U, 80U, 640U, 400U};
                regs->eax = copy_to_user((void *)(uintptr_t)regs->edx,
                        &window, sizeof(window))
                    ? 0U : (uint32_t)-BK_EFAULT;
            } else if (regs->ecx == 0x82187201U) { /* VFAT_IOCTL_READDIR_BOTH */
                syscall_resources_t *resource = resource_for(
                    task_current_process_id(), false);
                int raw = resource_fd(resource, regs->ebx);
                linux_i386_kernel_dirent_t pair[2];
                fat_dir_entry_t entry;
                bool end = false;
                int32_t position;
                if (raw < 0) {
                    regs->eax = (uint32_t)-BK_EBADF;
                    break;
                }
                if (!vfs_is_directory(raw)) {
                    regs->eax = (uint32_t)-BK_ENOTTY;
                    break;
                }
                if (!user_write_range_ok(regs,
                        (void *)(uintptr_t)regs->edx, sizeof(pair))) {
                    regs->eax = (uint32_t)-BK_EFAULT;
                    break;
                }
                position = vfs_tell(raw);
                if (position < 0 || !vfs_fat_dir_entry_at(raw,
                        (uint32_t)position, &entry, &end)) {
                    regs->eax = (uint32_t)-BK_ENOTTY;
                    break;
                }
                kmemset(pair, 0, sizeof(pair));
                if (!end) {
                    pair[0].inode = (uint32_t)position + 1U;
                    pair[0].offset = (uint32_t)position + 1U;
                    pair[0].record_length = sizeof(pair[0]);
                    kstrncpy(pair[0].name, entry.short_name[0]
                            ? entry.short_name : entry.name,
                            sizeof(pair[0].name) - 1U);
                    if (entry.short_name[0] &&
                        kstrcmp(entry.name, entry.short_name)) {
                        pair[1].inode = pair[0].inode;
                        pair[1].offset = pair[0].offset;
                        pair[1].record_length = sizeof(pair[1]);
                        kstrncpy(pair[1].name, entry.name,
                                sizeof(pair[1].name) - 1U);
                    }
                    if (vfs_seek(raw, position + 1, 0U) < 0) {
                        regs->eax = (uint32_t)-BK_EIO;
                        break;
                    }
                }
                regs->eax = copy_to_user((void *)(uintptr_t)regs->edx,
                        pair, sizeof(pair))
                    ? 0U : (uint32_t)-BK_EFAULT;
            } else regs->eax = (uint32_t)-BK_ENOTTY;
            break;
        case SYS_LINUX_SET_TID_ADDRESS:
            if (regs->ebx && !user_write_range_ok(regs,
                    (void *)(uintptr_t)regs->ebx, sizeof(uint32_t)))
                regs->eax = (uint32_t)-BK_EFAULT;
            else if (!task_set_current_clear_child_tid(regs->ebx))
                regs->eax = (uint32_t)-BK_EINVAL;
            else regs->eax = task_current_tid();
            break;
        case SYS_LINUX_SET_ROBUST_LIST:
            regs->eax=(uint32_t)task_linux_robust(regs,false);
            break;
        case SYS_LINUX_TIME: {
            uint32_t seconds=linux_realtime_seconds();
            if (regs->ebx && (!user_write_range_ok(regs,
                    (void *)(uintptr_t)regs->ebx, sizeof(seconds)) ||
                !copy_to_user((void *)(uintptr_t)regs->ebx,
                              &seconds, sizeof(seconds))))
                regs->eax=(uint32_t)-BK_EFAULT;
            else regs->eax=seconds;
            break;
        }
        case SYS_LINUX_NANOSLEEP: {
            linux_i386_timespec_t request;
            uint32_t hz=pit_get_frequency_hz();
            uint64_t ticks;
            if (!copy_from_user(&request,
                    (const void *)(uintptr_t)regs->ebx,sizeof(request)) ||
                request.seconds<0 || request.nanoseconds<0 ||
                request.nanoseconds>=1000000000) {
                regs->eax=(uint32_t)-BK_EINVAL;
                break;
            }
            if (regs->ecx && !user_write_range_ok(regs,
                    (void *)(uintptr_t)regs->ecx,sizeof(request))) {
                regs->eax=(uint32_t)-BK_EFAULT;
                break;
            }
            ticks=(uint64_t)(uint32_t)request.seconds*hz;
            if (request.nanoseconds)
                ticks+=((uint64_t)(uint32_t)request.nanoseconds*hz+
                        999999999ULL)/1000000000ULL;
            if (ticks>0xFFFFFFFFULL) ticks=0xFFFFFFFFULL;
            regs->eax=0U;
            if (ticks) {
                task_sleep_from_interrupt((uint32_t)ticks);
                SYSCALL_RETURN(task_schedule_voluntary(regs));
            }
            break;
        }
        case SYS_LINUX_SYSINFO: {
            linux_i386_sysinfo_t info;
            system_memory_info_t memory;
            uint32_t hz=pit_get_frequency_hz();
            if (!user_write_range_ok(regs,(void *)(uintptr_t)regs->ebx,
                                     sizeof(info))) {
                regs->eax=(uint32_t)-BK_EFAULT;
                break;
            }
            mm_get_system_info(&memory);
            kmemset(&info,0,sizeof(info));
            info.uptime=hz?(int32_t)(pit_get_ticks()/hz):0;
            info.total_ram=(uint32_t)memory.total_bytes;
            info.free_ram=(uint32_t)memory.free_bytes;
            info.processes=(uint16_t)task_count();
            info.memory_unit=1U;
            regs->eax=copy_to_user((void *)(uintptr_t)regs->ebx,
                    &info,sizeof(info))?0U:(uint32_t)-BK_EFAULT;
            break;
        }
        case SYS_LINUX_GETRLIMIT: {
            linux_i386_rlimit_t limit={0xFFFFFFFFU,0xFFFFFFFFU};
            if (!user_write_range_ok(regs,(void *)(uintptr_t)regs->ecx,
                                     sizeof(limit))) {
                regs->eax=(uint32_t)-BK_EFAULT;
                break;
            }
            if (regs->ebx==3U)
                limit.current=limit.maximum=TASK_STACK_MAX_SIZE;
            else if (regs->ebx==7U)
                limit.current=limit.maximum=SYSCALL_FD_SLOTS+3U;
            else if (regs->ebx==9U)
                limit.current=limit.maximum=(uint32_t)mm_heap_size();
            regs->eax=copy_to_user((void *)(uintptr_t)regs->ecx,
                    &limit,sizeof(limit))?0U:(uint32_t)-BK_EFAULT;
            break;
        }
        case SYS_LINUX_MADVISE:
            regs->eax = (uint32_t)sys_vm_madvise(regs->ebx,regs->ecx,regs->edx);
            break;
        case SYS_LINUX_PRLIMIT64: {
            struct { uint64_t current,maximum; } limit;
            if(regs->ebx && regs->ebx!=task_current_pid()) {
                regs->eax=(uint32_t)-BK_ESRCH;
                break;
            }
            if(regs->edx) {
                regs->eax=(uint32_t)-BK_EACCES;
                break;
            }
            limit.current=limit.maximum=0xFFFFFFFFFFFFFFFFULL;
            if(regs->ecx==3U)
                limit.current=limit.maximum=TASK_STACK_MAX_SIZE;
            else if(regs->ecx==7U)
                limit.current=limit.maximum=SYSCALL_FD_SLOTS+3U;
            else if(regs->ecx==9U)
                limit.current=limit.maximum=(uint32_t)mm_heap_size();
            if(!regs->esi) regs->eax=0U;
            else regs->eax=copy_to_user((void *)(uintptr_t)regs->esi,
                    &limit,sizeof(limit)) ? 0U : (uint32_t)-BK_EFAULT;
            break;
        }
        case SYS_LINUX_GETRANDOM: {
            uint32_t remaining=regs->ecx,address=regs->ebx;
            if(regs->edx&~3U || !user_write_range_ok(regs,
                    (void *)(uintptr_t)address,remaining)) {
                regs->eax=(uint32_t)-BK_EFAULT;
                break;
            }
            while(remaining) {
                uint32_t value=syscall_linux_random_word();
                uint32_t chunk=remaining<4U?remaining:4U;
                if(!copy_to_user((void *)(uintptr_t)address,&value,chunk)) {
                    regs->eax=(uint32_t)-BK_EFAULT;
                    break;
                }
                address+=chunk; remaining-=chunk;
            }
            if(!remaining) regs->eax=regs->ecx;
            break;
        }
        case SYS_LINUX_SCHED_GETAFFINITY: {
            uint32_t mask=task_get_affinity_mask(regs->ebx);
            uint32_t bytes=regs->ecx<sizeof(mask)?regs->ecx:sizeof(mask);
            if(!mask) regs->eax=(uint32_t)-BK_ESRCH;
            else if(!bytes || !copy_to_user((void *)(uintptr_t)regs->edx,
                                             &mask,bytes))
                regs->eax=(uint32_t)-BK_EFAULT;
            else regs->eax=bytes;
            break;
        }
        case SYS_LINUX_SCHED_SETAFFINITY: {
            uint32_t mask=0U;
            uint32_t bytes=regs->ecx<sizeof(mask)?regs->ecx:sizeof(mask);
            if(!bytes || !copy_from_user(&mask,
                    (const void *)(uintptr_t)regs->edx,bytes))
                regs->eax=(uint32_t)-BK_EFAULT;
            else regs->eax=task_set_affinity_mask(regs->ebx,mask)
                ? 0U : (uint32_t)-BK_EINVAL;
            break;
        }
        case SYS_OPEN:
            regs->eax = (uint32_t)sys_open(regs,
                (const char *)(uintptr_t)regs->ebx, regs->ecx);
            break;
        case SYS_READ:
            regs->eax = (uint32_t)sys_read(regs, regs->ebx,
                (void *)(uintptr_t)regs->ecx, regs->edx);
            break;
        case SYS_FILE_WRITE:
            regs->eax = (uint32_t)sys_write(regs, regs->ebx,
                (const void *)(uintptr_t)regs->ecx, regs->edx);
            break;
        case SYS_CLOSE: regs->eax = (uint32_t)sys_close(regs->ebx); break;
        case SYS_FILE_SEEK: {
            syscall_resources_t *resource =
                resource_for(task_current_process_id(), false);
            int raw_fd = resource_fd(resource, regs->ebx);
            regs->eax = raw_fd < 0 ? (uint32_t)-BK_EBADF :
                (uint32_t)vfs_seek(raw_fd, (int32_t)regs->ecx, regs->edx);
            break;
        }
        case SYS_FILE_TELL:
        case SYS_FILE_SIZE: {
            syscall_resources_t *resource =
                resource_for(task_current_process_id(), false);
            int raw_fd = resource_fd(resource, regs->ebx);
            if (raw_fd < 0) regs->eax = (uint32_t)-BK_EBADF;
            else if (regs->eax == SYS_FILE_TELL)
                regs->eax = (uint32_t)vfs_tell(raw_fd);
            else regs->eax = (uint32_t)vfs_size(raw_fd);
            break;
        }
        case SYS_FILE_STAT: {
            vfs_dir_entry_t entry;
            if (!copy_user_string(regs, (const char *)(uintptr_t)regs->ebx,
                                  path, sizeof(path))) {
                regs->eax = (uint32_t)-BK_EFAULT;
            } else if (!user_write_range_ok(regs,
                    (void *)(uintptr_t)regs->ecx, sizeof(entry))) {
                regs->eax = (uint32_t)-BK_EFAULT;
            } else if (!vfs_stat(path, &entry)) {
                regs->eax = (uint32_t)-BK_ENOENT;
            } else if (caller_is_user(regs) && !copy_to_user(
                    (void *)(uintptr_t)regs->ecx, &entry, sizeof(entry))) {
                regs->eax = (uint32_t)-BK_EFAULT;
            } else {
                if (!caller_is_user(regs))
                    kmemcpy((void *)(uintptr_t)regs->ecx, &entry,
                            sizeof(entry));
                regs->eax = 0U;
            }
            break;
        }
        case SYS_FILE_SYNC: {
            syscall_resources_t *resource =
                resource_for(task_current_process_id(), false);
            int raw_fd = resource_fd(resource, regs->ebx);
            regs->eax = raw_fd < 0 ? (uint32_t)-BK_EBADF :
                (vfs_flush(raw_fd) ? 0U : (uint32_t)-BK_EIO);
            break;
        }
        case SYS_FILE_TRUNCATE: {
            syscall_resources_t *resource =
                resource_for(task_current_process_id(), false);
            int raw_fd = resource_fd(resource, regs->ebx);
            regs->eax = raw_fd < 0 ? (uint32_t)-BK_EBADF :
                (vfs_truncate(raw_fd, regs->ecx) ? 0U : (uint32_t)-BK_EIO);
            break;
        }
        case SYS_FILE_DUP:
            regs->eax = (uint32_t)sys_dup(regs->ebx, (int32_t)regs->ecx);
            break;
        case SYS_LINUX_FCNTL64: {
            syscall_resources_t *resource=resource_for(
                task_current_process_id(),true);
            bool valid=resource_fd(resource,regs->ebx)>=0;
            if (!resource) regs->eax=(uint32_t)-BK_ENOMEM;
            else if(!valid) regs->eax=(uint32_t)-BK_EBADF;
            else if(regs->ecx==0U || regs->ecx==1030U) {
                uint32_t minimum=regs->edx;
                if (minimum>=SYSCALL_FD_SLOTS+3U) { regs->eax=(uint32_t)-BK_EINVAL;break; }
                /* 0..2 siguen reservados a la consola nativa. */
                if (minimum<3U) minimum=3U;
                while (minimum<SYSCALL_FD_SLOTS+3U && resource_fd(resource,minimum)>=0) ++minimum;
                int32_t duplicate=minimum==SYSCALL_FD_SLOTS+3U ? -BK_EMFILE : sys_dup(regs->ebx,(int32_t)minimum);
                if (duplicate>=0 && regs->ecx==1030U) resource->linux_fd_flags[duplicate]=1U;
                regs->eax=(uint32_t)duplicate;
            } else if(regs->ecx==1U) regs->eax=resource->linux_fd_flags[regs->ebx];
            else if(regs->ecx==2U) {
                resource->linux_fd_flags[regs->ebx]=regs->edx&1U;regs->eax=0U;
            } else if(regs->ecx==3U) regs->eax=(uint32_t)linux_raw_flags(resource_fd(resource,regs->ebx));
            else if(regs->ecx==4U) {
                int raw=resource_fd(resource,regs->ebx);
                if(regs->edx&~(0x800U|0x8000U|3U))regs->eax=(uint32_t)-BK_ENOSYS;
                else if(linux_device_for(raw)) {linux_device_for(raw)->flags=(linux_device_for(raw)->flags&3U)|(regs->edx&0x800U);regs->eax=0;}
                else if(raw<LINUX_IO_BASE)regs->eax=vfs_set_status_flags(raw,regs->edx)?0U:(uint32_t)-BK_EBADF;
                else {
                    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
                    linux_endpoint *e=linux_endpoint_for(raw);
                    if(e)e->flags=regs->edx&0x800;
                    regs->eax=e?0U:(uint32_t)-BK_EBADF;
                    kspin_unlock_irqrestore(&linux_io_lock,lock);
                }
            }
            else regs->eax=(uint32_t)-BK_ENOSYS;
            break;
        }
        case SYS_SIGNAL_REGISTER: {
            syscall_signal_registration_t *registration;
            if (regs->ebx && !user_range_ok(regs,
                    (const void *)(uintptr_t)regs->ebx, 1U)) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            registration = signal_registration_for(
                task_current_process_id(), true);
            if (!registration) regs->eax = (uint32_t)-BK_ENOMEM;
            else {
                registration->dispatcher = regs->ebx;
                regs->eax = 0U;
            }
            break;
        }
        case SYS_SIGNAL_SEND: {
            bool active = false;
            uint32_t main_pid = 0U;
            syscall_signal_registration_t *target;
            uint32_t arguments[1];
            int32_t signal_number = (int32_t)regs->ecx;
            if (!regs->ebx || signal_number < 0 || signal_number >= 32) {
                regs->eax = (uint32_t)-BK_EINVAL;
                break;
            }
            if (!task_query_process(regs->ebx, &active, NULL, &main_pid) ||
                !active) {
                regs->eax = (uint32_t)-BK_ESRCH;
                break;
            }
            if (signal_number == 0) { regs->eax = 0U; break; }
            if (signal_number == 9) {
                regs->eax = task_request_exit_process(regs->ebx, 137)
                    ? 0U : (uint32_t)-BK_ESRCH;
                break;
            }
            if (signal_number == 19) {
                regs->eax = (uint32_t)-BK_ENOSYS;
                break;
            }
            if (signal_number == 18) { regs->eax = 0U; break; }
            target = signal_registration_for(regs->ebx, false);
            if (!target || !target->dispatcher) {
                if (signal_number == 17) regs->eax = 0U;
                else regs->eax = task_request_exit_process(
                    regs->ebx, 128 + signal_number) ? 0U
                                                    : (uint32_t)-BK_ESRCH;
                break;
            }
            arguments[0] = (uint32_t)signal_number;
            regs->eax = task_queue_user_upcall(main_pid,
                target->dispatcher, arguments, 1U, NULL, 0U, -1)
                ? 0U : (uint32_t)-BK_EBUSY;
            break;
        }
        case SYS_GETCWD: {
            const char *cwd = vfs_getcwd();
            uint32_t length = (uint32_t)kstrlen(cwd) + 1U;
            if (regs->ecx < length || !user_write_range_ok(regs,
                    (void *)(uintptr_t)regs->ebx, length))
                regs->eax = (uint32_t)-BK_EFAULT;
            else {
                if (caller_is_user(regs) &&
                    !copy_to_user((void *)(uintptr_t)regs->ebx, cwd, length))
                    regs->eax = (uint32_t)-BK_EFAULT;
                else {
                    if (!caller_is_user(regs))
                        kmemcpy((void *)(uintptr_t)regs->ebx, cwd, length);
                    regs->eax = length - 1U;
                }
            }
            break;
        }
        case SYS_CHDIR:
        case SYS_MKDIR:
        case SYS_UNLINK:
            if (!copy_user_string(regs, (const char *)(uintptr_t)regs->ebx,
                                  path, sizeof(path))) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            if (regs->eax == SYS_CHDIR) result = vfs_chdir(path) ? 0 : -BK_ENOENT;
            else if (regs->eax == SYS_MKDIR) result = vfs_mkdir(path) ? 0 : -BK_EIO;
            else if(task_current_is_linux_abi() && linux_unix_unlink(path)==0)result=0;
            else result = vfs_remove(path) ? 0 : -BK_ENOENT;
            regs->eax = (uint32_t)result;
            break;
        case SYS_RENAME:
            if (!copy_user_string(regs, (const char *)(uintptr_t)regs->ebx,
                                  path, sizeof(path)) ||
                !copy_user_string(regs, (const char *)(uintptr_t)regs->ecx,
                                  second_path, sizeof(second_path)))
                regs->eax = (uint32_t)-BK_EFAULT;
            else regs->eax = vfs_rename(path, second_path) ? 0U
                                                           : (uint32_t)-BK_EIO;
            break;
        case SYS_GETDENTS:
            if (!copy_user_string(regs, (const char *)(uintptr_t)regs->ebx,
                                  path, sizeof(path)) ||
                regs->edx > VFS_MAX_DIR_ENTRIES ||
                !user_write_range_ok(regs, (void *)(uintptr_t)regs->ecx,
                                     regs->edx * sizeof(vfs_dir_entry_t)) ||
                !user_write_range_ok(regs, (void *)(uintptr_t)regs->esi,
                                     sizeof(uint32_t)))
                regs->eax = (uint32_t)-BK_EFAULT;
            else regs->eax = vfs_listdir(path,
                    (vfs_dir_entry_t *)(uintptr_t)regs->ecx, regs->edx,
                    (uint32_t *)(uintptr_t)regs->esi) ? 0U : (uint32_t)-BK_EIO;
            break;
        case SYS_ALLOC: regs->eax = (uint32_t)sys_alloc(regs->ebx); break;
        case SYS_REALLOC:
            regs->eax = (uint32_t)sys_realloc((void *)(uintptr_t)regs->ebx,
                                              regs->ecx);
            break;
        case SYS_FREE:
            regs->eax = (uint32_t)sys_free((void *)(uintptr_t)regs->ebx);
            break;
        case SYS_SPAWN:
            if (!copy_user_string(regs, (const char *)(uintptr_t)regs->ebx,
                                  path, sizeof(path)) ||
                (regs->ecx && !copy_user_string(regs,
                    (const char *)(uintptr_t)regs->ecx, second_path,
                    sizeof(second_path))))
                regs->eax = (uint32_t)-BK_EFAULT;
            else {
                int pid = elf_spawn_program_ex(path, gui_get_desktop(),
                                               regs->ecx ? second_path : NULL);
                if (pid >= 0)
                    syscall_inherit_files(task_current_process_id(),
                                          (uint32_t)pid);
                regs->eax = pid >= 0 ? (uint32_t)pid : (uint32_t)-BK_ENOENT;
            }
            break;
        case SYS_WAITPID: {
            int32_t status = 0;
            if (regs->ecx && !user_write_range_ok(regs,
                    (void *)(uintptr_t)regs->ecx, sizeof(status))) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            result = task_waitpid(regs->ebx, &status);
            if (result < 0) result = -BK_ECHILD;
            if (result > 0 && regs->ecx) {
                if (caller_is_user(regs)) {
                    if (!copy_to_user((void *)(uintptr_t)regs->ecx,
                                      &status, sizeof(status)))
                        result = -BK_EFAULT;
                } else {
                    *(int32_t *)(uintptr_t)regs->ecx = status;
                }
            }
            regs->eax = (uint32_t)result;
            break;
        }
        case SYS_KILL:
            regs->eax = task_request_exit(regs->ebx) ? 0U
                                                     : (uint32_t)-BK_ENOENT;
            break;
        case SYS_THREAD_CREATE: {
            const char *name = (const char *)(uintptr_t)regs->esi;
            task_entry_t entry = (task_entry_t)(uintptr_t)regs->ebx;
            if (!caller_is_user(regs) || !entry ||
                !user_range_ok(regs, (const void *)(uintptr_t)regs->ebx, 1U)) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            if (name) {
                if (!copy_user_string(regs, name, thread_name,
                                      sizeof(thread_name))) {
                    regs->eax = (uint32_t)-BK_EFAULT;
                    break;
                }
            } else {
                kstrcpy(thread_name, "app-thread");
            }
            regs->eax = (uint32_t)task_create_user_thread_ex(
                thread_name, entry, (void *)(uintptr_t)regs->ecx,
                task_current_process_id(), regs->edx, true);
            break;
        }
        case SYS_THREAD_JOIN: {
            int32_t thread_status = 0;
            int32_t joined;
            /* Validate the destination before consuming the one-shot join
             * completion. Otherwise an invalid user pointer would lose the
             * result and make a second join fail permanently. */
            if (regs->ecx && !user_write_range_ok(regs,
                    (void *)(uintptr_t)regs->ecx, sizeof(thread_status))) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            joined = task_thread_join_try(regs->ebx, &thread_status);
            if (joined > 0 && regs->ecx) {
                if (caller_is_user(regs)) {
                    if (!copy_to_user((void *)(uintptr_t)regs->ecx,
                                      &thread_status,
                                      sizeof(thread_status))) {
                        /* The mapping was checked before the join claim; a
                         * later failure means the process changed it during
                         * the syscall and is treated as EFAULT. */
                        regs->eax = (uint32_t)-BK_EFAULT;
                        break;
                    }
                } else {
                    *(int32_t *)(uintptr_t)regs->ecx = thread_status;
                }
            }
            regs->eax = (uint32_t)joined;
            break;
        }
        case SYS_THREAD_DETACH:
            regs->eax = task_thread_detach(regs->ebx) ? 0U :
                        (uint32_t)-BK_EINVAL;
            break;
        case SYS_THREAD_EXIT:
            task_exit_from_interrupt((int32_t)regs->ebx);
            SYSCALL_RETURN(task_schedule_voluntary(regs));
        case SYS_THREAD_SELF:
            regs->eax = task_current_tid();
            break;
        case SYS_THREAD_TLS_SET:
            if (regs->ebx && !user_write_range_ok(regs,
                    (void *)(uintptr_t)regs->ebx, 4096U)) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            if (!task_set_current_tls_base(regs->ebx)) {
                regs->eax = (uint32_t)-BK_EINVAL;
                break;
            }
            regs->fs = regs->ebx ? GDT_USER_FS : GDT_USER_DATA;
            regs->eax = 0U;
            break;
        case SYS_THREAD_TLS_GET:
            regs->eax = task_current_tls_base();
            break;
        case SYS_NET_RESOLVE:
            if (!copy_user_string(regs, (const char *)(uintptr_t)regs->ebx,
                                  path, sizeof(path)) ||
                !user_write_range_ok(regs, (void *)(uintptr_t)regs->ecx, 4U))
                regs->eax = (uint32_t)-BK_EFAULT;
            else regs->eax = network_resolve(path,
                    (uint8_t *)(uintptr_t)regs->ecx, regs->edx) ? 0U
                                                               : (uint32_t)-BK_EIO;
            break;
        case SYS_NET_SOCKET:
            regs->eax = (uint32_t)sys_net_socket(regs->ebx);
            break;
        case SYS_NET_CONNECT: {
            syscall_resources_t *resource =
                resource_for(task_current_process_id(), false);
            int raw_socket = resource_socket(resource, regs->ebx);
            if (raw_socket < 0) regs->eax = (uint32_t)-BK_EBADF;
            else if (!user_range_ok(regs,
                         (const void *)(uintptr_t)regs->ecx, 4U))
                regs->eax = (uint32_t)-BK_EFAULT;
            else regs->eax = network_socket_connect(raw_socket,
                    (const uint8_t *)(uintptr_t)regs->ecx,
                    (uint16_t)regs->edx, regs->esi) ? 0U
                                                   : (uint32_t)-BK_EIO;
            break;
        }
        case SYS_NET_SEND:
        case SYS_NET_RECV: {
            syscall_resources_t *resource =
                resource_for(task_current_process_id(), false);
            int raw_socket = resource_socket(resource, regs->ebx);
            if (raw_socket < 0) regs->eax = (uint32_t)-BK_EBADF;
            else if (regs->edx > SYSCALL_WRITE_MAX ||
                     (regs->eax == SYS_NET_SEND
                        ? !user_range_ok(regs,
                            (const void *)(uintptr_t)regs->ecx, regs->edx)
                        : !user_write_range_ok(regs,
                            (void *)(uintptr_t)regs->ecx, regs->edx)))
                regs->eax = (uint32_t)-BK_EFAULT;
            else if (regs->eax == SYS_NET_SEND)
                regs->eax = (uint32_t)network_socket_send(raw_socket,
                    (const void *)(uintptr_t)regs->ecx, regs->edx, regs->esi);
            else regs->eax = (uint32_t)network_socket_receive(raw_socket,
                    (void *)(uintptr_t)regs->ecx, regs->edx, regs->esi);
            break;
        }
        case SYS_NET_CLOSE:
            regs->eax = (uint32_t)sys_net_close(regs->ebx);
            break;
        case SYS_NET_HTTP_GET:
            if (!copy_user_string(regs, (const char *)(uintptr_t)regs->ebx,
                                  path, sizeof(path)) ||
                regs->edx > SYSCALL_WRITE_MAX ||
                !user_write_range_ok(regs, (void *)(uintptr_t)regs->ecx,
                                     regs->edx))
                regs->eax = (uint32_t)-BK_EFAULT;
            else if (kstrncmp(path, "https://", 8U) == 0)
                regs->eax = (uint32_t)network_https_get(path,
                    (void *)(uintptr_t)regs->ecx, regs->edx, regs->esi);
            else regs->eax = (uint32_t)network_http_get(path,
                    (void *)(uintptr_t)regs->ecx, regs->edx, regs->esi);
            break;
        case SYS_API_CALL: {
            uint32_t arguments[16];
            uint32_t argument_address = regs->useresp + sizeof(uint32_t);
            uint32_t stack_limit = 0U;
            uint32_t stack_base = 0U;
            uint32_t readable_bytes;
            uint64_t value;
            uint32_t callee_cleanup = 0;
            bool valid = false;

            /* API thunks have no signature metadata and the raw dispatcher
             * supplies up to 16 words. Requiring all 64 bytes to exist above
             * ESP rejects perfectly valid zero/one-argument calls near the
             * top of a guarded stack and returns -EFAULT (0xFFFFFFF2) to the
             * application, which then treats it as a pointer. Copy only the
             * words that really fit in this task's stack and zero-fill the
             * unused tail. */
            kmemset(arguments, 0, sizeof(arguments));
            if (!caller_is_user(regs) || regs->useresp < HEAP_START ||
                argument_address < regs->useresp ||
                !task_get_user_stack_bounds(task_current_pid(),
                                            &stack_limit, &stack_base) ||
                regs->useresp < stack_limit || regs->useresp >= stack_base ||
                argument_address < stack_limit || argument_address > stack_base) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }
            readable_bytes = stack_base - argument_address;
            if (readable_bytes > sizeof(arguments))
                readable_bytes = sizeof(arguments);
            if (readable_bytes && !copy_from_user(
                    arguments, (const void *)(uintptr_t)argument_address,
                    readable_bytes)) {
                regs->eax = (uint32_t)-BK_EFAULT;
                break;
            }

            /* Public APIs execute on the task's kernel stack. Reject the call
             * before a deep renderer/GUI chain can overwrite the syscall
             * frame; the process is terminated, but the desktop survives. */
            {
                uint32_t remaining = task_kernel_stack_remaining();
                if (remaining != 0xFFFFFFFFU &&
                    remaining < SYS_API_KERNEL_STACK_RESERVE) {
                    kprintf("[API] pila kernel agotada pid=%u api_token=%u "
                            "restante=%u; proceso terminado\n",
                            task_current_pid(), regs->ecx, remaining);
                    task_exit_from_interrupt(0xA501);
                    task_allow_kernel_switch_once();
                    SYSCALL_RETURN(task_schedule_voluntary(regs));
                }
            }
            {
                value = elf_user_api_dispatch(
                    regs->ecx, arguments, &valid, &callee_cleanup);

                if (!valid) {
                    regs->eax = (uint32_t)-BK_EACCES;
                    break;
                }

                regs->eax = (uint32_t)value;
                regs->edx = (uint32_t)(value >> 32);
            }
            if (callee_cleanup) {
                /*
                 * elf_api_call_raw informa el ret N ejecutado por
                 * el wrapper ABI, incluido el puntero oculto de retorno de structs.
                 * No aplicar N al ESP de Ring 3
                 * hasta validar firma, límites y dirección de retorno.
                 */
                uint32_t stack_limit = 0U;
                uint32_t stack_base = 0U;
                uint32_t new_useresp =
                    regs->useresp + sizeof(uint32_t) + callee_cleanup;
                uint32_t return_address = 0U;
                bool return_read_ok = caller_is_user(regs)
                    ? copy_from_user(&return_address,
                        (const void *)(uintptr_t)regs->useresp,
                        sizeof(return_address))
                    : true;
                if (!caller_is_user(regs))
                    return_address =
                        *(const uint32_t *)(uintptr_t)regs->useresp;
                bool cleanup_valid = return_read_ok &&
                    (callee_cleanup & 3U) == 0U &&
                    callee_cleanup <= SYS_API_MAX_CALLEE_CLEANUP &&
                    new_useresp >= regs->useresp &&
                    task_get_user_stack_bounds(
                        task_current_pid(), &stack_limit, &stack_base) &&
                    regs->useresp >= stack_limit &&
                    regs->useresp < stack_base &&
                    new_useresp >= stack_limit &&
                    new_useresp <= stack_base &&
                    (return_address >= HEAP_START && return_address < HEAP_END);

                if (!cleanup_valid) {
                    kprintf(
                        "[API] ABI invalida en %s: cleanup=%u "
                        "ESP=%x nuevo=%x retorno=%x stack=%x..%x\n",
                        elf_last_user_api_name(), callee_cleanup,
                        regs->useresp, new_useresp, return_address,
                        stack_limit, stack_base);
                    task_exit_from_interrupt(0x570D);
                    SYSCALL_RETURN(task_schedule_voluntary(regs));
                }

                regs->useresp = new_useresp;
                regs->eip = return_address;
            }
            /* Las apps nativas duermen dentro de una llamada API proxy. Al
             * volver de esa llamada es el punto seguro para entregar eventos
             * y callbacks pendientes conservando el valor de retorno. */
            (void)task_prepare_user_upcall(regs);
            break;
        }
        default: regs->eax = (uint32_t)-BK_ENOSYS; break;
    }
    kernel_domains_exit(domain_mask);
#undef SYSCALL_RETURN
    /* The compositor needs this gate too. Once the syscall has completed,
     * yield to a waiting frame before a hot Wine thread reacquires it.
     * Do not sleep a timer tick: repeated I/O otherwise pays that delay
     * throughout an interactive launch. Never yield across an outer domain. */
    if(caller_is_user(regs) && task_current_is_linux_abi() &&
       (domain_mask&KDOMAIN_VFS) &&
       gui_vision_frame_waiting() &&
       !kernel_domains_held_mask())task_yield();
    return regs;
}

/* Capturar antes de traducir: old_mmap, open, dup y tgkill cambian argumentos.
 * Linux conserva los seis registros de entrada al retornar de int 0x80.
 * El frame devuelto por el scheduler puede pertenecer a OTRA tarea. */
registers_t *syscall_handler(registers_t *regs) {
    registers_t input, *next;
    uint32_t pid, tid;
    char name[TASK_NAME_LEN];
    /* Kernel-origin traps retain native numbering even while servicing a
     * Linux process (notably SYS_YIELD while waiting for IPC). */
    if (!regs || !caller_is_user(regs) || !task_current_is_linux_abi())
        return syscall_dispatch(regs);
    input = *regs;
    W3P_SCOPE(input.eax,
              (input.eax == 3U || input.eax == 4U ||
               input.eax == 180U || input.eax == 181U) ? input.edx : 0U);
    pid = task_current_process_id();
    tid = task_current_tid();
    kstrncpy(name, task_current_name(), sizeof(name) - 1U);
    name[sizeof(name) - 1U] = 0;
    if (input.eax==173U || input.eax==119U) {
        task_linux_sigreturn(regs,input.eax==173U);
        return regs;
    }
    if (input.eax==7U || input.eax==114U) {
        regs->eax=(uint32_t)task_linux_wait(regs);return regs;
    }
    if (input.eax==1U || input.eax==252U) {
        regs->ebx=(input.ebx&255U)<<8;
        if (input.eax==252U) task_request_exit_process(pid,(int32_t)regs->ebx);
    }
#ifdef BLESK_LINUX_PROFILE
    uint32_t syscall_start = pit_get_ticks();
#endif
    next = syscall_dispatch(regs);
    if (input.eax == 240U && next == regs && (int32_t)regs->eax < 0 &&
        (int32_t)regs->eax != -4 && (int32_t)regs->eax != -11 &&
        (int32_t)regs->eax != -110)
        kprintf("[LINUX:FUTEX-ERROR] pid=%u tid=%u addr=%x op=%x value=%x result=%d\n",
                pid, tid, input.ebx, input.ecx, input.edx, (int32_t)regs->eax);
#ifdef BLESK_LINUX_PROFILE
    uint32_t syscall_elapsed = pit_get_ticks() - syscall_start;
    if (next == regs && syscall_elapsed >= 10U) {
        kprintf("[LINUX:SLOW] pid=%u nr=%u ticks=%u result=%d eip=%x\n",
                pid, input.eax, syscall_elapsed, (int32_t)regs->eax, input.eip);
        if (input.eax == 39U || input.eax == 295U) {
            char path[VFS_MAX_PATH];
            uint32_t pointer = input.eax == 39U ? input.ebx : input.ecx;
            if (copy_user_string(&input, (const char *)(uintptr_t)pointer,
                                 path, sizeof(path)))
                kprintf("[LINUX:SLOW-PATH] pid=%u nr=%u path=%s\n", pid, input.eax, path);
        }
        if (input.eax == 192U)
            kprintf("[LINUX:SLOW-MMAP] pid=%u base=%x len=%x prot=%x flags=%x\n",
                    pid, input.ebx, input.ecx, input.edx, input.esi);
        if (input.eax == 3U || input.eax == 4U) {
            int raw = resource_fd(resource_for(pid, false), input.ebx);
            const char *path = raw >= 0 && raw < LINUX_IO_BASE ? vfs_fd_path(raw) : NULL;
            kprintf("[LINUX:SLOW-FD] pid=%u fd=%u raw=%d path=%s\n",
                    pid, input.ebx, raw, path ? path : "(ipc/device)");
        }
    }
#endif
    if (regs->eip == input.eip) {
        regs->ebx = input.ebx; regs->ecx = input.ecx;
        regs->edx = input.edx; regs->esi = input.esi;
        regs->edi = input.edi; regs->ebp = input.ebp;
    }
    /* Un WAIT suspendido conserva un resultado provisional. No anunciarlo
     * como ETIMEDOUT: WAKE puede sustituirlo antes de volver al usuario. */
    if (!(input.eax==240 && next!=regs && ((input.ecx&127)==0 || (input.ecx&127)==9)))
        linux_i386_trace(&input, (int32_t)regs->eax, pid, tid, name);
    return next;
}
