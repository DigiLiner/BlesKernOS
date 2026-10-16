#include "../../../sdk/include/bleskernos_api.h"
#undef NULL
#include "../../../kernel/include/syscall.h"
#include "user_api_arm.h"

#define ARM_USER_HZ 100U
#define ARM_USER_FILE_CREATE 0x0010U
#define ARM_USER_FILE_TRUNC  0x0020U

static uint32_t cstrlen(const char *text) {
    uint32_t n = 0U;
    while (text && text[n]) ++n;
    return n;
}

uint32_t bk_sys_api_version(void) { return (uint32_t)syscall0(SYS_ABI_VERSION); }
uint32_t bk_sys_capabilities(void) {
    return BK_API_CAP_SYSTEM | BK_API_CAP_FILES | BK_API_CAP_PROCESS | BK_API_CAP_APP;
}
void bk_sys_log(const char *message) { bk_console_write(message); }
uint32_t bk_sys_getpid(void) { return (uint32_t)syscall0(SYS_GETPID); }
uint32_t bk_sys_ticks(void) { return (uint32_t)syscall0(SYS_TICKS); }
uint32_t bk_sys_uptime_ms(void) { return (uint32_t)syscall0(SYS_UPTIME_MS); }
void bk_sys_sleep_ms(uint32_t milliseconds) {
    uint32_t ticks = (milliseconds * ARM_USER_HZ + 999U) / 1000U;
    (void)syscall1(SYS_SLEEP, ticks ? ticks : 1U);
}
void bk_sys_yield(void) { (void)syscall0(SYS_YIELD); }
void bk_sys_reboot(void) {}
void bk_sys_shutdown(void) {}
void *bk_sys_alloc(size_t size) {
    int32_t result = syscall1(SYS_ALLOC, (uint32_t)size);
    return result > 0 ? (void *)(uintptr_t)(uint32_t)result : NULL;
}
void bk_sys_free(void *pointer) { (void)syscall1(SYS_FREE, (uint32_t)(uintptr_t)pointer); }
bool bk_sys_memory_info(bk_memory_info_t *info) { (void)info; return false; }

void bk_console_putchar(char character) {
    char c = character;
    (void)syscall3(SYS_WRITE, 1U, (uint32_t)(uintptr_t)&c, 1U);
}
void bk_console_write(const char *text) {
    uint32_t length = cstrlen(text);
    if (length) (void)syscall3(SYS_WRITE, 1U, (uint32_t)(uintptr_t)text, length);
}

static void emit_unsigned(uint32_t value, uint32_t base, bool upper) {
    char buffer[16];
    uint32_t count = 0U;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!value) { bk_console_putchar('0'); return; }
    while (value && count < sizeof(buffer)) {
        buffer[count++] = digits[value % base];
        value /= base;
    }
    while (count) bk_console_putchar(buffer[--count]);
}

void bk_console_printf(const char *format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, format);
    while (format && *format) {
        if (*format != '%') { bk_console_putchar(*format++); continue; }
        ++format;
        if (*format == '%') { bk_console_putchar('%'); ++format; continue; }
        if (*format == 's') {
            const char *s = __builtin_va_arg(args, const char *);
            bk_console_write(s ? s : "(null)");
        } else if (*format == 'c') {
            bk_console_putchar((char)__builtin_va_arg(args, int));
        } else if (*format == 'd' || *format == 'i') {
            int32_t v = __builtin_va_arg(args, int32_t);
            if (v < 0) { bk_console_putchar('-'); emit_unsigned((uint32_t)(-(v + 1)) + 1U, 10U, false); }
            else emit_unsigned((uint32_t)v, 10U, false);
        } else if (*format == 'u') {
            emit_unsigned(__builtin_va_arg(args, uint32_t), 10U, false);
        } else if (*format == 'x' || *format == 'X') {
            bool upper = *format == 'X';
            emit_unsigned(__builtin_va_arg(args, uint32_t), 16U, upper);
        } else if (*format == 'p') {
            bk_console_write("0x");
            emit_unsigned((uint32_t)(uintptr_t)__builtin_va_arg(args, void *), 16U, false);
        } else {
            bk_console_putchar('%');
            if (*format) bk_console_putchar(*format);
        }
        if (*format) ++format;
    }
    __builtin_va_end(args);
}

