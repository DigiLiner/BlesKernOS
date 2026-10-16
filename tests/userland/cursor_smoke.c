#include "../../kernel/include/api.h"
void bleskernos_program_main(gui_desktop_t *proxy) {
    (void)proxy;
    bk_sys_sleep_ms(4000);
    bk_gui_cursor_set_style(GUI_CURSOR_ARROW);
    bk_sys_sleep_ms(500);
    gui_desktop_t *d=bk_gui_get_desktop();
    kprintf("[CURSORTEST] ARROW x=%d y=%d\n", d->mouse_x,d->mouse_y);
    bk_sys_sleep_ms(2000);
    bk_gui_cursor_set_style(GUI_CURSOR_WAIT);
    kprintf("[CURSORTEST] WAIT x=%d y=%d\n", d->mouse_x,d->mouse_y);
    bk_sys_sleep_ms(6500);
    bk_gui_cursor_set_style(GUI_CURSOR_PROGRESS);
    kprintf("[CURSORTEST] PROGRESS x=%d y=%d\n", d->mouse_x,d->mouse_y);
    bk_sys_sleep_ms(5500);
    bk_gui_cursor_set_style(GUI_CURSOR_ARROW);
    kprintf("[CURSORTEST] DONE\n");
    while(!bk_proc_exit_requested())bk_sys_sleep_ms(100);
}
