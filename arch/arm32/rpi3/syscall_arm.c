#include "syscall_arm.h"
#include "elf_arm.h"
#include "timer.h"
#include "uart.h"
#include "../../../kernel/include/vga.h"
#include "../../../kernel/include/syscall.h"
#include "../../../kernel/include/task.h"
#include "../../../kernel/include/vfs.h"
#include "../../../kernel/include/memory.h"
#include "../../../kernel/include/api.h"
#include "../../../kernel/include/block.h"
#include "../../../kernel/include/fat.h"
#include "../../../kernel/include/rtc.h"
#include "../../../kernel/include/sound.h"
#include "../../../kernel/include/network.h"
#include "user_api_arm.h"

#define ARM_USER_STRING_MAX VFS_MAX_PATH

typedef struct arm_net_socket {
    uint32_t owner;
    uint32_t handle;
    int raw;
} arm_net_socket_t;
static arm_net_socket_t g_net_sockets[NET_SOCKET_MAX * 2U];
static uint32_t g_next_net_handle = 1U;

static arm_net_socket_t *net_socket_by_handle(uint32_t handle) {
    uint32_t i, owner = task_current_process_id();
    for (i = 0U; i < NET_SOCKET_MAX * 2U; ++i)
        if (g_net_sockets[i].owner == owner &&
            g_net_sockets[i].handle == handle) return &g_net_sockets[i];
    return NULL;
}

void rpi3_syscall_cleanup_process(uint32_t process_id) {
    uint32_t i;
    for (i = 0U; i < NET_SOCKET_MAX * 2U; ++i) {
        if (g_net_sockets[i].owner != process_id) continue;
        network_socket_close(g_net_sockets[i].raw);
        kmemset(&g_net_sockets[i], 0, sizeof(g_net_sockets[i]));
    }
}

static bool user_range(uint32_t address, uint32_t bytes, bool write) {
    return rpi3_task_current_user_range_ok(address, bytes, write);
}

static bool user_string(uint32_t address, char *out, uint32_t capacity) {
    uint32_t i;
    const char *source = (const char *)(uintptr_t)address;
    if (!out || capacity < 2U || !address) return false;
    for (i = 0U; i < capacity; ++i) {
        if (!user_range(address + i, 1U, false)) return false;
        out[i] = source[i];
        if (!out[i]) return true;
    }
    out[capacity - 1U] = '\0';
    return false;
}

static int32_t sys_write_user(uint32_t fd, uint32_t address, uint32_t bytes) {
    const char *text;
    uint32_t i;
    if (fd != 1U && fd != 2U) return -BK_EBADF;
    if (!bytes) return 0;
    if (!user_range(address, bytes, false)) return -BK_EFAULT;
    text = (const char *)(uintptr_t)address;
    for (i = 0U; i < bytes; ++i) vga_putchar(text[i]);
    return (int32_t)bytes;
}

static int32_t sys_open_user(uint32_t address, uint32_t flags) {
    char path[ARM_USER_STRING_MAX];
    if (!user_string(address, path, sizeof(path))) return -BK_EFAULT;
    return vfs_open(path, flags);
}

static int32_t sys_read_user(uint32_t fd, uint32_t address, uint32_t bytes) {
    if (!bytes) return 0;
    if (!user_range(address, bytes, true)) return -BK_EFAULT;
    return vfs_read((int)fd, (void *)(uintptr_t)address, bytes);
}

static int32_t sys_getcwd_user(uint32_t address, uint32_t capacity) {
    const char *cwd = task_current_cwd();
    uint32_t length = (uint32_t)kstrlen(cwd) + 1U;
    if (capacity < length || !user_range(address, length, true)) return -BK_EFAULT;
    kmemcpy((void *)(uintptr_t)address, cwd, length);
    return (int32_t)(length - 1U);
}

static int32_t sys_path1(uint32_t number, uint32_t address) {
    char path[ARM_USER_STRING_MAX];
    if (!user_string(address, path, sizeof(path))) return -BK_EFAULT;
    if (number == SYS_CHDIR) return vfs_chdir(path) ? 0 : -BK_ENOENT;
    if (number == SYS_MKDIR) return vfs_mkdir(path) ? 0 : -BK_EIO;
    if (number == SYS_UNLINK) return vfs_remove(path) ? 0 : -BK_ENOENT;
    return -BK_ENOSYS;
}

