#include "kernel/include/api.h"
#include "kernel/include/startup_sound.h"
#include "control_common.h"

typedef struct {
    const char *label;
    const char *path;
    bk_system_sound_event_t event;
} sound_event_item_t;

static const sound_event_item_t g_sound_events[] = {
    {"@HFD43644A", "/SYSTEM/SOUNDS/WINOPEN.WAV", BK_SYSTEM_SOUND_WINDOW_OPEN},
    {"@H1D1791E4", "/SYSTEM/SOUNDS/WINCLOSE.WAV", BK_SYSTEM_SOUND_WINDOW_CLOSE},
    {"@H817FB7F6", "/SYSTEM/SOUNDS/MINIMIZE.WAV", BK_SYSTEM_SOUND_WINDOW_MINIMIZE},
    {"@HB94A4DDD", "/SYSTEM/SOUNDS/RESTORE.WAV", BK_SYSTEM_SOUND_WINDOW_RESTORE},
    {"@H910F4D00", "/SYSTEM/SOUNDS/RESIZE.WAV", BK_SYSTEM_SOUND_WINDOW_RESIZE},
    {"Botones", "/SYSTEM/SOUNDS/BOTTON.WAV", BK_SYSTEM_SOUND_BUTTON},
    {"Scroll", "/SYSTEM/SOUNDS/SCROLL.WAV", BK_SYSTEM_SOUND_SCROLL},
};

#define SOUND_EVENT_COUNT ((int)(sizeof(g_sound_events) / sizeof(g_sound_events[0])))

typedef struct {
    gui_desktop_t *desktop;
    gui_window_t *window;
    gui_widget_t *startup_toggle;
    gui_widget_t *master_toggle;
    gui_widget_t *volume_dropdown;
    gui_widget_t *event_dropdown;
    gui_widget_t *event_toggle;
    gui_widget_t *event_test;
    char status[96];
    bool startup_enabled;
    bool master_enabled;
    uint8_t master_volume;
    int selected_event;
} sound_cpl_state_t;

static sound_cpl_state_t *sound_state(gui_window_t *window) {
    return window ? (sound_cpl_state_t *)window->content_context : NULL;
}

static const sound_event_item_t *sound_selected_item(sound_cpl_state_t *st) {
    if (!st || st->selected_event < 0 || st->selected_event >= SOUND_EVENT_COUNT)
        return NULL;
    return &g_sound_events[st->selected_event];
}

static void sound_sync_event_controls(sound_cpl_state_t *st) {
    const sound_event_item_t *item = sound_selected_item(st);
    bool enabled = false;
    bool available = false;
    if (!st || !item) return;
    enabled = bk_system_sound_event_enabled(item->event);
    available = bk_system_sound_event_available(item->event);
    if (st->event_toggle)
        bk_gui_widget_set_text(st->event_toggle, enabled ? "Encendido" : "Apagado");
    if (st->event_test)
        bk_gui_widget_set_enabled(st->event_test, st->master_enabled && enabled && available);
}

static void sound_startup_toggle(gui_window_t *window, uint32_t id UNUSED) {
    sound_cpl_state_t *st = sound_state(window);
    if (!st) return;
    st->startup_enabled = !st->startup_enabled;
    if (!startup_sound_set_enabled(st->startup_enabled)) {
        st->startup_enabled = !st->startup_enabled;
        bk_runtime_strcpy(st->status, "@H77022E31");
    } else {
        bk_runtime_strcpy(st->status, st->startup_enabled
            ? "Sonido de inicio activado." : "Sonido de inicio desactivado.");
    }
    if (st->startup_toggle)
        bk_gui_widget_set_text(st->startup_toggle,
            st->startup_enabled ? "Encendido" : "Apagado");
    window->dirty = true;
}

static void sound_master_toggle(gui_window_t *window, uint32_t id UNUSED) {
    sound_cpl_state_t *st = sound_state(window);
    bool next;
    if (!st) return;
    next = !st->master_enabled;
    if (!bk_system_sounds_set_enabled(next)) {
        bk_runtime_strcpy(st->status, "@H77022E31");
        return;
    }
    st->master_enabled = next;
    bk_gui_widget_set_text(st->master_toggle,
        st->master_enabled ? "Encendido" : "Apagado");
    bk_runtime_strcpy(st->status, st->master_enabled
        ? "Sonidos del sistema activados." : "Sonidos del sistema desactivados.");
    sound_sync_event_controls(st);
    window->dirty = true;
}

