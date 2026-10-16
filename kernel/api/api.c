#include "../include/vision_ipc.h"
#include "../include/pager.h"
#include "../include/swap.h"
#include "../include/admin.h"
#include "../include/perfmon.h"
#define BK_API_IMPLEMENTATION
#include "../include/api.h"
#include "../include/pit.h"
#include "../include/memory.h"
#include "../include/vfs.h"
#include "../include/gfx.h"
#include "../include/gfx3d.h"
#include "../include/mouse.h"
#include "../include/keyboard.h"
#include "../include/rtc.h"
#include "../include/datetime_prefs.h"
#include "../include/language.h"
#include "../include/sound.h"
#include "../include/system_sounds.h"
#include "../include/task.h"
#include "../include/usercopy.h"
#include "../include/syscall.h"
#include "../include/klock.h"
#include "../include/bkl_setup.h"
#include "../include/pic.h"
#include "../include/lpt.h"
#include "../../programs/programs.h"
#include "../libc/include/stdio.h"

static bool g_datetime_runtime_preferences_valid = false;
static bk_datetime_preferences_t g_datetime_runtime_preferences;
static volatile bool g_print_spooler_ready = false;

/* The historical SDK exposes gui_desktop_t to applications. The real desktop
 * lives in supervisor-only BSS now, so hand Ring 3 a harmless user-heap view
 * containing the screen geometry and translate it back at API boundaries. */
static gui_desktop_t *g_user_desktop_proxy;
static kspinlock_t g_user_desktop_proxy_lock = KSPINLOCK_INITIALIZER;

static gui_desktop_t *api_user_desktop_proxy(void) {
    gui_desktop_t *real = gui_get_desktop();
    gui_desktop_t *candidate = NULL;
    uint32_t flags;

    if (!real) return NULL;
    if (!g_user_desktop_proxy) {
        candidate = (gui_desktop_t *)kzalloc(sizeof(*candidate));
        if (!candidate) return NULL;
        (void)mm_set_allocation_owner(candidate, 0U);
        flags = kspin_lock_irqsave(&g_user_desktop_proxy_lock);
        if (!g_user_desktop_proxy) g_user_desktop_proxy = candidate;
        else kfree(candidate);
        kspin_unlock_irqrestore(&g_user_desktop_proxy_lock, flags);
    }
    if (!g_user_desktop_proxy) return NULL;
    g_user_desktop_proxy->surface.pixels = NULL;
    g_user_desktop_proxy->surface.width = real->surface.width;
    g_user_desktop_proxy->surface.height = real->surface.height;
    g_user_desktop_proxy->surface.pitch = real->surface.pitch;
    g_user_desktop_proxy->surface.clip = real->surface.clip;
    return g_user_desktop_proxy;
}

/* Also used by the ELF bridge for historical raw gui_desktop_* exports. */
gui_desktop_t *api_resolve_desktop(gui_desktop_t *desktop) {
    if (!desktop || desktop == g_user_desktop_proxy)
        return gui_get_desktop();
    return desktop;
}

static const char *api_export_string(const char *source) {
    const char *visible = task_user_export_string(source ? source : "");
    return visible ? visible : "";
}

uint32_t bk_sys_api_version(void) {
    return BK_API_VERSION;
}

uint32_t bk_sys_capabilities(void) {
    return BK_API_CAP_ALL;
}

void bk_sys_log(const char *message) {
    if (!message) return;
    kprintf("%s", message);
}

void bk_console_putchar(char character) {
    vga_putchar(character);
}

void bk_console_write(const char *text) {
    if (text) vga_puts(language_translate(text));
}

uint32_t bk_sys_getpid(void) {
    /* Match SYS_GETPID: worker threads share their process's resources. */
    return task_current_process_id();
}

void bk_sys_yield(void) {
    task_yield();
}

void bk_sys_sleep_ticks(uint32_t ticks) {
    task_sleep(ticks);
}

void bk_sys_sleep_ms(uint32_t milliseconds) {
    uint32_t hz = pit_get_frequency_hz();
    uint64_t ticks;

    if (!milliseconds) {
        task_yield();
        return;
    }
    if (!hz) hz = 100U;
    ticks = ((uint64_t)milliseconds * hz + 999U) / 1000U;
    task_sleep((uint32_t)(ticks ? ticks : 1U));
}

uint32_t bk_sys_ticks(void) {
    return pit_get_ticks();
}

uint32_t bk_sys_tick_frequency(void) {
    return pit_get_frequency_hz();
}

uint32_t bk_sys_uptime_ms(void) {
    uint32_t hz = pit_get_frequency_hz();
    return hz ? (uint32_t)(((uint64_t)pit_get_ticks() * 1000U) / hz) : 0;
}

void bk_sys_reboot(void) {
    (void)vfs_sync_all();
#if defined(BLES_ARCH_ARM32_RPI3)
    /* El reinicio por watchdog BCM2837 se conecta en la capa de plataforma.
       Hasta entonces, detener limpiamente es preferible a emitir puertos x86. */
    for (;;) __asm__ volatile ("wfe");
#else
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile ("cli; hlt");
#endif
}

void bk_sys_shutdown(void) {
    (void)vfs_sync_all();
#if defined(BLES_ARCH_ARM32_RPI3)
    for (;;) __asm__ volatile ("wfe");
#else
    /* ACPI poweroff usado por QEMU/Bochs; hardware sin ACPI cae en halt. */
    pic_outw(0x604U, 0x2000U);
    for (;;) __asm__ volatile ("cli; hlt");
#endif
}

void *bk_sys_alloc(size_t size) {
    void *result;
    if (size >= 1024U * 1024U)
        kprintf("[MM:PERF] ring3 alloc begin size=%u pid=%u\n",
                (uint32_t)size, task_current_process_id());
    result = kmalloc(size);
    if (size >= 1024U * 1024U)
        kprintf("[MM:PERF] ring3 alloc end ptr=%x pid=%u\n",
                (uint32_t)(uintptr_t)result, task_current_process_id());
    return result;
}

void *bk_sys_alloc_zero(size_t size) {
    return kzalloc(size);
}

void *bk_sys_realloc(void *ptr, size_t size) {
    return krealloc(ptr, size);
}

void bk_sys_free(void *ptr) {
    kfree(ptr);
}

bool bk_sys_memory_info(system_memory_info_t *info) {
    if (!info) return false;
    mm_get_system_info(info);
    return true;
}

int bk_file_open(const char *path, uint32_t flags) {
    return vfs_open(path, flags);
}

int bk_file_read(int fd, void *buffer, uint32_t size) {
    return vfs_read(fd, buffer, size);
}

int bk_file_write(int fd, const void *buffer, uint32_t size) {
    return vfs_write(fd, buffer, size);
}

int32_t bk_file_seek(int fd, int32_t offset, uint32_t origin) {
    return vfs_seek(fd, offset, origin);
}

int32_t bk_file_tell(int fd) {
    return vfs_tell(fd);
}

int32_t bk_file_size(int fd) {
    return vfs_size(fd);
}

bool bk_file_close(int fd) {
    return vfs_close(fd);
}

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

bool bk_file_stat(const char *path, vfs_dir_entry_t *entry) {
    return vfs_stat(path, entry);
}

bool bk_file_chdir(const char *path) {
    return vfs_chdir(path);
}

const char *bk_file_getcwd(void) {
    return api_export_string(vfs_getcwd());
}

bool bk_file_mkdir(const char *path) {
    return vfs_mkdir(path);
}

bool bk_file_remove(const char *path) {
    return vfs_remove(path);
}

int bk_file_transfer_check(const char *source,const char *directory) {
    return vfs_transfer_check(source,directory);
}

