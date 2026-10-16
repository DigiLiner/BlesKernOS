#include "kernel/include/api.h"
#include "programs/programs.h"

/* Private Vision import; settings remain shared with the existing CPL ABI. */
void screensaver_initialize(gui_desktop_t *desktop);
static bool started;

static void screensaverd_main(void *arg) {
    gui_desktop_t *desktop = arg;
    uint32_t last_launch_activity = 0xFFFFFFFFU;
    kprintf("[VISION:SAVER] Ring 3 worker started\n");
    while (!bk_proc_exit_requested()) {
        uint32_t now = bk_sys_ticks();
        uint32_t freq = bk_sys_tick_frequency();
        uint32_t last_input = bk_gui_get_last_input_tick();
        uint64_t timeout = (uint64_t)screensaver_get_timeout_seconds() * freq;
        if (freq && screensaver_is_enabled() &&
            last_input != last_launch_activity && (uint64_t)(now-last_input) >= timeout) {
            last_launch_activity = last_input;
            (void)screensaver_preview(desktop);
        }
        bk_sys_sleep_ticks(freq / 2U ? freq / 2U : 1U);
    }
    started = false;
    bk_proc_exit();
}

void screensaverd_install(gui_desktop_t *desktop) {
    if (!desktop || started) return;
    screensaver_initialize(desktop);
    started = true;
    if (bk_proc_spawn_thread("screensaverd", screensaverd_main, desktop) < 0) {
        started = false;
        kprintf("[VISION:SAVER] ERROR: cannot start Ring 3 worker\n");
    }
}
