#ifndef BK_SWAP_H
#define BK_SWAP_H
#include "types.h"
typedef struct { uint32_t slots, used, writes, reads, io_errors; bool enabled; } swap_stats_t;
void swap_init(void);
void swap_init_file(void); /* after mounting the boot filesystem */
bool swap_enable(bool enabled);
uint32_t swap_slot_alloc(void); /* zero is never a valid slot */
void swap_slot_free(uint32_t slot);
bool swap_page_write(uint32_t slot, const void *page);
bool swap_page_read(uint32_t slot, void *page);
void swap_stats(swap_stats_t *out);
#endif