size_t bk_string_length(const char *text) { return (size_t)cstrlen(text); }
int bk_string_compare(const char *left, const char *right) {
    if (!left) left = ""; if (!right) right = "";
    while (*left && *left == *right) { ++left; ++right; }
    return (uint8_t)*left - (uint8_t)*right;
}
char *bk_string_copy_n(char *destination, const char *source, size_t capacity) {
    size_t i = 0U;
    if (!destination || !capacity) return destination;
    if (!source) source = "";
    while (i < capacity && source[i]) { destination[i] = source[i]; ++i; }
    while (i < capacity) destination[i++] = '\0';
    return destination;
}
char *bk_string_concat(char *destination, const char *source) {
    size_t n = bk_string_length(destination);
    if (!source) return destination;
    while (*source) destination[n++] = *source++;
    destination[n] = '\0';
    return destination;
}
int bk_memory_compare(const void *left, const void *right, size_t size) {
    const uint8_t *a = (const uint8_t *)left, *b = (const uint8_t *)right;
    while (size--) { if (*a != *b) return *a < *b ? -1 : 1; ++a; ++b; }
    return 0;
}

int bk_file_open(const char *path, uint32_t flags) {
    return (int)syscall2(SYS_OPEN, (uint32_t)(uintptr_t)path, flags);
}
int bk_file_read(int descriptor, void *buffer, uint32_t size) {
    return (int)syscall3(SYS_READ, (uint32_t)descriptor, (uint32_t)(uintptr_t)buffer, size);
}
int bk_file_write(int descriptor, const void *buffer, uint32_t size) {
    return (int)syscall3(SYS_FILE_WRITE, (uint32_t)descriptor, (uint32_t)(uintptr_t)buffer, size);
}
int32_t bk_file_seek(int descriptor, int32_t offset, uint32_t origin) {
    return syscall3(SYS_FILE_SEEK, (uint32_t)descriptor, (uint32_t)offset, origin);
}
int32_t bk_file_tell(int descriptor) { return syscall1(SYS_FILE_TELL, (uint32_t)descriptor); }
int32_t bk_file_size(int descriptor) { return syscall1(SYS_FILE_SIZE, (uint32_t)descriptor); }
bool bk_file_close(int descriptor) { return syscall1(SYS_CLOSE, (uint32_t)descriptor) == 0; }

bool bk_file_read_all(const char *path, void **buffer, uint32_t *size) {
    int fd; int32_t bytes; int32_t got; void *data;
    if (!buffer || !size) return false;
    *buffer = NULL; *size = 0U;
    fd = bk_file_open(path, BK_FILE_READ_ONLY);
    if (fd < 0) return false;
    bytes = bk_file_size(fd);
    if (bytes < 0) { bk_file_close(fd); return false; }
    data = bk_sys_alloc((size_t)(bytes > 0 ? (uint32_t)bytes : 1U));
    if (!data) { bk_file_close(fd); return false; }
    got = bytes ? bk_file_read(fd, data, (uint32_t)bytes) : 0;
    bk_file_close(fd);
    if (got != bytes) { bk_sys_free(data); return false; }
    *buffer = data; *size = (uint32_t)bytes;
    return true;
}

bool bk_file_write_all(const char *path, const void *buffer, uint32_t size) {
    int fd = bk_file_open(path, BK_FILE_WRITE_ONLY | ARM_USER_FILE_CREATE | ARM_USER_FILE_TRUNC);
    int written;
    if (fd < 0) return false;
    written = size ? bk_file_write(fd, buffer, size) : 0;
    bk_file_close(fd);
    return written == (int)size;
}

