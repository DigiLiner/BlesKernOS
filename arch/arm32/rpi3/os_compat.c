#include "uart.h"
#include "timer.h"
#include "elf_arm.h"
#include "task_arm.h"
#include "../../../kernel/include/types.h"
#include "../../../kernel/include/stdarg.h"
#include "../../../kernel/include/klock.h"
#include "../../../kernel/include/memory.h"
#include "../../../kernel/include/task.h"
#include "../../../kernel/include/perfmon.h"
#include "../../../kernel/include/iso9660.h"
#include "../../../kernel/include/vfs.h"

struct gui_desktop;
extern void shelllauncher_open_from_desktop(struct gui_desktop *desktop);
extern void about_open(struct gui_desktop *desktop);
extern void runbox_open_from_desktop(struct gui_desktop *desktop);
extern void calendar_open_from_desktop(struct gui_desktop *desktop);
extern void texteditor_open(struct gui_desktop *desktop, const char *path);
extern void texteditor_open_from_desktop(struct gui_desktop *desktop);
extern void filebrowser_open_path(struct gui_desktop *desktop, const char *path);
extern void filebrowser_open_from_desktop(struct gui_desktop *desktop);
extern void imageviewer_open(struct gui_desktop *desktop, const char *path);
extern void imageviewer_open_from_desktop(struct gui_desktop *desktop);
extern void paint_open_from_desktop(struct gui_desktop *desktop);
extern void processmanager_open_from_desktop(struct gui_desktop *desktop);
extern void control_panel_arm_kernel_main(struct gui_desktop *desktop);
extern void mouse_arm_kernel_main(struct gui_desktop *desktop);
extern void keyboard_arm_kernel_main(struct gui_desktop *desktop);
extern void language_arm_kernel_main(struct gui_desktop *desktop);
extern void scandisk_arm_kernel_main(struct gui_desktop *desktop);
extern void help_center_arm_kernel_main(struct gui_desktop *desktop);
extern void find_files_arm_kernel_main(struct gui_desktop *desktop);
extern void archive_manager_arm_kernel_main(struct gui_desktop *desktop);
extern void disk_tools_arm_kernel_main(struct gui_desktop *desktop);
extern void network_status_arm_kernel_main(struct gui_desktop *desktop);
extern void clipboard_viewer_arm_kernel_main(struct gui_desktop *desktop);
extern void performance_monitor_arm_kernel_main(struct gui_desktop *desktop);
extern void appearance_arm_kernel_main(struct gui_desktop *desktop);
extern void display_arm_kernel_main(struct gui_desktop *desktop);
extern void sound_arm_kernel_main(struct gui_desktop *desktop);
extern void datetime_arm_kernel_main(struct gui_desktop *desktop);
extern void system_arm_kernel_main(struct gui_desktop *desktop);
extern void device_manager_arm_kernel_main(struct gui_desktop *desktop);
extern void internet_arm_kernel_main(struct gui_desktop *desktop);
extern void modem_arm_kernel_main(struct gui_desktop *desktop);

static void print_unsigned(uint32_t value, uint32_t base, bool upper) {
    char buf[16];
    uint32_t n = 0U;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!value) { rpi3_uart_putc('0'); return; }
    while (value && n < sizeof(buf)) {
        buf[n++] = digits[value % base];
        value /= base;
    }
    while (n) rpi3_uart_putc(buf[--n]);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    while (fmt && *fmt) {
        if (*fmt != '%') { rpi3_uart_putc(*fmt++); continue; }
        fmt++;
        if (*fmt == '%') { rpi3_uart_putc('%'); fmt++; continue; }
        /* Width/zero padding is deliberately ignored in the bootstrap. */
        while (*fmt >= '0' && *fmt <= '9') fmt++;
        if (*fmt == 'l') fmt++;
        switch (*fmt) {
            case 'c': rpi3_uart_putc((char)va_arg(ap, int)); break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                rpi3_uart_puts(s ? s : "(null)");
                break;
            }
            case 'd':
            case 'i': {
                int32_t v = va_arg(ap, int32_t);
                if (v < 0) { rpi3_uart_putc('-'); print_unsigned((uint32_t)(-v), 10U, false); }
                else print_unsigned((uint32_t)v, 10U, false);
                break;
            }
            case 'u': print_unsigned(va_arg(ap, uint32_t), 10U, false); break;
            case 'x': print_unsigned(va_arg(ap, uint32_t), 16U, false); break;
            case 'X': print_unsigned(va_arg(ap, uint32_t), 16U, true); break;
            case 'p': rpi3_uart_puts("0x"); print_unsigned((uint32_t)(uintptr_t)va_arg(ap, void *), 16U, false); break;
            default:
                rpi3_uart_putc('%');
                if (*fmt) rpi3_uart_putc(*fmt);
                break;
        }
        if (*fmt) fmt++;
    }
    va_end(ap);
}

