#include "common.h"

static bool has_separator(const char *text) {
    while (text && *text) {
        if (*text == '/' || *text == '\\') return true;
        text++;
    }
    return false;
}

static bool has_extension(const char *text) {
    const char *last = text;
    while (text && *text) {
        if (*text == '/' || *text == '\\') last = text + 1;
        text++;
    }
    while (last && *last) if (*last++ == '.') return true;
    return false;
}

static bool append_quoted(char *out, uint32_t capacity, const char *text) {
    uint32_t used = (uint32_t)bk_runtime_strlen(out);
    bool quote = false;
    const char *p = text ? text : "";
    for (; *p; p++) if (*p == ' ' || *p == '\t' || *p == '"') quote = true;
    if (used && used + 1U < capacity) out[used++] = ' ';
    if (quote && used + 1U < capacity) out[used++] = '"';
    out[used] = '\0';
    for (p = text ? text : ""; *p; p++) {
        if ((*p == '"' || *p == '\\') && used + 2U < capacity)
            out[used++] = '\\';
        if (used + 1U >= capacity) return false;
        out[used++] = *p;
    }
    if (quote) {
        if (used + 1U >= capacity) return false;
        out[used++] = '"';
    }
    out[used] = '\0';
    return true;
}

static bool exists(const char *path) {
    vfs_dir_entry_t entry;
    return path && bk_file_stat(path, &entry) && entry.type != VFS_NODE_DIR;
}

static int run_program(const char *path, int argc, char **argv) {
    char line[COMMAND_ARG_MAX] = "";
    int status;
    if (!exists(path)) return -1;
    if (!append_quoted(line, sizeof(line), path))
        return command_error("run", "linea de argumentos demasiado larga");
    for (int i = 2; i < argc; i++)
        if (!append_quoted(line, sizeof(line), argv[i]))
            return command_error("run", "linea de argumentos demasiado larga");
    status = bk_app_run_sync(path, line);
    if (status < 0) return command_error("run", "no se pudo ejecutar el programa");
    return status;
}

static int run(int argc, char **argv) {
    char path[VFS_MAX_PATH];
    int result;
    if (argc < 2)
        return command_error("run", "uso: run programa.BEX [argumentos]");

    /* Rutas de Codex como /CODEX/HELLO.BEX entran directamente. */
    if (has_separator(argv[1]) || has_extension(argv[1])) {
        result = run_program(argv[1], argc, argv);
        return result >= 0 ? result : command_error("run", "programa no encontrado");
    }

    bk_runtime_strncpy(path, "/SYSTEM/PROGRAMS/", sizeof(path) - 1U);
    path[sizeof(path) - 1U] = '\0';
    bk_runtime_strcat(path, argv[1]);
    bk_runtime_strcat(path, ".BEX");
    result = run_program(path, argc, argv);
    if (result >= 0) return result;

    bk_runtime_strncpy(path, "/SYSTEM/WIN32/", sizeof(path) - 1U);
    path[sizeof(path) - 1U] = '\0';
    bk_runtime_strcat(path, argv[1]);
    bk_runtime_strcat(path, ".EXE");
    result = run_program(path, argc, argv);
    if (result >= 0) return result;

    return command_error("run", "programa no encontrado");
}

BK_COMMAND_MAIN(run)