bool bk_file_stat(const char *path, bk_file_entry_t *entry) {
    return syscall2(SYS_FILE_STAT, (uint32_t)(uintptr_t)path,
                    (uint32_t)(uintptr_t)entry) == 0;
}
bool bk_file_list_dir(const char *path, bk_file_entry_t *entries,
                      uint32_t capacity, uint32_t *count) {
    return syscall4(SYS_GETDENTS, (uint32_t)(uintptr_t)path,
                    (uint32_t)(uintptr_t)entries, capacity,
                    (uint32_t)(uintptr_t)count) == 0;
}
bool bk_file_chdir(const char *path) { return syscall1(SYS_CHDIR, (uint32_t)(uintptr_t)path) == 0; }
const char *bk_file_getcwd(void) {
    static char cwd[BK_PATH_MAX];
    if (syscall2(SYS_GETCWD, (uint32_t)(uintptr_t)cwd, sizeof(cwd)) < 0) return NULL;
    return cwd;
}
bool bk_file_mkdir(const char *path) { return syscall1(SYS_MKDIR, (uint32_t)(uintptr_t)path) == 0; }
bool bk_file_remove(const char *path) { return syscall1(SYS_UNLINK, (uint32_t)(uintptr_t)path) == 0; }
bool bk_file_rename(const char *old_path, const char *new_path) {
    return syscall2(SYS_RENAME, (uint32_t)(uintptr_t)old_path,
                    (uint32_t)(uintptr_t)new_path) == 0;
}

bool bk_app_launch(const char *path, const char *argument) {
    return syscall2(SYS_SPAWN, (uint32_t)(uintptr_t)path,
                    (uint32_t)(uintptr_t)argument) > 0;
}
int bk_app_run_sync(const char *path, const char *command_line) {
    int32_t pid = syscall2(SYS_SPAWN, (uint32_t)(uintptr_t)path,
                           (uint32_t)(uintptr_t)command_line);
    int32_t status = -1;
    if (pid <= 0) return -1;
    for (;;) {
        int32_t result = syscall2(SYS_WAITPID, (uint32_t)pid,
                                  (uint32_t)(uintptr_t)&status);
        if (result == pid) return status;
        if (result < 0) return -1;
        (void)syscall0(SYS_YIELD);
    }
}
bool bk_proc_launch_arg_copy(char *buffer, uint32_t capacity) {
    return syscall2(SYS_LAUNCH_ARG_COPY, (uint32_t)(uintptr_t)buffer, capacity) == 0;
}
static int32_t arm_api3(uint32_t operation, uint32_t a, uint32_t b,
                        uint32_t c) {
    return syscall4(SYS_API_CALL, operation, a, b, c);
}

