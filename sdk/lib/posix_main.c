#include "bleskernos.h"

#define POSIX_ARG_CAPACITY 64
#define POSIX_COMMAND_CAPACITY 2048

extern int main(int argc, char **argv);
extern int bk_proc_launch_arg_copy(char *buffer, unsigned int capacity);

static int parse_arguments(char *command, char **argv, int capacity) {
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
            if (!quote && (value == '\"' || value == '\'')) { quote = value; continue; }
            if (quote && value == quote) { quote = 0; continue; }
            if (value == '\\' && *source) value = *source++;
            *destination++ = value;
        }
        *destination++ = '\0';
    }
    argv[argc] = (char *)0;
    return argc;
}

void bleskernos_program_main(void *unused) {
    char command[POSIX_COMMAND_CAPACITY];
    char *argv[POSIX_ARG_CAPACITY];
    int argc;
    int result;
    (void)unused;
    command[0] = '\0';
    (void)bk_proc_launch_arg_copy(command, sizeof(command));
    argc = parse_arguments(command, argv, POSIX_ARG_CAPACITY);
    if (!argc) {
        argv[0] = (char *)"bex-app";
        argv[1] = (char *)0;
        argc = 1;
    }
    result = main(argc, argv);
    bk_exit(result);
}
