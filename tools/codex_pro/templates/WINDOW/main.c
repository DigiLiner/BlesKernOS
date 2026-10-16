#include <bleskernos.h>
#include <bleskernos_api.h>

void bleskernos_program_main(bk_gui_desktop_t *desktop)
{
    bk_gui_window_t *window;

    if (!desktop)
        return;

    window = bk_gui_create_window(desktop, 96, 72, 420, 260, "Codex Project");
    if (!window)
        return;

    while (bk_gui_window_is_open(window))
        bk_sys_sleep_ms(20U);
}