bool bk_time_datetime(bk_datetime_t *datetime) {
    return arm_api3(RPI3_UAPI_TIME_DATETIME,
                    (uint32_t)(uintptr_t)datetime, 0U, 0U) == 0;
}
uint32_t bk_proc_count(void) {
    int32_t value = arm_api3(RPI3_UAPI_PROC_COUNT, 0U, 0U, 0U);
    return value < 0 ? 0U : (uint32_t)value;
}
bool bk_proc_info(uint32_t index, bk_process_info_t *info) {
    return arm_api3(RPI3_UAPI_PROC_INFO, index,
                    (uint32_t)(uintptr_t)info, 0U) == 0;
}
bool bk_proc_request_exit(uint32_t pid) {
    return arm_api3(RPI3_UAPI_PROC_KILL, pid, 0U, 0U) == 0;
}
uint32_t bk_proc_cpu_usage(void) {
    int32_t value = arm_api3(RPI3_UAPI_PROC_CPU_USAGE, 0U, 0U, 0U);
    return value < 0 ? 0U : (uint32_t)value;
}
uint32_t bk_device_block_count(void) {
    int32_t value = arm_api3(RPI3_UAPI_BLOCK_COUNT, 0U, 0U, 0U);
    return value < 0 ? 0U : (uint32_t)value;
}
bool bk_device_block_info(uint32_t index, bk_block_info_t *info) {
    return arm_api3(RPI3_UAPI_BLOCK_INFO, index,
                    (uint32_t)(uintptr_t)info, 0U) == 0;
}
uint32_t bk_device_partition_count(void) {
    int32_t value = arm_api3(RPI3_UAPI_PARTITION_COUNT, 0U, 0U, 0U);
    return value < 0 ? 0U : (uint32_t)value;
}
bool bk_device_partition_info(uint32_t index, bk_partition_info_t *info) {
    return arm_api3(RPI3_UAPI_PARTITION_INFO, index,
                    (uint32_t)(uintptr_t)info, 0U) == 0;
}
bool bk_device_volume_info(bk_volume_info_t *info) {
    return arm_api3(RPI3_UAPI_VOLUME_INFO,
                    (uint32_t)(uintptr_t)info, 0U, 0U) == 0;
}
bool bk_device_check_volume(bk_volume_check_report_t *report) {
    return arm_api3(RPI3_UAPI_VOLUME_CHECK,
                    (uint32_t)(uintptr_t)report, 0U, 0U) == 0;
}
bool bk_device_repair_volume(bk_volume_repair_report_t *repair,
                             bk_volume_check_report_t *after) {
    return arm_api3(RPI3_UAPI_VOLUME_REPAIR,
                    (uint32_t)(uintptr_t)repair,
                    (uint32_t)(uintptr_t)after, 0U) == 0;
}
bool bk_device_mount_volume(const char *device_name) {
    return arm_api3(RPI3_UAPI_VOLUME_MOUNT,
                    (uint32_t)(uintptr_t)device_name, 0U, 0U) == 0;
}
bool bk_device_format_fat(const char *device_name, const char *volume_label) {
    return arm_api3(RPI3_UAPI_VOLUME_FORMAT,
                    (uint32_t)(uintptr_t)device_name,
                    (uint32_t)(uintptr_t)volume_label, 0U) == 0;
}
uint32_t bk_device_pci_count(void) { return 0U; }
bool bk_device_pci_info(uint32_t index, bk_pci_info_t *info) {
    (void)index; (void)info; return false;
}
bool bk_sound_tone(uint32_t frequency_hz, uint32_t duration_ms) {
    return arm_api3(RPI3_UAPI_SOUND_TONE, frequency_hz, duration_ms, 0U) == 0;
}
const char *bk_sound_pcm_name(void) { return "bcm2837-pwm-dma-pcm"; }
bool bk_sound_pcm_available(void) {
    return arm_api3(RPI3_UAPI_SOUND_PCM_AVAILABLE, 0U, 0U, 0U) > 0;
}
bool bk_sound_pcm_busy(void) {
    return arm_api3(RPI3_UAPI_SOUND_PCM_BUSY, 0U, 0U, 0U) > 0;
}
bool bk_sound_play_pcm_u8(const uint8_t *samples, uint32_t length,
                          uint16_t sample_rate_hz, uint8_t volume) {
    return arm_api3(RPI3_UAPI_SOUND_PCM_PLAY,
                    (uint32_t)(uintptr_t)samples, length,
                    (uint32_t)sample_rate_hz | ((uint32_t)volume << 16)) == 0;
}
void bk_sound_stop(void) {
    (void)arm_api3(RPI3_UAPI_SOUND_STOP, 0U, 0U, 0U);
}
bool bk_net_get_info(bk_net_info_t *info) {
    return arm_api3(RPI3_UAPI_NET_INFO,
                    (uint32_t)(uintptr_t)info, 0U, 0U) == 0;
}
bool bk_net_dhcp(uint32_t timeout_ms) {
    return arm_api3(RPI3_UAPI_NET_DHCP, timeout_ms, 0U, 0U) == 0;
}
bool bk_net_ping(const uint8_t address[4], uint32_t timeout_ms,
                 uint32_t *round_trip_ms) {
    return arm_api3(RPI3_UAPI_NET_PING, (uint32_t)(uintptr_t)address,
                    timeout_ms, (uint32_t)(uintptr_t)round_trip_ms) == 0;
}
bool bk_lang_set(const char *code) {
    return arm_api3(RPI3_UAPI_LANGUAGE_SET,
                    (uint32_t)(uintptr_t)code, 0U, 0U) == 0;
}
void bk_input_mouse_set_sensitivity(uint8_t sensitivity) {
    (void)arm_api3(RPI3_UAPI_MOUSE_SET_SENSITIVITY, sensitivity, 0U, 0U);
}
uint8_t bk_input_mouse_get_sensitivity(void) {
    int32_t value = arm_api3(RPI3_UAPI_MOUSE_GET_SENSITIVITY, 0U, 0U, 0U);
    return value < 1 ? 1U : (uint8_t)value;
}
uint32_t bk_clipboard_get_text(char *buffer, uint32_t capacity) {
    int32_t value = arm_api3(RPI3_UAPI_CLIPBOARD_GET,
                    (uint32_t)(uintptr_t)buffer, capacity, 0U);
    return value < 0 ? 0U : (uint32_t)value;
}
bool bk_net_resolve(const char *hostname, uint8_t address[4],
                    uint32_t timeout_ms) {
    return syscall3(SYS_NET_RESOLVE, (uint32_t)(uintptr_t)hostname,
                    (uint32_t)(uintptr_t)address, timeout_ms) == 0;
}
int bk_net_socket_open(uint8_t type) { return syscall1(SYS_NET_SOCKET, type); }
bool bk_net_socket_connect(int socket, const uint8_t address[4], uint16_t port,
                           uint32_t timeout_ms) {
    return syscall4(SYS_NET_CONNECT, (uint32_t)socket,
                    (uint32_t)(uintptr_t)address, port, timeout_ms) == 0;
}
int32_t bk_net_socket_send(int socket, const void *data, uint32_t length,
                           uint32_t timeout_ms) {
    return syscall4(SYS_NET_SEND, (uint32_t)socket,
                    (uint32_t)(uintptr_t)data, length, timeout_ms);
}
int32_t bk_net_socket_receive(int socket, void *data, uint32_t capacity,
                              uint32_t timeout_ms) {
    return syscall4(SYS_NET_RECV, (uint32_t)socket,
                    (uint32_t)(uintptr_t)data, capacity, timeout_ms);
}
void bk_net_socket_close(int socket) { (void)syscall1(SYS_NET_CLOSE, (uint32_t)socket); }
int32_t bk_net_http_get(const char *url, void *data, uint32_t capacity,
                        uint32_t timeout_ms) {
    return syscall4(SYS_NET_HTTP_GET, (uint32_t)(uintptr_t)url,
                    (uint32_t)(uintptr_t)data, capacity, timeout_ms);
}
int bk_proc_spawn_thread(const char *name, bk_thread_entry_t entry, void *argument) {
    (void)name; (void)entry; (void)argument; return -1;
}
void bk_proc_exit(void) { (void)syscall1(SYS_EXIT, 0U); for (;;) {} }
void bk_proc_exit_status(int status) { (void)syscall1(SYS_EXIT, (uint32_t)status); for (;;) {} }

