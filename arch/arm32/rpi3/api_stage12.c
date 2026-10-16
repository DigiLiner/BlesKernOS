#include "../../../kernel/include/api.h"
#include "../../../kernel/include/about_dialog.h"
#include "../../../kernel/include/memory.h"
#include "../../../kernel/include/task.h"
#include "../../../kernel/include/vfs.h"
#include "../../../kernel/include/vga.h"
#include "../../../kernel/include/block.h"
#include "../../../kernel/include/graphics_resources.h"
#include "../../../kernel/include/language.h"
#include "../../../kernel/include/mouse.h"
#include "../../../kernel/libc/include/stdio.h"
#include "task_arm.h"
#include "elf_arm.h"
#include "timer.h"

uint32_t bk_sys_getpid(void) { return task_current_pid(); }
void bk_sys_yield(void) { task_yield(); }
void bk_sys_sleep_ticks(uint32_t ticks) { task_sleep(ticks); }
uint32_t bk_sys_tick_frequency(void) { return 100U; }
uint32_t bk_sys_uptime_ms(void) { return rpi3_timer_ticks() * 10U; }
void *bk_sys_alloc(size_t size) { return kmalloc(size); }
void *bk_sys_alloc_zero(size_t size) { return kzalloc(size); }
void bk_sys_free(void *pointer) { kfree(pointer); }
void bk_console_putchar(char character) { vga_putchar(character); }

bool bk_file_chdir(const char *path) { return vfs_chdir(path); }
const char *bk_file_getcwd(void) { return task_current_cwd(); }
bool bk_file_read_all(const char *path, void **buffer, uint32_t *size) {
    return vfs_read_all(path, buffer, size);
}
bool bk_file_write_all(const char *path, const void *buffer, uint32_t size) {
    return vfs_write_all(path, buffer, size);
}
bool bk_file_list_dir(const char *path, vfs_dir_entry_t *entries,
                      uint32_t max_entries, uint32_t *count) {
    return vfs_listdir(path, entries, max_entries, count);
}
bool bk_file_mkdir(const char *path) { return vfs_mkdir(path); }
bool bk_file_remove(const char *path) { return vfs_remove(path); }
bool bk_file_rename(const char *old_path, const char *new_path) {
    return vfs_rename(old_path, new_path);
}
const char *bk_lang_get(const char *key) { return language_get(key); }
const char *bk_lang_translate(const char *source) {
    return language_translate(source);
}
const char *bk_lang_current(void) { return language_current(); }
uint32_t bk_lang_generation(void) { return language_generation(); }
bool bk_lang_set(const char *code) { return language_set(code); }

bool bk_input_mouse(mouse_state_t *state) {
    if (!state) return false;
    mouse_get_state(state);
    return state->present;
}
void bk_input_mouse_set_sensitivity(uint8_t sensitivity) {
    mouse_set_sensitivity(sensitivity);
}
uint8_t bk_input_mouse_get_sensitivity(void) {
    return mouse_get_sensitivity();
}