bool bk_file_rename(const char *old_path, const char *new_path) {
    return vfs_rename(old_path, new_path);
}

bool bk_setup_extract_package(const char *path) {
    return bkl_setup_extract_package(path);
}

bool bk_setup_get_progress(bkl_setup_progress_t *progress) {
    return bkl_setup_get_progress(progress);
}

bool bk_file_space(uint64_t *total_bytes, uint64_t *free_bytes) {
    if (!total_bytes || !free_bytes) return false;
    return vfs_get_space(total_bytes, free_bytes);
}

const char *bk_lang_get(const char *key) {
    return api_export_string(language_get(key));
}
const char *bk_lang_translate(const char *source) {
    return api_export_string(language_translate(source));
}
const char *bk_lang_current(void) {
    return api_export_string(language_current());
}
uint32_t bk_lang_generation(void) { return language_generation(); }
uint32_t bk_lang_count(void) { return language_count(); }
bool bk_lang_info(uint32_t index, bk_language_info_t *info) {
    return language_info(index, info);
}
bool bk_lang_set(const char *code) {
    bool changed = language_set(code);
    if (changed) gui_request_paint();
    return changed;
}

bool bk_device_format_fat(const char *device_name, const char *volume_label) {
    block_device_t *device=block_get(device_name);fat_fs_t active;
    if(!device)return false;
    /* A name-only check misses the whole disk backing an active partition. */
    if(vfs_get_fs_info(&active) && active.device &&
       device->driver_data==active.device->driver_data && device->read==active.device->read)return false;
    if(vfs_volume_busy(device_name) || !vfs_sync_all())return false;
    vfs_forget_volume(device_name);
    return fat_format(device_name, volume_label);
}

uint32_t bk_print_lpt_count(void) {
    return lpt_port_count();
}

bool bk_print_lpt_info(uint32_t index, bk_print_lpt_info_t *info) {
    lpt_port_info_t internal;
    if (!info || !lpt_port_info(index, &internal)) return false;
    info->base = internal.base;
    info->raw_status = internal.raw_status;
    info->present = internal.present;
    info->busy = internal.busy;
    info->selected = internal.selected;
    info->paper_out = internal.paper_out;
    info->error = internal.error;
    info->acknowledged = internal.acknowledged;
    return true;
}

bool bk_print_port_info(uint32_t index, bk_print_port_info_t *info) {
    lpt_port_info_t internal;
    if (!info || info->struct_size < sizeof(*info) ||
        !lpt_port_info(index, &internal)) return false;
    kstrncpy(info->name, internal.name, sizeof(info->name) - 1U);
    info->name[sizeof(info->name) - 1U] = '\0';
    info->base = internal.base;
    info->raw_status = internal.raw_status;
    info->present = internal.present;
    info->busy = internal.busy;
    info->selected = internal.selected;
    info->paper_out = internal.paper_out;
    info->error = internal.error;
    info->acknowledged = internal.acknowledged;
    info->virtual_port = internal.virtual_port;
    return true;
}

bool bk_print_device_id(uint32_t index, char *buffer, uint32_t capacity) {
    return lpt_device_id(index, buffer, capacity);
}

int32_t bk_print_lpt_write(uint32_t index, const void *data, uint32_t length,
                           uint32_t idle_timeout_ms) {
    return lpt_write(index, data, length, idle_timeout_ms);
}

void bk_print_spooler_set_ready(bool ready) {
    g_print_spooler_ready = ready;
    if(ready)admin_ready(2);
}

bool bk_print_spooler_is_ready(void) {
    return g_print_spooler_ready;
}

uint32_t bk_sdr_count(void) {
    return sdr_device_count();
}

bool bk_sdr_info(uint32_t index, bk_sdr_info_t *info) {
    sdr_device_info_t internal;
    if (!info || info->struct_size < sizeof(*info)) return false;
    kmemset(&internal, 0, sizeof(internal));
    internal.struct_size = sizeof(internal);
    if (!sdr_device_info(index, &internal)) return false;

    kmemset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info);
    kstrncpy(info->name, internal.name, sizeof(info->name) - 1U);
    info->name[sizeof(info->name) - 1U] = '\0';
    kstrncpy(info->tuner, internal.tuner, sizeof(info->tuner) - 1U);
    info->tuner[sizeof(info->tuner) - 1U] = '\0';
    info->vendor_id = internal.vendor_id;
    info->product_id = internal.product_id;
    info->min_frequency_hz = internal.min_frequency_hz;
    info->max_frequency_hz = internal.max_frequency_hz;
    info->frequency_hz = internal.frequency_hz;
    info->sample_rate_hz = internal.sample_rate_hz;
    info->gain_tenth_db = internal.gain_tenth_db;
    info->streaming = internal.streaming;
    return true;
}

int32_t bk_sdr_open(uint32_t index) {
    return sdr_open(index);
}

void bk_sdr_close(int32_t handle) {
    sdr_close(handle);
}

bool bk_sdr_set_frequency(int32_t handle, uint32_t frequency_hz) {
    return sdr_set_frequency(handle, frequency_hz);
}

bool bk_sdr_set_sample_rate(int32_t handle, uint32_t sample_rate_hz) {
    return sdr_set_sample_rate(handle, sample_rate_hz);
}

bool bk_sdr_set_gain(int32_t handle, int32_t gain_tenth_db) {
    return sdr_set_gain(handle, gain_tenth_db);
}

int32_t bk_sdr_read(int32_t handle, void *buffer, uint32_t capacity,
                    uint32_t timeout_ms) {
    return sdr_read(handle, buffer, capacity, timeout_ms);
}

bool bk_vision_claim(void) {
    return gui_vision_claim_service();
}

bool bk_vision_initialize_engine(void) {
    return gui_vision_initialize_engine();
}

bool bk_vision_start_shell(void) {
    return gui_vision_start_shell();
}

void bk_vision_publish_ready(void) {
    gui_vision_publish_ready();
}

bool bk_vision_step(void) {
    return gui_vision_step();
}

void bk_vision_release(void) {
    gui_vision_release_service();
}

gui_desktop_t *bk_vision_desktop(void) {
    return gui_vision_desktop();
}

bool bk_vision_setup_mode(void) {
    return gui_vision_setup_mode();
}

bool bk_vision_take_layout_refresh(void) {
    return gui_vision_take_layout_refresh();
}

