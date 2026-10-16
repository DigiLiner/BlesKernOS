#include "common.h"

#define putchar bk_console_putchar
#define puts bk_console_write

static int parse_args(char *line, char **argv) {
    int argc = 0;
    char *read = line, *write = line;
    while (*read && argc < COMMAND_MAX_ARGS) {
        char quote = 0;
        while (*read == ' ' || *read == '\t') read++;
        if (!*read) break;
        argv[argc++] = write;
        while (*read) {
            if (!quote && (*read == ' ' || *read == '\t')) break;
            if (*read == '\\' && read[1]) { read++; *write++ = *read++; continue; }
            if (*read == '\'' || *read == '"') {
                if (!quote) { quote = *read++; continue; }
                if (quote == *read) { quote = 0; read++; continue; }
            }
            *write++ = *read++;
        }
        char *next = read;
        while (*next == ' ' || *next == '\t') next++;
        *write++ = '\0';
        read = next;
    }
    argv[argc] = NULL;
    return argc;
}

void command_load_args(command_args_t *args) {
    if (!args) return;
    (void)bk_proc_launch_arg_copy(args->storage, sizeof(args->storage));
    args->storage[sizeof(args->storage) - 1U] = '\0';
    args->argc = parse_args(args->storage, args->argv);

    if (args->argc >= 3 && command_is(args->argv[1], "--cwd")) {
        (void)bk_file_chdir(args->argv[2]);
        for (int i = 1; i + 2 < args->argc; i++) args->argv[i] = args->argv[i + 2];
        args->argc -= 2;
        args->argv[args->argc] = NULL;
    }
}

bool command_is(const char *left, const char *right) {
    return bk_runtime_strcmp(left ? left : "", right ? right : "") == 0;
}

uint32_t command_number(const char *text, bool *valid) {
    uint32_t value = 0, base = 10;
    if (valid) *valid = false;
    if (!text || !*text) return 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    }
    if (!*text) return 0;
    while (*text) {
        uint32_t digit;
        if (*text >= '0' && *text <= '9') digit = (uint32_t)(*text - '0');
        else if (*text >= 'a' && *text <= 'f') digit = (uint32_t)(*text - 'a' + 10);
        else if (*text >= 'A' && *text <= 'F') digit = (uint32_t)(*text - 'A' + 10);
        else return 0;
        if (digit >= base) return 0;
        if (value > (0xffffffffU-digit)/base) return 0;
        value = value * base + digit;
        text++;
    }
    if (valid) *valid = true;
    return value;
}

int command_error(const char *name, const char *reason) {
    kprintf("%s: %s\n", name, reason);
    return 2;
}

static void path_join(char *out, uint32_t capacity,
                      const char *base, const char *name) {
    uint32_t used;
    bk_runtime_strncpy(out, base && *base ? base : "/", capacity - 1U);
    out[capacity - 1U] = '\0';
    used = (uint32_t)bk_runtime_strlen(out);
    if (used && out[used - 1U] != '/' && used + 1U < capacity) {
        out[used++] = '/';
        out[used] = '\0';
    }
    if (name && used + bk_runtime_strlen(name) < capacity)
        bk_runtime_strcat(out, name);
}

int command_list_directory(const char *path) {
    vfs_dir_entry_t *entries;
    uint32_t count = 0;
    entries = (vfs_dir_entry_t *)bk_sys_alloc(
        sizeof(vfs_dir_entry_t) * COMMAND_DIR_MAX);
    if (!entries)
        return command_error("dir", "sin memoria para leer el directorio");
    if (!bk_file_list_dir(path ? path : ".", entries, COMMAND_DIR_MAX, &count)) {
        bk_sys_free(entries);
        return command_error("dir", "@H921A36F7");
    }
    for (uint32_t i = 0; i < count; i++)
        kprintf("%s %u %s\n", entries[i].type == VFS_NODE_DIR ? "<DIR>" : "",
                entries[i].size, entries[i].name);
    kprintf("%u elemento(s)\n", count);
    bk_sys_free(entries);
    return 0;
}

