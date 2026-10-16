#ifndef SHELL_H
#define SHELL_H

#include "types.h"

#define SHELL_MAX_CMD     1024
#define SHELL_MAX_ARGS    64
#define SHELL_HISTORY_LEN 10

typedef int (*cmd_func_t)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *help;
    cmd_func_t func;
} shell_cmd_t;

void shell_run(void);
void shell_execute_line(const char *line);
bool shell_take_exit_request(void);

#endif