bool bk_gui_file_drag_start_icon(gui_window_t *window,const char *path,
                                  const uint32_t *pixels,unsigned width,unsigned height) {
    gui_desktop_t *desktop=gui_get_desktop();
    if(!desktop || !window || !window->listed || !path || !path[0] ||
       kstrlen(path)>=260 || !(desktop->mouse_buttons&MOUSE_LEFT_BUTTON) ||
       (window->owner_pid && window->owner_pid!=task_current_process_id()) ||
       (pixels && (!width || !height || width>256 || height>256)))return false;
    desktop->drag_icon_ghost=false;
    if(pixels) {
        unsigned size=width>height?width:height;
        if(size>64)size=64;
        kmemset(desktop->drag_icon_mask,0,sizeof(desktop->drag_icon_mask));
        for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
            /* Keep non-square sprites centred without stretching their logo. */
            unsigned span=width>height?width:height;
            unsigned sx=x*span/size,sy=y*span/size;
            unsigned left=(span-width)/2,top=(span-height)/2;
            if(sx>=left && sy>=top && sx-left<width && sy-top<height &&
               (pixels[(sy-top)*width+sx-left]>>24))desktop->drag_icon_mask[y*64+x]=1;
        }
        for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
            uint8_t *mask=desktop->drag_icon_mask;
            if(mask[y*64+x] && (!x || !y || x==size-1 || y==size-1 ||
                !mask[y*64+x-1] || !mask[y*64+x+1] || !mask[(y-1)*64+x] || !mask[(y+1)*64+x]))
                mask[y*64+x]=2;
        }
        desktop->drag_icon_size=size;desktop->drag_icon_count=1;desktop->drag_icon_ghost=true;
    }
    kstrcpy(desktop->file_drag_path,path);desktop->file_drag_window=window;
    __sync_synchronize();desktop->file_drag_active=true;return true;
}
bool bk_gui_file_drag_start(gui_window_t *window,const char *path) {
    return bk_gui_file_drag_start_icon(window,path,NULL,0,0);
}
void bk_gui_file_drag_cancel(gui_window_t *window) {
    gui_desktop_t *desktop=gui_get_desktop();
    if(desktop && desktop->file_drag_window==window) {
        desktop->file_drag_active=false;desktop->file_drag_window=NULL;
        desktop->drag_outline_visible=false;desktop->drag_icon_ghost=false;gui_desktop_invalidate_all(desktop);
    }
}
bool bk_gui_file_drop_take(gui_window_t *window,const gui_event_t *event,char *path) {
    gui_desktop_t *desktop=gui_get_desktop();
    if(!desktop || !window || !event || !path || event->type!=GUI_EVENT_FILE_DROP ||
       (window->owner_pid && window->owner_pid!=task_current_process_id()))return false;
    unsigned slot=(unsigned)event->dx;
    if(slot>=4 || !desktop->file_drops[slot].used ||
       desktop->file_drops[slot].token!=(uint32_t)event->dy)return false;
    if(desktop->file_drop_windows[slot]!=window)return false;
    if(desktop->file_drop_batches[slot]) {
        unsigned index=desktop->file_drop_positions[slot]++;
        kstrncpy(path,desktop->file_drop_batches[slot][index],259);path[259]=0;
        if(desktop->file_drop_positions[slot]<desktop->file_drop_counts[slot])return true;
        kfree(desktop->file_drop_batches[slot]);desktop->file_drop_batches[slot]=NULL;
    } else {kstrncpy(path,desktop->file_drops[slot].path,259);path[259]=0;}
    desktop->file_drop_windows[slot]=NULL;
    __sync_lock_release(&desktop->file_drops[slot].used);return true;
}
uint32_t bk_gui_files_sequence(void) {
    gui_desktop_t *desktop=gui_get_desktop();return desktop ? desktop->file_change_sequence : 0;
}
void bk_gui_files_changed(void) {
    gui_desktop_t *desktop=gui_get_desktop();
    if(desktop) {
        __sync_add_and_fetch(&desktop->file_change_sequence,1);
        gui_desktop_invalidate_all(desktop);gui_request_paint();
    }
}

gui_desktop_t *bk_gui_desktop(void) {
    return api_user_desktop_proxy();
}

void bk_gui_request_paint(void) {
    if (task_current_is_user()) {
        (void)gui_desktop_request_owner_paint(gui_get_desktop(),
            task_current_process_id(), task_current_pid());
    }
    gui_request_paint();
}

static uint32_t api_vision_window_call(gui_window_t *,uint32_t,gui_rect_t,uint32_t,uint32_t);
gui_program_t *bk_gui_register_program(gui_desktop_t *desktop,
                                       const char *name, void *state,
                                       gui_program_paint_t paint,
                                       gui_program_event_t handle_event,
                                       gui_program_destroy_t destroy) {
    if(vision_ipc_routes_user()) {
        bk_vision_request_t request={0};uint32_t result=0;
        request.operation=VISION_IPC_PROGRAM_ADD;request.values[0]=(uint32_t)(uintptr_t)state;
        request.values[1]=(uint32_t)(uintptr_t)paint;request.values[2]=(uint32_t)(uintptr_t)handle_event;
        request.values[3]=(uint32_t)(uintptr_t)destroy;kstrncpy(request.title,name?name:"",sizeof(request.title)-1U);
        if(!vision_ipc_call(&request,&result))return NULL;
        return (gui_program_t *)(uintptr_t)result;
    }
    return gui_desktop_register_program(api_resolve_desktop(desktop), name,
                                        state, paint, handle_event, destroy);
}

void bk_gui_unregister_program(gui_desktop_t *desktop, gui_program_t *program) {
    if(vision_ipc_routes_user()) {(void)api_vision_window_call((gui_window_t *)program,VISION_IPC_PROGRAM_REMOVE,(gui_rect_t){0},0,0);return;}
    gui_desktop_unregister_program(api_resolve_desktop(desktop), program);
}

void bk_gui_invalidate_all(gui_desktop_t *desktop) {
    gui_desktop_invalidate_all(api_resolve_desktop(desktop));
}

static uint32_t api_vision_window_call(gui_window_t *window,uint32_t operation,
                                       gui_rect_t bounds,uint32_t a,uint32_t b) {
    bk_vision_request_t request={0};uint32_t result=0;
    request.operation=operation;request.window=(uint32_t)(uintptr_t)window;
    request.bounds=bounds;request.values[0]=a;request.values[1]=b;
    (void)vision_ipc_call(&request,&result);return result;
}
gui_window_t *bk_gui_create_window(gui_desktop_t *desktop, int x, int y,
                                   int w, int h, const char *title) {
    if(vision_ipc_routes_user()) {
        bk_vision_request_t request={0};uint32_t result=0;
        request.operation=VISION_IPC_CREATE;request.bounds=(gui_rect_t){x,y,w,h};
        if(title)kstrncpy(request.title,title,sizeof(request.title)-1U);
        (void)vision_ipc_call(&request,&result);
        return (gui_window_t *)(uintptr_t)result;
    }
    gui_window_t *window = gui_desktop_create_window(
        api_resolve_desktop(desktop), x, y, w, h, title);
    if (window && task_current_is_user()) {
        window->owner_pid = task_current_process_id();
        window->controller_pid = task_current_process_id();
    }
    return window;
}

bool bk_gui_alert(bk_alert_kind_t kind, const char *title,
                  const char *message, int32_t code) {
    gui_desktop_t *desktop = gui_get_desktop();
    gui_alert_kind_t gui_kind = GUI_ALERT_ERROR;

    if (!desktop || desktop->surface.width == 0U
        || desktop->surface.height == 0U) return false;
    if (kind == BK_ALERT_INFO) gui_kind = GUI_ALERT_INFO;
    else if (kind == BK_ALERT_WARNING) gui_kind = GUI_ALERT_WARNING;
    else if (kind == BK_ALERT_NETWORK) gui_kind = GUI_ALERT_NETWORK;
    gui_desktop_show_alert(desktop, gui_kind, title, message, code);
    return true;
}

bool bk_gui_error(const char *title, const char *message) {
    return bk_gui_alert(BK_ALERT_ERROR, title, message, 0);
}

bool bk_gui_network_error(const char *operation, int32_t code) {
    char message[160];

    snprintf(message, sizeof(message),
             "No se pudo completar la operacion de red: %s",
             operation && operation[0] ? operation : "conexion");
    return bk_gui_alert(BK_ALERT_NETWORK, "Error de red", message, code);
}

void bk_gui_close_window(gui_window_t *window) {
    if(vision_ipc_routes_user()) {
        (void)api_vision_window_call(window,VISION_IPC_CLOSE,(gui_rect_t){0},0,0);return;
    }
    gui_window_close(window);
}

void bk_gui_focus_window(gui_desktop_t *desktop, gui_window_t *window) {
    if(vision_ipc_routes_user()) {
        (void)api_vision_window_call(window,VISION_IPC_FOCUS,(gui_rect_t){0},0,0);return;
    }
    gui_desktop_focus_window(api_resolve_desktop(desktop), window);
}

