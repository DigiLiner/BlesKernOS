#include "common.h"

static int run(int argc, char **argv) {
    bool force = argc >= 2 && command_is(argv[1], "-f");
    int first = force ? 2 : 1;
    if (force && first>=argc)return 0;
    if (first >= argc) return command_error("rm", "uso: rm [-f] archivo...");
    for (int i = first; i < argc; i++) {
        bk_file_entry_t entry;
        if (!bk_file_stat(argv[i], &entry)) {
            if (force) continue;
            return command_error("rm", "archivo inexistente");
        }
        if (entry.type==BK_FILE_NODE_DIRECTORY)return command_error("rm","use rmdir para directorios vacios");
        if (!bk_file_remove(argv[i]))
            return command_error("rm", "no se pudo eliminar");
    }
    return 0;
}

BK_COMMAND_MAIN(run)
