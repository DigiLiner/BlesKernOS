#include "kernel/include/api.h"
#include "ss_timing.h"

#define SS_INPUT_GRACE_MS 500U

typedef struct {
    gui_desktop_t *desktop;
    gui_program_t *program;
    bool active;
    int x;
    int y;
    int dx;
    int dy;
    uint32_t last_motion_ms;
    uint32_t seed;
    uint32_t input_armed_ms;
} ss_logo_state_t;

static ss_logo_state_t *g_ss_logo;

static uint32_t rand_next(ss_logo_state_t *st) {
    st->seed = st->seed * 1103515245U + 12345U;
    return st->seed;
}

static void draw_stars(gui_surface_t *surface, ss_logo_state_t *st) {
    uint32_t old_seed = st->seed;
    st->seed = 0xB10506U;
    for (uint32_t i = 0; i < 90; i++) {
        int x = (int)(rand_next(st) % surface->width);
        int y = (int)(rand_next(st) % surface->height);
        uint32_t shade = 0x00404040 + ((i & 3U) * 0x00202020);
        bk_gui_gfx_putpixel(surface, x, y, shade);
    }
    st->seed = old_seed;
}

static void draw_logo(gui_surface_t *surface, int x, int y) {
    gui_rect_t box = {x, y, 208, 58};

    bk_gui_gfx_fill_rect(surface, box, 0x00000000);
    bk_gui_gfx_draw_rect(surface, box, 0x00FFFFFF);
    bk_gui_gfx_draw_rect(surface, (gui_rect_t){x + 2, y + 2, 204, 54}, 0x000080FF);
    bk_gui_gfx_fill_rect(surface, (gui_rect_t){x + 5, y + 5, 198, 48}, 0x00002040);
    bk_gui_font_draw_string_scaled(surface, x + 18, y + 14,
                                "BlesKernOS", 0x00FFFFFF, 2);
    bk_gui_font_draw_string(surface, x + 52, y + 39,
                         "Screen Saver", 0x0000C0FF, 0, false);
}

static void ss_logo_paint(gui_program_t *program, gui_desktop_t *desktop,
                          gui_surface_t *surface) {
    ss_logo_state_t *st = (ss_logo_state_t *)program->state;
    uint32_t now;
    int logo_w = 208;
    int logo_h = 58;

    if (!st || !desktop || !surface || !st->active) return;

    now = bk_sys_uptime_ms();
    uint32_t steps = (uint32_t)(now - st->last_motion_ms) / 10U;
    st->last_motion_ms += steps * 10U;
    if (steps > 8U) steps = 8U;
    while (steps--) {
        st->x += st->dx;
        st->y += st->dy;

        if (st->x <= 0) {
            st->x = 0;
            st->dx = 2;
        }
        if (st->y <= 0) {
            st->y = 0;
            st->dy = 2;
        }
        if (st->x + logo_w >= surface->width) {
            st->x = surface->width - logo_w - 1;
            st->dx = -2;
        }
        if (st->y + logo_h >= surface->height) {
            st->y = surface->height - logo_h - 1;
            st->dy = -2;
        }
    }

    bk_gui_gfx_clear(surface, 0x00000000);
    draw_stars(surface, st);
    draw_logo(surface, st->x, st->y);
}

static bool ss_logo_is_input_event(const gui_event_t *event) {
    return event &&
           (event->type == GUI_EVENT_MOUSE_MOVE ||
            event->type == GUI_EVENT_MOUSE_DOWN ||
            event->type == GUI_EVENT_MOUSE_UP ||
            event->type == GUI_EVENT_MOUSE_WHEEL ||
            event->type == GUI_EVENT_KEY);
}

static void ss_logo_close(gui_desktop_t *desktop, ss_logo_state_t *st) {
    gui_program_t *program;

    if (!st) return;
    program = st->program;
    st->active = false;
    if (g_ss_logo == st) g_ss_logo = NULL;
    if (desktop && program) {
        st->program = NULL;
        bk_gui_desktop_unregister_program(desktop, program);
    }
    bk_sys_free(st);
    bk_gui_gfx_invalidate_front();
    bk_gui_request_paint();
}

static bool ss_logo_event(gui_program_t *program, gui_desktop_t *desktop,
                          const gui_event_t *event) {
    ss_logo_state_t *st = program ? (ss_logo_state_t *)program->state : NULL;

    if (!st || !event) return false;
    if (ss_logo_is_input_event(event) &&
        (int32_t)(bk_sys_uptime_ms() - st->input_armed_ms) < 0)
        return true;
    if (ss_logo_is_input_event(event)) {
        ss_logo_close(desktop, st);
        return true;
    }
    return false;
}

bool ss_logo_is_active(void);
bool ss_logo_handle_global_event(gui_desktop_t *desktop,
                                 const gui_event_t *event) {
    if (!ss_logo_is_active() || !ss_logo_is_input_event(event)) return false;
    if ((int32_t)(bk_sys_uptime_ms() - g_ss_logo->input_armed_ms) < 0)
        return true;
    ss_logo_close(desktop, g_ss_logo);
    return true;
}

bool ss_logo_is_active(void) {
    return g_ss_logo != NULL && g_ss_logo->active;
}

void ss_logo_open_from_desktop(gui_desktop_t *desktop) {
    ss_logo_state_t *st;

    if (!desktop || ss_logo_is_active()) return;
    st = (ss_logo_state_t *)bk_sys_alloc_zero(sizeof(*st));
    if (!st) return;

    st->desktop = desktop;
    st->active = true;
    st->x = desktop->surface.width / 3;
    st->y = desktop->surface.height / 3;
    st->dx = 2;
    st->dy = 2;
    st->last_motion_ms = bk_sys_uptime_ms();
    st->seed = st->last_motion_ms ^ 0x516A0E0U;
    st->input_armed_ms = bk_sys_uptime_ms() + SS_INPUT_GRACE_MS;

    st->program = bk_gui_desktop_register_program(desktop, "ss_logo", st,
                                               ss_logo_paint,
                                               ss_logo_event,
                                               NULL);
    if (!st->program) {
        bk_sys_free(st);
        return;
    }

    g_ss_logo = st;
    bk_gui_desktop_invalidate_all(desktop);
    bk_gui_request_paint();
}

/* Optional entry points for ELF-style builds. Use whichever name your loader calls. */
void ss_logo_main(gui_desktop_t *desktop) {
    gui_desktop_t *target = desktop ? desktop : bk_gui_desktop();
    uint32_t last_request;

    if (!target || ss_logo_is_active()) return;
    ss_logo_open_from_desktop(target);
    if (!ss_logo_is_active()) return;

    /* Keep callbacks alive and request only the next animation frame. */
    last_request = bk_sys_uptime_ms();
    while (ss_logo_is_active() && !bk_proc_exit_requested()) {
        if (ss_frame_due(bk_sys_uptime_ms(), &last_request)) {
            bk_gui_desktop_invalidate_all(target);
            bk_gui_request_paint();
        }
        bk_sys_sleep_ms(5U);
    }

    if (bk_proc_exit_requested() && ss_logo_is_active())
        ss_logo_close(target, g_ss_logo);
}

void bleskernos_program_main(gui_desktop_t *desktop) {
    ss_logo_main(desktop);
}