void bk_gui_set_window_content(gui_window_t *window,
                               gui_window_content_paint_t paint,
                               void *context) {
    if(vision_ipc_routes_user()) {
        (void)api_vision_window_call(window,VISION_IPC_CONTENT, (gui_rect_t){0},(uint32_t)(uintptr_t)paint,(uint32_t)(uintptr_t)context);return;
    }
    gui_window_set_content(window, paint, context);
}

void bk_gui_set_window_event_handler(gui_window_t *window,
                                     gui_window_event_t handler,
                                     void *context) {
    if(vision_ipc_routes_user()) {
        (void)api_vision_window_call(window,VISION_IPC_EVENT, (gui_rect_t){0},(uint32_t)(uintptr_t)handler,(uint32_t)(uintptr_t)context);return;
    }
    gui_window_set_event_handler(window, handler, context);
}

void bk_gui_window_set_extended_events(gui_window_t *window, bool enabled) {
    if(vision_ipc_routes_user()) {
        (void)api_vision_window_call(window,VISION_IPC_EXTENDED, (gui_rect_t){0},enabled,0);return;
    }
    gui_window_set_extended_events(window, enabled);
}

void bk_gui_window_set_compact_content(gui_window_t *window, bool enabled) {
    if(vision_ipc_routes_user()) {
        (void)api_vision_window_call(window,VISION_IPC_COMPACT, (gui_rect_t){0},enabled,0);return;
    }
    if(!window)return;
    gui_desktop_paint_lock();window->compact_content=enabled;window->dirty=true;gui_desktop_paint_unlock();
}

void bk_gui_window_set_streaming(gui_window_t *window, bool enabled) {
    if(vision_ipc_routes_user()) {
        (void)api_vision_window_call(window,VISION_IPC_STREAMING, (gui_rect_t){0},enabled,0);return;
    }
    if (!window) return;
    gui_desktop_paint_lock();
    window->streaming_content = enabled;
    if (!enabled) window->dirty = true;
    gui_desktop_paint_unlock();
}

bool bk_gui_window_present_xrgb(gui_window_t *window,const uint32_t *pixels,
                                uint16_t width,uint16_t height) {
    uint64_t started=perfmon_scope_begin();
    if(!window || !pixels || !width || !height || !window->streaming_content ||
       !window->listed || (window->owner_pid &&
       window->owner_pid!=task_current_process_id()))return false;
    gui_desktop_paint_lock();
    bool result=window->listed && window->streaming_content &&
                gui_window_update_stream(window,pixels,width,height);
    if(result) {window->dirty=true;gui_request_paint();}
    gui_desktop_paint_unlock();
    if(result)perfmon_app_frame(window->owner_pid,false);
    perfmon_scope_end(PERF_SCOPE_APP_PUBLISH,started);return result;
}

void bk_gui_set_window_min_size(gui_window_t *window, int min_w, int min_h) {
    if(vision_ipc_routes_user()) {
        (void)api_vision_window_call(window,VISION_IPC_MIN_SIZE,(gui_rect_t){0},(uint32_t)min_w,(uint32_t)min_h);return;
    }
    gui_window_set_min_size(window, min_w, min_h);
}

int bk_gui_add_menu(gui_window_t *window, const char *label) {
    if(vision_ipc_routes_user()) {
        bk_vision_request_t request={0};uint32_t result=0;
        request.operation=VISION_IPC_MENU_ADD;request.window=(uint32_t)(uintptr_t)window;
        kstrncpy(request.title,label?label:"",sizeof(request.title)-1U);
        return vision_ipc_call(&request,&result) && result?(int)result-1:-1;
    }
    return gui_window_add_menu(window, label);
}

bool bk_gui_add_menu_item(gui_window_t *window, int menu, uint32_t id,
                          const char *label, gui_menu_callback_t callback,
                          void *context) {
    if(vision_ipc_routes_user()) {
        bk_vision_request_t request={0};uint32_t result=0;
        request.operation=VISION_IPC_MENU_ITEM;request.window=(uint32_t)(uintptr_t)window;
        request.values[0]=(uint32_t)menu;request.values[1]=id;
        request.values[2]=(uint32_t)(uintptr_t)callback;request.values[3]=(uint32_t)(uintptr_t)context;
        kstrncpy(request.title,label?label:"",sizeof(request.title)-1U);
        return vision_ipc_call(&request,&result) && result;
    }
    return gui_window_add_menu_item(window, menu, id, label, callback, context);
}

void bk_gui_destroy_window(gui_desktop_t *desktop, gui_window_t *window) {
    if(vision_ipc_routes_user()) {
        (void)api_vision_window_call(window,VISION_IPC_DESTROY,(gui_rect_t){0},0,0);return;
    }
    if (!window) return;
    desktop = api_resolve_desktop(desktop);
    if (desktop) gui_desktop_remove_window(desktop, window);
    gui_window_destroy(window);
}

bool bk_gui_window_is_open(const gui_window_t *window) {
    return window && window->listed;
}

void bk_gui_window_set_owner(gui_window_t *window, uint32_t pid) {
    if(vision_ipc_routes_user()) {
        (void)api_vision_window_call(window,VISION_IPC_OWNER,(gui_rect_t){0},pid,0);return;
    }
    if (window) window->owner_pid = pid;
}

void bk_gui_window_invalidate(gui_window_t *window) {
    if (!window) return;
    window->dirty = true;
    /* An asynchronous client's invalidation may arrive after Vision sampled
     * this window but before it clears dirty at frame completion. Keep it
     * until a content callback is actually queued (which consumes this bit).
     * Streaming and synchronous windows have no such callback lifecycle. */
    if (window->content_pid && window->content_paint &&
        !window->streaming_content)
        window->content_repaint = true;
    gui_request_paint();
}

bool bk_gui_window_set_gpu_viewport(gui_window_t *window,
                                    uint32_t surface_handle,
                                    uint16_t surface_width,
                                    uint16_t surface_height,
                                    gui_rect_t screen_rect) {
    uint64_t perf_started = perfmon_scope_begin();
    if (!window || !window->listed || !surface_handle ||
        !surface_width || !surface_height ||
        screen_rect.w <= 0 || screen_rect.h <= 0 ||
        (window->owner_pid && window->owner_pid != task_current_process_id())) {
        static bool reported;
        if (!reported) {
            reported = true;
            kprintf("[GUI:GPU] rejected listed=%u owner=%u process=%u tid=%u surface=%u size=%ux%u rect=%d,%d,%d,%d\n",
                window ? window->listed : 0U, window ? window->owner_pid : 0U,
                task_current_process_id(), task_current_pid(), surface_handle,
                (uint32_t)surface_width, (uint32_t)surface_height,
                screen_rect.x, screen_rect.y, screen_rect.w, screen_rect.h);
        }
        return false;
    }
    {
        bool layout_changed = !window->gpu_view_visible ||
            window->gpu_view_surface != surface_handle ||
            window->gpu_view_width != surface_width ||
            window->gpu_view_height != surface_height ||
            window->gpu_view_rect.x != screen_rect.x ||
            window->gpu_view_rect.y != screen_rect.y ||
            window->gpu_view_rect.w != screen_rect.w ||
            window->gpu_view_rect.h != screen_rect.h;

        gui_desktop_paint_lock();
        if (!gui_gpu_compositor_publish_viewport(window, surface_handle,
                surface_width, surface_height)) {
            gui_desktop_paint_unlock();
            return false;
        }
        window->gpu_view_surface = surface_handle;
        window->gpu_view_width = surface_width;
        window->gpu_view_height = surface_height;
        window->gpu_view_rect = screen_rect;
        window->gpu_view_visible = true;
        /* Publishing a new frame on an already-attached GPU surface is not a
         * CPU window repaint.  Keep it separate so the compositor can reuse
         * its cached desktop/window layers and only resubmit the GPU scene. */
        window->gpu_view_frame_dirty = true;
        if (layout_changed) {
            kprintf("[GUI:GPU] viewport owner=%u process=%u tid=%u surface=%u\n",
                    window->owner_pid, task_current_process_id(),
                    task_current_pid(), surface_handle);
            window->dirty = true;
            /* BLESKERNOS_SVGA3D_PIPELINE_V6: geometry/attachment changes are
             * urgent. Pure frame publication is intentionally coalesced by
             * gui_desktop_has_dirty() and the GUI FPS limiter; bumping the
             * global paint generation for every game frame can keep
             * urgent_paint asserted forever. */
            gui_request_paint();
        }
        gui_desktop_paint_unlock();
    }
    perfmon_app_frame(window->owner_pid, true);
    perfmon_scope_end(PERF_SCOPE_APP_PUBLISH, perf_started);
    return true;
}

