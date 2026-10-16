#include "../../kernel/include/api.h"
void bleskernos_program_main(gui_desktop_t *desktop UNUSED) {
    kbd_key_event_t event;
    while (bk_input_key_event(&event)) {
    }
    mouse_state_t before, after;
    bk_input_mouse(&before);
    kprintf("[DRIVERTEST] USBINPUT WAIT packets=%u\n", before.packets);
    uint32_t start = bk_sys_ticks(), limit = bk_sys_tick_frequency() * 20U;
    bool pressed = false, released = false, moved = false;
    while (bk_sys_ticks() - start < limit) {
        while (bk_input_key_event(&event)) {
            if (event.key == 'a') {
                if (event.pressed)
                    pressed = true;
                else
                    released = true;
            }
        }
        if (bk_input_mouse(&after) && after.packets > before.packets &&
            (after.x != before.x || after.y != before.y))
            moved = true;
        if (pressed && released && moved)
            break;
        bk_sys_sleep_ms(10U);
    }
    kprintf("[DRIVERTEST] USBINPUT press=%u release=%u mouse=%u\n", pressed, released, moved);
    kprintf("[DRIVERTEST] DONE result=%s\n", pressed && released && moved ? "PASS" : "FAIL");
}