static int32_t sys_rename_user(uint32_t old_address, uint32_t new_address) {
    char old_path[ARM_USER_STRING_MAX];
    char new_path[ARM_USER_STRING_MAX];
    if (!user_string(old_address, old_path, sizeof(old_path)) ||
        !user_string(new_address, new_path, sizeof(new_path))) return -BK_EFAULT;
    return vfs_rename(old_path, new_path) ? 0 : -BK_EIO;
}

static int32_t sys_getdents_user(uint32_t path_address, uint32_t entries_address,
                                 uint32_t capacity, uint32_t count_address) {
    char path[ARM_USER_STRING_MAX];
    uint32_t count = 0U;
    if (capacity > VFS_MAX_DIR_ENTRIES) return -BK_EINVAL;
    if (!user_string(path_address, path, sizeof(path))) return -BK_EFAULT;
    if (!user_range(entries_address, capacity * (uint32_t)sizeof(vfs_dir_entry_t), true) ||
        !user_range(count_address, sizeof(uint32_t), true)) return -BK_EFAULT;
    if (!vfs_listdir(path, (vfs_dir_entry_t *)(uintptr_t)entries_address,
                     capacity, &count)) return -BK_EIO;
    *(uint32_t *)(uintptr_t)count_address = count;
    return 0;
}

static int32_t sys_file_write_user(uint32_t fd, uint32_t address, uint32_t bytes) {
    if (!bytes) return 0;
    if (!user_range(address, bytes, false)) return -BK_EFAULT;
    return vfs_write((int)fd, (const void *)(uintptr_t)address, bytes);
}

static int32_t sys_file_stat_user(uint32_t path_address, uint32_t entry_address) {
    char path[ARM_USER_STRING_MAX];
    if (!user_string(path_address, path, sizeof(path))) return -BK_EFAULT;
    if (!user_range(entry_address, sizeof(vfs_dir_entry_t), true)) return -BK_EFAULT;
    return vfs_stat(path, (vfs_dir_entry_t *)(uintptr_t)entry_address) ? 0 : -BK_ENOENT;
}

static int32_t sys_launch_arg_copy_user(uint32_t address, uint32_t capacity) {
    if (!address || !capacity || capacity > 2048U) return -BK_EINVAL;
    if (!user_range(address, capacity, true)) return -BK_EFAULT;
    return rpi3_task_current_launch_arg_copy((char *)(uintptr_t)address, capacity)
        ? 0 : -BK_EINVAL;
}

static int32_t sys_spawn_user(uint32_t path_address, uint32_t arg_address) {
    char path[ARM_USER_STRING_MAX];
    char argument[ARM_USER_STRING_MAX];
    const char *arg = NULL;
    if (!user_string(path_address, path, sizeof(path))) return -BK_EFAULT;
    if (arg_address) {
        if (!user_string(arg_address, argument, sizeof(argument))) return -BK_EFAULT;
        arg = argument;
    }
    return rpi3_elf_spawn(path, arg);
}

/* DTOs not yet shared by kernel/include/api.h.  Their layout is the stable
 * public SDK layout; only copies cross the privilege boundary. */
typedef struct {
    bool mounted; char mount_name[16]; char device_name[16]; char filesystem[9];
    char volume_label[12]; uint8_t fat_bits; uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster; uint32_t total_sectors;
    uint32_t total_clusters; uint64_t total_bytes; uint64_t free_bytes;
    bool read_only;
} arm_user_volume_info_t;

typedef struct {
    gui_rect_t destination;
    gui_rect_t clip;
    uint32_t pixels;
    uint16_t width;
    uint16_t height;
} arm_user_image_draw_t;

static gui_window_t *arm_gui_window(uint32_t handle) {
    gui_desktop_t *desktop = gui_get_desktop();
    gui_window_t *window;
    uint32_t owner = task_current_process_id();
    if (!desktop || !handle) return NULL;
    for (window = desktop->first_window; window; window = window->next)
        if ((uint32_t)(uintptr_t)window == handle &&
            window->destroy_state == 0U && window->owner_pid == owner)
            return window;
    return NULL;
}