int bk_proc_spawn_thread(const char *name, bk_thread_entry_t entry,
                         void *argument) {
    return task_create_kernel(name ? name : "thread", entry, argument);
}
void bk_proc_exit(void) { task_exit(); }
bool bk_proc_exit_requested(void) { return false; }
bool bk_proc_request_exit(uint32_t pid) { return task_request_exit(pid); }
uint32_t bk_proc_cpu_count(void) { return 1U; }
uint32_t bk_proc_cpu_usage_core(uint32_t cpu_index) {
    (void)cpu_index;
    return 0U;
}
uint32_t bk_proc_runqueue_depth(uint32_t cpu_index) {
    return cpu_index == 0U ? rpi3_task_ready_count() : 0U;
}
uint32_t bk_proc_scheduler_steals(uint32_t cpu_index) {
    (void)cpu_index;
    return 0U;
}
uint32_t bk_proc_snapshot(bk_proc_info_t *infos, uint32_t capacity) {
    rpi3_task_public_info_t snapshot[32];
    uint32_t process_ids[32];
    uint32_t process_index[32];
    size_t heap_usage[32];
    uint32_t process_count = 0U;
    uint32_t count;
    if (!infos || !capacity) return 0U;
    if (capacity > 32U) capacity = 32U;
    count = rpi3_task_snapshot(snapshot, capacity);

    /* Process Manager used to scan the complete heap once per visible task.
       Besides wasting most of the compositor's time, every scan temporarily
       disables preemption on ARM.  Deduplicate process IDs and account all
       owners in one bounded heap walk, matching the main-kernel API. */
    for (uint32_t i = 0U; i < count; ++i) {
        uint32_t process_id = snapshot[i].process_id
            ? snapshot[i].process_id : snapshot[i].pid;
        uint32_t index = process_count;
        for (uint32_t j = 0U; j < process_count; ++j) {
            if (process_ids[j] == process_id) {
                index = j;
                break;
            }
        }
        if (index == process_count) {
            process_ids[process_count] = process_id;
            heap_usage[process_count] = 0U;
            process_count++;
        }
        process_index[i] = index;
    }
    mm_get_process_usage_batch(process_ids, heap_usage, process_count);

    for (uint32_t i = 0U; i < count; ++i) {
        infos[i].pid = snapshot[i].pid;
        infos[i].process_id = snapshot[i].process_id;
        kstrncpy(infos[i].name, snapshot[i].name, sizeof(infos[i].name) - 1U);
        infos[i].name[sizeof(infos[i].name) - 1U] = '\0';
        infos[i].state = (bk_proc_state_t)snapshot[i].state;
        infos[i].cpu_ticks = snapshot[i].cpu_ticks;
        infos[i].memory_bytes = (uint32_t)heap_usage[process_index[i]];
        if (infos[i].memory_bytes < snapshot[i].memory_hint)
            infos[i].memory_bytes = snapshot[i].memory_hint;
        infos[i].system = !snapshot[i].user;
        infos[i].user = snapshot[i].user;
        infos[i].exit_requested = false;
    }
    return count;
}
void bk_proc_set_memory_hint(uint32_t bytes) { task_set_memory_hint(bytes); }
void bk_proc_bind_window(gui_window_t *window) {
    if (window) window->owner_pid = task_current_process_id();
}

gui_window_t *bk_gui_create_window(gui_desktop_t *desktop, int x, int y,
                                   int w, int h, const char *title) {
    return gui_desktop_create_window(desktop ? desktop : gui_get_desktop(),
                                     x, y, w, h, title);
}
void bk_gui_set_window_content(gui_window_t *window,
                               gui_window_content_paint_t paint,
                               void *context) {
    gui_window_set_content(window, paint, context);
}
void bk_gui_set_window_event_handler(gui_window_t *window,
                                     gui_window_event_t handler,
                                     void *context) {
    gui_window_set_event_handler(window, handler, context);
}
void bk_gui_set_window_min_size(gui_window_t *window, int min_w, int min_h) {
    gui_window_set_min_size(window, min_w, min_h);
}
void bk_gui_focus_window(gui_desktop_t *desktop, gui_window_t *window) {
    if (!window) return;
    gui_desktop_focus_window(desktop ? desktop : gui_get_desktop(), window);
}
void bk_gui_window_invalidate(gui_window_t *window) {
    if (!window) return;
    window->dirty = true;
    gui_request_paint();
}
void bk_gui_request_paint(void) { gui_request_paint(); }
void bk_gui_close_window(gui_window_t *window) { gui_window_close(window); }
int bk_gui_add_menu(gui_window_t *window, const char *label) {
    return gui_window_add_menu(window, label);
}
bool bk_gui_add_menu_item(gui_window_t *window, int menu, uint32_t id,
                          const char *label, gui_menu_callback_t callback,
                          void *context) {
    return gui_window_add_menu_item(window, menu, id, label, callback, context);
}
void bk_gui_surface_draw_polyline(gui_surface_t *surface,
                                  const gui_point_t *points, uint32_t count,
                                  uint32_t color) {
    if (surface && points && count >= 2U && count <= 64U)
        gui_gfx_draw_polyline(surface, points, count, color);
}
bool bk_gui_widget_set_icon(gui_widget_t *widget, const char *resource_name) {
    gui_image_t image;
    if (!widget) return false;
    if (!resource_name || !resource_name[0]) {
        gui_widget_take_icon(widget, NULL, 0U, 0U);
        return true;
    }
    if (!bk_graphics_icon_load(resource_name, &image)) return false;
    gui_widget_take_icon(widget, image.pixels, image.width, image.height);
    return true;
}

