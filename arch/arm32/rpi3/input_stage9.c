#include "input_stage9.h"
#include "usb_dwc2_hid.h"
#include "uart.h"
#include "../../../kernel/include/keyboard.h"
#include "../../../kernel/include/mouse.h"
#include "../../../kernel/include/vga.h"

static uint8_t g_escape_state;
static bool g_usb_ready;

static void inject_tap(uint8_t key) {
    if (!key) return;
    kbd_inject_key(key, true);
    kbd_inject_key(key, false);
}

static void serial_keyboard_byte(uint8_t c) {
    if (g_escape_state == 1U) {
        if (c == '[') { g_escape_state = 2U; return; }
        inject_tap(KEY_ESCAPE);
        g_escape_state = 0U;
    } else if (g_escape_state == 2U) {
        if (c == 'A') inject_tap(KEY_UP);
        else if (c == 'B') inject_tap(KEY_DOWN);
        else if (c == 'C') inject_tap(KEY_RIGHT);
        else if (c == 'D') inject_tap(KEY_LEFT);
        else if (c == 'H') inject_tap(KEY_HOME);
        else if (c == 'F') inject_tap(KEY_END);
        g_escape_state = 0U;
        return;
    }

    if (c == 0x1BU) { g_escape_state = 1U; return; }
    if (c == '\r' || c == '\n') inject_tap(KEY_ENTER);
    else if (c == 0x7FU || c == 0x08U) inject_tap(KEY_BACKSPACE);
    else if (c == '\t') inject_tap(KEY_TAB);
    else if (c >= 0x20U && c <= 0x7EU) inject_tap(c);
}

int rpi3_stage9_input_init(void) {
    int rc;
    kbd_init();
    mouse_init();
    g_escape_state = 0U;
    rc = rpi3_dwc2_hid_init();
    g_usb_ready = rc == 0;
    if (g_usb_ready)
        kprintf("[INPUT] USB HID + Mini UART keyboard bridge online\n");
    else
        kprintf("[INPUT] DWC2 unavailable; Mini UART keyboard fallback online\n");
    return 0; /* Serial fallback keeps Stage 9 usable even without USB. */
}

void rpi3_stage9_input_poll(void) {
    char c;
    uint32_t budget = 16U;
    if (g_usb_ready) rpi3_dwc2_hid_poll();
    while (budget-- && rpi3_uart_try_getc(&c))
        serial_keyboard_byte((uint8_t)c);
}

bool rpi3_stage9_usb_ready(void) { return g_usb_ready; }