/* Stage 10 moved the task ABI into task_arm.c. The storage/VFS core now
 * observes real task identity, cwd, yield and preemption semantics. */
static uint32_t arm_cpu_id(void) {
    uint32_t mpidr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return mpidr & 3U;
}

/* Application scheduling intentionally remains on the boot CPU until each
   core has its own current-task pointer and user translation table.  Report
   only CPUs which may safely execute scheduler work. */
uint32_t smp_online_cpu_count(void) { return 1U; }
uint32_t pit_get_ticks(void) { return rpi3_timer_ticks(); }

/* -------------------------------------------------------------------------
 * Stage 12.7: real synchronization for the preemptive ARM32 port.
 *
 * The original ARM bring-up kept the x86 kernel's kmutex/krwlock API as
 * no-op stubs because Stages 6-9 were effectively single-threaded.  Once
 * Stage 10 introduced timer preemption those stubs became incorrect: block
 * I/O, VFS volume state, graphics resources and desktop painting could all be
 * entered by two tasks at different points in the same critical section.
 *
 * Only CPU0 executes BlesKernOS work today (secondary Pi cores are parked in
 * boot.S), so we do not need LDREX/STREX yet.  The scheduler preemption guard
 * gives us an atomic test/update on this UP kernel; a contended sleeping lock
 * simply yields until its owner releases it.  These semantics match the real
 * x86 klock layer closely enough that shared kernel code can stop special-
 * casing ARM. */

static uint32_t g_arm_lock_spin_acquires;
static uint32_t g_arm_lock_spin_contentions;
static uint32_t g_arm_lock_mutex_acquires;
static uint32_t g_arm_lock_mutex_contentions;
static uint32_t g_arm_lock_rw_read_acquires;
static uint32_t g_arm_lock_rw_write_acquires;
static uint32_t g_arm_lock_rw_contentions;

uint32_t klock_current_owner_token(void) {
    uint32_t pid = task_current_pid();
    return pid ? pid : 0x80000001U;
}

static void arm_lock_wait(void) {
    if (rpi3_task_scheduler_started()) task_yield();
    else __asm__ volatile("yield" : : : "memory");
}

void kmutex_init(kmutex_t *lock) {
    if (lock) kmemset(lock, 0, sizeof(*lock));
}

void kmutex_lock(kmutex_t *lock) {
    uint32_t owner;
    bool waiting = false;
    if (!lock) return;
    owner = klock_current_owner_token();
    for (;;) {
        task_preempt_disable();
        if (lock->owner == owner && lock->depth) {
            if (lock->depth != 0xFFFFU) lock->depth++;
            g_arm_lock_mutex_acquires++;
            task_preempt_enable();
            return;
        }
        if (lock->owner == 0U) {
            lock->owner = owner;
            lock->depth = 1U;
            if (waiting && lock->waiters) lock->waiters--;
            g_arm_lock_mutex_acquires++;
            task_preempt_enable();
            return;
        }
        if (!waiting) {
            lock->waiters++;
            lock->contentions++;
            g_arm_lock_mutex_contentions++;
            waiting = true;
        }
        task_preempt_enable();
        arm_lock_wait();
    }
}

bool kmutex_try_lock(kmutex_t *lock) {
    uint32_t owner;
    bool ok = false;
    if (!lock) return true;
    owner = klock_current_owner_token();
    task_preempt_disable();
    if (lock->owner == owner && lock->depth) {
        if (lock->depth != 0xFFFFU) lock->depth++;
        ok = true;
    } else if (lock->owner == 0U) {
        lock->owner = owner;
        lock->depth = 1U;
        ok = true;
    }
    if (ok) g_arm_lock_mutex_acquires++;
    task_preempt_enable();
    return ok;
}

void kmutex_unlock(kmutex_t *lock) {
    uint32_t owner;
    if (!lock) return;
    owner = klock_current_owner_token();
    task_preempt_disable();
    if (lock->owner == owner && lock->depth) {
        lock->depth--;
        if (!lock->depth) lock->owner = 0U;
    }
    task_preempt_enable();
}

uint16_t kmutex_depth_owned(const kmutex_t *lock, uint32_t owner) {
    return lock && lock->owner == owner ? lock->depth : 0U;
}