int command_print_file(const char *path, bool paged) {
    void *raw = NULL;
    uint32_t size = 0, lines = 0;
    if (!path || !bk_file_read_all(path, &raw, &size))
        return command_error("archivo", "@H129461B8");
    for (uint32_t i = 0; i < size; i++) {
        char c = (char)((uint8_t *)raw)[i];
        putchar(c);
        if (paged && c == '\n' && ++lines == 20U) {
            puts("-- mas --\n");
            lines = 0;
        }
    }
    if (!size || ((uint8_t *)raw)[size - 1U] != '\n') putchar('\n');
    bk_sys_free(raw);
    return 0;
}

int command_copy_file(const char *from, const char *to, bool remove_source) {
    void *data = NULL;
    uint32_t size = 0;
    if (!from || !to || !bk_file_read_all(from, &data, &size))
        return command_error("copy", "@HF45BFCE1");
    if (!bk_file_write_all(to, data, size)) {
        bk_sys_free(data);
        return command_error("copy", "@HFEB35CD2");
    }
    bk_sys_free(data);
    if (remove_source && !bk_file_remove(from))
        return command_error("move", "@H2C6079C1");
    kprintf("%u bytes: %s -> %s\n", size, from, to);
    return 0;
}

static void tree_walk(const char *path, uint32_t depth) {
    vfs_dir_entry_t *entries;
    uint32_t count = 0;
    if (depth > 12U) return;
    entries = (vfs_dir_entry_t *)bk_sys_alloc(
        sizeof(vfs_dir_entry_t) * COMMAND_DIR_MAX);
    if (!entries) return;
    if (!bk_file_list_dir(path, entries, COMMAND_DIR_MAX, &count)) {
        bk_sys_free(entries);
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t n = 0; n < depth; n++) puts("  ");
        kprintf("%s%s\n", entries[i].name, entries[i].type == VFS_NODE_DIR ? "/" : "");
        if (entries[i].type == VFS_NODE_DIR && !command_is(entries[i].name, ".") &&
            !command_is(entries[i].name, "..")) {
            char child[VFS_MAX_PATH];
            path_join(child, sizeof(child), path, entries[i].name);
            tree_walk(child, depth + 1U);
        }
    }
    bk_sys_free(entries);
}

void command_tree(const char *path) {
    kprintf("%s\n", path);
    tree_walk(path, 1);
}

