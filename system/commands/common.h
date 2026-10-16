#ifndef BK_COMMAND_COMMON_H
#define BK_COMMAND_COMMON_H

#include "../../sdk/include/bleskernos_api.h"

int snprintf(char *buffer,size_t capacity,const char *format,...);
static inline void *command_bytes_copy(void *to,const void *from,uint32_t n) {
    uint8_t *d=to;const uint8_t *s=from;for(uint32_t i=0;i<n;i++)d[i]=s[i];return to;
}
static inline void *command_bytes_set(void *to,int value,uint32_t n) {
    uint8_t *d=to;for(uint32_t i=0;i<n;i++)d[i]=(uint8_t)value;return to;
}
static inline int command_prefix_compare(const char *a,const char *b,uint32_t n) {
    for(uint32_t i=0;i<n;i++){if((uint8_t)a[i]!=(uint8_t)b[i])return (uint8_t)a[i]-(uint8_t)b[i];if(!a[i])return 0;}return 0;
}
#define bk_runtime_strncmp command_prefix_compare
#define bk_runtime_memcpy command_bytes_copy
#define bk_runtime_memset command_bytes_set
#define COMMAND_ARG_MAX 2048
#define COMMAND_MAX_ARGS 128
#define COMMAND_DIR_MAX BK_DIRECTORY_MAX

typedef struct {
    int argc;
    char *argv[COMMAND_MAX_ARGS + 1];
    char storage[COMMAND_ARG_MAX];
} command_args_t;

void command_load_args(command_args_t *args);
bool command_is(const char *left, const char *right);
uint32_t command_number(const char *text, bool *valid);
int command_filetool(int argc,char **argv);
int command_network(int argc,char **argv);
int command_help(int argc,char **argv);
bool command_usage(const char *name);
bool command_join(char *out,uint32_t cap,const char *base,const char *name);
int command_error(const char *name, const char *reason);
int command_list_directory(const char *path);
int command_print_file(const char *path, bool paged);
int command_copy_file(const char *from, const char *to, bool remove_source);
void command_tree(const char *path);
void command_find(const char *path, const char *pattern);
int command_show_processes(bool detailed);
int command_show_pci(bool usb_only);
int command_hexdump(const char *path);
int command_strings(const char *path);
int command_checksum(const char *path);
int command_toolchain_run(const char *program, int argc, char **argv,
                          const char *prefix);
bool command_append_argument(char *out,uint32_t capacity,const char *argument);
int command_build_native(const char *source, const char *output);

#define BK_COMMAND_MAIN(handler)                                      \
    void bleskernos_program_main(void *desktop UNUSED) {              \
        command_args_t command_args;                                  \
        (void)desktop;                                                \
        command_load_args(&command_args);                             \
        int command_status = command_args.argc                        \
            ? ((command_args.argc == 2 && command_is(command_args.argv[1], "--help") && command_usage(command_args.argv[0])) ? 0 : handler(command_args.argc, command_args.argv)) : 0;      \
        bk_proc_exit_status(command_status);                           \
    }

#endif