void bk_gui_window_clear_gpu_viewport(gui_window_t *window) {
    if (!window || (window->owner_pid &&
        window->owner_pid != task_current_process_id())) return;
    gui_desktop_paint_lock();
    gui_gpu_compositor_clear_viewport(window);
    window->gpu_view_surface = 0U;
    window->gpu_view_width = 0U;
    window->gpu_view_height = 0U;
    window->gpu_view_rect = (gui_rect_t){0, 0, 0, 0};
    window->gpu_view_visible = false;
    window->gpu_view_frame_dirty = false;
    window->dirty = true;
    gui_request_paint();
    gui_desktop_paint_unlock();
}

bool bk_gui_gpu_viewport_supported(void) {
    const uint32_t required = GFX3D_CAP_RENDER_TARGETS |
        GFX3D_CAP_TEXTURES | GFX3D_CAP_ALPHA_BLEND |
        GFX3D_CAP_WINDOW_SURFACES | GFX3D_CAP_SCALE |
        GFX3D_CAP_PRESENT;
    return gfx3d_available() &&
           (gfx3d_capabilities() & required) == required;
}

void bk_gui_window_set_text_context(
    gui_window_t *window, gui_rect_t bounds, bool has_selection,
    bool editable, gui_menu_callback_t callback, void *context) {
    gui_window_set_text_context(window, bounds, has_selection, editable,
                                callback, context);
}

void bk_gui_window_clear_text_context(gui_window_t *window) {
    gui_window_clear_text_context(window);
}

bool bk_gui_window_begin_immediate_paint(gui_window_t *window,
                                         gui_surface_t **surface) {
    gui_desktop_t *desktop = gui_get_desktop();

    if (surface) *surface = NULL;
    if (!window || !surface || !desktop || !window->listed) return false;

    /* Streaming clients still borrow a staging buffer: a Ring-3 renderer may
     * run on another CPU, so letting it write the live desktop would race the
     * compositor.  end_immediate_paint() skips the extra cache hop instead. */
    return gui_window_begin_content_paint(window, &desktop->surface, surface);
}

void bk_gui_window_end_immediate_paint(gui_window_t *window,
                                       gui_surface_t *surface) {
    gui_desktop_t *desktop = gui_get_desktop();
    bool captured;
    bool direct_surface;

    if (!window || !surface) return;
    direct_surface = window->content_staging_slot == -3;
    if (desktop && window->streaming_content && window->visible &&
        desktop->last_window == window) {
        gui_rect_t content = gui_window_content_rect(window);
        gui_rect_t screen = {0, 0, desktop->surface.width, desktop->surface.height};
        gui_rect_t visible;
        if (gui_rect_intersect(content, screen, &visible)) {
            /* One staging->desktop copy, instead of staging->cache->desktop.
             * Slot -3 is the low-memory path and already points at desktop. */
            if (!direct_surface) {
                for (int y = visible.y; y < visible.y + visible.h; y++) {
                    const uint32_t *src = &surface->pixels[(uint32_t)(y - surface->origin_y) * surface->pitch +
                                                          (uint32_t)(visible.x - surface->origin_x)];
                    uint32_t *dst = &desktop->surface.pixels[(uint32_t)y *
                                    desktop->surface.pitch + (uint32_t)visible.x];
                    kmemcpy(dst, src, (size_t)visible.w * sizeof(uint32_t));
                }
            }
            window->content_ready = false;
            gui_window_end_content_paint(window);
            gui_gfx_present_rect(&desktop->surface, visible);
            window->dirty = false;
            return;
        }
    }
    captured = gui_window_capture_content(window, surface);
    gui_window_end_content_paint(window);
    if (!desktop || !captured) {
        window->dirty = true;
        gui_request_paint();
        return;
    }

    /*
     * No llame gui_desktop_paint() desde aqui. Esta API se usa precisamente
     * mientras una aplicacion Ring 3 sigue dentro de un callback. Reentrar al
     * compositor completo vuelve a recorrer ventanas y a gestionar upcalls de
     * pintura para el mismo proceso; el primer cuadro alcanza el framebuffer,
     * pero la llamada puede no regresar y deja bloqueada la operacion de red.
     *
     * Si la ventana es la superior, basta copiar su cache terminado al surface
     * del escritorio y presentar solamente el cliente. No se ejecutan callbacks
     * ni se toca el resto de la pila de composicion. Si hay otra ventana arriba,
     * se conserva el camino normal para no pintarle por encima.
     */
    if (window->visible && desktop->last_window == window) {
        gui_rect_t content = gui_window_content_rect(window);
        gui_rect_t saved_clip = gui_gfx_get_clip(&desktop->surface);

        gui_gfx_reset_clip(&desktop->surface);
        /* Slot -3 means the application already painted the desktop surface
           directly (the constrained-memory path); there is no cache to copy. */
        if (!direct_surface)
            gui_window_paint_cached_content(&desktop->surface, window, content);
        gui_gfx_set_clip(&desktop->surface, saved_clip);
        gui_gfx_present_rect(&desktop->surface, content);
        window->dirty = false;
        return;
    }

    window->dirty = true;
    gui_desktop_invalidate_rect(desktop, window->bounds);
    gui_request_paint();
}

bool bk_gui_window_set_bounds(gui_window_t *window, gui_rect_t bounds) {
    if(vision_ipc_routes_user())
        return api_vision_window_call(window,VISION_IPC_BOUNDS,bounds,0,0)!=0U;
    gui_desktop_t *desktop;
    gui_rect_t old_bounds;
    if (!window || bounds.w < 64 || bounds.h < 64) return false;
    desktop = window->desktop;
    if (!desktop || !desktop->surface.width || !desktop->surface.height)
        return false;
    if (bounds.w > desktop->surface.width || bounds.h > desktop->surface.height)
        return false;

    /* Preserve the requested size; move the window back on-screen instead of
       silently shrinking a client that asked for an exact render resolution. */
    if (bounds.x < 0) bounds.x = 0;
    if (bounds.y < 0) bounds.y = 0;
    if (bounds.x + bounds.w > desktop->surface.width)
        bounds.x = desktop->surface.width - bounds.w;
    if (bounds.y + bounds.h > desktop->surface.height)
        bounds.y = desktop->surface.height - bounds.h;

    gui_desktop_paint_lock();
    old_bounds = window->bounds;
    window->bounds = bounds;
    window->mode_restore_valid = false;
    window->content_ready = false;
    window->stream_ready = false;
    window->dirty = true;
    gui_desktop_invalidate_rect(desktop, old_bounds);
    gui_desktop_invalidate_rect(desktop, bounds);
    gui_request_paint();
    gui_desktop_paint_unlock();
    return true;
}