static gui_widget_t *arm_gui_widget(uint32_t handle) {
    gui_desktop_t *desktop = gui_get_desktop();
    gui_window_t *window;
    if (!desktop || !handle) return NULL;
    for (window = desktop->first_window; window; window = window->next) {
        gui_widget_t *widget;
        if (window->owner_pid != task_current_process_id()) continue;
        for (widget = window->widgets; widget; widget = widget->next)
            if ((uint32_t)(uintptr_t)widget == handle) return widget;
    }
    return NULL;
}

static gui_surface_t *arm_gui_surface(uint32_t handle) {
    if (!handle || !rpi3_task_current_upcall_argument(1U, handle)) return NULL;
    return (gui_surface_t *)(uintptr_t)handle;
}

static int32_t sys_arm_gui(uint32_t request_address) {
    rpi3_gui_request_t request;
    gui_window_t *window;
    gui_surface_t *surface;
    gui_rect_t rect;
    char text[128];
    if (!user_range(request_address, sizeof(request), true)) return -BK_EFAULT;
    kmemcpy(&request, (const void *)(uintptr_t)request_address, sizeof(request));
    rect = (gui_rect_t){(int32_t)request.value[1], (int32_t)request.value[2],
                       (int32_t)request.value[3], (int32_t)request.value[4]};
    switch (request.operation) {
        case RPI3_GUI_DESKTOP:
            return (int32_t)(uintptr_t)gui_get_desktop();
        case RPI3_GUI_CREATE_WINDOW:
            if (!user_string(request.value[5], text, sizeof(text))) return -BK_EFAULT;
            window = gui_desktop_create_window(gui_get_desktop(),
                    (int32_t)request.value[0],
                    (int32_t)request.value[1], (int32_t)request.value[2],
                    (int32_t)request.value[3], text);
            if (window) window->owner_pid = task_current_process_id();
            return window ? (int32_t)(uintptr_t)window : -BK_ENOMEM;
        case RPI3_GUI_SET_CONTENT:
        case RPI3_GUI_SET_EVENT:
            window = arm_gui_window(request.value[0]);
            if (!window || !request.value[1] ||
                !user_range(request.value[1], 4U, false) ||
                (request.value[2] && !user_range(request.value[2], 1U, true)))
                return -BK_EACCES;
            if (request.operation == RPI3_GUI_SET_CONTENT)
                bk_gui_set_window_content(window,
                    (gui_window_content_paint_t)(uintptr_t)request.value[1],
                    (void *)(uintptr_t)request.value[2]);
            else
                bk_gui_set_window_event_handler(window,
                    (gui_window_event_t)(uintptr_t)request.value[1],
                    (void *)(uintptr_t)request.value[2]);
            return 0;
        case RPI3_GUI_SET_MIN_SIZE:
            window = arm_gui_window(request.value[0]);
            if (!window) return -BK_EACCES;
            bk_gui_set_window_min_size(window, (int32_t)request.value[1],
                                       (int32_t)request.value[2]);
            return 0;
        case RPI3_GUI_DESTROY_WINDOW:
            window = arm_gui_window(request.value[0]);
            if (!window) return -BK_EACCES;
            gui_desktop_remove_window(gui_get_desktop(), window);
            gui_window_destroy(window);
            return 0;
        case RPI3_GUI_WINDOW_OPEN:
            return arm_gui_window(request.value[0]) ? 1 : 0;
        case RPI3_GUI_WINDOW_INVALIDATE:
            window = arm_gui_window(request.value[0]);
            if (!window) return -BK_EACCES;
            bk_gui_window_invalidate(window);
            return 0;
        case RPI3_GUI_WINDOW_BOUNDS:
        case RPI3_GUI_CONTENT_RECT:
            window = arm_gui_window(request.value[0]);
            if (!window) return -BK_EACCES;
            if (request.operation == RPI3_GUI_WINDOW_BOUNDS)
                rect = window->bounds;
            else rect = gui_window_content_rect(window);
            request.value[1] = (uint32_t)rect.x; request.value[2] = (uint32_t)rect.y;
            request.value[3] = (uint32_t)rect.w; request.value[4] = (uint32_t)rect.h;
            kmemcpy((void *)(uintptr_t)request_address, &request, sizeof(request));
            return 0;
        case RPI3_GUI_FILL_RECT:
        case RPI3_GUI_DRAW_RECT:
            surface = arm_gui_surface(request.value[0]);
            if (!surface) return -BK_EACCES;
            if (request.operation == RPI3_GUI_FILL_RECT)
                gui_gfx_fill_rect(surface, rect, request.value[5]);
            else gui_gfx_draw_rect(surface, rect, request.value[5]);
            return 0;
        case RPI3_GUI_DRAW_TEXT:
            surface = arm_gui_surface(request.value[0]);
            if (!surface || !user_string(request.value[3], text, sizeof(text)))
                return -BK_EFAULT;
            gui_font_draw_string(surface, (int32_t)request.value[1],
                (int32_t)request.value[2], text, request.value[4],
                request.value[5], request.value[6] != 0U);
            return 0;
        case RPI3_GUI_TEXT_WIDTH:
            if (!user_string(request.value[0], text, sizeof(text))) return -BK_EFAULT;
            return (int32_t)gui_font_text_width(text);
        case RPI3_GUI_CREATE_BUTTON: {
            gui_widget_t *widget;
            window = arm_gui_window(request.value[0]);
            if (!window || !user_string(request.value[5], text, sizeof(text)) ||
                !user_range(request.value[6], 4U, false)) return -BK_EACCES;
            widget = gui_widget_create_button(gui_get_desktop(), window, rect, text,
                    (gui_widget_callback_t)(uintptr_t)request.value[6]);
            return widget ? (int32_t)(uintptr_t)widget : -BK_ENOMEM;
        }
        case RPI3_GUI_WIDGET_ID: {
            gui_widget_t *widget = arm_gui_widget(request.value[0]);
            return widget ? (int32_t)widget->id : -BK_EACCES;
        }
        case RPI3_GUI_REQUEST_PAINT:
            bk_gui_request_paint();
            return 0;
        case RPI3_GUI_DRAW_LINE:
            surface = arm_gui_surface(request.value[0]);
            if (!surface) return -BK_EACCES;
            gui_gfx_draw_line(surface, (int32_t)request.value[1],
                              (int32_t)request.value[2],
                              (int32_t)request.value[3],
                              (int32_t)request.value[4], request.value[5]);
            return 0;
        case RPI3_GUI_CREATE_TEXTBOX: {
            gui_widget_t *widget;
            window = arm_gui_window(request.value[0]);
            if (!window || !user_string(request.value[5], text, sizeof(text)) ||
                (request.value[7] && !user_range(request.value[7], 4U, false)))
                return -BK_EACCES;
            widget = gui_widget_create_textbox(gui_get_desktop(), window, rect,
                    text, (uint16_t)request.value[6],
                    (gui_widget_callback_t)(uintptr_t)request.value[7]);
            return widget ? (int32_t)(uintptr_t)widget : -BK_ENOMEM;
        }
        case RPI3_GUI_WIDGET_SET_TEXT: {
            gui_widget_t *widget = arm_gui_widget(request.value[0]);
            if (!widget || !user_string(request.value[1], text, sizeof(text)))
                return -BK_EFAULT;
            gui_widget_set_text(widget, text);
            return 0;
        }
        case RPI3_GUI_WIDGET_GET_TEXT: {
            gui_widget_t *widget = arm_gui_widget(request.value[0]);
            uint32_t capacity = request.value[2];
            if (!widget || !capacity || capacity > sizeof(text) ||
                !user_range(request.value[1], capacity, true)) return -BK_EFAULT;
            if (!gui_widget_get_text(widget, text, capacity)) return -BK_EIO;
            kmemcpy((void *)(uintptr_t)request.value[1], text, capacity);
            return 0;
        }
        case RPI3_GUI_WIDGET_SET_BOUNDS: {
            gui_widget_t *widget = arm_gui_widget(request.value[1]);
            window = arm_gui_window(request.value[0]);
            if (!window || !widget) return -BK_EACCES;
            rect = (gui_rect_t){(int32_t)request.value[2],
                    (int32_t)request.value[3], (int32_t)request.value[4],
                    (int32_t)request.value[5]};
            gui_widget_set_bounds(window, widget, rect);
            return 0;
        }
        case RPI3_GUI_WIDGET_SET_ENABLED: {
            gui_widget_t *widget = arm_gui_widget(request.value[0]);
            if (!widget) return -BK_EACCES;
            gui_widget_set_enabled(widget, request.value[1] != 0U);
            return 0;
        }
        case RPI3_GUI_WIDGET_SET_VISIBLE:
        case RPI3_GUI_WIDGET_SET_FOCUS: {
            gui_widget_t *widget = arm_gui_widget(request.value[1]);
            window = arm_gui_window(request.value[0]);
            if (!window || !widget) return -BK_EACCES;
            if (request.operation == RPI3_GUI_WIDGET_SET_VISIBLE)
                gui_widget_set_visible(window, widget, request.value[2] != 0U);
            else gui_widget_set_focus(window, widget, request.value[2] != 0U);
            return 0;
        }
        case RPI3_GUI_WIDGET_IS_FOCUSED: {
            gui_widget_t *widget = arm_gui_widget(request.value[1]);
            window = arm_gui_window(request.value[0]);
            if (!window || !widget) return -BK_EACCES;
            return gui_widget_is_focused(window, widget) ? 1 : 0;
        }
        case RPI3_GUI_DRAW_IMAGE: {
            arm_user_image_draw_t draw;
            gui_image_t image;
            uint32_t bytes;
            surface = arm_gui_surface(request.value[0]);
            if (!surface || !user_range(request.value[1], sizeof(draw), false))
                return -BK_EFAULT;
            kmemcpy(&draw, (const void *)(uintptr_t)request.value[1], sizeof(draw));
            if (!draw.width || !draw.height || draw.width > 2048U ||
                draw.height > 2048U) return -BK_EINVAL;
            bytes = (uint32_t)draw.width * (uint32_t)draw.height * 4U;
            if (!user_range(draw.pixels, bytes, false)) return -BK_EFAULT;
            image.pixels = (uint32_t *)(uintptr_t)draw.pixels;
            image.width = draw.width; image.height = draw.height;
            bk_gui_surface_draw_image(surface, draw.destination, draw.clip, &image);
            return 0;
        }
        default:
            return -BK_ENOSYS;
    }
}

