#include "../../kernel/include/api.h"
#include "../../kernel/include/paging.h"
extern void *mm_alloc_guarded_stack(uint32_t, bool, void **, uint32_t *) __attribute__((weak));
extern void mm_free_guarded_stack(void *, void *, uint32_t) __attribute__((weak));
extern bool paging_kernel_mmio_uncached(uint32_t, uint32_t) __attribute__((weak));
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;
    bool safe = !mm_alloc_guarded_stack && !mm_free_guarded_stack && !paging_kernel_mmio_uncached;
    kprintf("[DRIVERTEST] supervisor DMA/MMIO imports unavailable to Ring 3: %s\n", safe ? "PASS" : "FAIL");
    kprintf("[DRIVERTEST] DONE result=%s\n", safe ? "PASS" : "FAIL");
}
