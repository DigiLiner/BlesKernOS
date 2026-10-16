#include "common.h"

#define SH_LINE_MAX 2048U

static bool append(char *output, uint32_t capacity, const char *text) {
    uint32_t used = (uint32_t)bk_runtime_strlen(output);
    uint32_t length = (uint32_t)bk_runtime_strlen(text ? text : "");
    if (used + length + 1U > capacity) return false;
    bk_runtime_strcat(output, text ? text : "");
    return true;
}

static int run_recipe(const char *recipe) {
    char command[64];
    char path[BK_PATH_MAX];
    char launch[SH_LINE_MAX];
    uint32_t command_length = 0U;
    const char *rest;

    if (!recipe) return command_error("sh", "receta vacia");

    while (*recipe == ' ' || *recipe == '\t') recipe++;
    while (recipe[command_length] && recipe[command_length] != ' ' &&
           recipe[command_length] != '\t' && command_length + 1U < sizeof(command)) {
        command[command_length] = recipe[command_length];
        command_length++;
    }
    command[command_length] = '\0';
    if (!command[0]) return 0;
    rest = recipe + command_length;
    while (*rest == ' ' || *rest == '\t') rest++;

    path[0] = '\0';
    if (!append(path, sizeof(path), "/SYSTEM/COMMANDS/") ||
        !append(path, sizeof(path), command) ||
        !append(path, sizeof(path), ".BEX"))
        return command_error("sh", "nombre de comando demasiado largo");

    launch[0] = '\0';
    if (!append(launch, sizeof(launch), command) ||
        !append(launch, sizeof(launch), " --cwd") ||
        !command_append_argument(launch, sizeof(launch), bk_file_getcwd()) ||
        (*rest && (!append(launch, sizeof(launch), " ") ||
                   !append(launch, sizeof(launch), rest))))
        return command_error("sh", "receta demasiado larga");

    return bk_app_run_sync(path, launch);
}

static bool line_is_echo_off(const char *line) {
    static const char marker[] = "echo off";
    uint32_t i = 0U;
    if (!line) return false;
    if (*line == '@') line++;
    while (*line == ' ' || *line == '\t') line++;
    while (marker[i] && line[i]) {
        char a = line[i];
        char b = marker[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
        i++;
    }
    if (marker[i]) return false;
    return line[i] == '\0' || line[i] == '\r' || line[i] == '\n';
}

static int run_batch(const char *script_path) {
    void *raw = NULL;
    uint32_t size = 0U;
    char *script;
    uint32_t start = 0U;
    int status = 0;

    if (!script_path || !bk_file_read_all(script_path, &raw, &size))
        return command_error("sh", "no se pudo leer el batch");
    script = (char *)bk_sys_alloc(size + 1U);
    if (!script) {
        bk_sys_free(raw);
        return command_error("sh", "sin memoria para batch");
    }
    for (uint32_t i = 0U; i < size; i++)
        script[i] = ((const char *)raw)[i];
    script[size] = '\0';
    bk_sys_free(raw);

    for (uint32_t i = 0U; i <= size; i++) {
        if (i != size && script[i] != '\n' && script[i] != '\r') continue;
        script[i] = '\0';
        char *line = script + start;
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '@' && !line_is_echo_off(line)) line++;
        while (*line == ' ' || *line == '\t') line++;
        if (*line && !line_is_echo_off(line) && *line != '#') {
            kprintf("[SELFHOSTSH] %s\n", line);
            status = run_recipe(line);
            if (status != 0) break;
        }
        if (i < size && script[i + 1U] == '\n' && script[i] == '\0') i++;
        start = i + 1U;
    }

    bk_sys_free(script);
    return status;
}

static int run(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (command_is(argv[i], "-c") && i + 1 < argc)
            return run_recipe(argv[i + 1]);
        if ((command_is(argv[i], "--batch") || command_is(argv[i], "-f")) &&
            i + 1 < argc)
            return run_batch(argv[i + 1]);
    }
    return command_error("sh", "uso: sh -c comando | sh --batch archivo");
}

BK_COMMAND_MAIN(run)