uint16_t kmutex_drop_owner(kmutex_t *lock, uint32_t owner) {
    uint16_t depth = 0U;
    if (!lock || !owner) return 0U;
    task_preempt_disable();
    if (lock->owner == owner && lock->depth) {
        depth = lock->depth;
        lock->depth = 0U;
        lock->owner = 0U;
    }
    task_preempt_enable();
    return depth;
}

void kmutex_restore_depth(kmutex_t *lock, uint16_t depth) {
    if (!lock || !depth) return;
    kmutex_lock(lock);
    task_preempt_disable();
    lock->depth = depth;
    task_preempt_enable();
}

void kmutex_abandon_owner(kmutex_t *lock, uint32_t owner) {
    (void)kmutex_drop_owner(lock, owner);
}

void krwlock_init(krwlock_t *lock) {
    if (lock) kmemset(lock, 0, sizeof(*lock));
}

void krwlock_read_lock(krwlock_t *lock) {
    bool waiting = false;
    if (!lock) return;
    for (;;) {
        task_preempt_disable();
        if (lock->state >= 0 && lock->waiting_writers == 0U) {
            lock->state++;
            g_arm_lock_rw_read_acquires++;
            task_preempt_enable();
            return;
        }
        if (!waiting) {
            lock->contentions++;
            g_arm_lock_rw_contentions++;
            waiting = true;
        }
        task_preempt_enable();
        arm_lock_wait();
    }
}

void krwlock_read_unlock(krwlock_t *lock) {
    if (!lock) return;
    task_preempt_disable();
    if (lock->state > 0) lock->state--;
    task_preempt_enable();
}

void krwlock_write_lock(krwlock_t *lock) {
    uint32_t owner;
    bool waiting = false;
    if (!lock) return;
    owner = klock_current_owner_token();
    for (;;) {
        task_preempt_disable();
        if (lock->writer == owner && lock->writer_depth) {
            if (lock->writer_depth != 0xFFFFU) lock->writer_depth++;
            g_arm_lock_rw_write_acquires++;
            task_preempt_enable();
            return;
        }
        if (!waiting) {
            lock->waiting_writers++;
            waiting = true;
        }
        if (lock->state == 0) {
            lock->state = -1;
            lock->writer = owner;
            lock->writer_depth = 1U;
            if (waiting && lock->waiting_writers) lock->waiting_writers--;
            if (waiting) {
                lock->contentions++;
                g_arm_lock_rw_contentions++;
            }
            g_arm_lock_rw_write_acquires++;
            task_preempt_enable();
            return;
        }
        task_preempt_enable();
        arm_lock_wait();
    }
}

void krwlock_write_unlock(krwlock_t *lock) {
    uint32_t owner;
    if (!lock) return;
    owner = klock_current_owner_token();
    task_preempt_disable();
    if (lock->writer == owner && lock->writer_depth) {
        lock->writer_depth--;
        if (!lock->writer_depth) {
            lock->writer = 0U;
            lock->state = 0;
        }
    }
    task_preempt_enable();
}

void krwlock_abandon_writer(krwlock_t *lock, uint32_t owner) {
    if (!lock || !owner) return;
    task_preempt_disable();
    if (lock->writer == owner && lock->writer_depth) {
        lock->writer_depth = 0U;
        lock->writer = 0U;
        lock->state = 0;
    }
    task_preempt_enable();
}

void klock_get_cpu_stats(uint32_t cpu, klock_cpu_stats_t *out) {
    if (!out) return;
    kmemset(out, 0, sizeof(*out));
    if (cpu != 0U) return;
    out->spin_acquires = g_arm_lock_spin_acquires;
    out->spin_contentions = g_arm_lock_spin_contentions;
    out->mutex_acquires = g_arm_lock_mutex_acquires;
    out->mutex_contentions = g_arm_lock_mutex_contentions;
    out->rw_read_acquires = g_arm_lock_rw_read_acquires;
    out->rw_write_acquires = g_arm_lock_rw_write_acquires;
    out->rw_contentions = g_arm_lock_rw_contentions;
}

uint64_t perfmon_scope_begin(void) { return (uint64_t)rpi3_timer_now_us(); }
void perfmon_block_complete(uint32_t device_type UNUSED, bool write UNUSED,
                            uint8_t sectors UNUSED, bool success UNUSED,
                            uint64_t started UNUSED) { }

