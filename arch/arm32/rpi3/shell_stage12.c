#include "elf_arm.h"
#include "../../../kernel/include/task.h"
#include "../../../kernel/include/vfs.h"
#include "../../../kernel/include/vga.h"
#include "../../../kernel/include/memory.h"

#define ARM_SHELL_LINE 2048U
static volatile bool g_exit_requested;

static bool same(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b && *a == *b) { ++a; ++b; }
    return !*a && !*b;
}

static void append(char *destination, uint32_t capacity, const char *source) {
    uint32_t used = (uint32_t)kstrlen(destination);
    if (used >= capacity) return;
    while (source && *source && used + 1U < capacity)
        destination[used++] = *source++;
    destination[used] = '\0';
}

static void write_status(int32_t status) {
    if (status == 0) return;
    vga_puts("[ARM] proceso termino con codigo ");
    vga_putint(status);
    vga_putchar('\n');
}

void shell_execute_line(const char *line) {
    char copy[ARM_SHELL_LINE];
    char command[64];
    char executable[VFS_MAX_PATH];
    char *p, *argument;
    uint32_t i = 0U, n = 0U;
    int pid;
    int32_t status = 0;
    if (!line) return;
    while (line[i] && i + 1U < sizeof(copy)) { copy[i] = line[i]; ++i; }
    copy[i] = '\0';
    p = copy;
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) return;
    while (*p && *p != ' ' && *p != '\t' && n + 1U < sizeof(command))
        command[n++] = *p++;
    command[n] = '\0';
    while (*p == ' ' || *p == '\t') ++p;
    argument = p;

    if (same(command, "cd")) {
        if (!*argument) { vga_puts(task_current_cwd()); vga_putchar('\n'); }
        else if (!vfs_chdir(argument)) vga_puts("cd: directorio no encontrado\n");
        return;
    }
    if (same(command, "pwd")) { vga_puts(task_current_cwd()); vga_putchar('\n'); return; }
    if (same(command, "clear") || same(command, "cls")) { vga_clear(); return; }
    if (same(command, "exit")) { g_exit_requested = true; return; }
    if (same(command, "armapps")) {
        vga_puts("Los BEX ARM32 Ring 3 estan en /SYSTEM/COMMANDS\n");
        return;
    }

    /* Commands are real files on FAT.  No command image or implementation is
       embedded in the kernel anymore. */
    if (command[0] == '/' || command[0] == '.') {
        kstrncpy(executable, command, sizeof(executable) - 1U);
        executable[sizeof(executable) - 1U] = '\0';
    } else {
        kstrcpy(executable, "/SYSTEM/COMMANDS/");
        append(executable, sizeof(executable), command);
        append(executable, sizeof(executable), ".BEX");
    }
    pid = rpi3_elf_spawn(executable, copy);
    if (pid < 0) {
        vga_puts("Comando no encontrado o no disponible para ARM32: ");
        vga_puts(command); vga_putchar('\n');
        return;
    }
    for (;;) {
        int32_t waited = task_waitpid((uint32_t)pid, &status);
        if (waited == pid) break;
        if (waited < 0) { status = -1; break; }
        task_yield();
    }
    write_status(status);
}

bool bk_shell_take_exit_request(void) {
    bool value = g_exit_requested;
    g_exit_requested = false;
    return value;
}