void bk_gui_window_set_borderless(gui_window_t *window, bool borderless,
                                  uint8_t drag_height) {
    if(vision_ipc_routes_user()) {
        (void)api_vision_window_call(window,VISION_IPC_BORDERLESS,(gui_rect_t){0},borderless,drag_height);return;
    }
    if (!window) return;
    gui_desktop_paint_lock();
    gui_window_set_borderless(window, borderless, drag_height);
    if (window->desktop) gui_desktop_invalidate_rect(window->desktop, window->bounds);
    gui_request_paint();
    gui_desktop_paint_unlock();
}

bool bk_gui_window_bounds(const gui_window_t *window, gui_rect_t *bounds) {
    if (!window || !bounds) return false;
    *bounds = window->bounds;
    return true;
}

bool bk_gui_window_content_rect(const gui_window_t *window, gui_rect_t *rect) {
    if (!window || !rect) return false;
    *rect = gui_window_content_rect(window);
    return true;
}

void bk_gui_surface_clear(gui_surface_t *surface, uint32_t color) {
    gui_gfx_clear(surface, color);
}

void bk_gui_surface_putpixel(gui_surface_t *surface, int x, int y,
                             uint32_t color) {
    gui_gfx_putpixel(surface, x, y, color);
}

void bk_gui_surface_fill_rect(gui_surface_t *surface, gui_rect_t rect,
                              uint32_t color) {
    gui_gfx_fill_rect(surface, rect, color);
}

void bk_gui_surface_draw_rect(gui_surface_t *surface, gui_rect_t rect,
                              uint32_t color) {
    gui_gfx_draw_rect(surface, rect, color);
}

void bk_gui_surface_draw_line(gui_surface_t *surface, int x0, int y0,
                              int x1, int y1, uint32_t color) {
    gui_gfx_draw_line(surface, x0, y0, x1, y1, color);
}

void bk_gui_surface_draw_polyline(gui_surface_t *surface,
                                  const gui_point_t *points, uint32_t count,
                                  uint32_t color) {
    gui_point_t local_points[64];
    uint32_t bytes;
    if (!surface || !points || count < 2U || count > 64U) return;
    bytes = count * (uint32_t)sizeof(gui_point_t);
    if (task_current_is_user()) {
        if (!copy_from_user(local_points, points, bytes)) return;
    } else {
        kmemcpy(local_points, points, bytes);
    }
    gui_gfx_draw_polyline(surface, local_points, count, color);
}

void bk_gui_surface_draw_text(gui_surface_t *surface, int x, int y,
                              const char *text, uint32_t fg, uint32_t bg,
                              bool fill_bg) {
    gui_font_draw_string(surface, x, y, text, fg, bg, fill_bg);
}

bool bk_gui_image_decode_bmp(gui_image_t *image, const void *data,
                             uint32_t length) {
    return gui_bmp_decode(image, (const uint8_t *)data, length);
}

bool bk_gui_image_decode_png(gui_image_t *image, const void *data,
                             uint32_t length) {
    return gui_png_decode(image, (const uint8_t *)data, length);
}

bool bk_gui_image_decode_gif(gui_image_t *image, const void *data,
                             uint32_t length) {
    return gui_gif_decode(image, (const uint8_t *)data, length);
}

bool bk_gui_image_decode_jpeg(gui_image_t *image, const void *data,
                              uint32_t length) {
    return gui_jpeg_decode(image, (const uint8_t *)data, length);
}

bool bk_gui_image_decode_svg(gui_image_t *image, const void *data,
                             uint32_t length) {
    return gui_svg_decode(image, (const uint8_t *)data, length);
}

void bk_gui_image_free(gui_image_t *image) {
    gui_image_free(image);
}

bool bk_gui_cursor_set_resource(const char *resource_name,
                                int hotspot_x, int hotspot_y) {
    gui_image_t image;
    bool result;
    uint32_t owner_pid = task_current_pid();
    if (!resource_name || !resource_name[0] || !owner_pid) return false;
    if (!bk_graphics_icon_load(resource_name, &image)) return false;
    result = gui_desktop_set_cursor_image(gui_get_desktop(), image.pixels,
                                           image.width, image.height,
                                           hotspot_x, hotspot_y, owner_pid);
    gui_image_free(&image);
    return result;
}

void bk_gui_cursor_reset(void) {
    gui_desktop_reset_cursor(gui_get_desktop(), task_current_pid());
}

void bk_gui_cursor_set_style(uint8_t style) {
    gui_desktop_t *desktop = gui_get_desktop();
    if (!desktop || style >= GUI_CURSOR_COUNT) return;
    /* Un programa que pide espera vuelve desde su cursor de recurso al estilo
       del sistema; al terminar puede llamar reset para recuperar la flecha. */
    gui_desktop_reset_cursor(desktop, task_current_pid());
    gui_desktop_set_cursor_style(desktop, (gui_cursor_style_t)style);
}

#include "gui_raster_api.inc"

uint16_t bk_gui_text_width(const char *text) {
    return gui_font_text_width(text);
}

gui_widget_t *bk_gui_create_button(gui_desktop_t *desktop,
                                   gui_window_t *window, gui_rect_t bounds,
                                   const char *text,
                                   gui_widget_callback_t callback) {
    desktop = api_resolve_desktop(desktop);
    return gui_widget_create_button(desktop, window, bounds, text, callback);
}

gui_widget_t *bk_gui_create_slider(gui_desktop_t *desktop, gui_window_t *window,
    gui_rect_t bounds, int32_t minimum, int32_t maximum, int32_t step,
    int32_t value, gui_widget_callback_t callback) {
    if (bounds.w < 32 || bounds.h < 18 || minimum < -1000000 || maximum > 1000000 || maximum <= minimum || step <= 0 || step > maximum - minimum) return NULL;
    gui_widget_t *w = gui_widget_create_button(api_resolve_desktop(desktop), window, bounds, "", callback);
    if (w) {
        w->style = GUI_WIDGET_STYLE_SLIDER;
        w->range_min = minimum; w->range_max = maximum; w->range_step = step;
        bk_gui_slider_set_value(w, value);
    }
    return w;
}
gui_widget_t *bk_gui_create_choice(gui_desktop_t *desktop, gui_window_t *window,
    gui_rect_t bounds, const char *text, bool radio, gui_widget_callback_t callback) {
    gui_widget_t *w = gui_widget_create_button(api_resolve_desktop(desktop), window, bounds, text, callback);
    if (w) w->style = radio ? GUI_WIDGET_STYLE_RADIO : GUI_WIDGET_STYLE_CHECKBOX;
    return w;
}

gui_widget_t *bk_gui_create_textbox(gui_desktop_t *desktop,
                                    gui_window_t *window, gui_rect_t bounds,
                                    const char *text, uint16_t max_length,
                                    gui_widget_callback_t callback) {
    desktop = api_resolve_desktop(desktop);
    return gui_widget_create_textbox(desktop, window, bounds, text,
                                     max_length, callback);
}

uint32_t bk_gui_widget_id(const gui_widget_t *widget) {
    return widget ? widget->id : 0U;
}

void bk_gui_widget_set_bounds(gui_window_t *window, gui_widget_t *widget,
                              gui_rect_t bounds) {
    gui_widget_set_bounds(window, widget, bounds);
}

void bk_gui_widget_set_text(gui_widget_t *widget, const char *text) {
    gui_widget_set_text(widget, text);
}

bool bk_gui_widget_set_icon(gui_widget_t *widget, const char *resource_name) {
    gui_image_t image;
    if (!widget) return false;
    if (!resource_name || !resource_name[0]) {
        gui_widget_take_icon(widget, NULL, 0U, 0U);
        return true;
    }
    if (!bk_graphics_icon_load_size(resource_name, 16U, &image)) {
        gui_widget_take_icon(widget, NULL, 0U, 0U);
        return false;
    }
    gui_widget_take_icon(widget, image.pixels, image.width, image.height);
    return true;
}