static int32_t sys_arm_api(uint32_t operation, uint32_t a, uint32_t b,
                           uint32_t c) {
    (void)c;
    switch (operation) {
        case RPI3_UAPI_TIME_DATETIME: {
            rtc_datetime_t value;
            if (!user_range(a, sizeof(value), true)) return -BK_EFAULT;
            if (!rtc_get_datetime(&value)) return -BK_EIO;
            kmemcpy((void *)(uintptr_t)a, &value, sizeof(value));
            return 0;
        }
        case RPI3_UAPI_PROC_COUNT:
            return (int32_t)rpi3_task_live_count();
        case RPI3_UAPI_PROC_INFO: {
            bk_proc_info_t infos[32];
            uint32_t count;
            if (!user_range(b, sizeof(bk_proc_info_t), true)) return -BK_EFAULT;
            count = bk_proc_snapshot(infos, 32U);
            if (a >= count) return -BK_ENOENT;
            kmemcpy((void *)(uintptr_t)b, &infos[a], sizeof(infos[a]));
            return 0;
        }
        case RPI3_UAPI_PROC_KILL:
            return bk_proc_request_exit(a) ? 0 : -BK_ENOENT;
        case RPI3_UAPI_PROC_CPU_USAGE:
            return 0;
        case RPI3_UAPI_BLOCK_COUNT:
            return (int32_t)block_count();
        case RPI3_UAPI_BLOCK_INFO: {
            bk_block_info_t value;
            if (!user_range(b, sizeof(value), true)) return -BK_EFAULT;
            if (!bk_device_block_info(a, &value)) return -BK_ENOENT;
            kmemcpy((void *)(uintptr_t)b, &value, sizeof(value));
            return 0;
        }
        case RPI3_UAPI_PARTITION_COUNT:
            /* Pi images currently mount the FAT volume as an SD superfloppy. */
            return 0;
        case RPI3_UAPI_PARTITION_INFO:
            return -BK_ENOENT;
        case RPI3_UAPI_VOLUME_INFO: {
            arm_user_volume_info_t value;
            fat_fs_t fs;
            uint64_t total = 0U, free_bytes = 0U;
            if (!user_range(a, sizeof(value), true)) return -BK_EFAULT;
            if (!fat_get_current(&fs)) return -BK_ENOENT;
            kmemset(&value, 0, sizeof(value));
            value.mounted = true;
            kstrncpy(value.mount_name, fat_get_active_name(), sizeof(value.mount_name) - 1U);
            if (fs.device) kstrncpy(value.device_name, fs.device->name,
                                    sizeof(value.device_name) - 1U);
            kstrncpy(value.filesystem, fs.fs_name, sizeof(value.filesystem) - 1U);
            kstrncpy(value.volume_label, fs.volume_label, sizeof(value.volume_label) - 1U);
            value.fat_bits = (uint8_t)fs.type;
            value.bytes_per_sector = fs.bytes_per_sector;
            value.sectors_per_cluster = fs.sectors_per_cluster;
            value.total_sectors = fs.total_sectors;
            value.total_clusters = fs.total_clusters;
            (void)fat_get_space(&fs, &total, &free_bytes);
            value.total_bytes = total;
            value.free_bytes = free_bytes;
            value.read_only = fs.device ? fs.device->read_only : true;
            kmemcpy((void *)(uintptr_t)a, &value, sizeof(value));
            return 0;
        }
        case RPI3_UAPI_VOLUME_CHECK:
            if (!user_range(a, sizeof(fat_check_report_t), true)) return -BK_EFAULT;
            return fat_check_current((fat_check_report_t *)(uintptr_t)a) ? 0 : -BK_EIO;
        case RPI3_UAPI_VOLUME_REPAIR:
            if (!user_range(a, sizeof(fat_repair_report_t), true) ||
                (b && !user_range(b, sizeof(fat_check_report_t), true))) return -BK_EFAULT;
            return fat_repair_current((fat_repair_report_t *)(uintptr_t)a,
                    b ? (fat_check_report_t *)(uintptr_t)b : NULL) ? 0 : -BK_EIO;
        case RPI3_UAPI_VOLUME_MOUNT: {
            char name[16];
            if (!user_string(a, name, sizeof(name))) return -BK_EFAULT;
            return fat_set_active(name) ? 0 : -BK_ENOENT;
        }
        case RPI3_UAPI_VOLUME_FORMAT: {
            char name[16], label[12];
            if (!user_string(a, name, sizeof(name)) ||
                (b && !user_string(b, label, sizeof(label)))) return -BK_EFAULT;
            return fat_format(name, b ? label : NULL) ? 0 : -BK_EIO;
        }
        case RPI3_UAPI_PCI_COUNT:
            return 0;
        case RPI3_UAPI_PCI_INFO:
            return -BK_ENOENT;
        case RPI3_UAPI_SOUND_TONE:
            return sound_start_tone(a, b) ? 0 : -BK_EIO;
        case RPI3_UAPI_GUI_CALL:
            return sys_arm_gui(a);
        case RPI3_UAPI_SOUND_PCM_AVAILABLE:
            return sound_pcm_available() ? 1 : 0;
        case RPI3_UAPI_SOUND_PCM_BUSY:
            return sound_pcm_is_busy() ? 1 : 0;
        case RPI3_UAPI_SOUND_PCM_PLAY:
            if (!a || !b || b > 16384U || !user_range(a, b, false))
                return -BK_EFAULT;
            return sound_play_pcm_u8((const uint8_t *)(uintptr_t)a, b,
                    (uint16_t)(c & 0xFFFFU),
                    (uint8_t)((c >> 16) & 0xFFU)) ? 0 : -BK_EIO;
        case RPI3_UAPI_SOUND_STOP:
            sound_stop();
            return 0;
        case RPI3_UAPI_NET_INFO: {
            net_info_t value;
            if (!user_range(a, sizeof(value), true)) return -BK_EFAULT;
            network_get_info(&value);
            kmemcpy((void *)(uintptr_t)a, &value, sizeof(value));
            return 0;
        }
        case RPI3_UAPI_NET_DHCP:
            return network_dhcp(a) ? 0 : -BK_EIO;
        case RPI3_UAPI_NET_PING:
            if (!user_range(a, 4U, false) ||
                (c && !user_range(c, sizeof(uint32_t), true))) return -BK_EFAULT;
            return network_ping((const uint8_t *)(uintptr_t)a, b,
                    c ? (uint32_t *)(uintptr_t)c : NULL) ? 0 : -BK_EIO;
        case RPI3_UAPI_LANGUAGE_SET: {
            char code[16];
            if (!user_string(a, code, sizeof(code))) return -BK_EFAULT;
            return bk_lang_set(code) ? 0 : -BK_ENOENT;
        }
        case RPI3_UAPI_MOUSE_GET_SENSITIVITY:
            return (int32_t)bk_input_mouse_get_sensitivity();
        case RPI3_UAPI_MOUSE_SET_SENSITIVITY:
            if (a < 1U || a > 10U) return -BK_EINVAL;
            bk_input_mouse_set_sensitivity((uint8_t)a);
            return 0;
        case RPI3_UAPI_CLIPBOARD_GET:
            if (!b || b > BK_CLIPBOARD_TEXT_CAPACITY ||
                !user_range(a, b, true)) return -BK_EFAULT;
            return (int32_t)bk_clipboard_get_text((char *)(uintptr_t)a, b);
        default:
            return -BK_ENOSYS;
    }
}

