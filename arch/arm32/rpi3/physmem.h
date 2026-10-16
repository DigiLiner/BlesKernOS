#ifndef BLES_ARCH_ARM32_RPI3_PHYSMEM_H
#define BLES_ARCH_ARM32_RPI3_PHYSMEM_H

#include "../../../kernel/include/types.h"

#define RPI3_PAGE_SIZE 4096U

void rpi3_physmem_init(void);
void *rpi3_phys_alloc_page(void);
void *rpi3_phys_alloc_pages(uint32_t count);
uint32_t rpi3_phys_free_bytes(void);
uint32_t rpi3_phys_base(void);
uint32_t rpi3_phys_limit(void);

#endif