bool bk_gui_widget_get_text(const gui_widget_t *widget, char *buffer,
                            uint32_t capacity) {
    return gui_widget_get_text(widget, buffer, capacity);
}

void bk_gui_widget_set_enabled(gui_widget_t *widget, bool enabled) {
    gui_widget_set_enabled(widget, enabled);
}

void bk_gui_widget_set_visible(gui_window_t *window, gui_widget_t *widget,
                               bool visible) {
    gui_widget_set_visible(window, widget, visible);
}

void bk_gui_widget_set_focus(gui_window_t *window, gui_widget_t *widget,
                             bool focused) {
    gui_widget_set_focus(window, widget, focused);
}

bool bk_gui_widget_is_focused(const gui_window_t *window,
                              const gui_widget_t *widget) {
    return gui_widget_is_focused(window, widget);
}

void bk_gui_scrollbar_init_vertical(gui_scrollbar_t *bar,
                                     gui_rect_t bounds, uint32_t value,
                                     uint32_t visible, uint32_t total) {
    gui_scrollbar_init_vertical(bar, bounds, value, visible, total);
}

void bk_gui_scrollbar_paint_vertical(gui_surface_t *surface,
                                      const gui_scrollbar_t *bar) {
    gui_scrollbar_paint_vertical(surface, bar);
}

bool bk_gui_scrollbar_handle_event_vertical(
    const gui_scrollbar_t *bar, gui_scrollbar_drag_t *drag,
    const gui_event_t *event, uint32_t wheel_step, uint32_t *new_value) {
    return gui_scrollbar_handle_event_vertical(bar, drag, event,
                                                wheel_step, new_value);
}

bool bk_gfx_info(gfx_info_t *info) {
    const gfx_info_t *current;
    if (!info) return false;
    current = gfx_get_info();
    if (!current) return false;
    *info = *current;
    return true;
}

bool bk_gfx_set_mode(uint16_t width, uint16_t height, uint8_t bpp) {
    return gfx_set_display_mode(width, height, bpp);
}

void bk_gfx_clear(uint32_t rgb) {
    gfx_clear_rgb(rgb);
}

void bk_gfx_putpixel(int x, int y, uint32_t rgb) {
    gfx_putpixel_rgb(x, y, rgb);
    (void)gfx_flush();
}

uint32_t bk_gfx_getpixel(int x, int y) {
    return gfx_getpixel_rgb(x, y);
}

void bk_gfx_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    gfx_fill_rect_rgb(x, y, w, h, rgb);
}

bool bk_gfx_present_rect(int x, int y, int w, int h) {
    return gfx_present_rect(x, y, w, h);
}

bool bk_gfx_flush(void) {
    return gfx_flush();
}

bool bk_gfx_copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
    return gfx_copy_rect(src_x, src_y, dst_x, dst_y, w, h);
}

