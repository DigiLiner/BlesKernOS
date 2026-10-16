#include "../kernel/include/api.h"

#define TERM_MAX_COLS 96
#define TERM_SCROLLBACK_LINES 2048
#define TERM_LINE_HEIGHT 10
#define TERM_HISTORY_LINES 32

typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    gui_event_queue_t events;
    char lines[TERM_SCROLLBACK_LINES][TERM_MAX_COLS + 1];
    char input[SHELL_MAX_CMD];
    char command[SHELL_MAX_CMD];
    char command_cwd[VFS_MAX_PATH];
    /* Linea canonica que pertenece al programa foreground, no al shell. */
    char child_input[SHELL_MAX_CMD];
    uint32_t child_input_len;
    uint32_t input_len;
    uint32_t output_col;
    uint32_t line_count;
    uint32_t line_head;
    uint32_t scroll_row;
    uint32_t visible_rows;
    char history[TERM_HISTORY_LINES][SHELL_MAX_CMD];
    uint32_t history_count;
    uint32_t history_index;
    gui_scrollbar_drag_t scrollbar_drag;
    bool execute_requested;
    volatile bool command_running;
    volatile bool command_finished;
    volatile bool command_close_requested;
} terminal_state_t;

typedef struct {
    gui_rect_t background;
    gui_rect_t output;
    gui_rect_t scrollbar;
    gui_rect_t input;
    uint32_t columns;
    uint32_t output_rows;
    uint32_t input_rows;
} terminal_layout_t;

static terminal_state_t *g_terminal;

static char *terminal_line(terminal_state_t *st, uint32_t logical) {
    if (!st || logical >= st->line_count) return NULL;
    return st->lines[(st->line_head + logical) % TERM_SCROLLBACK_LINES];
}

static const char *terminal_line_const(const terminal_state_t *st,
                                       uint32_t logical) {
    if (!st || logical >= st->line_count) return NULL;
    return st->lines[(st->line_head + logical) % TERM_SCROLLBACK_LINES];
}

static void terminal_prompt(char *out, uint32_t capacity) {
    const char *cwd = bk_file_getcwd();
    if (!out || !capacity) return;
    bk_runtime_strncpy(out, "bles@bleskernos:", capacity - 1U);
    out[capacity - 1U] = '\0';
    if (cwd) bk_runtime_strcat(out, cwd);
    bk_runtime_strcat(out, "> ");
}

static uint32_t terminal_columns(const terminal_state_t *st) {
    int width = st && st->window ? st->window->bounds.w - 34 : 400;
    uint32_t columns = width > 16 ? (uint32_t)(width / 8) : 1U;
    if (columns > TERM_MAX_COLS) columns = TERM_MAX_COLS;
    return columns ? columns : 1U;
}

static uint32_t terminal_input_total_rows(const terminal_state_t *st,
                                          uint32_t columns) {
    uint32_t rows = 1;
    uint32_t column = 0U;
    char prompt[VFS_MAX_PATH + 24];
    if (!columns) columns = 1;
    terminal_prompt(prompt, sizeof(prompt));
    for (uint32_t i = 0; prompt[i]; i++) {
        if (column >= columns) { rows++; column = 0; }
        column++;
    }
    for (uint32_t i = 0; st && i < st->input_len; i++) {
        if (st->input[i] == '\n') { rows++; column = 0; continue; }
        if (column >= columns) { rows++; column = 0; }
        column++;
    }
    return rows;
}

static void terminal_build_layout(const terminal_state_t *st,
                                  terminal_layout_t *layout) {
    gui_rect_t content = bk_gui_window_content_rect_raw(st->window);
    uint32_t total_rows;
    uint32_t screen_rows;
    /* La terminal usa todo el cliente: negro puro, sin margen coloreado. */
    layout->background = content;
    layout->columns = terminal_columns(st);
    screen_rows = layout->background.h > 8
                ? (uint32_t)((layout->background.h - 8) / TERM_LINE_HEIGHT) : 1U;
    total_rows = terminal_input_total_rows(st, layout->columns);
    layout->input_rows = total_rows;
    if (layout->input_rows > screen_rows)
        layout->input_rows = screen_rows;
    if (!layout->input_rows) layout->input_rows = 1;
    /* El prompt vive en una zona propia. Antes compartía la última fila de
     * salida y los logs largos terminaban dibujándose debajo/encima del prompt. */
    layout->output_rows = screen_rows > layout->input_rows
                        ? screen_rows - layout->input_rows : 1U;
    layout->output = (gui_rect_t){layout->background.x + 4,
        layout->background.y + 3,
        layout->background.w - GUI_SCROLLBAR_SIZE - 10,
        (int)layout->output_rows * TERM_LINE_HEIGHT};
    layout->scrollbar = (gui_rect_t){
        layout->background.x + layout->background.w - GUI_SCROLLBAR_SIZE,
        layout->background.y, GUI_SCROLLBAR_SIZE, layout->background.h};
    layout->input = (gui_rect_t){layout->output.x,
        layout->output.y + (int)layout->output_rows * TERM_LINE_HEIGHT,
        layout->output.w,
        (int)layout->input_rows * TERM_LINE_HEIGHT};
}

