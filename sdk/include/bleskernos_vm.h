#ifndef BLESKERNOS_VM_H
#define BLESKERNOS_VM_H
typedef struct {
    uint32_t virtual_pages,resident_pages,swapped_pages;
    uint32_t faults,evictions,allocation_failures;
    uint32_t swap_slots,swap_used,swap_writes,swap_reads,swap_io_errors;
    uint32_t swap_enabled;
} bk_vm_statistics_t;
enum {BK_MEMORY_OTHER,BK_MEMORY_GUI,BK_MEMORY_VFS,BK_MEMORY_VM,
      BK_MEMORY_PROCESS,BK_MEMORY_AUDIO,BK_MEMORY_NETWORK,BK_MEMORY_DRIVER,
      BK_MEMORY_SUBSYSTEMS};
typedef struct {uint32_t bytes,blocks;} bk_memory_subsystem_t;
uint32_t bk_memory_subsystems(bk_memory_subsystem_t *out,uint32_t capacity);
bool bk_vm_statistics(uint32_t process_id,bk_vm_statistics_t *out);
bool bk_vm_swap_enable(bool enabled);
#endif
