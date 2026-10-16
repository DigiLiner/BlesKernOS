#ifndef LINUX_EXEC_H
#define LINUX_EXEC_H
#include "types.h"
typedef struct {
    uint32_t argc,envc,used,offsets[128];
    char data[16384];
} linux_exec_args_t;
bool task_linux_exec_stack(uint32_t pid,const linux_exec_args_t *args);
bool task_linux_exec_commit(uint32_t pid);
void syscall_swap_process_ids(uint32_t first,uint32_t second);
#endif