static void sound_volume_select(gui_window_t *window, uint32_t id UNUSED) {
    sound_cpl_state_t *st = sound_state(window);
    int selected;
    uint8_t volume;
    if (!st || !st->volume_dropdown) return;
    selected = bk_gui_widget_dropdown_get_selected(st->volume_dropdown);
    if (selected < 0) return;
    if (selected > 10) selected = 10;
    volume = (uint8_t)(selected * 10);
    if (!bk_system_sound_set_master_volume(volume)) {
        bk_runtime_strcpy(st->status, "No se pudo guardar el volumen.");
        return;
    }
    st->master_volume = volume;
    snprintf(st->status, sizeof(st->status), "Volumen general: %u%%",
             (uint32_t)volume);
    window->dirty = true;
}

static void sound_event_select(gui_window_t *window, uint32_t id UNUSED) {
    sound_cpl_state_t *st = sound_state(window);
    int selected;
    if (!st) return;
    selected = bk_gui_widget_dropdown_get_selected(st->event_dropdown);
    if (selected >= 0 && selected < SOUND_EVENT_COUNT)
        st->selected_event = selected;
    sound_sync_event_controls(st);
    window->dirty = true;
}

static void sound_event_toggle(gui_window_t *window, uint32_t id UNUSED) {
    sound_cpl_state_t *st = sound_state(window);
    const sound_event_item_t *item;
    bool enabled;
    if (!st) return;
    item = sound_selected_item(st);
    if (!item) return;
    enabled = !bk_system_sound_event_enabled(item->event);
    if (!bk_system_sound_event_set_enabled(item->event, enabled)) {
        bk_runtime_strcpy(st->status, "@H77022E31");
        return;
    }
    bk_runtime_strcpy(st->status, enabled
        ? "Sonido de evento activado." : "Sonido de evento desactivado.");
    sound_sync_event_controls(st);
    window->dirty = true;
}

static void sound_test(gui_window_t *window, uint32_t id UNUSED) {
    sound_cpl_state_t *st = sound_state(window);
    const sound_event_item_t *item;
    bool ok;
    if (!st) return;
    item = sound_selected_item(st);
    if (!item) return;
    if (!bk_system_sound_event_available(item->event)) {
        snprintf(st->status, sizeof(st->status), "Falta %s", item->path);
        window->dirty = true;
        return;
    }
    ok = bk_system_sound_event_play(item->event);
    snprintf(st->status, sizeof(st->status), "%s",
             ok ? "Reproduciendo sonido de evento." :
                  "No se pudo reproducir el sonido.");
    window->dirty = true;
}

static void sound_stop_test(gui_window_t *window, uint32_t id UNUSED) {
    sound_cpl_state_t *st = sound_state(window);
    bk_sound_stop();
    if (st) snprintf(st->status, sizeof(st->status), "@H3CC2BCB2");
    if (window) window->dirty = true;
}

static void sound_paint(gui_window_t *window UNUSED, gui_surface_t *s,
                        void *context) {
    sound_cpl_state_t *st = (sound_cpl_state_t *)context;
    const sound_event_item_t *item = sound_selected_item(st);
    gui_rect_t client = bk_gui_window_content_rect_raw(st->window);
    int x = client.x + 14;
    int y = client.y + 10;
    bool available = item && bk_system_sound_event_available(item->event);

    cpl_draw_group(s, (gui_rect_t){x, y, 444, 92}, "@H1CB39E60");
    bk_gui_font_draw_string(s, x + 16, y + 24, "@HECCBEA01", CPL_TEXT, 0, false);
    bk_gui_font_draw_string(s, x + 112, y + 24, bk_sound_pcm_name(),
                         0x00006020, 0, false);
    bk_gui_font_draw_string(s, x + 16, y + 48,
        bk_sound_pcm_available() ? "@H42B1D0B4" : "@HBEEE50E3",
        CPL_TEXT, 0, false);
    bk_gui_font_draw_string(s, x + 16, y + 68,
        "Los sonidos de interfaz se precargan para no bloquear Vision.",
        CPL_SHADOW, 0, false);

    cpl_draw_group(s, (gui_rect_t){x, y + 104, 444, 72}, "@H9C5DA8E1");
    bk_gui_font_draw_string(s, x + 16, y + 130, "Estado:", CPL_TEXT, 0, false);
    bk_gui_font_draw_string(s, x + 82, y + 130,
        st->startup_enabled ? "Encendido" : "Apagado",
        st->startup_enabled ? 0x00006020 : 0x00802020, 0, false);
    bk_gui_font_draw_string(s, x + 16, y + 151,
        "/SYSTEM/SOUNDS/START.WAV", CPL_SHADOW, 0, false);

    cpl_draw_group(s, (gui_rect_t){x, y + 188, 444, 188}, "@H12689BA5");
    bk_gui_font_draw_string(s, x + 16, y + 214, "@HE0FE4F71", CPL_TEXT, 0, false);
    bk_gui_font_draw_string(s, x + 16, y + 247, "Volumen general:", CPL_TEXT, 0, false);
    bk_gui_font_draw_string(s, x + 16, y + 280, "@H68190FFE", CPL_TEXT, 0, false);
    bk_gui_font_draw_string(s, x + 16, y + 313, "Estado:", CPL_TEXT, 0, false);
    if (item) {
        bk_gui_font_draw_string(s, x + 84, y + 313,
            bk_system_sound_event_enabled(item->event) ? "Encendido" : "Apagado",
            bk_system_sound_event_enabled(item->event) ? 0x00006020 : 0x00802020,
            0, false);
        bk_gui_font_draw_string(s, x + 16, y + 338,
            available ? "Disponible:" : "Falta:",
            available ? 0x00006020 : 0x00802020, 0, false);
        bk_gui_font_draw_string_clipped(s, x + 88, y + 338, item->path,
            CPL_SHADOW, (gui_rect_t){x + 88, y + 334, 330, 18});
    }

    bk_gui_font_draw_string_clipped(s, x + 4,
        client.y + client.h - 17,
        st->status[0] ? st->status : "Seleccione un evento para configurarlo.",
        CPL_TEXT, (gui_rect_t){x + 4, client.y + client.h - 19, 432, 16});
}

