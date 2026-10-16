#include "../kernel/include/api.h"
#include "../kernel/include/bex_loader.h"
#include "../kernel/include/kernel_domains.h"

typedef struct {
    gui_desktop_t *desktop;
    char path[VFS_MAX_PATH];
    char launch_arg[TASK_LAUNCH_ARG_LEN];
    bool has_launch_arg;
} program_async_request_t;

static const char *program_filename(const char *path) {
    const char *name = path;
    if (!path) return "";
    while (*path) {
        if (*path++ == '/') name = path;
    }
    return name;
}

static void program_report_failure(gui_desktop_t *desktop, const char *path,
                                   const char *reason) {
    char text[160];
    uint32_t used = 0U;
    const char *name = program_filename(path);
    if (!desktop) return;
    while (name && *name && used + 1U < sizeof(text)) text[used++] = *name++;
    if (used + 2U < sizeof(text)) { text[used++] = ':'; text[used++] = ' '; }
    while (reason && *reason && used + 1U < sizeof(text))
        text[used++] = *reason++;
    text[used] = '\0';
    /* Async launchers execute outside the GUI task.  Serialize the tiny state
     * update with the normal desktop transaction; entering an already-held
     * GUI domain is recursive for the legacy synchronous callers. */
    kernel_domains_enter(KDOMAIN_GUI);
    gui_desktop_show_alert(desktop, GUI_ALERT_ERROR,
                           "No se pudo iniciar", text, 0);
    kernel_domains_exit(KDOMAIN_GUI);
}

static bool program_name_is(const char *name, const char *expected) {
    while (*name && *expected) {
        char left = *name++;
        char right = *expected++;
        if (left >= 'a' && left <= 'z') left = (char)(left - 'a' + 'A');
        if (right >= 'a' && right <= 'z') right = (char)(right - 'a' + 'A');
        if (left != right) return false;
    }
    return *name == '\0' && *expected == '\0';
}

bool program_is_object(const char *path) {
    const char *name = program_filename(path);
    const char *dot = NULL;
    while (*name) {
        if (*name == '.') dot = name;
        name++;
    }
    return dot && (program_name_is(dot, ".O") ||
                   program_name_is(dot, ".ELF") ||
                   program_name_is(dot, ".CPL") ||
                   program_name_is(dot, ".SCV"));
}

bool program_is_win32_executable(const char *path) {
    const char *name = program_filename(path);
    const char *dot = NULL;

    while (*name) {
        if (*name == '.') dot = name;
        name++;
    }
    return dot && program_name_is(dot, ".EXE");
}

static bool program_is_batch_executable(const char *path) {
    const char *name = program_filename(path);
    const char *dot = NULL;
    while (*name) {
        if (*name == '.') dot = name;
        name++;
    }
    return dot && program_name_is(dot, ".BAT");
}

bool program_is_bex_executable(const char *path) {
    const char *name = program_filename(path);
    const char *dot = NULL;

    while (*name) {
        if (*name == '.') dot = name;
        name++;
    }
    return dot && program_name_is(dot, ".BEX");
}

const char *program_launch_arg(void) {
    const char *arg = bk_proc_launch_arg();

    return arg ? arg : "";
}

bool program_execute_path(gui_desktop_t *desktop, const char *path) {
    return program_execute_path_arg(desktop, path, NULL);
}

static bool program_execute_path_arg_impl(gui_desktop_t *desktop, const char *path,
                              const char *launch_arg) {
    if (!desktop || !path) return false;

    if (program_is_bex_executable(path)) {
        if (!bex_execute_program(path, desktop, launch_arg)) {
            kprintf("[BOSCOMP] no se pudo ejecutar %s: %s\n",
                    path, bex_last_error());
            program_report_failure(desktop, path, bex_last_error());
            return false;
        }
        return true;
    }

    if (program_is_win32_executable(path) || program_is_batch_executable(path)) {
        if (!bex_execute_program("/SYSTEM/PROGRAMS/WINE.BEX", desktop, path)) {
            kprintf("[WINE] no se pudo iniciar %s: %s\n", path,
                    bex_last_error());
            program_report_failure(desktop, path, bex_last_error());
            return false;
        }
        return true;
    }

    if (!program_is_object(path)) {
        kprintf("[APP] formato de programa no compatible: %s\n", path);
        program_report_failure(desktop, path,
                               "formato de programa no compatible");
        return false;
    }
    if (!bk_app_elf_execute(path, desktop, launch_arg)) {
        kprintf("[ELF] no se pudo ejecutar %s: %s\n", path, bk_app_elf_last_error());
        program_report_failure(desktop, path, bk_app_elf_last_error());
        return false;
    }
    return true;
}

bool program_execute_path_arg(gui_desktop_t *desktop, const char *path,
                              const char *launch_arg) {
    gui_compositor_busy_begin();
    bool result = program_execute_path_arg_impl(desktop, path, launch_arg);
    gui_compositor_busy_end();
    return result;
}

static void program_async_worker(void *argument) {
    program_async_request_t *request = (program_async_request_t *)argument;

    if (request) {
        /* Crucially, this task owns no GUI/GFX domain while VFS and the
         * executable loaders read/relocate the image.  The desktop remains
         * schedulable and can paint the released menu immediately. */
        (void)program_execute_path_arg(
            request->desktop, request->path,
            request->has_launch_arg ? request->launch_arg : NULL);
        gui_compositor_busy_end();
        kfree(request);
    }
    task_exit();
}

bool program_execute_path_async(gui_desktop_t *desktop, const char *path) {
    return program_execute_path_async_arg(desktop, path, NULL);
}

bool program_execute_path_async_arg(gui_desktop_t *desktop, const char *path,
                                    const char *launch_arg) {
    program_async_request_t *request;
    int pid;

    if (!desktop || !path || !path[0]) return false;
    request = (program_async_request_t *)kzalloc(sizeof(*request));
    if (!request) return false;
    request->desktop = desktop;
    kstrncpy(request->path, path, sizeof(request->path) - 1U);
    request->path[sizeof(request->path) - 1U] = '\0';
    if (launch_arg && launch_arg[0]) {
        kstrncpy(request->launch_arg, launch_arg,
                 sizeof(request->launch_arg) - 1U);
        request->launch_arg[sizeof(request->launch_arg) - 1U] = '\0';
        request->has_launch_arg = true;
    }

    gui_compositor_busy_begin();
    pid = task_create_kernel("app-loader", program_async_worker, request);
    if (pid < 0) {
        gui_compositor_busy_end();
        kfree(request);
        program_report_failure(desktop, path,
                               "sin memoria para crear el cargador");
        return false;
    }
    return true;
}
