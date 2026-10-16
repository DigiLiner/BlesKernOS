#include "../../sdk/include/bleskernos_api.h"

#define BUTTON_COUNT 16U

static bk_gui_window_t *g_window;
static uint32_t g_ids[BUTTON_COUNT];
static const char *g_labels[BUTTON_COUNT] = {
    "7", "8", "9", "/", "4", "5", "6", "*",
    "1", "2", "3", "-", "C", "0", "=", "+"
};
static int32_t g_left;
static int32_t g_value;
static char g_operation;
static bool g_entering;
static char g_display[24] = "0";

static void number_text(int32_t value) {
    char reverse[16]; uint32_t count = 0U, out = 0U;
    uint32_t number = value < 0 ? (uint32_t)(-(value + 1)) + 1U : (uint32_t)value;
    if (value < 0) g_display[out++] = '-';
    if (!number) reverse[count++] = '0';
    while (number && count < sizeof(reverse)) {
        reverse[count++] = (char)('0' + number % 10U); number /= 10U;
    }
    while (count) g_display[out++] = reverse[--count];
    g_display[out] = '\0';
}

static void calculate(void) {
    if (g_operation == '+') g_left += g_value;
    else if (g_operation == '-') g_left -= g_value;
    else if (g_operation == '*') g_left *= g_value;
    else if (g_operation == '/' && g_value) g_left /= g_value;
    else if (!g_operation) g_left = g_value;
    g_value = g_left; number_text(g_value);
}

static void press(const char *label) {
    if (label[0] >= '0' && label[0] <= '9' && !label[1]) {
        if (!g_entering) { g_value = 0; g_entering = true; }
        if (g_value < 100000000) g_value = g_value * 10 + label[0] - '0';
        number_text(g_value);
    } else if (label[0] == 'C') {
        g_left = g_value = 0; g_operation = 0; g_entering = false; number_text(0);
    } else if (label[0] == '=') {
        calculate(); g_operation = 0; g_entering = false;
    } else {
        if (g_entering || !g_operation) calculate();
        g_operation = label[0]; g_entering = false;
    }
    bk_gui_window_invalidate(g_window);
}

static void button_callback(bk_gui_window_t *window, uint32_t id) {
    uint32_t i; (void)window;
    for (i = 0U; i < BUTTON_COUNT; ++i)
        if (g_ids[i] == id) { press(g_labels[i]); return; }
}

static void paint_callback(bk_gui_window_t *window, bk_gui_surface_t *surface,
                           void *context) {
    bk_gui_rect_t content;
    uint16_t width;
    (void)context;
    if (!bk_gui_window_content_rect(window, &content)) return;
    bk_gui_surface_fill_rect(surface,
        (bk_gui_rect_t){content.x + 8, content.y + 7, content.w - 16, 30},
        0x00F8FFF0U);
    bk_gui_surface_draw_rect(surface,
        (bk_gui_rect_t){content.x + 8, content.y + 7, content.w - 16, 30},
        0x00405040U);
    width = bk_gui_text_width(g_display);
    bk_gui_surface_draw_text(surface, content.x + content.w - 16 - width,
                             content.y + 17, g_display,
                             0x00102020U, 0x00F8FFF0U, false);
}

static bool event_callback(bk_gui_window_t *window,
                           const bk_gui_event_t *event, void *context) {
    char key[2]; (void)window; (void)context;
    if (!event) return false;
    if (event->type != BK_GUI_EVENT_KEY) return false;
    key[0] = event->key; key[1] = '\0';
    if ((key[0] >= '0' && key[0] <= '9') || key[0] == '+' || key[0] == '-' ||
        key[0] == '*' || key[0] == '/') press(key);
    else if (key[0] == '\n') press("=");
    else if (key[0] == 27) press("C");
    else return false;
    return true;
}

void bleskernos_program_main(void *unused) {
    bk_gui_desktop_t *desktop = bk_gui_desktop();
    uint32_t i;
    (void)unused;
    if (!desktop) return;
    g_window = bk_gui_create_window(desktop, 170, 55, 238, 250, "Calculadora");
    if (!g_window) return;
    bk_gui_set_window_content(g_window, paint_callback, NULL);
    bk_gui_set_window_event_handler(g_window, event_callback, NULL);
    bk_gui_set_window_min_size(g_window, 238, 250);
    for (i = 0U; i < BUTTON_COUNT; ++i) {
        bk_gui_widget_t *button = bk_gui_create_button(desktop, g_window,
            (bk_gui_rect_t){8 + (int)(i % 4U) * 55,
                            43 + (int)(i / 4U) * 37, 49, 30},
            g_labels[i], button_callback);
        g_ids[i] = bk_gui_widget_id(button);
    }
    bk_gui_window_invalidate(g_window);
    while (bk_gui_window_is_open(g_window)) bk_sys_sleep_ms(40U);
    bk_gui_destroy_window(desktop, g_window);
    g_window = NULL;
}