static void terminal_clamp_scroll(terminal_state_t *st,
                                  uint32_t visible_rows) {
    uint32_t maximum = st->line_count > visible_rows
                     ? st->line_count - visible_rows : 0;
    if (st->scroll_row > maximum) st->scroll_row = maximum;
}

static void terminal_newline(terminal_state_t *st) {
    uint32_t visible;
    bool follow;
    if (!st) return;
    visible = st->visible_rows ? st->visible_rows : 10U;
    follow = st->scroll_row + visible >= st->line_count;
    if (st->line_count < TERM_SCROLLBACK_LINES) {
        st->line_count++;
    } else {
        /* Ring buffer: no copiar 2K líneas por cada newline cuando el log
         * está lleno. scroll_row sigue siendo un índice lógico visible. */
        st->line_head = (st->line_head + 1U) % TERM_SCROLLBACK_LINES;
        if (st->scroll_row) st->scroll_row--;
    }
    bk_runtime_memset(terminal_line(st, st->line_count - 1U), 0,
                      TERM_MAX_COLS + 1);
    st->output_col = 0;
    if (follow) st->scroll_row = st->line_count > visible
                               ? st->line_count - visible : 0;
}

static void terminal_output_char(char c, void *context) {
    terminal_state_t *st = (terminal_state_t *)context;
    char *line;
    if (!st || c == '\r') return;

    /* The console sink is written by the currently running command task while
       the GUI task paints the same scrollback.  On the preemptive ARM port a
       timer could previously switch in the middle of line_count/head/output_col
       publication, leaving the painter with a mixed ring-buffer snapshot. */
    bk_proc_critical_enter();
    if (c == '\n') {
        terminal_newline(st);
        if (st->window) bk_gui_window_invalidate(st->window);
        bk_proc_critical_leave();
        return;
    }
    if (c == '\b') {
        line = terminal_line(st, st->line_count - 1U);
        if (st->output_col && line) line[--st->output_col] = '\0';
        bk_proc_critical_leave();
        return;
    }
    if (c < 32) {
        bk_proc_critical_leave();
        return;
    }
    if (st->output_col >= terminal_columns(st)) terminal_newline(st);
    line = terminal_line(st, st->line_count - 1U);
    if (line) {
        line[st->output_col++] = c;
        line[st->output_col] = '\0';
    }
    bk_proc_critical_leave();
}

static void terminal_output_clear(void *context) {
    terminal_state_t *st = (terminal_state_t *)context;
    if (!st) return;
    bk_proc_critical_enter();
    bk_runtime_memset(st->lines, 0, sizeof(st->lines));
    st->output_col = 0;
    st->line_count = 1;
    st->line_head = 0;
    st->scroll_row = 0;
    bk_proc_critical_leave();
}

static void terminal_write(terminal_state_t *st UNUSED, const char *text) {
    /* Pasar también el texto generado por el propio frontend por la consola.
     * Así prompt/comando y salida del hijo siguen exactamente la misma ruta
     * visual y el mismo espejo COM1 de vga_putchar(). */
    while (text && *text) bk_console_putchar(*text++);
}

/* Stage 12.4 executes commands in the Terminal task itself.  Terminal is
 * already a dedicated kernel task, while GUI and USB input live in separate
 * scheduler tasks, so an extra "terminal-command" worker only adds shared-
 * state and task-exit races without improving responsiveness. */
static void terminal_finish_command(terminal_state_t *st) {
    if (!st) return;
    st->command_close_requested = bk_shell_take_exit_request();
    st->command_running = false;
    if (st->command_close_requested && st->window) {
        st->command_close_requested = false;
        bk_gui_window_close(st->window);
    }
    if (st->window) bk_gui_window_invalidate(st->window);
}