static bool contains(const char *text, const char *needle) {
    if (!needle || !*needle) return true;
    for (; text && *text; text++) {
        const char *a = text, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

static void find_walk(const char *path, const char *pattern, uint32_t depth) {
    vfs_dir_entry_t *entries;
    uint32_t count = 0;
    if (depth > 12U) return;
    entries = (vfs_dir_entry_t *)bk_sys_alloc(
        sizeof(vfs_dir_entry_t) * COMMAND_DIR_MAX);
    if (!entries) return;
    if (!bk_file_list_dir(path, entries, COMMAND_DIR_MAX, &count)) {
        bk_sys_free(entries);
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        char child[VFS_MAX_PATH];
        path_join(child, sizeof(child), path, entries[i].name);
        if (contains(entries[i].name, pattern)) kprintf("%s\n", child);
        if (entries[i].type == VFS_NODE_DIR && !command_is(entries[i].name, ".") &&
            !command_is(entries[i].name, "..")) find_walk(child, pattern, depth + 1U);
    }
    bk_sys_free(entries);
}

void command_find(const char *path, const char *pattern) {
    find_walk(path, pattern, 0);
}

int command_show_processes(bool detailed) {
    kprintf("PID ESTADO MEMORIA NOMBRE\n");
    for (uint32_t i = 0; i < bk_proc_count(); i++) {
        bk_proc_info_t info;
        if (!bk_proc_info(i, &info)) continue;
        kprintf("%u %u %u %s%s\n", info.pid, info.state, info.memory_bytes,
                info.name, info.system ? " [sistema]" : "");
        if (detailed) kprintf("  cpu=%u proceso=%u usuario=%u salir=%u\n",
                              info.cpu_ticks, info.process_id, info.user,
                              info.exit_requested);
    }
    return 0;
}

int command_show_pci(bool usb_only) {
    uint32_t found = 0;
    for (uint32_t i = 0; i < bk_device_pci_count(); i++) {
        bk_pci_info_t device;
        if (!bk_device_pci_info(i, &device) ||
            (usb_only && (device.class_code != 0x0C || device.subclass != 3)))
            continue;
        kprintf("%u:%u.%u %x:%x %s IRQ %u\n", device.bus, device.slot,
                device.function, device.vendor_id, device.device_id,
                device.class_name, device.interrupt_line);
        found++;
    }
    if (!found) puts("No se encontraron dispositivos.\n");
    return found ? 0 : 1;
}

int command_hexdump(const char *path) {
    static const char hex[] = "0123456789ABCDEF";
    void *raw = NULL;
    uint32_t size = 0;
    if (!path || !bk_file_read_all(path, &raw, &size)) return command_error("hexdump", "@H129461B8");
    for (uint32_t off = 0; off < size; off += 16U) {
        kprintf("%x: ", off);
        for (uint32_t i = 0; i < 16U; i++) {
            if (off + i < size) {
                uint8_t b = ((uint8_t *)raw)[off + i];
                putchar(hex[b >> 4]); putchar(hex[b & 15]);
            } else puts("  ");
            putchar(' ');
        }
        for (uint32_t i = 0; i < 16U && off + i < size; i++) {
            char c = (char)((uint8_t *)raw)[off + i];
            putchar(c >= 32 && c <= 126 ? c : '.');
        }
        putchar('\n');
    }
    bk_sys_free(raw);
    return 0;
}

int command_strings(const char *path) {
    void *raw = NULL;
    uint32_t size = 0, start = 0, run = 0;
    if (!path || !bk_file_read_all(path, &raw, &size)) return command_error("strings", "@H129461B8");
    for (uint32_t i = 0; i <= size; i++) {
        bool printable = i < size && ((uint8_t *)raw)[i] >= 32 && ((uint8_t *)raw)[i] <= 126;
        if (printable) { if (!run) start = i; run++; continue; }
        if (run >= 4U) {
            for (uint32_t n = 0; n < run; n++) putchar((char)((uint8_t *)raw)[start + n]);
            putchar('\n');
        }
        run = 0;
    }
    bk_sys_free(raw);
    return 0;
}

int command_checksum(const char *path) {
    void *raw = NULL;
    uint32_t size = 0, hash = 2166136261U;
    if (!path || !bk_file_read_all(path, &raw, &size)) return command_error("checksum", "@H129461B8");
    for (uint32_t i = 0; i < size; i++) { hash ^= ((uint8_t *)raw)[i]; hash *= 16777619U; }
    bk_sys_free(raw);
    kprintf("FNV1a32 %x %s\n", hash, path);
    return 0;
}

#define TOOLCHAIN_COMMAND_MAX 2047U
#define TOOLCHAIN_ELF "/SYSTEM/DEVEL/TCC/I386-TCC.EXE"

static bool toolchain_append(char *out, uint32_t capacity, const char *text) {
    uint32_t used = (uint32_t)bk_runtime_strlen(out);
    uint32_t length = (uint32_t)bk_runtime_strlen(text ? text : "");
    if (used + length + 1U > capacity) return false;
    bk_runtime_strcat(out, text ? text : "");
    return true;
}

static bool toolchain_append_arg(char *out, uint32_t capacity,
                                 const char *argument) {
    bool quote = false;
    const char *p = argument ? argument : "";
    uint32_t used = (uint32_t)bk_runtime_strlen(out);
    if (used + 2U > capacity) return false;
    out[used++] = ' ';
    out[used] = '\0';
    for (; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '"') quote = true;
    if (!quote) return toolchain_append(out, capacity, argument ? argument : "");
    if (used + 2U > capacity) return false;
    out[used++] = '"';
    out[used] = '\0';
    for (p = argument ? argument : ""; *p; p++) {
        char part[3] = {0, 0, 0};
        if (*p == '"' || *p == '\\') part[0] = '\\', part[1] = *p;
        else part[0] = *p;
        if (!toolchain_append(out, capacity, part)) return false;
    }
    used = (uint32_t)bk_runtime_strlen(out);
    if (used + 2U > capacity) return false;
    out[used++] = '"';
    out[used] = '\0';
    return true;
}

bool command_append_argument(char *out,uint32_t capacity,const char *argument) {
    return toolchain_append_arg(out,capacity,argument);
}

int command_toolchain_run(const char *program, int argc, char **argv,
                          const char *prefix) {
    char line[TOOLCHAIN_COMMAND_MAX + 1U];
    int status;
    const char *executable = program ? program : TOOLCHAIN_ELF;

    line[0] = '\0';
    if (!toolchain_append(line, sizeof(line), executable))
        return command_error("toolchain", "linea de comandos demasiado larga");
    if (prefix && *prefix) {
        uint32_t used = (uint32_t)bk_runtime_strlen(line);
        if (used + 2U > sizeof(line))
            return command_error("toolchain", "linea de comandos demasiado larga");
        line[used++] = ' ';
        line[used] = '\0';
        if (!toolchain_append(line, sizeof(line), prefix))
            return command_error("toolchain", "linea de comandos demasiado larga");
    }
    for (int i = 1; i < argc; i++) {
        if (!toolchain_append_arg(line, sizeof(line), argv[i]))
            return command_error("toolchain", "linea de comandos demasiado larga");
    }

    status = bk_app_run_sync(executable, line);

    if (status < 0)
        return command_error("toolchain", "no se pudo ejecutar la herramienta");
    return status;
}

int command_build_native(const char *source, const char *output) {
    char raw[BK_PATH_MAX];
    char compile_line[TOOLCHAIN_COMMAND_MAX + 1U];
    char link_line[TOOLCHAIN_COMMAND_MAX + 1U];
    int status;

    if (!source || !output) return command_error("build", "uso: build fuente.c salida.BEX");
    if (bk_runtime_strlen(output) + 7U >= sizeof(raw))
        return command_error("build", "ruta de salida demasiado larga");
    bk_runtime_strncpy(raw, output, sizeof(raw) - 1U);
    raw[sizeof(raw) - 1U] = '\0';
    bk_runtime_strcat(raw, ".RAW.O");

    compile_line[0] = '\0';
    toolchain_append(compile_line, sizeof(compile_line), TOOLCHAIN_ELF);
    toolchain_append(compile_line, sizeof(compile_line),
        " -m32 -ffreestanding -fno-builtin -nostdlib -nostdinc -fno-pic"
        " -fno-pie -fno-stack-protector -I/SYSTEM/SDK/INCLUDE -c");
    if (!toolchain_append_arg(compile_line, sizeof(compile_line), source) ||
        !toolchain_append(compile_line, sizeof(compile_line), " -o") ||
        !toolchain_append_arg(compile_line, sizeof(compile_line), raw))
        return command_error("build", "linea de compilacion demasiado larga");
    kprintf("[CC] %s -> %s\n", source, raw);
    status = bk_app_run_sync(TOOLCHAIN_ELF, compile_line);
    if (status != 0) return status < 0 ? 2 : status;

    link_line[0] = '\0';
    toolchain_append(link_line, sizeof(link_line), TOOLCHAIN_ELF);
    toolchain_append(link_line, sizeof(link_line), " -nostdlib -r");
    if (!toolchain_append_arg(link_line, sizeof(link_line), raw) ||
        !toolchain_append_arg(link_line, sizeof(link_line),
                              "/SYSTEM/SDK/LIB/LIBBLESK.A") ||
        !toolchain_append(link_line, sizeof(link_line), " -o") ||
        !toolchain_append_arg(link_line, sizeof(link_line), output))
        return command_error("build", "linea de enlace demasiado larga");
    kprintf("[LD] %s -> %s\n", raw, output);
    status = bk_app_run_sync(TOOLCHAIN_ELF, link_line);
    if (status == 0) {
        (void)bk_file_remove(raw);
        kprintf("[OK] BEX nativo listo: %s\n", output);
    }
    return status < 0 ? 2 : status;
}

#include "filetools.inc"
#include "command_help.inc"

#include "networktools.inc"
