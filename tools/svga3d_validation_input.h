/* Included only by the disposable validation kernel, never the normal OS. */
static gui_window_t *svga_validation_window(gui_desktop_t *desk) {
    for (gui_window_t *win = desk->first_window; win; win = win->next)
        if (win->listed && win->visible &&
            kstrncmp(win->title, "Tux Racer", 9U) == 0) return win;
    return NULL;
}

static void svga_validation_input(void *arg UNUSED) {
    gui_event_t event;
    const char keys[] = {13, 'p', 13, 13};
    bool ready = false;
    for (uint32_t n = 0U; n < 120U && !ready; n++) {
        kernel_domains_enter(KDOMAIN_GUI | KDOMAIN_GFX);
        gui_window_t *win = svga_validation_window(gui_get_desktop());
        ready = win && (win->gpu_view_visible || win->stream_ready);
        kernel_domains_exit(KDOMAIN_GUI | KDOMAIN_GFX);
        task_sleep(pit_get_frequency_hz());
    }
    if (!ready) {
        kprintf("[VALIDATION] FAIL Tux did not publish a frame in 120 seconds\n");
        task_exit();
        return;
    }
    for (uint32_t i = 0U; i < sizeof(keys); i++) {
        task_sleep(pit_get_frequency_hz() * 8U);
        kernel_domains_enter(KDOMAIN_GUI | KDOMAIN_GFX);
        gui_desktop_t *desk = gui_get_desktop();
        gui_window_t *win = svga_validation_window(desk);
        if (win) {
            gui_desktop_focus_window(desk, win);
            kmemset(&event, 0, sizeof(event));
            event.type = GUI_EVENT_KEY; event.key = keys[i];
            gui_desktop_handle_event(desk, &event);
            event.type = GUI_EVENT_KEY_UP;
            gui_desktop_handle_event(desk, &event);
            kprintf("[VALIDATION] key=%u owner=%u gpu=%u ticks=%u\n",
                    (uint32_t)keys[i], win->owner_pid,
                    win->gpu_view_surface, pit_get_ticks());
        }
        kernel_domains_exit(KDOMAIN_GUI | KDOMAIN_GFX);
    }
    task_sleep(pit_get_frequency_hz() * 20U);
    /* Course loading may outlast the menu keystrokes. Skip the intro only
     * after it has had time to load, then exercise a click during racing. */
    kernel_domains_enter(KDOMAIN_GUI | KDOMAIN_GFX);
    gui_desktop_t *desk = gui_get_desktop();
    gui_window_t *win = svga_validation_window(desk);
    if (win) {
        gui_desktop_focus_window(desk, win);
        kmemset(&event, 0, sizeof(event));
        event.type = GUI_EVENT_KEY; event.key = 13;
        gui_desktop_handle_event(desk, &event);
        event.type = GUI_EVENT_KEY_UP;
        gui_desktop_handle_event(desk, &event);
        kprintf("[VALIDATION] skip intro ticks=%u\n", pit_get_ticks());
    }
    kernel_domains_exit(KDOMAIN_GUI | KDOMAIN_GFX);
    task_sleep(pit_get_frequency_hz() * 10U);
    kernel_domains_enter(KDOMAIN_GUI | KDOMAIN_GFX);
    desk = gui_get_desktop();
    win = svga_validation_window(desk);
    if (win) {
        kmemset(&event, 0, sizeof(event));
        event.x = win->bounds.x + win->bounds.w / 2;
        event.y = win->bounds.y + win->bounds.h / 2;
        event.type = GUI_EVENT_MOUSE_MOVE;
        gui_desktop_handle_event(desk, &event);
        event.type = GUI_EVENT_MOUSE_DOWN; event.button = 1U; event.buttons = 1U;
        gui_desktop_handle_event(desk, &event);
        event.type = GUI_EVENT_MOUSE_UP; event.buttons = 0U;
        gui_desktop_handle_event(desk, &event);
        kprintf("[VALIDATION] click inside Tux at %d,%d ticks=%u\n",
                event.x, event.y, pit_get_ticks());
    }
    kernel_domains_exit(KDOMAIN_GUI | KDOMAIN_GFX);
    for (uint32_t n = 0U; n < 6U; n++) {
        task_sleep(pit_get_frequency_hz() * 10U);
        kprintf("[VALIDATION] alive after click ticks=%u\n", pit_get_ticks());
    }
    kernel_domains_enter(KDOMAIN_GUI | KDOMAIN_GFX);
    win = svga_validation_window(gui_get_desktop());
    if (win) gui_window_close(win);
    kernel_domains_exit(KDOMAIN_GUI | KDOMAIN_GFX);
    task_sleep(pit_get_frequency_hz() * 10U);
    kernel_domains_enter(KDOMAIN_GUI | KDOMAIN_GFX);
    bool closed = svga_validation_window(gui_get_desktop()) == NULL;
    kernel_domains_exit(KDOMAIN_GUI | KDOMAIN_GFX);
    if (closed) {
        int pid = elf_spawn_program_ex("/SYSTEM/PROGRAMS/TUXRACER.BEX", gui_get_desktop(), NULL);
        kprintf("[VALIDATION] reopened Tux pid=%d ticks=%u\n", pid, pit_get_ticks());
    } else {
        kprintf("[VALIDATION] FAIL Tux did not close\n");
    }
    task_exit();
}
