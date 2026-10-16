#include "common.h"

static bool directory_exists(const char *path) {
    bk_file_entry_t entry;
    return bk_file_stat(path, &entry) && entry.type == BK_FILE_NODE_DIRECTORY;
}

static bool mkdir_parents(const char *path) {
    char copy[VFS_MAX_PATH];
    uint32_t start = 0U;
    if (!path || !*path || bk_runtime_strlen(path) >= sizeof(copy)) return false;
    bk_runtime_strncpy(copy, path, sizeof(copy) - 1U);
    copy[sizeof(copy) - 1U] = '\0';
    if (copy[0] == '/') start = 1U;
    for (uint32_t i = start; ; i++) {
        char saved;
        if (copy[i] != '/' && copy[i] != '\\' && copy[i] != '\0') continue;
        saved = copy[i];
        copy[i] = '\0';
        if (copy[0] && !directory_exists(copy) && !bk_file_mkdir(copy)) return false;
        copy[i] = saved;
        if (!saved) break;
    }
    return true;
}

static int run(int argc, char **argv) {
    bool parents = argc >= 2 && command_is(argv[1], "-p");
    int first = parents ? 2 : 1;
    if (first >= argc) return command_error("mkdir", "uso: mkdir [-p] directorio...");
    for (int i = first; i < argc; i++) {
        bool ok = parents ? mkdir_parents(argv[i])
                          : (directory_exists(argv[i]) || bk_file_mkdir(argv[i]));
        if (!ok) return command_error("mkdir", "no se pudo crear el directorio");
    }
    return 0;
}

BK_COMMAND_MAIN(run)
