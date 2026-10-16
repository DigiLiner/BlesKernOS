#include "uart.h"
#include "../../../kernel/include/vga.h"
#include "../../../kernel/include/task.h"
#include "../../../kernel/include/klock.h"

static vga_output_char_t g_output;
static vga_output_clear_t g_clear;
static void *g_context;
static uint32_t g_route;
static int g_cursor_x, g_cursor_y;

#define VGA_CONSOLE_INPUT_CAPACITY 512U
static char g_console_input[VGA_CONSOLE_INPUT_CAPACITY];
static uint32_t g_console_input_head;
static uint32_t g_console_input_tail;
static kspinlock_t g_console_input_lock = KSPINLOCK_INITIALIZER;

void vga_init(void) { g_cursor_x = g_cursor_y = 0; }
void vga_clear(void) {
    if (g_clear && g_route && task_current_console_route() == g_route)
        g_clear(g_context);
    g_cursor_x = g_cursor_y = 0;
}
void vga_set_color(vga_color_t fg, vga_color_t bg) { (void)fg; (void)bg; }
void vga_putchar(char c) {
    rpi3_uart_putc(c);
    if (g_output && g_route && task_current_console_route() == g_route) {
        g_output(c, g_context);
        return;
    }
    if (c == '\n') { g_cursor_x = 0; ++g_cursor_y; }
    else if (c == '\r') g_cursor_x = 0;
    else if (c == '\b') { if (g_cursor_x) --g_cursor_x; }
    else if (c == '\t') g_cursor_x = (g_cursor_x + 4) & ~3;
    else ++g_cursor_x;
}
void vga_puts(const char *text) { while (text && *text) vga_putchar(*text++); }
void vga_puthex(uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";
    int shift;
    vga_puts("0x");
    for (shift = 28; shift >= 0; shift -= 4) vga_putchar(hex[(value >> shift) & 15U]);
}
void vga_putdec(uint32_t value) {
    char buf[11]; uint32_t n = 0U;
    if (!value) { vga_putchar('0'); return; }
    while (value && n < sizeof(buf)) { buf[n++] = (char)('0' + value % 10U); value /= 10U; }
    while (n) vga_putchar(buf[--n]);
}
void vga_putint(int32_t value) {
    if (value < 0) { vga_putchar('-'); vga_putdec((uint32_t)(-(value + 1)) + 1U); }
    else vga_putdec((uint32_t)value);
}
void vga_set_cursor(int x, int y) { g_cursor_x = x; g_cursor_y = y; }
void vga_get_cursor(int *x, int *y) { if (x) *x = g_cursor_x; if (y) *y = g_cursor_y; }
void vga_console_input_reset(void) {
    uint32_t flags = kspin_lock_irqsave(&g_console_input_lock);
    g_console_input_head = 0U;
    g_console_input_tail = 0U;
    kspin_unlock_irqrestore(&g_console_input_lock, flags);
}

bool vga_console_input_push(char character) {
    uint32_t flags;
    uint32_t next;
    bool stored = false;

    if (!g_output || g_route == 0U) return false;
    flags = kspin_lock_irqsave(&g_console_input_lock);
    next = (g_console_input_head + 1U) % VGA_CONSOLE_INPUT_CAPACITY;
    if (next != g_console_input_tail) {
        g_console_input[g_console_input_head] = character;
        g_console_input_head = next;
        stored = true;
    }
    kspin_unlock_irqrestore(&g_console_input_lock, flags);
    return stored;
}

int32_t vga_console_input_read(char *buffer, uint32_t length) {
    uint32_t route = task_current_console_route();
    uint32_t copied;

    if ((!buffer && length) || !route || route != g_route || !g_output)
        return -1;
    if (!length) return 0;

    for (;;) {
        uint32_t flags = kspin_lock_irqsave(&g_console_input_lock);
        copied = 0U;
        while (copied < length && g_console_input_tail != g_console_input_head) {
            buffer[copied++] = g_console_input[g_console_input_tail];
            g_console_input_tail = (g_console_input_tail + 1U) %
                                   VGA_CONSOLE_INPUT_CAPACITY;
        }
        kspin_unlock_irqrestore(&g_console_input_lock, flags);
        if (copied) return (int32_t)copied;
        if (task_current_console_route() != route || g_route != route || !g_output)
            return -1;
        task_yield();
    }
}

void vga_set_output_sink(vga_output_char_t output, vga_output_clear_t clear,
                         void *context) {
    g_output = output; g_clear = clear; g_context = context;
    vga_console_input_reset();
    if (output) {
        g_route = task_current_process_id();
        task_set_current_console_route(g_route);
    } else {
        g_route = 0U;
        task_set_current_console_route(0U);
    }
}
uint32_t vga_com1_log_size(void) { return 0U; }
uint32_t vga_com1_log_dropped(void) { return 0U; }
bool vga_com1_log_save(const char *path) { (void)path; return false; }