void bk_gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t rgb) {
    uint8_t color = (uint8_t)(rgb & 0xFFU);
    if (gfx_get_info() && gfx_is_linear_framebuffer()) {
        int dx = x1 - x0;
        int dy = y1 - y0;
        int sx = dx < 0 ? -1 : 1;
        int sy = dy < 0 ? -1 : 1;
        int err;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        err = dx - dy;
        for (;;) {
            gfx_putpixel_rgb(x0, y0, rgb);
            if (x0 == x1 && y0 == y1) break;
            int e2 = err * 2;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
        (void)gfx_flush();
        return;
    }
    gfx_draw_line(x0, y0, x1, y1, color);
}

void bk_gfx_draw_text(int x, int y, const char *text, uint8_t fg,
                      uint8_t bg, bool fill_bg) {
    gfx_draw_string(x, y, text, fg, bg, fill_bg);
}

bool bk_input_mouse(mouse_state_t *state) {
    if (!state) return false;
    mouse_get_state(state);
    return state->present;
}

bool bk_input_key_event(kbd_key_event_t *event) {
    return kbd_next_app_event(event);
}

bool bk_input_key_modifiers(kbd_modifiers_t *modifiers) {
    if (!modifiers) return false;
    kbd_get_modifiers(modifiers);
    return true;
}

void bk_input_mouse_set_position(int32_t x, int32_t y) {
    mouse_set_position(x, y);
}

void bk_input_mouse_set_sensitivity(uint8_t sensitivity) {
    mouse_set_sensitivity(sensitivity);
}

uint8_t bk_input_mouse_get_sensitivity(void) {
    return mouse_get_sensitivity();
}

bool bk_sound_has_sb16(void) {
    return sound_has_sb16();
}

bool bk_sound_pcm_available(void) {
    return sound_pcm_available();
}

bool bk_sound_pcm_busy(void) {
    return sound_pcm_is_busy();
}

const char *bk_sound_pcm_name(void) {
    return api_export_string(sound_pcm_name());
}

bool bk_sound_play_pcm_u8(const uint8_t *samples, uint32_t length,
                          uint16_t sample_rate_hz, uint8_t volume) {
    return sound_play_pcm_u8(samples, length, sample_rate_hz, volume);
}

bool bk_sound_tone(uint32_t frequency_hz, uint32_t duration_ms) {
    return sound_start_tone(frequency_hz, duration_ms);
}

void bk_sound_stop(void) {
    sound_stop();
}

bool bk_system_sounds_enabled(void) {
    return system_sounds_enabled();
}

bool bk_system_sounds_set_enabled(bool enabled) {
    return system_sounds_set_enabled(enabled);
}

uint8_t bk_system_sound_master_volume(void) {
    return system_sounds_master_volume();
}

bool bk_system_sound_set_master_volume(uint8_t percent) {
    return system_sounds_set_master_volume(percent);
}

bool bk_system_sound_event_enabled(bk_system_sound_event_t event) {
    return system_sound_event_enabled((system_sound_event_t)event);
}

bool bk_system_sound_event_set_enabled(bk_system_sound_event_t event,
                                       bool enabled) {
    return system_sound_event_set_enabled((system_sound_event_t)event, enabled);
}

bool bk_system_sound_event_available(bk_system_sound_event_t event) {
    return system_sound_event_available((system_sound_event_t)event);
}

bool bk_system_sound_event_play(bk_system_sound_event_t event) {
    return system_sound_event_play((system_sound_event_t)event);
}

void bk_system_sound_resize_begin(void) {
    system_sound_resize_begin();
}

void bk_system_sound_resize_end(void) {
    system_sound_resize_end();
}

bool bk_time_datetime(rtc_datetime_t *datetime) {
    return rtc_get_datetime(datetime);
}

bool bk_datetime_runtime_preferences_get(bk_datetime_preferences_t *prefs) {
    if (!prefs || !g_datetime_runtime_preferences_valid) return false;
    *prefs = g_datetime_runtime_preferences;
    return true;
}

void bk_datetime_runtime_preferences_set(
    const bk_datetime_preferences_t *prefs) {
    if (!prefs) return;
    g_datetime_runtime_preferences = *prefs;
    g_datetime_runtime_preferences_valid = true;
}

uint32_t bk_proc_count(void) {
    return task_count();
}

const task_t *bk_proc_get(uint32_t index) {
    return task_user_export_snapshot(task_get(index));
}

bool bk_proc_info(uint32_t index, bk_proc_info_t *info) {
    const task_t *task;

    if (!info) return false;
    task = task_get(index);
    if (!task) return false;
    info->pid = task->pid;
    info->process_id = task->process_id;
    kstrncpy(info->name, task->name, sizeof(info->name));
    info->name[sizeof(info->name) - 1U] = '\0';
    info->state = (bk_proc_state_t)task->state;
    info->cpu_ticks = task->cpu_ticks;
    info->memory_bytes = task_process_memory_bytes(
        task->process_id ? task->process_id : task->pid);
    info->system = task->system;
    info->user = task->user;
    info->exit_requested = task->exit_requested;
    return true;
}

uint32_t bk_proc_snapshot(bk_proc_info_t *infos, uint32_t capacity) {
    task_public_snapshot_t tasks_snapshot[TASK_MAX];
    uint32_t process_ids[TASK_MAX];
    size_t heap_usage[TASK_MAX];
    uint64_t private_usage[TASK_MAX];
    uint32_t process_count = 0U;
    uint32_t count;

    if (!infos || !capacity) return 0U;
    if (capacity > TASK_MAX) capacity = TASK_MAX;
    count = task_snapshot_public(tasks_snapshot, capacity);

    for (uint32_t i = 0U; i < count; i++) {
        uint32_t process_id = tasks_snapshot[i].process_id
            ? tasks_snapshot[i].process_id : tasks_snapshot[i].pid;
        uint32_t process_index = process_count;
        for (uint32_t j = 0U; j < process_count; j++) {
            if (process_ids[j] == process_id) {
                process_index = j;
                break;
            }
        }
        if (process_index == process_count) {
            process_ids[process_count] = process_id;
            private_usage[process_count] = 0U;
            heap_usage[process_count] = 0U;
            process_count++;
        }
        private_usage[process_index] += tasks_snapshot[i].stack_bytes;
        private_usage[process_index] += tasks_snapshot[i].memory_hint_bytes;
    }
    mm_get_process_usage_batch(process_ids, heap_usage, process_count);

    for (uint32_t i = 0U; i < count; i++) {
        uint32_t process_id = tasks_snapshot[i].process_id
            ? tasks_snapshot[i].process_id : tasks_snapshot[i].pid;
        uint64_t bytes = 0U;
        for (uint32_t j = 0U; j < process_count; j++) {
            if (process_ids[j] != process_id) continue;
            bytes = heap_usage[j] > private_usage[j]
                ? (uint64_t)heap_usage[j] : private_usage[j];
            break;
        }
        infos[i].pid = tasks_snapshot[i].pid;
        infos[i].process_id = tasks_snapshot[i].process_id;
        kstrncpy(infos[i].name, tasks_snapshot[i].name,
                 sizeof(infos[i].name));
        infos[i].name[sizeof(infos[i].name) - 1U] = '\0';
        infos[i].state = (bk_proc_state_t)tasks_snapshot[i].state;
        infos[i].cpu_ticks = tasks_snapshot[i].cpu_ticks;
        infos[i].memory_bytes = bytes > 0xFFFFFFFFULL
            ? 0xFFFFFFFFU : (uint32_t)bytes;
        infos[i].system = tasks_snapshot[i].system;
        infos[i].user = tasks_snapshot[i].user;
        infos[i].exit_requested = tasks_snapshot[i].exit_requested;
    }
    return count;
}

bool bk_proc_request_exit(uint32_t pid) {
    return task_request_exit(pid);
}

bool bk_proc_exit_requested(void) {
    return task_exit_requested();
}

void bk_proc_set_memory_hint(uint32_t bytes) {
    task_set_memory_hint(bytes);
}

void bk_proc_bind_window(gui_window_t *window) {
    task_bind_window(window);
}

const char *bk_proc_launch_arg(void) {
    const char *argument = task_launch_arg();
    return api_export_string(argument ? argument : "");
}

int bk_proc_spawn_thread(const char *name, bk_thread_entry_t entry,
                         void *argument) {
    int tid;
    if (!entry) return -1;
    if (!task_current_is_user())
        return task_create_kernel(name ? name : "kernel-worker",
                                  (task_entry_t)entry, argument);
    tid = task_create_user_thread(name ? name : "app-thread", entry,
                                  argument, task_current_process_id());
    if (tid < 0) {
        kprintf("[THREAD] no se pudo crear worker proceso=%u entry=%x nombre=%s\n",
                task_current_process_id(), (uint32_t)(uintptr_t)entry,
                name ? name : "app-thread");
    }
    return tid;
}

void bk_proc_exit(void) {
    task_exit();
}

void bk_proc_exit_status(int status) {
    task_exit_status(status);
}

bool bk_app_launch(const char *path, const char *argument) {
    gui_desktop_t *desktop = gui_get_desktop();
    if (!desktop || !path) return false;
    return program_execute_path_arg(desktop, path, argument);
}

int bk_app_run_sync(const char *path, const char *command_line) {
    gui_desktop_t *desktop = gui_get_desktop();
    uint32_t parent_pid = task_current_process_id();
    uint32_t pid = 0U;
    int child;
    int32_t status = 0;
    int32_t waited;

    if (!desktop || !path || !path[0]) return -1;
    if (program_is_win32_executable(path)) {
        char wine_line[TASK_LAUNCH_ARG_LEN];
        if (command_line && command_line[0]) {
            if (kstrlen(command_line) >= sizeof(wine_line)) return -1;
            kstrncpy(wine_line, command_line, sizeof(wine_line) - 1U);
        } else {
            if (kstrlen(path) + 3U > sizeof(wine_line)) return -1;
            snprintf(wine_line, sizeof(wine_line), "\"%s\"", path);
        }
        wine_line[sizeof(wine_line) - 1U] = '\0';
        child = elf_spawn_program_ex("/WINEGATE/bin/wine-tool.ELF",
                                     desktop, wine_line);
        if (child < 0) return -1;
        pid = (uint32_t)child;
    } else if (program_is_bex_executable(path) || program_is_object(path)) {
        child = elf_spawn_program_ex(path, desktop, command_line);
        if (child < 0) return -1;
        pid = (uint32_t)child;
    } else {
        return -1;
    }

    /* Preserve redirected stdin/stdout/stderr and any other open descriptors.
     * This matters for native developer tools: an IDE can redirect its own
     * fd 1/2 to BUILD.LOG, run TCC/GNU tools synchronously and receive the
     * child's diagnostics instead of losing them on the global console. */
    syscall_inherit_files(parent_pid, pid);

    do {
        waited = task_waitpid(pid, &status);
        if (waited == 0) task_yield();
    } while (waited == 0);
    if (waited <= 0) return -1;
    if (program_is_win32_executable(path))
        return (status & 0x7f) ? 128 + (status & 0x7f) : (status >> 8) & 0xff;
    return (int)status;
}

bool bk_shell_take_exit_request(void) {
    return shell_take_exit_request();
}

bool bk_vm_statistics(uint32_t process_id,bk_vm_statistics_t *out) {
    if(!out)return false;
    pager_stats_t p;swap_stats_t s;
    pager_stats(process_id,&p);swap_stats(&s);
    *out=(bk_vm_statistics_t){p.virtual_pages,p.resident_pages,p.swapped_pages,
        p.faults,p.evictions,p.failures,s.slots,s.used,s.writes,s.reads,s.io_errors,s.enabled};
    return true;
}
bool bk_vm_swap_enable(bool enabled){return enabled?swap_enable(true):pager_swapoff();}

uint32_t bk_memory_subsystems(bk_memory_subsystem_t *out,uint32_t capacity) {
    if(!out || capacity<MM_SUBSYSTEM_COUNT)return 0;
    mm_subsystem_usage_t usage[MM_SUBSYSTEM_COUNT];mm_get_subsystem_usage(usage);
    for(uint32_t i=0;i<MM_SUBSYSTEM_COUNT;i++) {
        out[i].bytes=usage[i].bytes;out[i].blocks=usage[i].blocks;
    }
    return MM_SUBSYSTEM_COUNT;
}