static void terminal_execute(terminal_state_t *st) {
    char prompt[VFS_MAX_PATH + 24];
    uint32_t visible;
    if (!st || st->command_running) return;
    visible = st->visible_rows ? st->visible_rows : 10U;
    st->scroll_row = st->line_count > visible
                   ? st->line_count - visible : 0;
    terminal_prompt(prompt, sizeof(prompt));
    terminal_write(st, prompt);
    terminal_write(st, st->input);
    terminal_output_char('\n', st);

    if (st->input[0]) {
        uint32_t slot = st->history_count % TERM_HISTORY_LINES;
        bk_runtime_strncpy(st->history[slot], st->input, SHELL_MAX_CMD - 1U);
        st->history[slot][SHELL_MAX_CMD - 1U] = '\0';
        st->history_count++;
    }
    st->history_index = st->history_count;
    bk_runtime_strncpy(st->command, st->input, sizeof(st->command) - 1U);
    st->command[sizeof(st->command) - 1U] = '\0';
    st->input_len = 0;
    st->input[0] = '\0';
    st->execute_requested = false;

    if (!st->command[0]) {
        if (st->window) bk_gui_window_invalidate(st->window);
        return;
    }

    st->command_finished = false;
    st->command_close_requested = false;
    st->command_cwd[0] = '\0';
    st->child_input_len = 0U;
    st->child_input[0] = '\0';
    bk_console_input_reset();
    st->command_running = true;
#if defined(BLES_ARCH_ARM32_RPI3)
    kprintf("[ARM:SHELL] command begin: %s\n", st->command);
#endif
    /* Synchronous only with respect to the Terminal task.  bk_shell_execute_line
       waits/yields while the child USER task runs, so GUI, input and heartbeat
       continue to be scheduled normally. */
    bk_shell_execute_line(st->command);
#if defined(BLES_ARCH_ARM32_RPI3)
    kprintf("[ARM:SHELL] command returned: %s\n", st->command);
#endif
    terminal_finish_command(st);
}

static void terminal_content(gui_window_t *window UNUSED,
                             gui_surface_t *surface, void *context) {
    terminal_state_t *st = (terminal_state_t *)context;
    terminal_layout_t layout;
    gui_scrollbar_t scrollbar;
    if (!st || !st->window || !st->window->visible) return;
    terminal_build_layout(st, &layout);
    st->visible_rows = layout.output_rows;
    terminal_clamp_scroll(st, layout.output_rows);
    /* El clamp puede cambiar la fila visible de la línea actual. */
    terminal_build_layout(st, &layout);
    bk_gui_gfx_fill_rect(surface, layout.background, 0x00000000);
    for (uint32_t i = 0; i < layout.output_rows; i++) {
        uint32_t line = st->scroll_row + i;
        if (line >= st->line_count) break;
        const char *text = terminal_line_const(st, line);
        if (!text) break;
        bk_gui_font_draw_string_clipped(surface, layout.output.x,
            layout.output.y + (int)i * TERM_LINE_HEIGHT,
            text, 0x00FFFFFF, layout.output);
    }

    {
        char prompt[VFS_MAX_PATH + SHELL_MAX_CMD + 4];
        char segment[TERM_MAX_COLS + 1];
        uint32_t total_rows = terminal_input_total_rows(st, layout.columns);
        uint32_t skip = total_rows > layout.input_rows
                      ? total_rows - layout.input_rows : 0;
        uint32_t row = 0, column = 0;
        terminal_prompt(prompt, sizeof(prompt));
        {
            uint32_t used = (uint32_t)bk_runtime_strlen(prompt);
            uint32_t i = 0;
            while (st->input[i] && used + 1U < sizeof(prompt))
                prompt[used++] = st->input[i++];
            prompt[used] = '\0';
        }
        bk_runtime_memset(segment, 0, sizeof(segment));
        for (uint32_t i = 0U; i < column && i < TERM_MAX_COLS; i++)
            segment[i] = ' ';
        for (uint32_t i = 0;; i++) {
            char c = prompt[i];
            bool end_row = !c || c == '\n' || column >= layout.columns;
            if (end_row) {
                segment[column] = '\0';
                if (row >= skip && row - skip < layout.input_rows)
                    bk_gui_font_draw_string_clipped(surface, layout.input.x,
                        layout.input.y + (int)(row - skip) * TERM_LINE_HEIGHT,
                        segment, 0x00FFFFFF, layout.input);
                row++; column = 0;
                bk_runtime_memset(segment, 0, sizeof(segment));
                if (!c) break;
                if (c == '\n') continue;
            }
            segment[column++] = c;
        }
    }

    bk_gui_scrollbar_init_vertical(&scrollbar, layout.scrollbar,
        st->scroll_row, layout.output_rows, st->line_count);
    bk_gui_scrollbar_paint_vertical(surface, &scrollbar);
}