static int32_t arm_gui(rpi3_gui_request_t *request) {
    return arm_api3(RPI3_UAPI_GUI_CALL,
                    (uint32_t)(uintptr_t)request, 0U, 0U);
}

bk_gui_desktop_t *bk_gui_desktop(void) {
    rpi3_gui_request_t r = {RPI3_GUI_DESKTOP, {0U}};
    int32_t value = arm_gui(&r);
    return value > 0 ? (bk_gui_desktop_t *)(uintptr_t)(uint32_t)value : NULL;
}

bk_gui_window_t *bk_gui_create_window(bk_gui_desktop_t *desktop, int x, int y,
                                      int w, int h, const char *title) {
    rpi3_gui_request_t r = {RPI3_GUI_CREATE_WINDOW, {0U}};
    int32_t value;
    (void)desktop;
    r.value[0] = (uint32_t)x; r.value[1] = (uint32_t)y;
    r.value[2] = (uint32_t)w; r.value[3] = (uint32_t)h;
    r.value[5] = (uint32_t)(uintptr_t)title;
    value = arm_gui(&r);
    return value > 0 ? (bk_gui_window_t *)(uintptr_t)(uint32_t)value : NULL;
}

void bk_gui_set_window_content(bk_gui_window_t *window, bk_gui_paint_t paint,
                               void *context) {
    rpi3_gui_request_t r = {RPI3_GUI_SET_CONTENT, {0U}};
    r.value[0] = (uint32_t)(uintptr_t)window;
    r.value[1] = (uint32_t)(uintptr_t)paint;
    r.value[2] = (uint32_t)(uintptr_t)context;
    (void)arm_gui(&r);
}

