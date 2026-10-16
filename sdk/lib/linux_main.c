#include "bleskernos.h"
#include "bleskernos_api.h"

#define LINUX_ARG_CAPACITY 64
#define LINUX_COMMAND_CAPACITY 2048

extern int main(int argc, char **argv);

static int linux_parse_arguments(char *command, char **argv, int capacity) {
    char *source = command;
    char *destination = command;
    int argc = 0;
    while (*source && argc + 1 < capacity) {
        char quote = 0;
        while (*source == ' ' || *source == '\t') source++;
        if (!*source) break;
        argv[argc++] = destination;
        while (*source) {
            char value = *source++;
            if (!quote && (value == ' ' || value == '\t')) break;
            if (!quote && (value == '\"' || value == '\'')) {
                quote = value;
                continue;
            }
            if (quote && value == quote) { quote = 0; continue; }
            if (value == '\\' && *source) value = *source++;
            *destination++ = value;
        }
        *destination++ = '\0';
    }
    argv[argc] = (char *)0;
    return argc;
}

static __attribute__((noreturn)) void linux_exit_group(int status) {
    __asm__ volatile ("int $0x80" : : "a"(252U), "b"(status) : "memory");
    for (;;) __asm__ volatile ("int $0x80" : : "a"(158U) : "memory");
}

void bleskernos_program_main(void *unused) {
    char command[LINUX_COMMAND_CAPACITY];
    char *argv[LINUX_ARG_CAPACITY];
    int argc;
    int result;
    (void)unused;

    /* El argumento de lanzamiento pertenece a la ABI BlesK. Hay que copiarlo
     * antes de cambiar de personalidad; a partir de entonces int 0x80 es Linux. */
    command[0] = '\0';
    (void)bk_proc_launch_arg_copy(command, sizeof(command));
    argc = linux_parse_arguments(command, argv, LINUX_ARG_CAPACITY);
    if (!argc) {
        argv[0] = (char *)"linux-bex";
        argv[1] = (char *)0;
        argc = 1;
    }
    if (bk_personality_linux_i386() < 0) bk_exit(126);
    result = main(argc, argv);
    linux_exit_group(result);
}