static bool terminal_window_event(gui_window_t *window UNUSED,
                                  const gui_event_t *event,
                                  void *context) {
    terminal_state_t *st = (terminal_state_t *)context;
    if (!st || !event) return false;
    if (event->type != GUI_EVENT_KEY &&
        event->type != GUI_EVENT_MOUSE_WHEEL &&
        event->type != GUI_EVENT_MOUSE_DOWN &&
        event->type != GUI_EVENT_MOUSE_MOVE &&
        event->type != GUI_EVENT_MOUSE_UP) return false;

    /* terminal_execute() espera sincronicamente al proceso foreground. Por eso
     * el task de la Terminal no puede consumir st->events mientras scanf()
     * esta bloqueado. El desktop, que llama este handler, entrega la linea
     * directamente al stdin asociado al console_route del hijo. */
    if (st->command_running && event->type == GUI_EVENT_KEY) {
        if (event->key == KEY_ENTER || event->key == '\r' ||
            event->key == '\n') {
            for (uint32_t i = 0U; i < st->child_input_len; i++)
                (void)bk_console_input_push(st->child_input[i]);
            (void)bk_console_input_push('\n');
            terminal_output_char('\n', st);
            st->child_input_len = 0U;
            st->child_input[0] = '\0';
            return true;
        }
        if (event->key == KEY_BACKSPACE || event->key == '\b') {
            if (st->child_input_len) {
                st->child_input[--st->child_input_len] = '\0';
                terminal_output_char('\b', st);
            }
            return true;
        }
        if (event->key >= 32U && event->key < 127U &&
            st->child_input_len + 1U < sizeof(st->child_input)) {
            char c = (char)event->key;
            st->child_input[st->child_input_len++] = c;
            st->child_input[st->child_input_len] = '\0';
            terminal_output_char(c, st);
        }
        return true;
    }
    return bk_gui_event_queue_push(&st->events, event);
}

static void terminal_process_event(terminal_state_t *st,
                                   const gui_event_t *event) {
    if (!st || !event) return;
    if (event->type != GUI_EVENT_KEY) {
        terminal_layout_t layout;
        gui_scrollbar_t scrollbar;
        uint32_t new_scroll;
        terminal_build_layout(st, &layout);
        bk_gui_scrollbar_init_vertical(&scrollbar, layout.scrollbar,
            st->scroll_row, layout.output_rows, st->line_count);
        if ((st->scrollbar_drag.active ||
             bk_gui_rect_contains(layout.background, event->x, event->y)) &&
            bk_gui_scrollbar_handle_event_vertical(&scrollbar,
                &st->scrollbar_drag, event, 3, &new_scroll)) {
            st->scroll_row = new_scroll;
            terminal_clamp_scroll(st, layout.output_rows);
            if (st->window) st->window->dirty = true;
        }
        return;
    }
    /* Al volver a escribir, regresar al final como una terminal convencional. */
    st->scroll_row = st->line_count > st->visible_rows
                   ? st->line_count - st->visible_rows : 0U;
    if (event->key == KEY_UP || event->key == KEY_DOWN) {
        uint32_t oldest = st->history_count > TERM_HISTORY_LINES
                        ? st->history_count - TERM_HISTORY_LINES : 0U;
        if (event->key == KEY_UP && st->history_index > oldest)
            st->history_index--;
        if (event->key == KEY_DOWN && st->history_index < st->history_count)
            st->history_index++;
        st->input[0] = '\0';
        st->input_len = 0;
        if (st->history_index < st->history_count) {
            const char *item = st->history[st->history_index % TERM_HISTORY_LINES];
            bk_runtime_strncpy(st->input, item, sizeof(st->input) - 1U);
            st->input[sizeof(st->input) - 1U] = '\0';
            st->input_len = (uint32_t)bk_runtime_strlen(st->input);
        }
    } else if ((event->key == KEY_ENTER || event->key == '\r' ||
                event->key == '\n') && event->shift) {
        if (st->input_len + 1 < sizeof(st->input)) {
            st->input[st->input_len++] = '\n';
            st->input[st->input_len] = '\0';
        }
    } else if (event->key == KEY_ENTER || event->key == '\r' ||
               event->key == '\n') {
        st->execute_requested = true;
    } else if (event->key == '\b') {
        if (st->input_len) st->input[--st->input_len] = '\0';
    } else if (event->key >= 32 && event->key < 127 &&
               st->input_len + 1 < sizeof(st->input)) {
        st->input[st->input_len++] = event->key;
        st->input[st->input_len] = '\0';
    }
    if (st->window) st->window->dirty = true;
}