void bk_gui_set_window_event_handler(bk_gui_window_t *window,
                                     bk_gui_event_handler_t handler,
                                     void *context) {
    rpi3_gui_request_t r = {RPI3_GUI_SET_EVENT, {0U}};
    r.value[0] = (uint32_t)(uintptr_t)window;
    r.value[1] = (uint32_t)(uintptr_t)handler;
    r.value[2] = (uint32_t)(uintptr_t)context;
    (void)arm_gui(&r);
}

void bk_gui_set_window_min_size(bk_gui_window_t *window, int min_w, int min_h) {
    rpi3_gui_request_t r = {RPI3_GUI_SET_MIN_SIZE, {0U}};
    r.value[0] = (uint32_t)(uintptr_t)window;
    r.value[1] = (uint32_t)min_w; r.value[2] = (uint32_t)min_h;
    (void)arm_gui(&r);
}

void bk_gui_destroy_window(bk_gui_desktop_t *desktop, bk_gui_window_t *window) {
    rpi3_gui_request_t r = {RPI3_GUI_DESTROY_WINDOW, {0U}};
    (void)desktop; r.value[0] = (uint32_t)(uintptr_t)window;
    (void)arm_gui(&r);
}

void bk_gui_close_window(bk_gui_window_t *window) {
    bk_gui_destroy_window(NULL, window);
}

bool bk_gui_window_is_open(const bk_gui_window_t *window) {
    rpi3_gui_request_t r = {RPI3_GUI_WINDOW_OPEN, {0U}};
    r.value[0] = (uint32_t)(uintptr_t)window;
    return arm_gui(&r) > 0;
}

void bk_gui_window_set_owner(bk_gui_window_t *window, uint32_t pid) {
    (void)window; (void)pid; /* Ownership is assigned atomically at create. */
}

void bk_gui_window_invalidate(bk_gui_window_t *window) {
    rpi3_gui_request_t r = {RPI3_GUI_WINDOW_INVALIDATE, {0U}};
    r.value[0] = (uint32_t)(uintptr_t)window; (void)arm_gui(&r);
}

static bool arm_gui_rect(uint32_t operation, const bk_gui_window_t *window,
                         bk_gui_rect_t *rect) {
    rpi3_gui_request_t r = {0U, {0U}};
    if (!rect) return false;
    r.operation = operation; r.value[0] = (uint32_t)(uintptr_t)window;
    if (arm_gui(&r) < 0) return false;
    rect->x = (int32_t)r.value[1]; rect->y = (int32_t)r.value[2];
    rect->w = (int32_t)r.value[3]; rect->h = (int32_t)r.value[4];
    return true;
}

bool bk_gui_window_bounds(const bk_gui_window_t *window, bk_gui_rect_t *rect) {
    return arm_gui_rect(RPI3_GUI_WINDOW_BOUNDS, window, rect);
}
bool bk_gui_window_content_rect(const bk_gui_window_t *window, bk_gui_rect_t *rect) {
    return arm_gui_rect(RPI3_GUI_CONTENT_RECT, window, rect);
}

