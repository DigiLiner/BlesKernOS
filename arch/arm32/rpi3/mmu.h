#ifndef BLES_ARCH_ARM32_RPI3_MMU_H
#define BLES_ARCH_ARM32_RPI3_MMU_H

#include "../../../kernel/include/types.h"

int rpi3_mmu_init(void);
int rpi3_mmu_enabled(void);
uint32_t rpi3_mmu_ttbr0(void);
/* Stage 11 keeps one shared short-descriptor table but changes AP bits at
 * context switches so only the currently running user slot is accessible
 * from USR mode. Kernel mappings remain privileged-only. */
bool rpi3_mmu_activate_user_window(uint32_t base, uint32_t bytes);
void rpi3_mmu_deactivate_user_window(void);

#endif