bool bk_about_attach(gui_window_t *window, gui_desktop_t *desktop,
                     const bk_about_info_t *info) {
    (void)window; (void)desktop; (void)info;
    /* About-button plumbing is cosmetic; the real terminal itself is shared. */
    return false;
}
void bk_about_show(gui_desktop_t *desktop, const bk_about_info_t *info) {
    desktop = desktop ? desktop : gui_get_desktop();
    if (!desktop || !info) return;
    gui_desktop_show_alert(desktop, GUI_ALERT_INFO,
                           info->name ? info->name : "BlesKernOS",
                           info->description ? info->description : "", 0);
}

bool bk_device_block_info(uint32_t index, bk_block_info_t *info) {
    block_device_t *device;
    if (!info) return false;
    device = block_at(index);
    if (!device) return false;
    kmemset(info, 0, sizeof(*info));
    kstrncpy(info->name, device->name, sizeof(info->name) - 1U);
    kstrncpy(info->type_name, block_type_name(device->type),
             sizeof(info->type_name) - 1U);
    info->type = (bk_block_type_t)device->type;
    info->sector_count = device->sector_count;
    info->sector_size = device->sector_size;
    info->read_only = device->read_only;
    info->removable = device->type == BLOCK_DEVICE_USB ||
                      device->type == BLOCK_DEVICE_SD;
    return true;
}

uint8_t task_cpu_usage(void) { return 0U; }
const char *task_state_name(task_state_t state) {
    static const char *names[] = {"unused", "ready", "running", "sleep", "zombie"};
    return (uint32_t)state < 5U ? names[(uint32_t)state] : "unknown";
}

bool rtc_get_date(rtc_date_t *date) {
    rtc_datetime_t datetime;
    if (!date || !rtc_get_datetime(&datetime)) return false;
    *date = datetime.date;
    return true;
}

bool driver_load(const char *path) { (void)path; return false; }
const char *elf_last_error(void) { return rpi3_elf_last_error(); }
bool program_is_object(const char *path) {
    uint32_t n = path ? (uint32_t)kstrlen(path) : 0U;
    return n >= 4U && path[n - 4U] == '.' &&
           (path[n - 3U] == 'B' || path[n - 3U] == 'b') &&
           (path[n - 2U] == 'E' || path[n - 2U] == 'e') &&
           (path[n - 1U] == 'X' || path[n - 1U] == 'x');
}
bool program_is_win32_executable(const char *path) { (void)path; return false; }

struct FILE { int descriptor; };
FILE *fopen(const char *path, const char *mode) {
    FILE *stream;
    uint32_t flags;
    if (!path || !mode) return NULL;
    if (mode[0] == 'w') flags = VFS_O_WRONLY | VFS_O_CREATE | VFS_O_TRUNC;
    else if (mode[0] == 'a') flags = VFS_O_WRONLY | VFS_O_CREATE;
    else flags = VFS_O_RDONLY;
    stream = (FILE *)kmalloc(sizeof(*stream));
    if (!stream) return NULL;
    stream->descriptor = vfs_open(path, flags);
    if (stream->descriptor < 0) { kfree(stream); return NULL; }
    if (mode[0] == 'a') (void)vfs_seek(stream->descriptor, 0, SEEK_END);
    return stream;
}
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream) {
    size_t bytes;
    int written;
    if (!stream || !buffer || !size || count > 0xFFFFFFFFU / size) return 0U;
    bytes = size * count;
    written = vfs_write(stream->descriptor, buffer, (uint32_t)bytes);
    return written < 0 ? 0U : (size_t)written / size;
}
int fclose(FILE *stream) {
    bool ok;
    if (!stream) return EOF;
    ok = vfs_flush(stream->descriptor) && vfs_close(stream->descriptor);
    kfree(stream);
    return ok ? 0 : EOF;
}