static void arm_gui_draw_rect(uint32_t operation, bk_gui_surface_t *surface,
                              bk_gui_rect_t rect, uint32_t color) {
    rpi3_gui_request_t r = {0U, {0U}};
    r.operation = operation; r.value[0] = (uint32_t)(uintptr_t)surface;
    r.value[1] = (uint32_t)rect.x; r.value[2] = (uint32_t)rect.y;
    r.value[3] = (uint32_t)rect.w; r.value[4] = (uint32_t)rect.h;
    r.value[5] = color; (void)arm_gui(&r);
}
void bk_gui_surface_fill_rect(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                              uint32_t color) {
    arm_gui_draw_rect(RPI3_GUI_FILL_RECT, surface, rect, color);
}
void bk_gui_surface_draw_rect(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                              uint32_t color) {
    arm_gui_draw_rect(RPI3_GUI_DRAW_RECT, surface, rect, color);
}
void bk_gui_surface_draw_line(bk_gui_surface_t *surface, int x0, int y0,
                              int x1, int y1, uint32_t color) {
    rpi3_gui_request_t r = {RPI3_GUI_DRAW_LINE, {0U}};
    r.value[0] = (uint32_t)(uintptr_t)surface;
    r.value[1] = (uint32_t)x0; r.value[2] = (uint32_t)y0;
    r.value[3] = (uint32_t)x1; r.value[4] = (uint32_t)y1;
    r.value[5] = color; (void)arm_gui(&r);
}
void bk_gui_surface_draw_image(bk_gui_surface_t *surface,
                               bk_gui_rect_t destination,
                               bk_gui_rect_t clip,
                               const bk_gui_image_t *image) {
    typedef struct {
        bk_gui_rect_t destination;
        bk_gui_rect_t clip;
        uint32_t pixels;
        uint16_t width;
        uint16_t height;
    } arm_image_draw_t;
    rpi3_gui_request_t r = {RPI3_GUI_DRAW_IMAGE, {0U}};
    arm_image_draw_t draw;
    if (!image || !image->pixels) return;
    draw.destination = destination; draw.clip = clip;
    draw.pixels = (uint32_t)(uintptr_t)image->pixels;
    draw.width = image->width; draw.height = image->height;
    r.value[0] = (uint32_t)(uintptr_t)surface;
    r.value[1] = (uint32_t)(uintptr_t)&draw; (void)arm_gui(&r);
}
void bk_gui_surface_draw_text(bk_gui_surface_t *surface, int x, int y,
                              const char *text, uint32_t foreground,
                              uint32_t background, bool fill_background) {
    rpi3_gui_request_t r = {RPI3_GUI_DRAW_TEXT, {0U}};
    r.value[0] = (uint32_t)(uintptr_t)surface;
    r.value[1] = (uint32_t)x; r.value[2] = (uint32_t)y;
    r.value[3] = (uint32_t)(uintptr_t)text; r.value[4] = foreground;
    r.value[5] = background; r.value[6] = fill_background ? 1U : 0U;
    (void)arm_gui(&r);
}
uint16_t bk_gui_text_width(const char *text) {
    rpi3_gui_request_t r = {RPI3_GUI_TEXT_WIDTH, {0U}};
    int32_t value; r.value[0] = (uint32_t)(uintptr_t)text; value = arm_gui(&r);
    return value > 0 ? (uint16_t)value : 0U;
}