void bleskernos_program_main(gui_desktop_t *desktop) {
    sound_cpl_state_t *st;
    if (!desktop) return;
    st = (sound_cpl_state_t *)bk_sys_alloc_zero(sizeof(*st));
    if (!st) return;
    st->desktop = desktop;
    st->startup_enabled = startup_sound_enabled();
    st->master_enabled = bk_system_sounds_enabled();
    st->master_volume = bk_system_sound_master_volume();
    st->selected_event = 0;
    st->window = bk_gui_create_window(desktop, 90, 34, 480, 468, "@H1A175EBB");
    if (!st->window) { bk_sys_free(st); return; }
    (void)bk_about_attach(st->window, desktop, &(bk_about_info_t){
        "@H1A175EBB", "@H1C1E4EC2", "@H73740315",
        "@H7A28E1E5", "/ICONS/SOUND.BMP"});
    bk_gui_set_window_content(st->window, sound_paint, st);
    st->window->owner_pid = bk_sys_getpid();
    bk_proc_bind_window(st->window);

    st->startup_toggle = bk_gui_widget_create_button(
        desktop, st->window, (gui_rect_t){342, 139, 100, 22},
        st->startup_enabled ? "Encendido" : "Apagado", sound_startup_toggle);

    st->master_toggle = bk_gui_widget_create_button(
        desktop, st->window, (gui_rect_t){342, 223, 100, 22},
        st->master_enabled ? "Encendido" : "Apagado", sound_master_toggle);

    st->volume_dropdown = bk_gui_widget_create_dropdown(
        desktop, st->window, (gui_rect_t){242, 256, 88, 22}, sound_volume_select);
    for (int i = 0; i <= 10; i++) {
        char label[12];
        char value[4];
        snprintf(label, sizeof(label), "%d %%", i * 10);
        snprintf(value, sizeof(value), "%d", i * 10);
        (void)bk_gui_widget_dropdown_add_item(st->volume_dropdown, label, value);
    }
    bk_gui_widget_dropdown_set_selected(st->volume_dropdown,
        (int)((st->master_volume + 5U) / 10U));

    st->event_dropdown = bk_gui_widget_create_dropdown(
        desktop, st->window, (gui_rect_t){92, 289, 228, 22}, sound_event_select);
    for (int i = 0; i < SOUND_EVENT_COUNT; i++) {
        char value[4];
        snprintf(value, sizeof(value), "%d", i);
        (void)bk_gui_widget_dropdown_add_item(st->event_dropdown,
                                               g_sound_events[i].label, value);
    }
    bk_gui_widget_dropdown_set_selected(st->event_dropdown, 0);

    st->event_toggle = bk_gui_widget_create_button(
        desktop, st->window, (gui_rect_t){342, 289, 100, 22},
        "Encendido", sound_event_toggle);
    st->event_test = bk_gui_widget_create_button(
        desktop, st->window, (gui_rect_t){342, 322, 100, 22},
        "Probar", sound_test);
    (void)bk_gui_widget_set_icon(st->event_test, "Playp16");
    {
        gui_widget_t *stop = bk_gui_widget_create_button(
            desktop, st->window, (gui_rect_t){342, 355, 100, 22},
            "Detener", sound_stop_test);
        (void)bk_gui_widget_set_icon(stop, "Close");
    }

    sound_sync_event_controls(st);
    while (!bk_proc_exit_requested() && st->window->listed) bk_sys_sleep_ticks(2);
    bk_sound_stop();
    cpl_destroy_window(st->desktop, st->window);
    bk_sys_free(st);
    bk_proc_exit();
}