static void terminal_cleanup(terminal_state_t *st) {
    if (!st) return;
    bk_console_set_output_sink(NULL, NULL, NULL);
    if (st->window) {
        bk_gui_desktop_remove_window(st->desktop, st->window);
        bk_gui_window_destroy_raw(st->window);
        bk_proc_bind_window(NULL);
    }
    if (g_terminal == st) g_terminal = NULL;
    bk_sys_free(st);
}

static void terminal_main(void *argument) {
    terminal_state_t *st = (terminal_state_t *)argument;
    gui_event_t event;

    if (!st || !st->desktop) {
        terminal_cleanup(st);
        bk_proc_exit();
    }

    bk_proc_set_memory_hint(sizeof(*st));
    st->line_count = 1;
    st->line_head = 0;
    st->visible_rows = 10;
    bk_gui_event_queue_reset(&st->events);
    st->window = bk_gui_create_window(st->desktop, 70, 42, 590, 340,
                                           "Terminal");
    if (st->window) {
        (void)bk_about_attach(st->window, st->desktop, &(bk_about_info_t){
            "@HB7ABBB6B", "@H1D1E5055", "@HFD5D39B4",
            "@H7A28E1E5", "/ICONS/SHELL.BMP"});
        bk_gui_set_window_content(st->window, terminal_content, st);
        bk_gui_set_window_event_handler(st->window, terminal_window_event, st);
        bk_gui_set_window_min_size(st->window, 260, 150);
        st->window->owner_pid = bk_sys_getpid();
        bk_proc_bind_window(st->window);
    }
    bk_console_set_output_sink(terminal_output_char, terminal_output_clear, st);
    #if defined(BLES_ARCH_ARM32_RPI3)
    terminal_write(st, "BlesKernOS Terminal 1.2 (ARM32)\n");
#else
    terminal_write(st, "BlesKernOS Terminal 1.2 (i386)\n");
#endif
    terminal_write(st, "Escriba 'help' para obtener ayuda. Flechas: historial.\n\n");

    while (!bk_proc_exit_requested()) {
        while (bk_gui_event_queue_pop(&st->events, &event)) {
            terminal_process_event(st, &event);
        }
        if (st->execute_requested && !st->command_running) terminal_execute(st);
        if ((!st->window || !st->window->listed) && !st->command_running) break;
        bk_sys_sleep_ticks(1);
    }

    terminal_cleanup(st);
    bk_proc_exit();
}

bool shelllauncher_get_runtime_info(program_runtime_info_t *info) {
    if (!info || !g_terminal) return false;
    info->window = g_terminal->window;
    info->memory_bytes = sizeof(*g_terminal) +
                         (g_terminal->window ? (uint32_t)sizeof(gui_window_t) : 0);
    return true;
}

void shelllauncher_open_from_desktop(gui_desktop_t *desktop) {
    terminal_state_t *st;
    int pid;

    if (!desktop) return;

    /* Terminal is a shared kernel frontend on ARM Stage 12.  Do not allocate
       a second ~200 KiB scrollback state if the user double-clicks while the
       first instance is already alive. */
    if (g_terminal) {
        if (g_terminal->window && g_terminal->window->listed) {
            bk_gui_desktop_raise_window(desktop, g_terminal->window);
            bk_gui_focus_window(desktop, g_terminal->window);
            bk_gui_window_invalidate(g_terminal->window);
        }
        return;
    }

#if defined(BLES_ARCH_ARM32_RPI3)
    kprintf("[ARM:SHELL] open request state_bytes=%u\n", (uint32_t)sizeof(*st));
#endif
    st = (terminal_state_t *)bk_sys_alloc_zero(sizeof(*st));
    if (!st) {
#if defined(BLES_ARCH_ARM32_RPI3)
        kprintf("[ARM:SHELL] state allocation FAILED\n");
#endif
        return;
    }
    st->desktop = desktop;
    g_terminal = st;
#if defined(BLES_ARCH_ARM32_RPI3)
    kprintf("[ARM:SHELL] state=%p spawning terminal task\n", st);
#endif
    pid = bk_proc_spawn_thread("shell", terminal_main, st);
    if (pid < 0) {
#if defined(BLES_ARCH_ARM32_RPI3)
        kprintf("[ARM:SHELL] task spawn FAILED rc=%d\n", pid);
#endif
        g_terminal = NULL;
        bk_sys_free(st);
        return;
    }
#if defined(BLES_ARCH_ARM32_RPI3)
    kprintf("[ARM:SHELL] terminal task queued pid=%d\n", pid);
#endif
}

void shelllauncher_install(gui_desktop_t *desktop UNUSED) {}

void bleskernos_program_main(gui_desktop_t *desktop) {
    shelllauncher_open_from_desktop(desktop);
}