bk_gui_widget_t *bk_gui_create_button(bk_gui_desktop_t *desktop,
                                      bk_gui_window_t *window,
                                      bk_gui_rect_t rect, const char *text,
                                      bk_gui_widget_callback_t callback) {
    rpi3_gui_request_t r = {RPI3_GUI_CREATE_BUTTON, {0U}};
    int32_t value; (void)desktop;
    r.value[0] = (uint32_t)(uintptr_t)window;
    r.value[1] = (uint32_t)rect.x; r.value[2] = (uint32_t)rect.y;
    r.value[3] = (uint32_t)rect.w; r.value[4] = (uint32_t)rect.h;
    r.value[5] = (uint32_t)(uintptr_t)text;
    r.value[6] = (uint32_t)(uintptr_t)callback;
    value = arm_gui(&r);
    return value > 0 ? (bk_gui_widget_t *)(uintptr_t)(uint32_t)value : NULL;
}
bk_gui_widget_t *bk_gui_create_textbox(bk_gui_desktop_t *desktop,
                                       bk_gui_window_t *window,
                                       bk_gui_rect_t rect, const char *text,
                                       uint16_t max_length,
                                       bk_gui_widget_callback_t callback) {
    rpi3_gui_request_t r = {RPI3_GUI_CREATE_TEXTBOX, {0U}};
    int32_t value; (void)desktop;
    r.value[0] = (uint32_t)(uintptr_t)window;
    r.value[1] = (uint32_t)rect.x; r.value[2] = (uint32_t)rect.y;
    r.value[3] = (uint32_t)rect.w; r.value[4] = (uint32_t)rect.h;
    r.value[5] = (uint32_t)(uintptr_t)text; r.value[6] = max_length;
    r.value[7] = (uint32_t)(uintptr_t)callback;
    value = arm_gui(&r);
    return value > 0 ? (bk_gui_widget_t *)(uintptr_t)(uint32_t)value : NULL;
}
uint32_t bk_gui_widget_id(const bk_gui_widget_t *widget) {
    rpi3_gui_request_t r = {RPI3_GUI_WIDGET_ID, {0U}};
    int32_t value; r.value[0] = (uint32_t)(uintptr_t)widget; value = arm_gui(&r);
    return value > 0 ? (uint32_t)value : 0U;
}
void bk_gui_widget_set_bounds(bk_gui_window_t *window,
                              bk_gui_widget_t *widget, bk_gui_rect_t rect) {
    rpi3_gui_request_t r = {RPI3_GUI_WIDGET_SET_BOUNDS, {0U}};
    r.value[0] = (uint32_t)(uintptr_t)window;
    r.value[1] = (uint32_t)(uintptr_t)widget;
    r.value[2] = (uint32_t)rect.x; r.value[3] = (uint32_t)rect.y;
    r.value[4] = (uint32_t)rect.w; r.value[5] = (uint32_t)rect.h;
    (void)arm_gui(&r);
}
void bk_gui_widget_set_text(bk_gui_widget_t *widget, const char *value) {
    rpi3_gui_request_t r = {RPI3_GUI_WIDGET_SET_TEXT, {0U}};
    r.value[0] = (uint32_t)(uintptr_t)widget;
    r.value[1] = (uint32_t)(uintptr_t)value; (void)arm_gui(&r);
}
bool bk_gui_widget_get_text(const bk_gui_widget_t *widget, char *buffer,
                            uint32_t capacity) {
    rpi3_gui_request_t r = {RPI3_GUI_WIDGET_GET_TEXT, {0U}};
    r.value[0] = (uint32_t)(uintptr_t)widget;
    r.value[1] = (uint32_t)(uintptr_t)buffer; r.value[2] = capacity;
    return arm_gui(&r) == 0;
}
void bk_gui_widget_set_enabled(bk_gui_widget_t *widget, bool enabled) {
    rpi3_gui_request_t r = {RPI3_GUI_WIDGET_SET_ENABLED, {0U}};
    r.value[0] = (uint32_t)(uintptr_t)widget;
    r.value[1] = enabled ? 1U : 0U; (void)arm_gui(&r);
}
static void arm_gui_widget_window_bool(uint32_t operation,
                                       bk_gui_window_t *window,
                                       bk_gui_widget_t *widget, bool value) {
    rpi3_gui_request_t r = {0U, {0U}}; r.operation = operation;
    r.value[0] = (uint32_t)(uintptr_t)window;
    r.value[1] = (uint32_t)(uintptr_t)widget;
    r.value[2] = value ? 1U : 0U; (void)arm_gui(&r);
}
void bk_gui_widget_set_visible(bk_gui_window_t *window,
                               bk_gui_widget_t *widget, bool visible) {
    arm_gui_widget_window_bool(RPI3_GUI_WIDGET_SET_VISIBLE, window, widget, visible);
}
void bk_gui_widget_set_focus(bk_gui_window_t *window,
                             bk_gui_widget_t *widget, bool focused) {
    arm_gui_widget_window_bool(RPI3_GUI_WIDGET_SET_FOCUS, window, widget, focused);
}
bool bk_gui_widget_is_focused(const bk_gui_window_t *window,
                              const bk_gui_widget_t *widget) {
    rpi3_gui_request_t r = {RPI3_GUI_WIDGET_IS_FOCUSED, {0U}};
    r.value[0] = (uint32_t)(uintptr_t)window;
    r.value[1] = (uint32_t)(uintptr_t)widget;
    return arm_gui(&r) > 0;
}
void bk_gui_request_paint(void) {
    rpi3_gui_request_t r = {RPI3_GUI_REQUEST_PAINT, {0U}}; (void)arm_gui(&r);
}
void bk_proc_bind_window(bk_gui_window_t *window) { (void)window; }
bool bk_proc_exit_requested(void) { return false; }
void bk_proc_set_memory_hint(uint32_t bytes) { (void)bytes; }