arm_task_frame_t *rpi3_syscall_dispatch(arm_task_frame_t *frame) {
    uint32_t number;
    int32_t result = -BK_ENOSYS;
    if (!frame) return frame;
    number = frame->r7;

    switch (number) {
        case SYS_EXIT:
            rpi3_task_svc_exit((int32_t)frame->r0);
            return rpi3_task_svc_schedule(frame);
        case SYS_WRITE:
            result = sys_write_user(frame->r0, frame->r1, frame->r2);
            break;
        case SYS_GETPID:
            result = (int32_t)task_current_process_id();
            break;
        case SYS_GETPPID:
            result = (int32_t)task_current_parent_pid();
            break;
        case SYS_YIELD:
            frame->r0 = 0U;
            return rpi3_task_svc_schedule(frame);
        case SYS_SLEEP:
            rpi3_task_svc_sleep(frame->r0);
            frame->r0 = 0U;
            return rpi3_task_svc_schedule(frame);
        case SYS_UPTIME_MS:
            result = (int32_t)(rpi3_timer_ticks() * (1000U / RPI3_TIMER_HZ));
            break;
        case SYS_ABI_VERSION:
            result = SYSCALL_ABI_VERSION;
            break;
        case SYS_OPEN:
            result = sys_open_user(frame->r0, frame->r1);
            break;
        case SYS_READ:
            result = sys_read_user(frame->r0, frame->r1, frame->r2);
            break;
        case SYS_CLOSE:
            result = vfs_close((int)frame->r0) ? 0 : -BK_EBADF;
            break;
        case SYS_GETCWD:
            result = sys_getcwd_user(frame->r0, frame->r1);
            break;
        case SYS_CHDIR:
        case SYS_MKDIR:
        case SYS_UNLINK:
            result = sys_path1(number, frame->r0);
            break;
        case SYS_RENAME:
            result = sys_rename_user(frame->r0, frame->r1);
            break;
        case SYS_GETDENTS:
            result = sys_getdents_user(frame->r0, frame->r1, frame->r2, frame->r3);
            break;
        case SYS_ALLOC: {
            void *p = rpi3_task_user_alloc(frame->r0);
            result = p ? (int32_t)(uintptr_t)p : -BK_ENOMEM;
            break;
        }
        case SYS_REALLOC: {
            void *p;
            if (frame->r0 && !user_range(frame->r0, 1U, true)) {
                result = -BK_EFAULT;
                break;
            }
            p = rpi3_task_user_realloc((void *)(uintptr_t)frame->r0, frame->r1);
            result = p || frame->r1 == 0U ? (int32_t)(uintptr_t)p : -BK_ENOMEM;
            break;
        }
        case SYS_FREE:
            result = rpi3_task_user_free((void *)(uintptr_t)frame->r0) ? 0 : -BK_EACCES;
            break;
        case SYS_SPAWN:
            result = sys_spawn_user(frame->r0, frame->r1);
            break;
        case SYS_WAITPID: {
            int32_t status = 0;
            result = task_waitpid(frame->r0, &status);
            if (result > 0 && frame->r1) {
                if (!user_range(frame->r1, sizeof(status), true)) result = -BK_EFAULT;
                else *(int32_t *)(uintptr_t)frame->r1 = status;
            }
            break;
        }
        case SYS_KILL:
            result = task_request_exit(frame->r0) ? 0 : -BK_EINVAL;
            break;
        case SYS_FILE_WRITE:
            result = sys_file_write_user(frame->r0, frame->r1, frame->r2);
            break;
        case SYS_FILE_SEEK:
            result = vfs_seek((int)frame->r0, (int32_t)frame->r1, frame->r2);
            break;
        case SYS_FILE_TELL:
            result = vfs_tell((int)frame->r0);
            break;
        case SYS_FILE_SIZE:
            result = vfs_size((int)frame->r0);
            break;
        case SYS_FILE_STAT:
            result = sys_file_stat_user(frame->r0, frame->r1);
            break;
        case SYS_LAUNCH_ARG_COPY:
            result = sys_launch_arg_copy_user(frame->r0, frame->r1);
            break;
        case SYS_TICKS:
            result = (int32_t)rpi3_timer_ticks();
            break;
        case SYS_THREAD_SELF:
            result = (int32_t)task_current_tid();
            break;
        case SYS_NET_RESOLVE: {
            char host[ARM_USER_STRING_MAX];
            if (!user_string(frame->r0, host, sizeof(host)) ||
                !user_range(frame->r1, 4U, true)) result = -BK_EFAULT;
            else result = network_resolve(host,
                    (uint8_t *)(uintptr_t)frame->r1, frame->r2) ? 0 : -BK_EIO;
            break;
        }
        case SYS_NET_SOCKET: {
            int raw = network_socket_open((uint8_t)frame->r0);
            uint32_t i;
            result = -BK_ENOMEM;
            if (raw < 0) { result = -BK_EIO; break; }
            for (i = 0U; i < NET_SOCKET_MAX * 2U; ++i) {
                if (g_net_sockets[i].owner) continue;
                g_net_sockets[i].owner = task_current_process_id();
                g_net_sockets[i].handle = g_next_net_handle++;
                if (!g_next_net_handle) g_next_net_handle = 1U;
                g_net_sockets[i].raw = raw;
                result = (int32_t)g_net_sockets[i].handle;
                break;
            }
            if (result < 0) network_socket_close(raw);
            break;
        }
        case SYS_NET_CONNECT: {
            arm_net_socket_t *socket = net_socket_by_handle(frame->r0);
            if (!socket) result = -BK_EBADF;
            else if (!user_range(frame->r1, 4U, false)) result = -BK_EFAULT;
            else result = network_socket_connect(socket->raw,
                (const uint8_t *)(uintptr_t)frame->r1, (uint16_t)frame->r2,
                frame->r3) ? 0 : -BK_EIO;
            break;
        }
        case SYS_NET_SEND:
        case SYS_NET_RECV: {
            arm_net_socket_t *socket = net_socket_by_handle(frame->r0);
            bool write = number == SYS_NET_RECV;
            if (!socket) result = -BK_EBADF;
            else if (frame->r2 > 1024U * 1024U ||
                     !user_range(frame->r1, frame->r2, write)) result = -BK_EFAULT;
            else if (number == SYS_NET_SEND)
                result = network_socket_send(socket->raw,
                    (const void *)(uintptr_t)frame->r1, frame->r2, frame->r3);
            else result = network_socket_receive(socket->raw,
                    (void *)(uintptr_t)frame->r1, frame->r2, frame->r3);
            break;
        }
        case SYS_NET_CLOSE: {
            arm_net_socket_t *socket = net_socket_by_handle(frame->r0);
            if (!socket) result = -BK_EBADF;
            else { network_socket_close(socket->raw); kmemset(socket, 0, sizeof(*socket)); result = 0; }
            break;
        }
        case SYS_NET_HTTP_GET: {
            char url[ARM_USER_STRING_MAX];
            if (!user_string(frame->r0, url, sizeof(url)) ||
                frame->r2 > 1024U * 1024U ||
                !user_range(frame->r1, frame->r2, true)) result = -BK_EFAULT;
            else result = network_http_get(url, (void *)(uintptr_t)frame->r1,
                                           frame->r2, frame->r3);
            break;
        }
        case SYS_UPCALL_RETURN:
            if (!rpi3_task_finish_user_upcall(frame)) {
                frame->r0 = (uint32_t)-BK_EINVAL;
                return frame;
            }
            (void)rpi3_task_prepare_user_upcall(frame);
            return frame;
        case SYS_API_CALL:
            result = sys_arm_api(frame->r0, frame->r1, frame->r2, frame->r3);
            break;
        default:
            result = -BK_ENOSYS;
            break;
    }
    frame->r0 = (uint32_t)result;
    (void)rpi3_task_prepare_user_upcall(frame);
    return frame;
}