bool iso9660_is_mounted(void) { return false; }
bool iso9660_resolve(const char *path UNUSED, iso9660_entry_t *entry UNUSED) { return false; }
bool iso9660_list(const iso9660_entry_t *directory UNUSED,
                  iso9660_entry_t *entries UNUSED, uint32_t max_entries UNUSED,
                  uint32_t *count) { if (count) *count = 0U; return false; }
bool iso9660_read_at(const iso9660_entry_t *entry UNUSED, uint32_t offset UNUSED,
                     void *buffer UNUSED, uint32_t size UNUSED,
                     uint32_t *bytes_read) { if (bytes_read) *bytes_read = 0U; return false; }

bool compat_mode_is_low_memory(void) { return false; }
bool compat_mode_use_compact_language(void) { return false; }

/* Stage 7 GUI bridge: these platform-neutral services are still implemented
 * by x86-era subsystems in the main kernel. Keep the ARM bring-up single-task
 * while the real input/scheduler ports are built in later stages. */
bool compat_mode_use_front_shadow(void) { return false; }
bool compat_mode_prefer_800x600(void) { return false; }
uint32_t pit_get_frequency_hz(void) { return RPI3_TIMER_HZ; }

/* -------------------------------------------------------------------------
 * Stage 8 desktop bridge. These implementations are intentionally tiny and
 * disappear as their real ARM scheduler/input/RTC/resource counterparts land.
 * They let the existing desktop/window/compositor code run unchanged now.
 */
#include "../../../kernel/include/memory.h"
#include "../../../kernel/include/task.h"
#include "../../../kernel/include/rtc.h"
#include "../../../kernel/include/datetime_prefs.h"
#include "../../../kernel/include/clipboard.h"
#include "../../../kernel/include/graphics_resources.h"
#include "../../../kernel/include/about_dialog.h"
#include "../../../gui/gui.h"
#include "../../../gui/image.h"

bool compat_mode_allow_icon_images(void) { return true; }
bool compat_mode_allow_wallpaper(void) { return true; }
bool compat_mode_allow_screensaver(void) { return false; }

uint32_t bk_sys_ticks(void) { return rpi3_timer_ticks() * (1000U / RPI3_TIMER_HZ); }

void kspin_init(kspinlock_t *lock) {
    if (lock) kmemset(lock, 0, sizeof(*lock));
}
static bool arm_spin_try_acquire(volatile uint32_t *state) {
    uint32_t observed, failed;
    __asm__ volatile(
        "ldrex %0, [%2]\n\t"
        "cmp %0, #0\n\t"
        "strexeq %1, %3, [%2]\n\t"
        "movne %1, #1\n\t"
        : "=&r"(observed), "=&r"(failed)
        : "r"(state), "r"(1U)
        : "cc", "memory");
    if (!failed) __asm__ volatile("dmb ish" ::: "memory");
    return failed == 0U;
}
uint32_t kspin_lock_irqsave(kspinlock_t *lock) {
    uint32_t cpsr;
    uint32_t owner = arm_cpu_id() + 1U;
    __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) : : "memory");
    if (lock) {
        if (lock->owner_cpu == owner && lock->depth) {
            if (lock->depth != 0xFFFFU) lock->depth++;
        } else {
            bool contended = false;
            while (!arm_spin_try_acquire(&lock->state)) {
                contended = true;
                __asm__ volatile("yield" ::: "memory");
            }
            if (contended) {
                lock->contentions++;
                g_arm_lock_spin_contentions++;
            }
            lock->owner_cpu = owner;
            lock->depth = 1U;
        }
        g_arm_lock_spin_acquires++;
    }
    return cpsr;
}
bool kspin_try_lock_irqsave(kspinlock_t *lock, uint32_t *flags_out) {
    uint32_t flags;
    uint32_t owner = arm_cpu_id() + 1U;
    bool ok = true;
    __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(flags) : : "memory");
    if (lock) {
        if (lock->owner_cpu == owner && lock->depth) {
            if (lock->depth != 0xFFFFU) lock->depth++;
        } else if (!arm_spin_try_acquire(&lock->state)) {
            ok = false;
            lock->contentions++;
            g_arm_lock_spin_contentions++;
        } else {
            lock->owner_cpu = owner;
            lock->depth = 1U;
        }
        if (ok) g_arm_lock_spin_acquires++;
    }
    if (!ok && (flags & 0x80U) == 0U) __asm__ volatile("cpsie i" ::: "memory");
    if (flags_out) *flags_out = flags;
    return ok;
}
void kspin_unlock_irqrestore(kspinlock_t *lock, uint32_t flags) {
    uint32_t owner = arm_cpu_id() + 1U;
    if (lock && lock->owner_cpu == owner && lock->depth) {
        lock->depth--;
        if (!lock->depth) {
            lock->owner_cpu = 0U;
            __asm__ volatile("dmb ish" ::: "memory");
            lock->state = 0U;
            __asm__ volatile("dsb ish\n\tsev" ::: "memory");
        }
    }
    if ((flags & 0x80U) == 0U) __asm__ volatile("cpsie i" ::: "memory");
}
bool kspin_is_held(const kspinlock_t *lock) {
    return lock && lock->owner_cpu == arm_cpu_id() + 1U && lock->depth != 0U;
}

