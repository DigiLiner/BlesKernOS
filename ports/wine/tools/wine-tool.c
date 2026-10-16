/* Console Windows tools run on upstream Wine through the Linux i386 ABI. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_ARGS 256

/* The native command wrappers supply one command-line string. Match their
 * quoted arguments without treating ordinary Windows path separators as escapes. */
static int split_command(char *line, char **args, unsigned capacity) {
    char *read = line, *write = line;
    unsigned count = 0;
    while (*read) {
        int quoted = 0;
        while (*read == ' ' || *read == '\t') ++read;
        if (!*read) break;
        if (count + 1 >= capacity) return -1;
        args[count++] = write;
        while (*read && (quoted || (*read != ' ' && *read != '\t'))) {
            if (*read == '"') { quoted = !quoted; ++read; }
            else if (*read == '\\' && (read[1] == '"' || (quoted && read[1] == '\\'))) {
                ++read; *write++ = *read++;
            } else *write++ = *read++;
        }
        if (quoted) return -1;
        while (*read == ' ' || *read == '\t') ++read;
        *write++ = '\0';
    }
    args[count] = NULL;
    return (int)count;
}

#ifndef WINE_TOOL_TEST
int main(int argc, char **argv) {
    char *args[MAX_ARGS + 1];
    char *environment[] = {
        "LD_LIBRARY_PATH=/WINEGATE/lib", "WINEDLLPATH=/OPT/WINE/lib/wine",
        "WINEPREFIX=/WINEGATE/prefix", "HOME=/WINEGATE",
        "PATH=/OPT/WINE/bin:/WINEGATE/bin", "TMPDIR=/tmp", "TEMP=/tmp",
        "TMP=/tmp", "LC_ALL=en_US.UTF-8", "WINEDEBUG=-all", "USER=root",
        "LOGNAME=root", "TZ=ART3", NULL
    };
    if (argc != 2 || split_command(argv[1], args + 1, MAX_ARGS) <= 0) {
        fputs("wine-tool: invalid command line\n", stderr);
        return 2;
    }
    args[0] = "/OPT/WINE/bin/wine";
    execve(args[0], args, environment);
    perror("wine-tool: exec Wine");
    return 127;
}
#endif
