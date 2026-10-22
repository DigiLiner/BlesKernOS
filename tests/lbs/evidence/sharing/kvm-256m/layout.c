#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define PAGING_PRIVATE_TABLE_MAX 512
typedef struct {
    uint32_t process_id;
    uint32_t borrowed_from;
    uint32_t users;
    bool retiring;
    uint32_t *directory;
    uint32_t *table[PAGING_PRIVATE_TABLE_MAX];
    uint16_t pde_index[PAGING_PRIVATE_TABLE_MAX];
    uint16_t table_count;
    uint32_t user_vm_hint;
} paging_process_space_t;
int main(void){printf("%u %u %u",(unsigned)sizeof(paging_process_space_t),(unsigned)offsetof(paging_process_space_t,process_id),(unsigned)offsetof(paging_process_space_t,directory));}