void perfmon_scope_end(perf_scope_id_t scope, uint64_t started) {
    (void)scope; (void)started;
}
void perfmon_gui_frame(uint32_t dirty_pixels, uint32_t screen_pixels,
                       bool content_changed, bool hardware_cursor,
                       bool gpu_presented) {
    (void)dirty_pixels; (void)screen_pixels; (void)content_changed;
    (void)hardware_cursor; (void)gpu_presented;
}

bool gui_gpu_compositor_enabled(void) { return false; }
bool gui_gpu_compositor_needed(const gui_desktop_t *desktop) {
    (void)desktop; return false;
}
void gui_gpu_compositor_begin_frame(gui_desktop_t *desktop,
                                    const gui_surface_t *surface) {
    (void)desktop; (void)surface;
}
void gui_gpu_compositor_capture_background(const gui_surface_t *surface) {
    (void)surface;
}
void gui_gpu_compositor_capture_window(gui_window_t *window,
                                       const gui_surface_t *surface) {
    (void)window; (void)surface;
}
bool gui_gpu_compositor_publish_viewport(gui_window_t *window,
                                         uint32_t source,
                                         uint16_t width, uint16_t height) {
    (void)window; (void)source; (void)width; (void)height; return false;
}
void gui_gpu_compositor_clear_viewport(gui_window_t *window) {
    (void)window;
}
void gui_gpu_compositor_forget_window(gui_window_t *window) {
    (void)window;
}
bool gui_gpu_compositor_present(gui_desktop_t *desktop,
                                const gui_surface_t *surface,
                                bool draw_cursor) {
    (void)desktop; (void)surface; (void)draw_cursor; return false;
}
bool gui_gpu_compositor_present_cached(gui_desktop_t *desktop,
                                       bool draw_cursor) {
    (void)desktop; (void)draw_cursor; return false;
}
bool gui_compositor_cursor_argb(const gui_desktop_t *desktop,
                                uint32_t *pixels,
                                uint16_t *width, uint16_t *height,
                                int16_t *hot_x, int16_t *hot_y) {
    (void)desktop; (void)pixels; (void)width; (void)height;
    (void)hot_x; (void)hot_y; return false;
}
void gui_gpu_compositor_shutdown(void) { }

static int arm_gui_max_int(int left, int right) { return left > right ? left : right; }
static int arm_gui_min_int(int left, int right) { return left < right ? left : right; }
void bk_gui_surface_draw_image(gui_surface_t *surface, gui_rect_t destination,
                               gui_rect_t clip, const gui_image_t *image) {
    int x0, y0, x1, y1;
    if (!surface || !surface->pixels || !image || !image->pixels ||
        !image->width || !image->height || destination.w <= 0 ||
        destination.h <= 0 || clip.w <= 0 || clip.h <= 0) return;
    x0 = arm_gui_max_int(destination.x, clip.x);
    y0 = arm_gui_max_int(destination.y, clip.y);
    x1 = arm_gui_min_int(destination.x + destination.w, clip.x + clip.w);
    y1 = arm_gui_min_int(destination.y + destination.h, clip.y + clip.h);
    x0 = arm_gui_max_int(x0, surface->clip.x);
    y0 = arm_gui_max_int(y0, surface->clip.y);
    x1 = arm_gui_min_int(x1, surface->clip.x + surface->clip.w);
    y1 = arm_gui_min_int(y1, surface->clip.y + surface->clip.h);
    if (x0 >= x1 || y0 >= y1) return;
    for (int y = y0; y < y1; y++) {
        uint32_t sy = (uint32_t)(y - destination.y) * image->height /
                      (uint32_t)destination.h;
        if (sy >= image->height) sy = image->height - 1U;
        for (int x = x0; x < x1; x++) {
            uint32_t sx = (uint32_t)(x - destination.x) * image->width /
                          (uint32_t)destination.w;
            uint32_t pixel, alpha;
            if (sx >= image->width) sx = image->width - 1U;
            pixel = image->pixels[sy * image->width + sx];
            alpha = pixel >> 24;
            if (alpha == 0U) continue;
            if (alpha < 255U) {
                uint32_t bg = surface->pixels[(uint32_t)y * surface->pitch +
                                              (uint32_t)x];
                uint32_t inv = 255U - alpha;
                uint32_t red = (((pixel >> 16) & 0xFFU) * alpha +
                    ((bg >> 16) & 0xFFU) * inv) / 255U;
                uint32_t green = (((pixel >> 8) & 0xFFU) * alpha +
                    ((bg >> 8) & 0xFFU) * inv) / 255U;
                uint32_t blue = ((pixel & 0xFFU) * alpha +
                    (bg & 0xFFU) * inv) / 255U;
                pixel = (red << 16) | (green << 8) | blue;
            }
            gui_gfx_putpixel(surface, x, y, pixel & 0x00FFFFFFU);
        }
    }
}

void bk_about_detach(gui_window_t *window) { (void)window; }
static char g_arm_clipboard[8192];
static uint32_t g_arm_clipboard_generation;
bool bk_clipboard_set_text(const char *text) {
    uint32_t i = 0U;
    if (!text) return false;
    task_preempt_disable();
    while (text[i] && i + 1U < sizeof(g_arm_clipboard)) {
        g_arm_clipboard[i] = text[i];
        i++;
    }
    g_arm_clipboard[i] = '\0';
    g_arm_clipboard_generation++;
    task_preempt_enable();
    return text[i] == '\0';
}
uint32_t bk_clipboard_get_text(char *buffer, uint32_t capacity) {
    uint32_t i = 0U;
    if (!buffer || !capacity) return (uint32_t)kstrlen(g_arm_clipboard);
    task_preempt_disable();
    while (g_arm_clipboard[i] && i + 1U < capacity) {
        buffer[i] = g_arm_clipboard[i];
        i++;
    }
    buffer[i] = '\0';
    task_preempt_enable();
    return i;
}
void bk_clipboard_clear(void) {
    task_preempt_disable();
    g_arm_clipboard[0] = '\0';
    g_arm_clipboard_generation++;
    task_preempt_enable();
}
uint32_t bk_clipboard_generation(void) { return g_arm_clipboard_generation; }

bool rtc_get_datetime(rtc_datetime_t *datetime) {
    uint32_t seconds;
    if (!datetime) return false;
    seconds = rpi3_timer_ticks() / RPI3_TIMER_HZ;
    datetime->date.year = 1994U;
    datetime->date.month = 1U;
    datetime->date.day = 1U;
    datetime->time.hour = (uint8_t)((seconds / 3600U) % 24U);
    datetime->time.minute = (uint8_t)((seconds / 60U) % 60U);
    datetime->time.second = (uint8_t)(seconds % 60U);
    return true;
}

static bk_datetime_preferences_t g_arm_datetime_prefs = {true, 0};
bool bk_datetime_runtime_preferences_get(bk_datetime_preferences_t *prefs) {
    if (!prefs) return false;
    *prefs = g_arm_datetime_prefs;
    return true;
}
void bk_datetime_runtime_preferences_set(const bk_datetime_preferences_t *prefs) {
    if (prefs) g_arm_datetime_prefs = *prefs;
}

bool program_execute_path(gui_desktop_t *desktop, const char *path) {
    int pid;
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/SHELL.BEX") == 0) {
        shelllauncher_open_from_desktop(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/ABOUT.BEX") == 0) {
        about_open(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/RUNBOX.BEX") == 0) {
        runbox_open_from_desktop(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/CALENDAR.BEX") == 0) {
        calendar_open_from_desktop(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/TEXTEDITOR.BEX") == 0) {
        texteditor_open_from_desktop(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/FILE.BEX") == 0) {
        filebrowser_open_from_desktop(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/IMAGEVIEWER.BEX") == 0) {
        imageviewer_open_from_desktop(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/PAINT.BEX") == 0) {
        paint_open_from_desktop(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX") == 0) {
        processmanager_open_from_desktop(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/CONTROL/CONTROL.BEX") == 0) {
        control_panel_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/CONTROL/MOUSE.CPL") == 0) {
        mouse_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/CONTROL/KEYBOARD.CPL") == 0) {
        keyboard_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/CONTROL/LANGUAGE.CPL") == 0) {
        language_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/SCANDISK.BEX") == 0) {
        scandisk_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/HELP_CENTER.BEX") == 0) {
        help_center_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/FIND_FILES.BEX") == 0) {
        find_files_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/ARCHIVE_MANAGER.BEX") == 0) {
        archive_manager_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/DISK_TOOLS.BEX") == 0) {
        disk_tools_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/NETWORK_STATUS.BEX") == 0) {
        network_status_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/CLIPBOARD_VIEWER.BEX") == 0) {
        clipboard_viewer_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/PERFORMANCE_MONITOR.BEX") == 0) {
        performance_monitor_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/CONTROL/APPEAR.CPL") == 0) {
        appearance_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/CONTROL/DISPLAY.CPL") == 0) {
        display_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/CONTROL/SOUND.CPL") == 0) {
        sound_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/CONTROL/DATETIME.CPL") == 0) {
        datetime_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/CONTROL/SYSTEM.CPL") == 0) {
        system_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/CONTROL/DEVMGR.CPL") == 0) {
        device_manager_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/CONTROL/INTERNET.CPL") == 0) {
        internet_arm_kernel_main(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/CONTROL/MODEM.CPL") == 0) {
        modem_arm_kernel_main(desktop); return true;
    }
    pid = rpi3_elf_spawn(path, NULL);
    if (pid < 0) {
        kprintf("[ARM:PROGRAM] launch rejected path=%s reason=%s\n",
                path ? path : "(null)", rpi3_elf_last_error());
        return false;
    }
    kprintf("[ARM:PROGRAM] native EM_ARM launch pid=%u path=%s\n",
            (uint32_t)pid, path);
    return true;
}
bool program_execute_path_arg(gui_desktop_t *desktop, const char *path,
                              const char *launch_arg) {
    int pid;
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/SHELL.BEX") == 0) {
        shelllauncher_open_from_desktop(desktop); return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/TEXTEDITOR.BEX") == 0) {
        if (launch_arg && launch_arg[0]) texteditor_open(desktop, launch_arg);
        else texteditor_open_from_desktop(desktop);
        return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/FILE.BEX") == 0) {
        if (launch_arg && launch_arg[0]) filebrowser_open_path(desktop, launch_arg);
        else filebrowser_open_from_desktop(desktop);
        return true;
    }
    if (path && kstrcmp(path, "/SYSTEM/PROGRAMS/IMAGEVIEWER.BEX") == 0) {
        if (launch_arg && launch_arg[0]) imageviewer_open(desktop, launch_arg);
        else imageviewer_open_from_desktop(desktop);
        return true;
    }
    if (path && (kstrcmp(path, "/SYSTEM/PROGRAMS/ABOUT.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/PROGRAMS/RUNBOX.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/PROGRAMS/CALENDAR.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/PROGRAMS/PAINT.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/CONTROL/CONTROL.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/CONTROL/MOUSE.CPL") == 0 ||
                 kstrcmp(path, "/SYSTEM/CONTROL/KEYBOARD.CPL") == 0 ||
                 kstrcmp(path, "/SYSTEM/CONTROL/LANGUAGE.CPL") == 0 ||
                 kstrcmp(path, "/SYSTEM/PROGRAMS/SCANDISK.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/PROGRAMS/HELP_CENTER.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/PROGRAMS/FIND_FILES.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/PROGRAMS/ARCHIVE_MANAGER.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/PROGRAMS/DISK_TOOLS.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/PROGRAMS/NETWORK_STATUS.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/PROGRAMS/CLIPBOARD_VIEWER.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/PROGRAMS/PERFORMANCE_MONITOR.BEX") == 0 ||
                 kstrcmp(path, "/SYSTEM/CONTROL/APPEAR.CPL") == 0 ||
                 kstrcmp(path, "/SYSTEM/CONTROL/DISPLAY.CPL") == 0 ||
                 kstrcmp(path, "/SYSTEM/CONTROL/SOUND.CPL") == 0 ||
                 kstrcmp(path, "/SYSTEM/CONTROL/DATETIME.CPL") == 0 ||
                 kstrcmp(path, "/SYSTEM/CONTROL/SYSTEM.CPL") == 0 ||
                 kstrcmp(path, "/SYSTEM/CONTROL/DEVMGR.CPL") == 0 ||
                 kstrcmp(path, "/SYSTEM/CONTROL/INTERNET.CPL") == 0 ||
                 kstrcmp(path, "/SYSTEM/CONTROL/MODEM.CPL") == 0))
        return program_execute_path(desktop, path);
    pid = rpi3_elf_spawn(path, launch_arg);
    if (pid < 0) {
        kprintf("[ARM:PROGRAM] launch rejected path=%s reason=%s\n",
                path ? path : "(null)", rpi3_elf_last_error());
        return false;
    }
    return true;
}

typedef struct arm_program_launch_request {
    gui_desktop_t *desktop;
    char path[VFS_MAX_PATH];
    char argument[2048];
    bool has_argument;
} arm_program_launch_request_t;

static void arm_program_launch_worker(void *opaque) {
    arm_program_launch_request_t *request =
        (arm_program_launch_request_t *)opaque;
    if (request) {
        if (request->has_argument)
            (void)program_execute_path_arg(request->desktop, request->path,
                                           request->argument);
        else
            (void)program_execute_path(request->desktop, request->path);
        kfree(request);
    }
    task_exit();
}

bool program_execute_path_async_arg(gui_desktop_t *desktop, const char *path,
                                    const char *launch_arg) {
    arm_program_launch_request_t *request;
    int pid;
    if (!path || !path[0]) return false;
    request = (arm_program_launch_request_t *)kzalloc(sizeof(*request));
    if (!request) return false;
    request->desktop = desktop;
    kstrncpy(request->path, path, sizeof(request->path) - 1U);
    if (launch_arg) {
        request->has_argument = true;
        kstrncpy(request->argument, launch_arg,
                 sizeof(request->argument) - 1U);
    }
    pid = task_create_kernel("arm-launch", arm_program_launch_worker, request);
    if (pid < 0) {
        kfree(request);
        return false;
    }
    return true;
}

bool program_execute_path_async(gui_desktop_t *desktop, const char *path) {
    return program_execute_path_async_arg(desktop, path, NULL);
}

bool iso9660_mount_default(void) { return false; }

/* Small snprintf subset used by the native desktop labels. */
static uint32_t arm_append_char(char *out, uint32_t cap, uint32_t pos, char c) {
    if (out && cap && pos + 1U < cap) out[pos] = c;
    return pos + 1U;
}
static uint32_t arm_append_string(char *out, uint32_t cap, uint32_t pos,
                                  const char *s) {
    if (!s) s = "(null)";
    while (*s) pos = arm_append_char(out, cap, pos, *s++);
    return pos;
}
static uint32_t arm_append_uint(char *out, uint32_t cap, uint32_t pos,
                                uint32_t value, uint32_t base) {
    char temp[16]; uint32_t n = 0U;
    const char *digits = "0123456789abcdef";
    if (!value) return arm_append_char(out, cap, pos, '0');
    while (value && n < sizeof(temp)) { temp[n++] = digits[value % base]; value /= base; }
    while (n) pos = arm_append_char(out, cap, pos, temp[--n]);
    return pos;
}
int vsnprintf(char *out, size_t capacity, const char *fmt, va_list args) {
    uint32_t pos = 0U;
    while (fmt && *fmt) {
        if (*fmt != '%') { pos = arm_append_char(out, capacity, pos, *fmt++); continue; }
        fmt++;
        if (*fmt == '%') { pos = arm_append_char(out, capacity, pos, '%'); fmt++; continue; }
        while (*fmt == '0' || (*fmt >= '1' && *fmt <= '9')) fmt++;
        switch (*fmt) {
            case 's': pos = arm_append_string(out, capacity, pos, va_arg(args, const char *)); break;
            case 'c': pos = arm_append_char(out, capacity, pos, (char)va_arg(args, int)); break;
            case 'u': pos = arm_append_uint(out, capacity, pos, va_arg(args, uint32_t), 10U); break;
            case 'x': pos = arm_append_uint(out, capacity, pos, va_arg(args, uint32_t), 16U); break;
            case 'd': case 'i': {
                int32_t v = va_arg(args, int32_t);
                if (v < 0) { pos = arm_append_char(out, capacity, pos, '-'); v = -v; }
                pos = arm_append_uint(out, capacity, pos, (uint32_t)v, 10U);
                break;
            }
            default: pos = arm_append_char(out, capacity, pos, '%');
                     if (*fmt) pos = arm_append_char(out, capacity, pos, *fmt); break;
        }
        if (*fmt) fmt++;
    }
    if (out && capacity) out[pos < capacity ? pos : capacity - 1U] = '\0';
    return (int)pos;
}
int snprintf(char *out, size_t capacity, const char *fmt, ...) {
    va_list args; int result;
    va_start(args, fmt);
    result = vsnprintf(out, capacity, fmt, args);
    va_end(args);
    return result;
}
