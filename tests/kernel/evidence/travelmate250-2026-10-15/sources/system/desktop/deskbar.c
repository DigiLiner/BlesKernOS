#include "programs.h"
#include "kernel/include/memory.h"
#include "kernel/include/vfs.h"
#include "kernel/include/rtc.h"
#include "kernel/include/task.h"
#include "kernel/include/pic.h"
#include "kernel/include/pit.h"
#include "kernel/include/datetime_prefs.h"
#include "kernel/include/api.h"
#include "kernel/include/compat_mode.h"
#include "../../kernel/libc/include/stdio.h"

#define DESKBAR_HEIGHT          52
#define DESKBAR_PANEL_MARGIN     4
#define DESKBAR_PANEL_MAX_W    920
#define DESKBAR_PANEL_MIN_W    620
#define DESKBAR_DESIGN_W       967
#define DESKBAR_DESIGN_H        85
#define DESKBAR_START_SRC_X    366
#define DESKBAR_START_SRC_Y     11
#define DESKBAR_START_SRC_W    240
#define DESKBAR_START_SRC_H     61

/* Sampled from the original CDE panel bitmap used as the visual reference.
 * The bitmap itself is NOT embedded; these are only palette values. */
#define CDE_FACE          0x0077767B
#define CDE_FACE_HOVER    0x0088868D
#define CDE_TOP_FACE      0x00999999
#define CDE_FACE_LIGHT    0x00D1D1D2
#define CDE_MID_LIGHT     0x00ADADAD
#define CDE_SHADOW        0x004E4E4E
#define CDE_DARK          0x00212121
#define CDE_START_FACE    0x005E5C64
#define CDE_START_EDGE    0x00A8988F
#define CDE_EDGE_BLUE     0x004992A7
#define CDE_INK           0x00191D1C
#define CDE_PAPER         0x00F0EFE7
#define DESKBAR_LAUNCHER_COUNT 10
#define DESKBAR_LAUNCHER_ICON_SIZE 32
#define DESKBAR_BUTTON_W       76
#define DESKBAR_BUTTON_H       24
#define DESKBAR_MENU_W         260
#define DESKBAR_SUBMENU_W      250
#define DESKBAR_ITEM_H         38
#define DESKBAR_MAIN_ITEMS     8
#define DESKBAR_MAX_SUB_ITEMS  11
#define DESKBAR_MAX_CASCADE_LEVELS 4
#define DESKBAR_CALENDAR_W      405
#define DESKBAR_CALENDAR_H      285
#define DESKBAR_MAIN_ICON_COUNT DESKBAR_MAIN_ITEMS

typedef struct {
    char label[24];
    char path[VFS_MAX_PATH];
    bool enabled;
    bool is_dir;
} deskbar_sub_item_t;

typedef struct {
    bool menu_open;
    bool button_hovered;
    bool button_pressed;
    bool clock_hovered;
    bool clock_pressed;
    uint32_t hovered_window_id;
    uint32_t pressed_window_id;
    int hovered_main_item;
    int pressed_main_item;
    int open_section;
    int hovered_sub_item;
    int pressed_sub_item;
    int pressed_cascade_level;
    int pressed_cascade_item;
    int cached_section;
    int cached_count;
    deskbar_sub_item_t cached_items[DESKBAR_MAX_SUB_ITEMS];

    int open_cascade_levels;
    int cascade_parent_index[DESKBAR_MAX_CASCADE_LEVELS];
    int hovered_cascade_item[DESKBAR_MAX_CASCADE_LEVELS];
    int cascade_count[DESKBAR_MAX_CASCADE_LEVELS];
    char cascade_path[DESKBAR_MAX_CASCADE_LEVELS][VFS_MAX_PATH];
    deskbar_sub_item_t cascade_items[DESKBAR_MAX_CASCADE_LEVELS][DESKBAR_MAX_SUB_ITEMS];
    bk_datetime_preferences_t clock_prefs;
    gui_image_t main_icons[DESKBAR_MAIN_ICON_COUNT];
    bool main_icon_loaded[DESKBAR_MAIN_ICON_COUNT];
    gui_image_t folder_icon;
    gui_image_t application_icon;
    gui_image_t document_icon;
    bool folder_icon_loaded;
    bool application_icon_loaded;
    bool document_icon_loaded;

    /* Native BlesKernOS launchers shown in the CDE-style front panel. */
    int hovered_launcher;
    int pressed_launcher;
    gui_image_t launcher_icons[DESKBAR_LAUNCHER_COUNT];
    bool launcher_icon_loaded[DESKBAR_LAUNCHER_COUNT];

    uint32_t start_font;
    bool start_font_loaded;
    uint16_t start_font_size, start_text_width, start_text_height;
    int start_button_width, start_button_height;

    /* The two ribbed CDE side grips are live resize handles.  The panel keeps
       itself centered while either side changes its width. */
    int resize_hover_side; /* -1 left, +1 right, 0 none */
    int resize_drag_side;  /* -1 left, +1 right, 0 none */
    int resize_grab_offset;

} deskbar_state_t;

#ifdef BLES_VISION_SERVICE
static deskbar_state_t *g_vision_deskbar_state;
#endif

/* Zero means "use the default width".  Once the user drags either side grip
   this stores the requested centered width for the current desktop session. */
static int g_deskbar_panel_width;

/* Original Adobe/DEC X11 bitmap face, installed with the normal font packs. */
static void deskbar_load_start_font(deskbar_state_t *state) {
    for (uint32_t id=0;id<bk_font_count();id++) {
        bk_font_info_t info;
        if (bk_font_info(id,&info) && !kstrcmp(info.name,"Helvetica Bold Oblique Bitmap")) {
            state->start_font=id;state->start_font_loaded=true;return;
        }
    }
}

static const char *g_main_items[DESKBAR_MAIN_ITEMS] = {
    "@HC79ED5CE",
    "Ejecutar...",
    "@H06585867",
    "Programs",
    "Documents",
    "Misc",
    "@H59EF0B79",
    "Apagar"
};

typedef struct {
    const char *label;
    const char *icon_name;
    const char *program_path;
    int design_x;
    int design_w;
} deskbar_launcher_spec_t;

/* CDE has five body cells on each side of the center well.  Use all ten as
 * real BlesKernOS launchers.  Their artwork comes from GRAPHICS.PAK
 * (KDE 1) instead of the older /ICONS BMP set.  Labels are retained
 * only for logging/tooltips/future configuration: the front panel itself is
 * intentionally icon-only. */
static const deskbar_launcher_spec_t g_cde_launchers[DESKBAR_LAUNCHER_COUNT] = {
    {"Files",     "WindowsExplorer", "/SYSTEM/PROGRAMS/FILE.BEX",           25, 57},
    {"Editor",    "Notepad",         "/SYSTEM/PROGRAMS/TEXTEDITOR.BEX",     82, 58},
    {"Web",       "Globe",           "/SYSTEM/PROGRAMS/NETSURF.BEX",       140, 58},
    {"Paint",     "Mspaint",         "/SYSTEM/PROGRAMS/PAINT.BEX",         198, 58},
    {"Calc",      "Calculator",      "/SYSTEM/PROGRAMS/CALCULATOR.BEX",    256, 66},
    {"Shell",     "MsDos",           "/SYSTEM/PROGRAMS/SHELL.BEX",         646, 65},
    {"MidAmp",    "MediaAudio",      "/SYSTEM/PROGRAMS/MIDAMP.BEX",        711, 57},
    {"Image",     "Camera",          "/SYSTEM/PROGRAMS/IMAGEVIEWER.BEX",   768, 56},
    {"Processes", "Taskman100",      "/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX",824, 57},
    {"Config",    "Settings",        "/SYSTEM/CONTROL/CONTROL.BEX",        881, 59}
};

static void deskbar_copy_text(char *dst, size_t dst_len, const char *src) {
    if (!dst || !dst_len) return;
    if (!src) src = "";
    kstrncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void deskbar_clear_cascade_from(deskbar_state_t *state, int level) {
    if (!state) return;
    if (level < 0) level = 0;
    if (level > DESKBAR_MAX_CASCADE_LEVELS) level = DESKBAR_MAX_CASCADE_LEVELS;

    for (int i = level; i < DESKBAR_MAX_CASCADE_LEVELS; i++) {
        state->cascade_parent_index[i] = -1;
        state->hovered_cascade_item[i] = -1;
        state->cascade_count[i] = 0;
        state->cascade_path[i][0] = '\0';
    }

    if (state->open_cascade_levels > level)
        state->open_cascade_levels = level;
    if (state->pressed_cascade_level >= level) {
        state->pressed_cascade_level = -1;
        state->pressed_cascade_item = -1;
    }
}

static void deskbar_reset_menu_state(deskbar_state_t *state) {
    if (!state) return;
    state->hovered_main_item = -1;
    state->pressed_main_item = -1;
    state->open_section = 0;
    state->hovered_sub_item = -1;
    state->pressed_sub_item = -1;
    state->pressed_cascade_level = -1;
    state->pressed_cascade_item = -1;
    /* Keep the last directory listing across Start-menu closes.  Clearing
       this cache here forced vfs_listdir() again on the next click. */
    state->open_cascade_levels = 0;
    deskbar_clear_cascade_from(state, 0);
}

static void deskbar_reset_state(deskbar_state_t *state) {
    if (!state) return;
    state->menu_open = false;
    state->button_hovered = false;
    state->button_pressed = false;
    state->clock_hovered = false;
    state->clock_pressed = false;
    state->hovered_window_id = 0;
    state->pressed_window_id = 0;
    state->hovered_launcher = -1;
    state->pressed_launcher = -1;
    state->resize_hover_side = 0;
    state->resize_drag_side = 0;
    state->resize_grab_offset = 0;
    state->cached_section = -1;
    state->cached_count = 0;
    deskbar_reset_menu_state(state);
}

static int deskbar_panel_width_clamp(const gui_desktop_t *desktop, int width) {
    int available;
    int minimum;
    (void)width;
    if (!desktop) return DESKBAR_PANEL_MIN_W;
    available = (int)desktop->surface.width - DESKBAR_PANEL_MARGIN * 2;
    if (available < 1) available = (int)desktop->surface.width;
    minimum = DESKBAR_PANEL_MIN_W;
    if (minimum > available) minimum = available;
    return minimum;
}

static gui_rect_t deskbar_bar_rect(const gui_desktop_t *desktop) {
    int width;
    int height;
    int x;
    int y;
    if (!desktop) return (gui_rect_t){0, 0, 0, 0};

    width = deskbar_panel_width_clamp(desktop, DESKBAR_PANEL_MIN_W);
    height = (int)desktop->surface.height < DESKBAR_HEIGHT
        ? (int)desktop->surface.height : DESKBAR_HEIGHT;
    x = ((int)desktop->surface.width - width) / 2;
    y = (int)desktop->surface.height - height;
    return (gui_rect_t){x, y, width, height};
}

/* The mockup remains useful as a coordinate system only.  Every visible
 * pixel is now produced by GUI primitives; no screenshot/PNG is embedded. */
static gui_rect_t deskbar_design_rect(const gui_desktop_t *desktop,
                                      int sx, int sy, int sw, int sh) {
    gui_rect_t bar = deskbar_bar_rect(desktop);
    int x0, y0, x1, y1;
    if (bar.w <= 0 || bar.h <= 0) return (gui_rect_t){0, 0, 0, 0};

    x0 = bar.x + (sx * bar.w + DESKBAR_DESIGN_W / 2) / DESKBAR_DESIGN_W;
    x1 = bar.x + ((sx + sw) * bar.w + DESKBAR_DESIGN_W / 2) / DESKBAR_DESIGN_W;
    y0 = bar.y + (sy * bar.h + DESKBAR_DESIGN_H / 2) / DESKBAR_DESIGN_H;
    y1 = bar.y + ((sy + sh) * bar.h + DESKBAR_DESIGN_H / 2) / DESKBAR_DESIGN_H;
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    return (gui_rect_t){x0, y0, x1 - x0, y1 - y0};
}

static gui_rect_t deskbar_resize_handle_rect(const gui_desktop_t *desktop,
                                                 int side) {
    if (side < 0)
        return deskbar_design_rect(desktop, 5, 20, 20, 60);
    if (side > 0)
        return deskbar_design_rect(desktop, 940, 20, 22, 60);
    return (gui_rect_t){0, 0, 0, 0};
}

static int deskbar_resize_hit_side(const gui_desktop_t *desktop, int x, int y) {
    (void)desktop; (void)x; (void)y;
    return 0; /* Fixed compact panel: decorative grips do not resize it. */
}

static void deskbar_resize_to_pointer(gui_desktop_t *desktop,
                                      deskbar_state_t *state, int pointer_x) {
    gui_rect_t old_bar;
    gui_rect_t new_bar;
    int center;
    int edge;
    int width;

    if (!desktop || !state || !state->resize_drag_side) return;
    old_bar = deskbar_bar_rect(desktop);
    center = (int)desktop->surface.width / 2;

    if (state->resize_drag_side < 0) {
        edge = pointer_x - state->resize_grab_offset;
        width = (center - edge) * 2;
    } else {
        edge = pointer_x - state->resize_grab_offset;
        width = (edge - center) * 2;
    }

    width = deskbar_panel_width_clamp(desktop, width);
    if (g_deskbar_panel_width == width) return;
    g_deskbar_panel_width = width;
    new_bar = deskbar_bar_rect(desktop);

    /* Damage both shapes so the wallpaper is restored where a narrower panel
       used to be and the newly exposed panel area appears immediately. */
    gui_desktop_invalidate_rect(desktop, old_bar);
    gui_desktop_invalidate_rect(desktop, new_bar);
    gui_request_paint();
}

static gui_rect_t deskbar_button_rect(const gui_desktop_t *desktop) {
    return deskbar_design_rect(desktop, DESKBAR_START_SRC_X,
        DESKBAR_START_SRC_Y, DESKBAR_START_SRC_W, DESKBAR_START_SRC_H);
}

static gui_rect_t deskbar_status_rect(const gui_desktop_t *desktop) {
    /* v4: the old hand-drawn clock/calendar pair has been removed.  Their
       cells are now ordinary application launchers, so there is no separate
       status hit target in the front panel. */
    (void)desktop;
    return (gui_rect_t){0, 0, 0, 0};
}

gui_rect_t deskbar_clock_rect(const gui_desktop_t *desktop) {
    if (!desktop) return (gui_rect_t){0, 0, 0, 0};
    return deskbar_status_rect(desktop);
}

static void deskbar_open_clock_calendar(gui_desktop_t *desktop) {
    if (!desktop) return;
    (void)program_execute_path_async(desktop, "/SYSTEM/CONTROL/DATETIME.CPL");
}

static void deskbar_load_clock_preferences(deskbar_state_t *state) {
    if (!state) return;
    if (bk_datetime_runtime_preferences_get(&state->clock_prefs)) return;
    bk_datetime_preferences_load(&state->clock_prefs);
    bk_datetime_runtime_preferences_set(&state->clock_prefs);
}

static void deskbar_build_clock_text(const deskbar_state_t *state,
                                     char *text,
                                     size_t text_size) {
    rtc_datetime_t raw;
    rtc_datetime_t shown;
    uint8_t hour;
    const char *suffix = "";

    if (!text || text_size < 2) return;
    text[0] = '\0';

    if (!rtc_get_datetime(&raw)) {
        raw.date.year = 2026;
        raw.date.month = 1;
        raw.date.day = 1;
        raw.time.hour = 0;
        raw.time.minute = 0;
        raw.time.second = 0;
    }

    bk_datetime_apply_preferences(state ? &state->clock_prefs : NULL,
                                  &raw, &shown);
    hour = shown.time.hour;
    if (state && !state->clock_prefs.format_24h) {
        suffix = hour >= 12 ? " PM" : " AM";
        hour %= 12;
        if (!hour) hour = 12;
        snprintf(text, text_size, "%02u:%02u%s",
                 hour, shown.time.minute, suffix);
        return;
    }

    snprintf(text, text_size, "%02u:%02u:%02u",
             shown.time.hour, shown.time.minute, shown.time.second);
}

static gui_rect_t deskbar_window_area_rect(const gui_desktop_t *desktop) {
    (void)desktop;
    /* Running-window buttons are intentionally hidden for the visual mockup.
       The existing window/task behaviour is left intact underneath and can
       be redesigned after the panel layout is approved. */
    return (gui_rect_t){0, 0, 0, 0};
}

static gui_rect_t deskbar_menu_rect(const gui_desktop_t *desktop) {
    gui_rect_t bar = deskbar_bar_rect(desktop);
    gui_rect_t button = deskbar_button_rect(desktop);
    int menu_h = DESKBAR_MAIN_ITEMS * DESKBAR_ITEM_H + 6;
    int x = button.x + (button.w - DESKBAR_MENU_W) / 2;
    if (x < 0) x = 0;
    if (x + DESKBAR_MENU_W > desktop->surface.width)
        x = desktop->surface.width - DESKBAR_MENU_W;
    if (x < 0) x = 0;
    return (gui_rect_t){x, bar.y - menu_h + 1, DESKBAR_MENU_W, menu_h};
}

static gui_rect_t deskbar_main_item_rect(const gui_desktop_t *desktop, int index) {
    gui_rect_t menu = deskbar_menu_rect(desktop);
    return (gui_rect_t){menu.x + 3, menu.y + 3 + (index * DESKBAR_ITEM_H), menu.w - 6, DESKBAR_ITEM_H};
}

static gui_rect_t deskbar_submenu_rect(const gui_desktop_t *desktop, int section, int item_count) {
    gui_rect_t item = deskbar_main_item_rect(desktop, section);
    int height = item_count * DESKBAR_ITEM_H + 6;
    int y = item.y;
    if (y + height > desktop->surface.height - DESKBAR_HEIGHT + 1) {
        y = (desktop->surface.height - DESKBAR_HEIGHT + 1) - height;
    }
    if (y < 0) y = 0;
    return (gui_rect_t){item.x + item.w - 1, y, DESKBAR_SUBMENU_W, height};
}

static gui_rect_t deskbar_sub_item_rect(const gui_desktop_t *desktop, int section, int item_index, int item_count) {
    gui_rect_t menu = deskbar_submenu_rect(desktop, section, item_count);
    return (gui_rect_t){menu.x + 3, menu.y + 3 + (item_index * DESKBAR_ITEM_H), menu.w - 6, DESKBAR_ITEM_H};
}

static gui_rect_t deskbar_cascade_item_rect(const gui_desktop_t *desktop,
                                            const deskbar_state_t *state,
                                            int level, int item_index,
                                            int item_count);
static bool deskbar_main_item_has_submenu(int index);

static gui_rect_t deskbar_submenu_from_parent_rect(const gui_desktop_t *desktop,
                                                   gui_rect_t parent,
                                                   int item_count) {
    int height = item_count * DESKBAR_ITEM_H + 6;
    int x = parent.x + parent.w - 1;
    int y = parent.y;

    if (x + DESKBAR_SUBMENU_W > desktop->surface.width) {
        x = parent.x - DESKBAR_SUBMENU_W + 1;
    }
    if (x < 0) x = 0;
    if (y + height > desktop->surface.height - DESKBAR_HEIGHT + 1) {
        y = (desktop->surface.height - DESKBAR_HEIGHT + 1) - height;
    }
    if (y < 0) y = 0;
    return (gui_rect_t){x, y, DESKBAR_SUBMENU_W, height};
}

static gui_rect_t deskbar_cascade_rect(const gui_desktop_t *desktop,
                                       const deskbar_state_t *state,
                                       int level, int item_count) {
    gui_rect_t parent;

    if (!desktop || !state || level < 0 ||
        level >= DESKBAR_MAX_CASCADE_LEVELS ||
        item_count <= 0) {
        return (gui_rect_t){0, 0, 0, 0};
    }

    if (level == 0) {
        parent = deskbar_sub_item_rect(desktop, state->open_section,
                                       state->cascade_parent_index[0],
                                       state->cached_count);
    } else {
        parent = deskbar_cascade_item_rect(desktop, state, level - 1,
                                           state->cascade_parent_index[level],
                                           state->cascade_count[level - 1]);
    }

    return deskbar_submenu_from_parent_rect(desktop, parent, item_count);
}

static gui_rect_t deskbar_cascade_item_rect(const gui_desktop_t *desktop,
                                            const deskbar_state_t *state,
                                            int level, int item_index,
                                            int item_count) {
    gui_rect_t menu = deskbar_cascade_rect(desktop, state, level, item_count);
    return (gui_rect_t){menu.x + 3, menu.y + 3 + (item_index * DESKBAR_ITEM_H),
                        menu.w - 6, DESKBAR_ITEM_H};
}

static int deskbar_hit_cascade_item(const gui_desktop_t *desktop,
                                    const deskbar_state_t *state,
                                    int level, int item_count, int x, int y) {
    for (int i = 0; i < item_count; i++) {
        if (gui_rect_contains(deskbar_cascade_item_rect(desktop, state,
                level, i, item_count), x, y)) return i;
    }
    return -1;
}

static void deskbar_invalidate(gui_desktop_t *desktop,
                               const deskbar_state_t *state) {
    if (!desktop || !state) return;
    /* A full-width bar plus a tall Start menu collapses into one huge dirty
     * bounding rectangle in the compositor. While the menu is open only the
     * Start button itself changes on the bar; keep the damage on the left. */
    if (state->menu_open)
        gui_desktop_invalidate_rect(desktop, deskbar_button_rect(desktop));
    else
        gui_desktop_invalidate_rect(desktop, deskbar_bar_rect(desktop));
    if (!state->menu_open) return;

    gui_desktop_invalidate_rect(desktop, deskbar_menu_rect(desktop));
    if (deskbar_main_item_has_submenu(state->open_section)) {
        int count = state->cached_count;
        if (count > 0)
            gui_desktop_invalidate_rect(desktop,
                deskbar_submenu_rect(desktop, state->open_section, count));
    }
    for (int level = 0;
         level < state->open_cascade_levels &&
         level < DESKBAR_MAX_CASCADE_LEVELS;
         level++) {
        int count = state->cascade_count[level];
        if (count > 0)
            gui_desktop_invalidate_rect(desktop,
                deskbar_cascade_rect(desktop, state, level, count));
    }
}

static uint32_t deskbar_visual_state_key(const deskbar_state_t *state) {
    uint32_t key;
    if (!state) return 0U;
    key = (state->menu_open ? 1U : 0U) |
          (state->button_hovered ? 2U : 0U) |
          (state->button_pressed ? 4U : 0U) |
          (state->clock_hovered ? 8U : 0U) |
          (state->clock_pressed ? 16U : 0U);
    key = key * 33U + state->hovered_window_id;
    key = key * 33U + state->pressed_window_id;
    key = key * 33U + (uint32_t)(state->hovered_launcher + 2);
    key = key * 33U + (uint32_t)(state->pressed_launcher + 2);
    key = key * 33U + (uint32_t)(state->resize_hover_side + 2);
    key = key * 33U + (uint32_t)(state->resize_drag_side + 2);
    key = key * 33U + (uint32_t)(state->hovered_main_item + 2);
    key = key * 33U + (uint32_t)(state->hovered_sub_item + 2);
    key = key * 33U + (uint32_t)(state->open_section + 2);
    key = key * 33U + (uint32_t)(state->open_cascade_levels + 1);
    for (int i = 0; i < DESKBAR_MAX_CASCADE_LEVELS; i++)
        key = key * 33U +
              (uint32_t)(state->hovered_cascade_item[i] + 2);
    return key;
}

static void deskbar_draw_panel(gui_surface_t *surface, gui_rect_t rect, uint32_t face, bool sunken) {
    if (!surface || rect.w <= 0 || rect.h <= 0) return;

    gui_gfx_fill_rect(surface, rect, face);
    if (rect.w < 2 || rect.h < 2) return;

    if (sunken) {
        gui_gfx_fill_rect(surface, (gui_rect_t){rect.x, rect.y, rect.w, 1}, 0x00484840);
        gui_gfx_fill_rect(surface, (gui_rect_t){rect.x, rect.y, 1, rect.h}, 0x00484840);
        gui_gfx_fill_rect(surface, (gui_rect_t){rect.x + 1, rect.y + 1, rect.w - 2, 1}, 0x00807C74);
        gui_gfx_fill_rect(surface, (gui_rect_t){rect.x + 1, rect.y + 1, 1, rect.h - 2}, 0x00807C74);
        gui_gfx_fill_rect(surface, (gui_rect_t){rect.x + rect.w - 1, rect.y, 1, rect.h}, 0x00FFFFFF);
        gui_gfx_fill_rect(surface, (gui_rect_t){rect.x, rect.y + rect.h - 1, rect.w, 1}, 0x00FFFFFF);
        gui_gfx_fill_rect(surface, (gui_rect_t){rect.x + rect.w - 2, rect.y + 1, 1, rect.h - 2}, 0x00D8D8D0);
        gui_gfx_fill_rect(surface, (gui_rect_t){rect.x + 1, rect.y + rect.h - 2, rect.w - 2, 1}, 0x00D8D8D0);
        return;
    }

    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x, rect.y, rect.w, 1}, 0x00FFFFFF);
    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x, rect.y, 1, rect.h}, 0x00FFFFFF);
    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x + 1, rect.y + 1, rect.w - 2, 1}, 0x00E4E4DC);
    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x + 1, rect.y + 1, 1, rect.h - 2}, 0x00E4E4DC);
    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x + rect.w - 1, rect.y, 1, rect.h}, 0x00484840);
    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x, rect.y + rect.h - 1, rect.w, 1}, 0x00484840);
    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x + rect.w - 2, rect.y + 1, 1, rect.h - 2}, 0x00807C74);
    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x + 1, rect.y + rect.h - 2, rect.w - 2, 1}, 0x00807C74);
}

static void deskbar_draw_arrow(gui_surface_t *surface, int x, int y, uint32_t color) {
    gui_gfx_fill_rect(surface, (gui_rect_t){x, y, 1, 7}, color);
    gui_gfx_fill_rect(surface, (gui_rect_t){x + 1, y + 1, 1, 5}, color);
    gui_gfx_fill_rect(surface, (gui_rect_t){x + 2, y + 2, 1, 3}, color);
}

static void deskbar_cde_draw_panel(gui_surface_t *surface, gui_rect_t rect,
                                   uint32_t face, bool sunken) {
    uint32_t tl0 = sunken ? CDE_DARK : CDE_FACE_LIGHT;
    uint32_t tl1 = sunken ? CDE_SHADOW : CDE_MID_LIGHT;
    uint32_t br0 = sunken ? CDE_FACE_LIGHT : CDE_DARK;
    uint32_t br1 = sunken ? CDE_MID_LIGHT : CDE_SHADOW;

    if (!surface || rect.w <= 0 || rect.h <= 0) return;
    gui_gfx_fill_rect(surface, rect, face);
    if (rect.w < 2 || rect.h < 2) return;

    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x, rect.y, rect.w, 1}, tl0);
    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x, rect.y, 1, rect.h}, tl0);
    gui_gfx_fill_rect(surface,
        (gui_rect_t){rect.x + rect.w - 1, rect.y, 1, rect.h}, br0);
    gui_gfx_fill_rect(surface,
        (gui_rect_t){rect.x, rect.y + rect.h - 1, rect.w, 1}, br0);

    if (rect.w < 4 || rect.h < 4) return;
    gui_gfx_fill_rect(surface,
        (gui_rect_t){rect.x + 1, rect.y + 1, rect.w - 2, 1}, tl1);
    gui_gfx_fill_rect(surface,
        (gui_rect_t){rect.x + 1, rect.y + 1, 1, rect.h - 2}, tl1);
    gui_gfx_fill_rect(surface,
        (gui_rect_t){rect.x + rect.w - 2, rect.y + 1, 1, rect.h - 2}, br1);
    gui_gfx_fill_rect(surface,
        (gui_rect_t){rect.x + 1, rect.y + rect.h - 2, rect.w - 2, 1}, br1);
}

static gui_rect_t deskbar_cde_launcher_rect(const gui_desktop_t *desktop,
                                             int index) {
    if (!desktop || index < 0 || index >= DESKBAR_LAUNCHER_COUNT)
        return (gui_rect_t){0, 0, 0, 0};
    return deskbar_design_rect(desktop,
        g_cde_launchers[index].design_x, 21,
        g_cde_launchers[index].design_w, 58);
}

static int deskbar_cde_hit_launcher(const gui_desktop_t *desktop, int x, int y) {
    for (int i = 0; i < DESKBAR_LAUNCHER_COUNT; i++)
        if (gui_rect_contains(deskbar_cde_launcher_rect(desktop, i), x, y))
            return i;
    return -1;
}

static void deskbar_cde_draw_launcher(gui_surface_t *surface,
                                      const gui_desktop_t *desktop,
                                      const deskbar_state_t *state,
                                      int index) {
    gui_rect_t cell;
    gui_rect_t button;
    int icon_size, icon_x, icon_y;
    bool hover, pressed;

    if (!surface || !desktop || !state ||
        index < 0 || index >= DESKBAR_LAUNCHER_COUNT) return;

    cell = deskbar_cde_launcher_rect(desktop, index);
    if (cell.w <= 0 || cell.h <= 0) return;
    hover = state->hovered_launcher == index;
    pressed = state->pressed_launcher == index;

    /* CDE keeps launchers sitting directly in the panel.  Only reveal a
       shallow bevel on interaction so the idle silhouette matches the BMP. */
    button = (gui_rect_t){cell.x + 2, cell.y + 1,
                          cell.w - 4, cell.h - 2};
    if (pressed)
        deskbar_cde_draw_panel(surface, button, 0x006B6970, true);
    else if (hover)
        deskbar_cde_draw_panel(surface, button, 0x00818086, false);

    /* Draw native 32px launchers without resampling. */
    icon_size = DESKBAR_LAUNCHER_ICON_SIZE;
    icon_x = cell.x + (cell.w - icon_size) / 2;
    icon_y = cell.y + (cell.h - icon_size) / 2;
    if (pressed) { icon_x++; icon_y++; }

    if (state->launcher_icon_loaded[index] &&
        state->launcher_icons[index].pixels) {
        bk_gui_surface_draw_image(surface,
            (gui_rect_t){icon_x, icon_y, icon_size, icon_size},
            cell, &state->launcher_icons[index]);
    } else {
        gui_rect_t fallback = {icon_x + 5, icon_y + 5,
                               icon_size - 10, icon_size - 10};
        deskbar_cde_draw_panel(surface, fallback, CDE_TOP_FACE, false);
        gui_font_draw_char(surface,
                           fallback.x + (fallback.w - 8) / 2,
                           fallback.y + (fallback.h - 8) / 2,
                           g_cde_launchers[index].label[0],
                           CDE_INK, 0, false);
    }
}

static gui_rect_t deskbar_cde_inset(gui_rect_t rect, int amount) {
    rect.x += amount;
    rect.y += amount;
    rect.w -= amount * 2;
    rect.h -= amount * 2;
    if (rect.w < 1) rect.w = 1;
    if (rect.h < 1) rect.h = 1;
    return rect;
}

static void deskbar_cde_draw_up_triangle(gui_surface_t *surface,
                                         int center_x, int top_y,
                                         uint32_t color) {
    if (!surface) return;
    for (int row = 0; row < 4; row++) {
        gui_gfx_fill_rect(surface,
            (gui_rect_t){center_x - row, top_y + row,
                         row * 2 + 1, 1}, color);
    }
}

static void deskbar_cde_draw_grip(gui_surface_t *surface, gui_rect_t rect,
                                  bool hovered, bool pressed) {
    uint32_t face;
    if (!surface || rect.w < 3 || rect.h < 4) return;
    face = pressed ? 0x006B6970U : (hovered ? CDE_FACE_HOVER : CDE_FACE);
    gui_gfx_fill_rect(surface, rect, face);
    gui_gfx_fill_rect(surface,
        (gui_rect_t){rect.x, rect.y, 1, rect.h}, 0x00E8EAE8);
    gui_gfx_fill_rect(surface,
        (gui_rect_t){rect.x + rect.w - 1, rect.y, 1, rect.h}, CDE_DARK);
    for (int y = rect.y + 3; y < rect.y + rect.h - 2; y += 3) {
        gui_gfx_fill_rect(surface,
            (gui_rect_t){rect.x + 3, y, rect.w - 6, 1}, 0x00E0E2E0);
        if (y + 1 < rect.y + rect.h - 1)
            gui_gfx_fill_rect(surface,
                (gui_rect_t){rect.x + 3, y + 1, rect.w - 6, 1},
                CDE_SHADOW);
    }
}

static bool deskbar_cde_datetime(const deskbar_state_t *state,
                                 rtc_datetime_t *shown) {
    rtc_datetime_t raw;
    if (!shown) return false;
    if (!rtc_get_datetime(&raw)) {
        raw.date.year = 2026;
        raw.date.month = 1;
        raw.date.day = 1;
        raw.time.hour = 0;
        raw.time.minute = 0;
        raw.time.second = 0;
    }
    bk_datetime_apply_preferences(state ? &state->clock_prefs : NULL,
                                  &raw, shown);
    return true;
}

static void deskbar_cde_draw_clock(gui_surface_t *surface,
                                   gui_rect_t rect,
                                   const deskbar_state_t *state) {
    static const int vx[12] =
        {0, 50, 87, 100, 87, 50, 0, -50, -87, -100, -87, -50};
    static const int vy[12] =
        {-100, -87, -50, 0, 50, 87, 100, 87, 50, 0, -50, -87};
    rtc_datetime_t dt;
    int cx, cy, radius, minute_slot, hour_slot;
    gui_rect_t inner;

    if (!surface || rect.w < 8 || rect.h < 8) return;
    deskbar_cde_draw_panel(surface, rect, CDE_TOP_FACE, false);
    inner = deskbar_cde_inset(rect, 3);
    gui_gfx_fill_rect(surface, inner, 0x00B9AFA1);
    gui_gfx_draw_rect(surface, inner, CDE_DARK);

    cx = inner.x + inner.w / 2;
    cy = inner.y + inner.h / 2;
    radius = (inner.w < inner.h ? inner.w : inner.h) / 2 - 3;
    if (radius < 3) radius = 3;

    for (int i = 0; i < 12; i++) {
        int tx = cx + (vx[i] * radius) / 100;
        int ty = cy + (vy[i] * radius) / 100;
        gui_gfx_fill_rect(surface, (gui_rect_t){tx, ty, 1, 1}, CDE_INK);
    }

    (void)deskbar_cde_datetime(state, &dt);
    minute_slot = ((int)dt.time.minute + 2) / 5;
    if (minute_slot >= 12) minute_slot = 0;
    hour_slot = (int)(dt.time.hour % 12U);
    if (dt.time.minute >= 30U) hour_slot = (hour_slot + 1) % 12;

    gui_gfx_draw_line(surface, cx, cy,
        cx + (vx[minute_slot] * (radius - 2)) / 100,
        cy + (vy[minute_slot] * (radius - 2)) / 100,
        CDE_INK);
    gui_gfx_draw_line(surface, cx, cy,
        cx + (vx[hour_slot] * (radius - 5)) / 100,
        cy + (vy[hour_slot] * (radius - 5)) / 100,
        CDE_DARK);
    gui_gfx_fill_rect(surface, (gui_rect_t){cx - 1, cy - 1, 3, 3},
                      CDE_DARK);
}

static void deskbar_cde_draw_calendar(gui_surface_t *surface,
                                      gui_rect_t rect,
                                      const deskbar_state_t *state) {
    static const char *months[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    rtc_datetime_t dt;
    char day[4];
    const char *month = "---";
    gui_rect_t paper;
    int tx;

    if (!surface || rect.w < 8 || rect.h < 12) return;
    deskbar_cde_draw_panel(surface, rect, CDE_FACE, false);
    paper = deskbar_cde_inset(rect, 3);
    gui_gfx_fill_rect(surface, paper, CDE_PAPER);
    gui_gfx_draw_rect(surface, paper, CDE_DARK);
    gui_gfx_fill_rect(surface,
        (gui_rect_t){paper.x, paper.y, paper.w, 3}, CDE_DARK);

    (void)deskbar_cde_datetime(state, &dt);
    if (dt.date.month >= 1U && dt.date.month <= 12U)
        month = months[dt.date.month - 1U];
    snprintf(day, sizeof(day), "%u", (unsigned)dt.date.day);

    tx = paper.x + (paper.w - (int)gui_font_text_width(month)) / 2;
    gui_font_draw_string(surface, tx, paper.y + 5, month, CDE_INK, 0, false);
    tx = paper.x + (paper.w - (int)gui_font_text_width(day)) / 2;
    gui_font_draw_string(surface, tx, paper.y + 16, day, CDE_INK, 0, false);

    gui_gfx_fill_rect(surface,
        (gui_rect_t){paper.x + 4, rect.y, 2, 5}, 0x00F4F5F0);
    gui_gfx_fill_rect(surface,
        (gui_rect_t){paper.x + paper.w - 6, rect.y, 2, 5}, 0x00F4F5F0);
}

static void deskbar_cde_draw_shell(gui_surface_t *surface,
                                   gui_desktop_t *desktop,
                                   deskbar_state_t *state) {
    static const int top_edges[] = {
        5, 25, 82, 140, 198, 256, 322, 646,
        711, 768, 824, 881, 940, 962
    };
    static const int arrow_cells[] = {4, 8, 11, 12};
    gui_rect_t bar;
    gui_rect_t button;
    uint32_t start_face;

    if (!surface || !desktop || !state) return;
    bar = deskbar_bar_rect(desktop);
    button = deskbar_button_rect(desktop);

    /* Outer CDE-style chassis.  The turquoise 1px outline and the two gray
       body values come directly from the supplied CDE BMP reference. */
    gui_gfx_fill_rect(surface, bar, CDE_EDGE_BLUE);
    if (bar.w > 2 && bar.h > 2) {
        gui_rect_t chassis = {bar.x + 1, bar.y + 1, bar.w - 2, bar.h - 2};
        deskbar_cde_draw_panel(surface, chassis, CDE_FACE, false);
    }
    if (bar.w > 8 && bar.h > 8)
        gui_gfx_fill_rect(surface,
            (gui_rect_t){bar.x + 3, bar.y + 3, bar.w - 6, bar.h - 6},
            CDE_FACE);

    /* The narrow top drawers/slots use the lighter #999999 face seen in the
       original instead of the body gray. */
    for (unsigned i = 0; i + 1U < sizeof(top_edges) / sizeof(top_edges[0]); i++) {
        gui_rect_t cell = deskbar_design_rect(desktop, top_edges[i], 4,
            top_edges[i + 1] - top_edges[i], 17);
        deskbar_cde_draw_panel(surface, cell, CDE_TOP_FACE, false);
    }

    for (unsigned i = 0; i < sizeof(arrow_cells) / sizeof(arrow_cells[0]); i++) {
        int cell = arrow_cells[i];
        int center = (top_edges[cell] + top_edges[cell + 1]) / 2;
        gui_rect_t p = deskbar_design_rect(desktop, center, 7, 1, 1);
        deskbar_cde_draw_up_triangle(surface, p.x, p.y, CDE_SHADOW);
        deskbar_cde_draw_up_triangle(surface, p.x, p.y - 1, 0x00E5E7E4);
    }

    /* Tiny minus mark in the first cell, matching the old CDE handle. */
    {
        gui_rect_t m = deskbar_design_rect(desktop, 11, 10, 9, 2);
        gui_gfx_fill_rect(surface, m, CDE_SHADOW);
        if (m.h > 1)
            gui_gfx_fill_rect(surface,
                (gui_rect_t){m.x, m.y, m.w, 1}, 0x00E7E9E6);
    }

    /* Body group dividers. */
    {
        const int split_x[] = {25, 322, 646, 940};
        for (unsigned i = 0; i < sizeof(split_x) / sizeof(split_x[0]); i++) {
            gui_rect_t line = deskbar_design_rect(desktop, split_x[i], 20, 2, 60);
            gui_gfx_fill_rect(surface, line, CDE_DARK);
            if (line.w > 1)
                gui_gfx_fill_rect(surface,
                    (gui_rect_t){line.x + 1, line.y, 1, line.h}, 0x00D9DCDA);
        }
    }

    deskbar_cde_draw_grip(surface, deskbar_resize_handle_rect(desktop, -1),
                          state->resize_hover_side < 0,
                          state->resize_drag_side < 0);
    deskbar_cde_draw_grip(surface, deskbar_resize_handle_rect(desktop, 1),
                          state->resize_hover_side > 0,
                          state->resize_drag_side > 0);

    /* v4 deliberately drops the hand-drawn clock/calendar.  The five cells
       at the left are now the same kind of icon-only launcher as the five on
       the right, which makes the front panel denser and more symmetrical. */

    /* All ten cells are real BlesKernOS programs. */
    for (int i = 0; i < DESKBAR_LAUNCHER_COUNT; i++)
        deskbar_cde_draw_launcher(surface, desktop, state, i);

    /* The former blank center well is the Bles/Start control. */
    start_face = state->button_hovered ? CDE_FACE_HOVER : CDE_START_FACE;
    deskbar_cde_draw_panel(surface, button, start_face, true);
    {
        /* Warm inner rim from the original recessed center well. */
        if (button.w > 6 && button.h > 6)
            gui_gfx_draw_rect(surface,
                (gui_rect_t){button.x + 3, button.y + 3,
                             button.w - 6, button.h - 6}, CDE_START_EDGE);

        int shift=(state->button_pressed || state->menu_open) ? 1 : 0;
        bool drawn=false;
        if (state->start_font_loaded) {
            if (state->start_button_width!=button.w || state->start_button_height!=button.h) {
                /* Native strikes and exact integer enlargements keep pixel widths uniform. */
                static const uint16_t sizes[]={48,36,28,24,18,14,12,10,8};
                uint16_t size=0, width=0, height=0;
                for (uint32_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
                    size=sizes[i];
                    width=bk_font_text_width(state->start_font,size,"Start",5);
                    height=bk_font_line_height(state->start_font,size);
                    if (width && height && width<=button.w-12 && height<=button.h-6) break;
                    size=0;
                }
                state->start_font_size=size;state->start_text_width=width;state->start_text_height=height;
                state->start_button_width=button.w;state->start_button_height=button.h;
            }
            uint16_t size=state->start_font_size, width=state->start_text_width, height=state->start_text_height;
            if (size>=8 && width && height) {
                int x=button.x+(button.w-width)/2+shift;
                int y=button.y+(button.h-height)/2+shift;
                drawn=bk_font_draw_text(surface,state->start_font,size,x,y,"Start",5,0x00c0c0c0,button);
            }
        }
        if (!drawn) gui_font_draw_string(surface,
            button.x+(button.w-5*8)/2+shift,button.y+(button.h-8)/2+shift,
            "Start",0x00c0c0c0,0,false);

        if (state->button_pressed || state->menu_open) {
            gui_gfx_draw_rect(surface, button, CDE_DARK);
            if (button.w > 2 && button.h > 2)
                gui_gfx_draw_rect(surface,
                    (gui_rect_t){button.x + 1, button.y + 1,
                                 button.w - 2, button.h - 2},
                    CDE_SHADOW);
        }
    }
}

static uint32_t deskbar_ticks_from_ms(uint32_t ms) {
    uint32_t hz = pit_get_frequency_hz();
    uint64_t ticks;
    if (!hz) hz = 100U;
    ticks = ((uint64_t)hz * (uint64_t)ms + 999ULL) / 1000ULL;
    return ticks ? (uint32_t)ticks : 1U;
}

static void deskbar_focus_window(gui_desktop_t *desktop, gui_window_t *window) {
    if (!desktop || !window) return;

    if (window->minimized) {
        gui_rect_t from;
        gui_rect_t to = window->bounds;
        if (!deskbar_window_button_rect(desktop, window, &from)) {
            from = (gui_rect_t){
                desktop->surface.width / 2 - 24,
                desktop->surface.height - 27,
                48, 24
            };
        }
        (void)bk_system_sound_event_play(BK_SYSTEM_SOUND_WINDOW_RESTORE);
        gui_desktop_start_restore_animation(
            desktop, window->id, from, to, pit_get_ticks(),
            deskbar_ticks_from_ms(180U));
        return;
    }

    gui_desktop_raise_window(desktop, window);
    gui_desktop_focus_window(desktop, window);
}

#ifndef BLES_ARCH_ARM32_RPI3
static inline void deskbar_outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}
#endif

static void deskbar_power_off(void) {
#ifdef BLES_ARCH_ARM32_RPI3
    /* Firmware power management is platform-specific and will be wired to
     * the Raspberry mailbox in a later stage. Do not issue PC ACPI ports. */
    kprintf("[DESKBAR] power-off requested (ARM backend pending)\n");
    return;
#else
    cli();
    deskbar_outw(0x604, 0x2000);
    deskbar_outw(0xB004, 0x2000);
    deskbar_outw(0x4004, 0x3400);
    for (;;) __asm__ volatile ("hlt");
#endif
}

static bool deskbar_main_item_has_submenu(int index) {
    return index >= 2 && index <= 5;
}

static void deskbar_make_pretty_name(const char *src, char *dst, size_t dst_len) {
    size_t pos = 0;
    bool upper_next = true;

    if (!dst || !dst_len) return;
    dst[0] = '\0';
    if (!src) return;

    while (*src && *src != '.' && pos + 1 < dst_len) {
        char c = *src++;
        if (c == '_') c = ' ';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (upper_next && c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
            upper_next = false;
        } else if (c == ' ') {
            upper_next = true;
        }
        dst[pos++] = c;
    }
    dst[pos] = '\0';
}

static void deskbar_join_path(char *dst, size_t dst_len, const char *base, const char *name) {
    size_t pos = 0;

    if (!dst || !dst_len) return;
    dst[0] = '\0';
    if (!base) base = "/";
    if (!name) name = "";

    while (*base && pos + 1 < dst_len) dst[pos++] = *base++;
    if (pos > 0 && dst[pos - 1] != '/' && pos + 1 < dst_len) dst[pos++] = '/';
    while (*name && pos + 1 < dst_len) dst[pos++] = *name++;
    dst[pos] = '\0';
}

static bool deskbar_should_list_entry(const vfs_dir_entry_t *entry) {
    if (!entry) return false;
    if (entry->type != VFS_NODE_FILE && entry->type != VFS_NODE_DIR) return false;
    if (!entry->name[0]) return false;
    if (entry->name[0] == '.') return false;
    return true;
}

static bool deskbar_is_executable(const char *filename) {
    if (!filename) return false;
    const char *ext = filename;
    while (*ext && *ext != '.') ext++;
    if (!*ext) return false;
    // Comparación insensible a mayúsculas
    if (kstrcmp(ext, ".o") == 0 || kstrcmp(ext, ".O") == 0 ||
        kstrcmp(ext, ".bex") == 0 || kstrcmp(ext, ".BEX") == 0 ||
        kstrcmp(ext, ".cpl") == 0 || kstrcmp(ext, ".CPL") == 0 ||
        kstrcmp(ext, ".exe") == 0 || kstrcmp(ext, ".EXE") == 0)
        return true;
    return false;
}

static void deskbar_add_menu_entry(deskbar_sub_item_t *item,
                                   const char *dir,
                                   const vfs_dir_entry_t *entry) {
    if (!item || !entry) return;
    deskbar_make_pretty_name(entry->name, item->label, sizeof(item->label));
    deskbar_join_path(item->path, sizeof(item->path), dir, entry->name);
    item->enabled = true;
    item->is_dir = entry->type == VFS_NODE_DIR;
}

static int deskbar_build_dir_menu(const char *dir, deskbar_sub_item_t *items,
                                  int max_items, const char *empty_label) {
    vfs_dir_entry_t entries[DESKBAR_MAX_SUB_ITEMS];
    uint32_t count = 0;
    int actual = 0;

    if (!items || max_items <= 0) return 0;
    if (!dir) dir = "/";
    if (!empty_label) empty_label = "Empty";

    if (!vfs_listdir(dir, entries, DESKBAR_MAX_SUB_ITEMS, &count) || count == 0) {
        deskbar_copy_text(items[0].label, sizeof(items[0].label), empty_label);
        items[0].path[0] = '\0';
        items[0].enabled = false;
        items[0].is_dir = false;
        return 1;
    }

    /*
     * Estilo Windows 95: carpetas primero, archivos despues.
     * Asi al pasar el cursor por una carpeta se abre la continuacion
     * hacia la derecha.
     */
    for (uint32_t pass = 0; pass < 2 && actual < max_items; pass++) {
        for (uint32_t i = 0; i < count && actual < max_items; i++) {
            if (!deskbar_should_list_entry(&entries[i])) continue;
            if (pass == 0 && entries[i].type != VFS_NODE_DIR) continue;
            if (pass == 1 && entries[i].type == VFS_NODE_DIR) continue;
            deskbar_add_menu_entry(&items[actual], dir, &entries[i]);
            actual++;
        }
    }

    if (actual == 0) {
        deskbar_copy_text(items[0].label, sizeof(items[0].label), empty_label);
        items[0].path[0] = '\0';
        items[0].enabled = false;
        items[0].is_dir = false;
        return 1;
    }

    return actual;
}

static int deskbar_build_submenu(int section, deskbar_sub_item_t *items, int max_items) {
    const char *dir = NULL;
    const char *empty_label = "@H91EE7FB9";

    if (section == 2) {
        dir = "/";
        empty_label = "Empty";
    } else if (section == 3) {
        dir = "/SYSTEM/PROGRAMS";
        empty_label = "@HC4395171";
    } else if (section == 4) {
        dir = "/DOCS";
    } else if (section == 5) {
        dir = "/MISC";
    } else return 0;

    if (section==3 && max_items>=2) {
        int count=deskbar_build_dir_menu(dir,items+2,max_items-2,empty_label);
        const char *labels[]={"Desfragmentador de disco","Viewer"};
        const char *paths[]={"/SYSTEM/PROGRAMS/DEFRAG.BEX","/SYSTEM/PROGRAMS/VIEWER.BEX"};
        for (int i=0;i<2;i++) {
            kmemset(&items[i],0,sizeof(items[i]));
            deskbar_copy_text(items[i].label,sizeof(items[i].label),labels[i]);
            deskbar_copy_text(items[i].path,sizeof(items[i].path),paths[i]);
            items[i].enabled=true;
        }
        for (int i=2;i<count+2;i++)
            if (kstrcmp(items[i].path,paths[0])==0 || kstrcmp(items[i].path,paths[1])==0) {
                for (int j=i;j<count+1;j++) items[j]=items[j+1];
                count--; i--;
            }
        return count+2;
    }
    return deskbar_build_dir_menu(dir, items, max_items, empty_label);
}

static void deskbar_open_cascade_level(deskbar_state_t *state, int level,
                                       const char *path, int parent_index) {
    if (!state || !path) return;
    if (level < 0 || level >= DESKBAR_MAX_CASCADE_LEVELS) return;

    if (state->open_cascade_levels > level &&
        state->cascade_parent_index[level] == parent_index &&
        kstrcmp(state->cascade_path[level], path) == 0) {
        return;
    }

    deskbar_clear_cascade_from(state, level);
    state->cascade_parent_index[level] = parent_index;
    deskbar_copy_text(state->cascade_path[level],
                      sizeof(state->cascade_path[level]), path);
    state->cascade_count[level] = deskbar_build_dir_menu(
        path, state->cascade_items[level], DESKBAR_MAX_SUB_ITEMS, "Empty");
    state->hovered_cascade_item[level] = -1;
    state->open_cascade_levels = level + 1;
}

static void deskbar_cache_submenu(deskbar_state_t *state, int section) {
    if (!state || section <= 0) return;
    if (state->cached_section == section) return;
    state->cached_count = deskbar_build_submenu(
        section, state->cached_items, DESKBAR_MAX_SUB_ITEMS);
    state->cached_section = section;
    deskbar_clear_cascade_from(state, 0);
}

static int deskbar_window_item_width(gui_window_t *window) {
    int width = (int)gui_font_text_width(window->title) + 16;
    if (width < 48) width = 48;
    if (width > 92) width = 92;
    return width;
}


bool deskbar_window_button_rect(const gui_desktop_t *desktop,
                                const gui_window_t *target,
                                gui_rect_t *out) {
    gui_window_t *window;
    gui_rect_t area;
    int cursor;

    if (!desktop || !target || !out) return false;
    area = deskbar_window_area_rect(desktop);
    cursor = area.x;
    task_preempt_disable();
    window = desktop->first_window;
    while (window) {
        int width;
        gui_rect_t item;
        if (!window->listed) {
            window = window->next;
            continue;
        }
        width = deskbar_window_item_width(window);
        item = (gui_rect_t){cursor, area.y, width, area.h};
        if (cursor + width > area.x + area.w) break;
        if (window == target) {
            *out = item;
            task_preempt_enable();
            return true;
        }
        cursor += width + 4;
        window = window->next;
    }
    task_preempt_enable();
    return false;
}

static gui_window_t *deskbar_window_at(gui_desktop_t *desktop, int x, int y) {
    gui_window_t *window;
    gui_rect_t area;
    int cursor;

    if (!desktop) return NULL;
    area = deskbar_window_area_rect(desktop);

    /* Deskbar receives the desktop mouse stream, not just events inside the
     * task buttons.  Avoid walking every listed window and measuring every
     * title for the overwhelmingly common case where the pointer is
     * elsewhere (desktop, Start menu, client windows, clock, etc.). */
    if (!gui_rect_contains(area, x, y)) return NULL;

    cursor = area.x;
    task_preempt_disable();
    window = desktop->first_window;

    while (window) {
        if (!window->listed) {
            window = window->next;
            continue;
        }
        int width = deskbar_window_item_width(window);
        gui_rect_t item = (gui_rect_t){cursor, area.y, width, area.h};

        if (cursor + width > area.x + area.w) break;
        if (gui_rect_contains(item, x, y)) {
            task_preempt_enable();
            return window;
        }

        cursor += width + 4;
        window = window->next;
    }

    task_preempt_enable();
    return NULL;
}

static int deskbar_hit_main_item(const gui_desktop_t *desktop, int x, int y) {
    for (int i = 0; i < DESKBAR_MAIN_ITEMS; i++) {
        if (gui_rect_contains(deskbar_main_item_rect(desktop, i), x, y)) return i;
    }
    return -1;
}

static int deskbar_hit_sub_item(const gui_desktop_t *desktop, int section, const deskbar_sub_item_t *items, int item_count, int x, int y) {
    (void)items;
    for (int i = 0; i < item_count; i++) {
        if (gui_rect_contains(deskbar_sub_item_rect(desktop, section, i, item_count), x, y)) return i;
    }
    return -1;
}

static void deskbar_draw_menu_item(gui_surface_t *surface, gui_rect_t rect,
                                   const char *label, bool active,
                                   bool sunken, bool enabled, bool has_arrow,
                                   const gui_image_t *icon) {
    int text_x = rect.x + 8;
    if (active) {
        deskbar_draw_panel(surface, rect, sunken ? 0x00282868 : 0x00383878, sunken);
    } else {
        gui_gfx_fill_rect(surface, rect, 0x00C0C0C0);
    }

    if (icon && icon->pixels) {
        bk_gui_surface_draw_image(surface,
            (gui_rect_t){rect.x + 4, rect.y + (rect.h - 32) / 2, 32, 32},
            rect, icon);
        text_x = rect.x + 43;
    }
    gui_rect_t text_clip = {
        text_x, rect.y + 2,
        rect.x + rect.w - text_x - (has_arrow ? 18 : 6), rect.h - 4
    };
    gui_font_draw_string_clipped(surface, text_x,
                                 rect.y + (rect.h - 8) / 2,
                                 label, enabled ? (active ? 0x00FFFFFF : 0x00101010) : 0x00707070,
                                 text_clip);
    if (has_arrow)
        deskbar_draw_arrow(surface, rect.x + rect.w - 10,
                           rect.y + (rect.h - 7) / 2,
                           enabled ? (active ? 0x00FFFFFF : 0x00101010)
                                   : 0x00707070);
}

static void deskbar_draw_windows(gui_desktop_t *desktop, gui_surface_t *surface, const deskbar_state_t *state) {
    gui_window_t *window;
    gui_rect_t area;
    int cursor;

    if (!desktop || !surface || !state) return;

    area = deskbar_window_area_rect(desktop);
    cursor = area.x;
    task_preempt_disable();
    window = desktop->first_window;

    while (window) {
        if (!window->listed) {
            window = window->next;
            continue;
        }
        int width = deskbar_window_item_width(window);
        gui_rect_t item = (gui_rect_t){cursor, area.y, width, area.h};
        uint32_t face = window->focused ? 0x00B8B8C8 : 0x00C0C0C0;
        bool sunken = window->focused;

        if (cursor + width > area.x + area.w) break;
        if (state->pressed_window_id == window->id) {
            face = 0x00A8A8B8;
            sunken = true;
        } else if (state->hovered_window_id == window->id) {
            face = 0x00D0D0D0;
        }

        int title_x = item.x + 6;
        deskbar_draw_panel(surface, item, face, sunken);
        if (window->icon_pixels && window->icon_width && window->icon_height) {
            gui_image_t icon = {
                (uint32_t *)window->icon_pixels,
                window->icon_width,
                window->icon_height
            };
            bk_gui_surface_draw_image(surface,
                (gui_rect_t){item.x + 5, item.y + 4, 16, 16}, item, &icon);
            title_x = item.x + 25;
        }
        gui_font_draw_string_clipped(surface, title_x, item.y + 8,
                                     window->title, 0x00101010,
                                     (gui_rect_t){title_x, item.y + 2,
                                                  item.x + item.w - title_x - 4,
                                                  item.h - 4});

        cursor += width + 4;
        window = window->next;
    }
    task_preempt_enable();
}

static void deskbar_launch_menu_item(gui_desktop_t *desktop,
                                     const deskbar_sub_item_t *item) {
    if (!desktop || !item || !item->enabled || item->is_dir) return;

    if (deskbar_is_executable(item->path)) {
        (void)program_execute_path_async(desktop, item->path);
    } else {
        (void)program_execute_path_async_arg(desktop, "/SYSTEM/PROGRAMS/TEXTEDITOR.BEX",
                                       item->path);
    }
}

static void deskbar_paint(gui_program_t *program, gui_desktop_t *desktop, gui_surface_t *surface) {
    deskbar_state_t *state = (deskbar_state_t *)program->state;
    int sub_count = 0;

    if (!desktop || !surface || !state) return;

    deskbar_load_clock_preferences(state);
    deskbar_cde_draw_shell(surface, desktop, state);

    if (!state->menu_open) return;

    {
        gui_rect_t menu = deskbar_menu_rect(desktop);
        deskbar_draw_panel(surface, menu, 0x00C0C0C0, false);

        for (int i = 0; i < DESKBAR_MAIN_ITEMS; i++) {
            gui_rect_t item = deskbar_main_item_rect(desktop, i);
            bool active = i == state->hovered_main_item ||
                          (i == state->open_section &&
                           deskbar_main_item_has_submenu(i));
            deskbar_draw_menu_item(surface, item, g_main_items[i], active,
                                   i == state->pressed_main_item, true,
                                   deskbar_main_item_has_submenu(i),
                                   state->main_icon_loaded[i]
                                       ? &state->main_icons[i] : NULL);
            if (i == 0 || i == 1) {
                gui_gfx_fill_rect(surface, (gui_rect_t){item.x + 2, item.y + item.h - 1, item.w - 4, 1}, 0x00807C74);
                gui_gfx_fill_rect(surface, (gui_rect_t){item.x + 2, item.y + item.h, item.w - 4, 1}, 0x00FFFFFF);
            }
            if (i == DESKBAR_MAIN_ITEMS - 2) {
                gui_gfx_fill_rect(surface,
                    (gui_rect_t){item.x + 2, item.y + item.h - 1,
                                 item.w - 4, 1},
                    0x00807C74);
                gui_gfx_fill_rect(surface,
                    (gui_rect_t){item.x + 2, item.y + item.h,
                                 item.w - 4, 1},
                    0x00FFFFFF);
            }
        }

        if (deskbar_main_item_has_submenu(state->open_section)) {
            deskbar_cache_submenu(state, state->open_section);
            sub_count = state->cached_count;
            if (sub_count > 0) {
                gui_rect_t submenu = deskbar_submenu_rect(desktop, state->open_section, sub_count);
                deskbar_draw_panel(surface, submenu, 0x00C0C0C0, false);
                for (int i = 0; i < sub_count; i++) {
                    gui_rect_t item = deskbar_sub_item_rect(desktop, state->open_section, i, sub_count);
                    bool active = i == state->hovered_sub_item;
                    deskbar_draw_menu_item(surface, item,
                        state->cached_items[i].label, active,
                        i == state->pressed_sub_item,
                        state->cached_items[i].enabled,
                        state->cached_items[i].enabled &&
                        state->cached_items[i].is_dir,
                        state->cached_items[i].is_dir
                            ? (state->folder_icon_loaded
                                ? &state->folder_icon : NULL)
                            : (deskbar_is_executable(
                                    state->cached_items[i].path)
                                ? (state->application_icon_loaded
                                    ? &state->application_icon : NULL)
                                : (state->document_icon_loaded
                                    ? &state->document_icon : NULL)));
                }
            }

            for (int level = 0; level < state->open_cascade_levels; level++) {
                int count = state->cascade_count[level];
                if (count <= 0) continue;

                gui_rect_t cascade = deskbar_cascade_rect(desktop, state,
                                                          level, count);
                deskbar_draw_panel(surface, cascade, 0x00C0C0C0, false);

                for (int i = 0; i < count; i++) {
                    gui_rect_t item = deskbar_cascade_item_rect(desktop,
                        state, level, i, count);
                    bool active = i == state->hovered_cascade_item[level];
                    bool pressed = state->pressed_cascade_level == level &&
                                   state->pressed_cascade_item == i;

                    deskbar_draw_menu_item(surface, item,
                        state->cascade_items[level][i].label, active,
                        pressed,
                        state->cascade_items[level][i].enabled,
                        state->cascade_items[level][i].enabled &&
                        state->cascade_items[level][i].is_dir,
                        state->cascade_items[level][i].is_dir
                            ? (state->folder_icon_loaded
                                ? &state->folder_icon : NULL)
                            : (deskbar_is_executable(
                                    state->cascade_items[level][i].path)
                                ? (state->application_icon_loaded
                                    ? &state->application_icon : NULL)
                                : (state->document_icon_loaded
                                    ? &state->document_icon : NULL)));
                }
            }
        }
    }
}

static bool deskbar_handle_event(gui_program_t *program, gui_desktop_t *desktop, const gui_event_t *event) {
    deskbar_state_t *state = (deskbar_state_t *)program->state;
    gui_rect_t bar;
    gui_rect_t button;
    gui_rect_t status;
    gui_rect_t menu;
    bool on_bar;
    bool on_button;
    bool on_status;
    bool on_menu = false;
    bool on_submenu = false;
    int main_hit = -1;
    int sub_hit = -1;
    int launcher_hit = -1;
    int resize_hit = 0;
    int sub_count = 0;
    int cascade_hit_level = -1;
    int cascade_hit_item = -1;
    gui_window_t *hit_window;
    uint32_t old_visual_key;
    bool old_menu_open;
    bool old_button_hovered;
    bool old_clock_hovered;
    int old_hovered_launcher;
    int old_resize_hover_side;
    uint32_t old_hovered_window_id;
    int old_hovered_main_item;
    int old_hovered_sub_item;
    int old_open_section;
    int old_cached_section;
    int old_cached_count;
    int old_open_cascade_levels;
    int old_hovered_cascade[DESKBAR_MAX_CASCADE_LEVELS];
    int old_cascade_count[DESKBAR_MAX_CASCADE_LEVELS];
    int old_cascade_parent[DESKBAR_MAX_CASCADE_LEVELS];
    gui_rect_t old_submenu_rect = {0, 0, 0, 0};
    gui_rect_t old_cascade_rect[DESKBAR_MAX_CASCADE_LEVELS];
    bool old_submenu_valid = false;
    bool old_cascade_valid[DESKBAR_MAX_CASCADE_LEVELS];

    if (!desktop || !event || !state) return false;
    if (event->type != GUI_EVENT_MOUSE_MOVE &&
        event->type != GUI_EVENT_MOUSE_DOWN &&
        event->type != GUI_EVENT_MOUSE_UP) return false;
    old_menu_open = state->menu_open;
    old_visual_key = deskbar_visual_state_key(state);
    old_button_hovered = state->button_hovered;
    old_clock_hovered = state->clock_hovered;
    old_hovered_launcher = state->hovered_launcher;
    old_resize_hover_side = state->resize_hover_side;
    old_hovered_window_id = state->hovered_window_id;
    old_hovered_main_item = state->hovered_main_item;
    old_hovered_sub_item = state->hovered_sub_item;
    old_open_section = state->open_section;
    old_cached_section = state->cached_section;
    old_cached_count = state->cached_count;
    old_open_cascade_levels = state->open_cascade_levels;
    if (old_menu_open && deskbar_main_item_has_submenu(old_open_section) &&
        old_cached_count > 0) {
        old_submenu_rect = deskbar_submenu_rect(desktop, old_open_section,
                                                old_cached_count);
        old_submenu_valid = true;
    }
    for (int i = 0; i < DESKBAR_MAX_CASCADE_LEVELS; i++) {
        old_hovered_cascade[i] = state->hovered_cascade_item[i];
        old_cascade_count[i] = state->cascade_count[i];
        old_cascade_parent[i] = state->cascade_parent_index[i];
        old_cascade_valid[i] = old_menu_open &&
            i < old_open_cascade_levels && old_cascade_count[i] > 0;
        old_cascade_rect[i] = old_cascade_valid[i]
            ? deskbar_cascade_rect(desktop, state, i, old_cascade_count[i])
            : (gui_rect_t){0, 0, 0, 0};
    }
    /* Do not dirty the complete open menu before every mouse-move. That made
       a one-row hover repaint hundreds of thousands of pixels. Mouse buttons
       are infrequent and still preserve the old-state invalidation path. */
    if (state->menu_open && event->type != GUI_EVENT_MOUSE_MOVE) {
        deskbar_invalidate(desktop, state);
    } else if (!state->menu_open && event->type == GUI_EVENT_MOUSE_DOWN) {
        /* Paint the pressed state after dispatch, but keep ordinary closed-bar
         * damage as a cheap 30-pixel strip. */
        deskbar_invalidate(desktop, state);
    } else if (event->type == GUI_EVENT_MOUSE_UP) {
        /* Releasing Start may open a tall menu.  Dirtying the whole bar here
         * would union that menu with the entire screen width. */
        if (state->button_pressed)
            gui_desktop_invalidate_rect(desktop,
                                        deskbar_button_rect(desktop));
        else if (state->clock_pressed)
            gui_desktop_invalidate_rect(desktop,
                                        deskbar_status_rect(desktop));
        else
            deskbar_invalidate(desktop, state);
    }

    if (desktop->drag_window && !state->menu_open &&
        !state->button_pressed && state->pressed_window_id == 0) {
        state->button_hovered = false;
        state->clock_hovered = false;
        state->hovered_launcher = -1;
        state->hovered_window_id = 0;
        return false;
    }

    bar = deskbar_bar_rect(desktop);
    button = deskbar_button_rect(desktop);
    status = deskbar_status_rect(desktop);
    menu = deskbar_menu_rect(desktop);
    on_bar = gui_rect_contains(bar, event->x, event->y);
    on_button = gui_rect_contains(button, event->x, event->y);
    on_status = gui_rect_contains(status, event->x, event->y);
    resize_hit = deskbar_resize_hit_side(desktop, event->x, event->y);
    launcher_hit = resize_hit ? -1 :
        deskbar_cde_hit_launcher(desktop, event->x, event->y);
    hit_window = resize_hit ? NULL :
        deskbar_window_at(desktop, event->x, event->y);

    /* Resizing owns the pointer until release.  The panel stays centered, so
       dragging either ribbed side grip changes both outer margins equally. */
    if (state->resize_drag_side) {
        if (event->type == GUI_EVENT_MOUSE_MOVE) {
            deskbar_resize_to_pointer(desktop, state, event->x);
            state->resize_hover_side = state->resize_drag_side;
            gui_desktop_set_cursor_style(desktop, GUI_CURSOR_SIZE_WE);
            return true;
        }
        if (event->type == GUI_EVENT_MOUSE_UP) {
            gui_rect_t current_bar;
            state->resize_drag_side = 0;
            state->resize_hover_side =
                deskbar_resize_hit_side(desktop, event->x, event->y);
            current_bar = deskbar_bar_rect(desktop);
            gui_desktop_invalidate_rect(desktop, current_bar);
            gui_desktop_set_cursor_style(desktop,
                state->resize_hover_side ? GUI_CURSOR_SIZE_WE : GUI_CURSOR_ARROW);
            gui_request_paint();
            return true;
        }
    }

    if (state->menu_open) {
        on_menu = gui_rect_contains(menu, event->x, event->y);
        main_hit = deskbar_hit_main_item(desktop, event->x, event->y);

        if (deskbar_main_item_has_submenu(state->open_section)) {
            deskbar_cache_submenu(state, state->open_section);
            sub_count = state->cached_count;

            if (sub_count > 0) {
                gui_rect_t submenu = deskbar_submenu_rect(desktop,
                    state->open_section, sub_count);
                if (gui_rect_contains(submenu, event->x, event->y)) {
                    on_submenu = true;
                    sub_hit = deskbar_hit_sub_item(desktop,
                        state->open_section, state->cached_items,
                        sub_count, event->x, event->y);
                }
            }

            for (int level = 0;
                 level < state->open_cascade_levels &&
                 level < DESKBAR_MAX_CASCADE_LEVELS;
                 level++) {
                int count = state->cascade_count[level];
                if (count <= 0) continue;

                gui_rect_t cascade = deskbar_cascade_rect(desktop, state,
                                                          level, count);
                if (gui_rect_contains(cascade, event->x, event->y)) {
                    on_submenu = true;
                    cascade_hit_level = level;
                    cascade_hit_item = deskbar_hit_cascade_item(desktop,
                        state, level, count, event->x, event->y);
                    break;
                }
            }
        }
    }

    if (event->type == GUI_EVENT_MOUSE_MOVE) {
        state->resize_hover_side = resize_hit;
        if (resize_hit)
            gui_desktop_set_cursor_style(desktop, GUI_CURSOR_SIZE_WE);
        else if (old_resize_hover_side)
            gui_desktop_set_cursor_style(desktop, GUI_CURSOR_ARROW);
        state->button_hovered = resize_hit ? false : on_button;
        state->clock_hovered = resize_hit ? false : on_status;
        state->hovered_launcher = resize_hit ? -1 : launcher_hit;
        state->hovered_window_id = hit_window ? hit_window->id : 0;

        if (state->menu_open) {
            for (int i = 0; i < DESKBAR_MAX_CASCADE_LEVELS; i++)
                state->hovered_cascade_item[i] = -1;

            if (cascade_hit_level >= 0) {
                state->hovered_main_item = -1;
                state->hovered_sub_item = -1;
                state->hovered_cascade_item[cascade_hit_level] =
                    cascade_hit_item;

                if (cascade_hit_item >= 0 &&
                    cascade_hit_level < state->open_cascade_levels) {
                    deskbar_sub_item_t *item =
                        &state->cascade_items[cascade_hit_level]
                                             [cascade_hit_item];

                    if (item->enabled && item->is_dir) {
                        deskbar_open_cascade_level(state,
                            cascade_hit_level + 1, item->path,
                            cascade_hit_item);
                    } else {
                        deskbar_clear_cascade_from(state,
                            cascade_hit_level + 1);
                    }
                }
            } else {
                state->hovered_main_item = main_hit;
                state->hovered_sub_item = sub_hit;

                if (deskbar_main_item_has_submenu(main_hit) &&
                    state->open_section != main_hit) {
                    state->open_section = main_hit;
                    deskbar_cache_submenu(state, main_hit);
                    deskbar_clear_cascade_from(state, 0);
                } else if (main_hit >= 0 &&
                           !deskbar_main_item_has_submenu(main_hit)) {
                    state->open_section = 0;
                    deskbar_clear_cascade_from(state, 0);
                } else if (sub_hit >= 0 && sub_hit < sub_count) {
                    deskbar_sub_item_t *item = &state->cached_items[sub_hit];
                    if (item->enabled && item->is_dir) {
                        deskbar_open_cascade_level(state, 0, item->path,
                                                   sub_hit);
                    } else {
                        deskbar_clear_cascade_from(state, 0);
                    }
                } else if (!on_submenu && main_hit >= 0) {
                    deskbar_clear_cascade_from(state, 0);
                }
            }
        }

        if (deskbar_visual_state_key(state) != old_visual_key) {
            bool structural = old_open_section != state->open_section ||
                old_cached_section != state->cached_section ||
                old_open_cascade_levels != state->open_cascade_levels;
            for (int i = 0; i < DESKBAR_MAX_CASCADE_LEVELS; i++) {
                if (old_cascade_count[i] != state->cascade_count[i] ||
                    old_cascade_parent[i] != state->cascade_parent_index[i])
                    structural = true;
            }

            if (structural) {
                /* Restore menus that disappeared/moved, then paint the new
                   hierarchy. Structural changes are rare compared with hover. */
                if (old_submenu_valid)
                    gui_desktop_invalidate_rect(desktop, old_submenu_rect);
                for (int i = 0; i < DESKBAR_MAX_CASCADE_LEVELS; i++)
                    if (old_cascade_valid[i])
                        gui_desktop_invalidate_rect(desktop,
                                                    old_cascade_rect[i]);
                deskbar_invalidate(desktop, state);
            } else {
                if (old_button_hovered != state->button_hovered)
                    gui_desktop_invalidate_rect(desktop,
                                                deskbar_button_rect(desktop));
                if (old_clock_hovered != state->clock_hovered)
                    gui_desktop_invalidate_rect(desktop,
                                                deskbar_status_rect(desktop));
                if (old_resize_hover_side != state->resize_hover_side) {
                    if (old_resize_hover_side)
                        gui_desktop_invalidate_rect(desktop,
                            deskbar_resize_handle_rect(desktop,
                                old_resize_hover_side));
                    if (state->resize_hover_side)
                        gui_desktop_invalidate_rect(desktop,
                            deskbar_resize_handle_rect(desktop,
                                state->resize_hover_side));
                }
                if (old_hovered_launcher != state->hovered_launcher) {
                    if (old_hovered_launcher >= 0)
                        gui_desktop_invalidate_rect(desktop,
                            deskbar_cde_launcher_rect(desktop, old_hovered_launcher));
                    if (state->hovered_launcher >= 0)
                        gui_desktop_invalidate_rect(desktop,
                            deskbar_cde_launcher_rect(desktop, state->hovered_launcher));
                }
                if (old_hovered_window_id != state->hovered_window_id)
                    gui_desktop_invalidate_rect(desktop,
                                                deskbar_window_area_rect(desktop));
                if (old_hovered_main_item != state->hovered_main_item) {
                    if (old_hovered_main_item >= 0)
                        gui_desktop_invalidate_rect(desktop,
                            deskbar_main_item_rect(desktop,
                                                   old_hovered_main_item));
                    if (state->hovered_main_item >= 0)
                        gui_desktop_invalidate_rect(desktop,
                            deskbar_main_item_rect(desktop,
                                                   state->hovered_main_item));
                }
                if (old_hovered_sub_item != state->hovered_sub_item &&
                    state->open_section > 0 && state->cached_count > 0) {
                    if (old_hovered_sub_item >= 0)
                        gui_desktop_invalidate_rect(desktop,
                            deskbar_sub_item_rect(desktop, state->open_section,
                                old_hovered_sub_item, state->cached_count));
                    if (state->hovered_sub_item >= 0)
                        gui_desktop_invalidate_rect(desktop,
                            deskbar_sub_item_rect(desktop, state->open_section,
                                state->hovered_sub_item, state->cached_count));
                }
                for (int i = 0; i < DESKBAR_MAX_CASCADE_LEVELS; i++) {
                    if (old_hovered_cascade[i] ==
                        state->hovered_cascade_item[i] ||
                        state->cascade_count[i] <= 0) continue;
                    if (old_hovered_cascade[i] >= 0)
                        gui_desktop_invalidate_rect(desktop,
                            deskbar_cascade_item_rect(desktop, state, i,
                                old_hovered_cascade[i], state->cascade_count[i]));
                    if (state->hovered_cascade_item[i] >= 0)
                        gui_desktop_invalidate_rect(desktop,
                            deskbar_cascade_item_rect(desktop, state, i,
                                state->hovered_cascade_item[i],
                                state->cascade_count[i]));
                }
            }
            gui_request_paint();
        }
        return on_bar || on_menu || on_submenu || on_status || resize_hit ||
               launcher_hit >= 0 || hit_window != NULL;
    }

    if (event->type == GUI_EVENT_MOUSE_DOWN) {
        if (resize_hit) {
            int edge = resize_hit < 0 ? bar.x : bar.x + bar.w - 1;
            state->resize_drag_side = resize_hit;
            state->resize_hover_side = resize_hit;
            state->resize_grab_offset = event->x - edge;
            state->button_pressed = false;
            state->clock_pressed = false;
            state->pressed_launcher = -1;
            state->pressed_window_id = 0;
            if (state->menu_open) {
                state->menu_open = false;
                deskbar_reset_menu_state(state);
            }
            gui_desktop_set_cursor_style(desktop, GUI_CURSOR_SIZE_WE);
            gui_desktop_invalidate_rect(desktop, bar);
            gui_request_paint();
            return true;
        }

        if (on_button) {
            state->button_pressed = true;
            state->clock_pressed = false;
            state->pressed_window_id = 0;
            state->pressed_launcher = -1;
            state->pressed_main_item = -1;
            state->pressed_sub_item = -1;
            state->pressed_cascade_level = -1;
            state->pressed_cascade_item = -1;
            return true;
        }

        if (on_status) {
            state->clock_pressed = true;
            state->button_pressed = false;
            state->pressed_window_id = 0;
            state->pressed_launcher = -1;
            state->pressed_main_item = -1;
            state->pressed_sub_item = -1;
            state->pressed_cascade_level = -1;
            state->pressed_cascade_item = -1;
            return true;
        }

        state->button_pressed = false;
        state->clock_pressed = false;

        if (launcher_hit >= 0) {
            state->pressed_launcher = launcher_hit;
            state->pressed_window_id = 0;
            state->pressed_main_item = -1;
            state->pressed_sub_item = -1;
            state->pressed_cascade_level = -1;
            state->pressed_cascade_item = -1;
            gui_desktop_invalidate_rect(desktop,
                deskbar_cde_launcher_rect(desktop, launcher_hit));
            gui_request_paint();
            return true;
        }
        state->pressed_launcher = -1;

        if (hit_window) {
            state->pressed_window_id = hit_window->id;
            state->pressed_main_item = -1;
            state->pressed_sub_item = -1;
            state->pressed_cascade_level = -1;
            state->pressed_cascade_item = -1;
            return true;
        }

        if (state->menu_open) {
            if (cascade_hit_level >= 0 && cascade_hit_item >= 0) {
                state->pressed_cascade_level = cascade_hit_level;
                state->pressed_cascade_item = cascade_hit_item;
                state->pressed_sub_item = -1;
                state->pressed_main_item = -1;
                return true;
            }

            if (sub_hit >= 0) {
                state->pressed_sub_item = sub_hit;
                state->pressed_cascade_level = -1;
                state->pressed_cascade_item = -1;
                state->pressed_main_item = -1;
                return true;
            }

            if (main_hit >= 0) {
                state->pressed_main_item = main_hit;
                state->pressed_sub_item = -1;
                state->pressed_cascade_level = -1;
                state->pressed_cascade_item = -1;
                return true;
            }

            if (on_menu || on_submenu) return true;

            state->menu_open = false;
            deskbar_reset_menu_state(state);
            return on_bar || on_status;
        }

        state->pressed_window_id = 0;
        return on_bar || on_status;
    }

    if (event->type == GUI_EVENT_MOUSE_UP) {
        bool handled = false;

        if (state->button_pressed) {
            if (on_button) {
                state->menu_open = !state->menu_open;
                deskbar_reset_menu_state(state);
                handled = true;
            }
            state->button_pressed = false;
        } else if (state->clock_pressed) {
            if (on_status) {
                state->menu_open = false;
                deskbar_reset_menu_state(state);
                deskbar_open_clock_calendar(desktop);
                handled = true;
            }
            state->clock_pressed = false;
        } else if (state->pressed_launcher >= 0) {
            int pressed = state->pressed_launcher;
            if (launcher_hit == pressed && pressed < DESKBAR_LAUNCHER_COUNT) {
                state->menu_open = false;
                deskbar_reset_menu_state(state);
                (void)program_execute_path_async(desktop,
                    g_cde_launchers[pressed].program_path);
                handled = true;
            }
            gui_desktop_invalidate_rect(desktop,
                deskbar_cde_launcher_rect(desktop, pressed));
            state->pressed_launcher = -1;
        } else if (state->pressed_window_id) {
            if (hit_window && hit_window->id == state->pressed_window_id) {
                deskbar_focus_window(desktop, hit_window);
                state->menu_open = false;
                deskbar_reset_menu_state(state);
                handled = true;
            }
            state->pressed_window_id = 0;
        } else if (state->pressed_cascade_level >= 0) {
            int level = state->pressed_cascade_level;
            int pressed = state->pressed_cascade_item;

            if (cascade_hit_level == level &&
                cascade_hit_item == pressed &&
                level >= 0 && level < DESKBAR_MAX_CASCADE_LEVELS &&
                pressed >= 0 && pressed < state->cascade_count[level]) {
                deskbar_sub_item_t *item = &state->cascade_items[level][pressed];

                if (item->enabled && item->is_dir) {
                    deskbar_open_cascade_level(state, level + 1,
                                               item->path, pressed);
                } else if (item->enabled) {
                    deskbar_launch_menu_item(desktop, item);
                    state->menu_open = false;
                    deskbar_reset_menu_state(state);
                }
                handled = true;
            }

            state->pressed_cascade_level = -1;
            state->pressed_cascade_item = -1;
        } else if (state->pressed_sub_item >= 0) {
            if (deskbar_main_item_has_submenu(state->open_section)) {
                deskbar_cache_submenu(state, state->open_section);
                sub_count = state->cached_count;
                sub_hit = deskbar_hit_sub_item(desktop, state->open_section,
                    state->cached_items, sub_count, event->x, event->y);

                if (sub_hit == state->pressed_sub_item &&
                    sub_hit < sub_count &&
                    state->cached_items[sub_hit].enabled) {
                    deskbar_sub_item_t *item = &state->cached_items[sub_hit];

                    if (item->is_dir) {
                        deskbar_open_cascade_level(state, 0,
                                                   item->path, sub_hit);
                    } else {
                        deskbar_launch_menu_item(desktop, item);
                        state->menu_open = false;
                        deskbar_reset_menu_state(state);
                    }
                    handled = true;
                }
            }
            state->pressed_sub_item = -1;
        } else if (state->pressed_main_item >= 0) {
            main_hit = deskbar_hit_main_item(desktop, event->x, event->y);
            if (main_hit == state->pressed_main_item) {
                if (main_hit == 0) {
                    (void)program_execute_path_async(desktop, "/SYSTEM/PROGRAMS/ABOUT.BEX");
                    state->menu_open = false;
                    deskbar_reset_menu_state(state);
                } else if (main_hit == 1) {
                    (void)program_execute_path_async(desktop, "/SYSTEM/PROGRAMS/RUNBOX.BEX");
                    state->menu_open = false;
                    deskbar_reset_menu_state(state);
                } else if (main_hit == DESKBAR_MAIN_ITEMS - 2) {
                    (void)program_execute_path_async(desktop,
                        "/SYSTEM/CONTROL/CONTROL.BEX");
                    state->menu_open = false;
                    deskbar_reset_menu_state(state);
                } else if (main_hit == DESKBAR_MAIN_ITEMS - 1) {
                    state->menu_open = false;
                    deskbar_reset_menu_state(state);
                    handled = true;
                    deskbar_power_off();
                } else if (deskbar_main_item_has_submenu(main_hit)) {
                    state->open_section = main_hit;
                    deskbar_cache_submenu(state, main_hit);
                }
                handled = true;
            }
            state->pressed_main_item = -1;
        }

        if (!state->menu_open) {
            state->hovered_main_item = -1;
            state->hovered_sub_item = -1;
            for (int i = 0; i < DESKBAR_MAX_CASCADE_LEVELS; i++)
                state->hovered_cascade_item[i] = -1;
        } else {
            state->hovered_main_item = main_hit;
            state->hovered_sub_item = sub_hit;
            for (int i = 0; i < DESKBAR_MAX_CASCADE_LEVELS; i++)
                state->hovered_cascade_item[i] = -1;
            if (cascade_hit_level >= 0)
                state->hovered_cascade_item[cascade_hit_level] =
                    cascade_hit_item;
        }

        state->resize_hover_side = resize_hit;
        state->button_hovered = resize_hit ? false : on_button;
        state->clock_hovered = resize_hit ? false : on_status;
        state->hovered_launcher = resize_hit ? -1 : launcher_hit;
        state->hovered_window_id = hit_window ? hit_window->id : 0;

        /* Al entrar a esta funcion se invalida el estado anterior. Si el
           mouse-up acaba de abrir un menu/submenu, su rectangulo nuevo no
           existia todavia y quedaba sin pintar hasta el proximo movimiento. */
        if (old_visual_key != deskbar_visual_state_key(state)) {
            if (old_menu_open && !state->menu_open) {
                /* The old menu was already dirtied before changing state.
                 * Repaint only the Start button in its released state; adding
                 * the entire bar here would recreate the giant bounding box. */
                gui_desktop_invalidate_rect(desktop,
                                            deskbar_button_rect(desktop));
            } else {
                deskbar_invalidate(desktop, state);
            }
            gui_request_paint();
        }

        return handled || on_bar || on_menu || on_submenu || on_status ||
               resize_hit || launcher_hit >= 0 || hit_window != NULL;
    }

    return false;
}

static void deskbar_destroy(gui_program_t *program);

#ifdef BLES_VISION_SERVICE
void vision_deskbar_overlay_rects(gui_program_t *program, gui_desktop_t *desktop) {
    deskbar_state_t *state = g_vision_deskbar_state;
    if (!state || !program || !desktop) return;
#define DESKBAR_OVERLAY(rect) do { if (program->staging_overlay_count < 12U) \
    program->staging_overlays[program->staging_overlay_count++] = (rect); } while (0)
    DESKBAR_OVERLAY(deskbar_bar_rect(desktop));
    if (state->menu_open) {
        DESKBAR_OVERLAY(deskbar_menu_rect(desktop));
        if (deskbar_main_item_has_submenu(state->open_section) && state->cached_count > 0)
            DESKBAR_OVERLAY(deskbar_submenu_rect(desktop,state->open_section,state->cached_count));
        for (int i=0;i<state->open_cascade_levels && i<DESKBAR_MAX_CASCADE_LEVELS;i++)
            if (state->cascade_count[i]>0)
                DESKBAR_OVERLAY(deskbar_cascade_rect(desktop,state,i,state->cascade_count[i]));
    }
#undef DESKBAR_OVERLAY
}

void vision_deskbar_paint_layer(gui_desktop_t *desktop, gui_surface_t *surface) {
    gui_program_t proxy = {0};
    if (!g_vision_deskbar_state) return;
    proxy.state = g_vision_deskbar_state;
    deskbar_paint(&proxy, desktop, surface);
}

bool vision_deskbar_handle_event_layer(gui_desktop_t *desktop,
                                       const gui_event_t *event) {
    gui_program_t proxy = {0};
    if (!g_vision_deskbar_state || !event) return false;
    proxy.state = g_vision_deskbar_state;
    return deskbar_handle_event(&proxy, desktop, event);
}

void vision_deskbar_shutdown_layer(void) {
    gui_program_t proxy = {0};
    if (!g_vision_deskbar_state) return;
    proxy.state = g_vision_deskbar_state;
    deskbar_destroy(&proxy);
    g_vision_deskbar_state = NULL;
}
#endif

static void deskbar_destroy(gui_program_t *program) {
    deskbar_state_t *state;
    if (!program || !program->state) return;
    state = (deskbar_state_t *)program->state;
    for (int i = 0; i < DESKBAR_MAIN_ICON_COUNT; i++)
        if (state->main_icon_loaded[i])
            bk_gui_image_free(&state->main_icons[i]);
    if (state->folder_icon_loaded) bk_gui_image_free(&state->folder_icon);
    if (state->application_icon_loaded)
        bk_gui_image_free(&state->application_icon);
    if (state->document_icon_loaded)
        bk_gui_image_free(&state->document_icon);
    for (int i = 0; i < DESKBAR_LAUNCHER_COUNT; i++)
        if (state->launcher_icon_loaded[i])
            bk_gui_image_free(&state->launcher_icons[i]);
    kfree(state);
    program->state = NULL;
}

void deskbar_install(gui_desktop_t *desktop) {
    deskbar_state_t *state;
#ifndef BLES_VISION_SERVICE
    gui_program_t *program;
#endif

    if (!desktop) return;

    kprintf("[DESKBAR] alloc state bytes=%u\n",
            (uint32_t)sizeof(deskbar_state_t));
    state = (deskbar_state_t *)kzalloc(sizeof(deskbar_state_t));
    if (!state) {
        kprintf("[DESKBAR] ERROR sin memoria para state\n");
        return;
    }

    kprintf("[DESKBAR] reset state\n");
    deskbar_reset_state(state);
    g_deskbar_panel_width = 0;
    kprintf("[DESKBAR] load clock preferences\n");
    deskbar_load_clock_preferences(state);

    deskbar_load_start_font(state);

    kprintf("[DESKBAR] load React95 launcher icons\n");
    for (int i = 0; i < DESKBAR_LAUNCHER_COUNT; i++) {
        state->launcher_icon_loaded[i] =
            bk_graphics_icon_load_size(g_cde_launchers[i].icon_name, 32,
                                  &state->launcher_icons[i]);
        kprintf("[DESKBAR] launcher %s / %s %s\n",
                g_cde_launchers[i].label, g_cde_launchers[i].icon_name,
                state->launcher_icon_loaded[i] ? "OK" : "FAIL");
    }

    if (compat_mode_allow_icon_images()) {
        static const char *main_icon_names[DESKBAR_MAIN_ICON_COUNT] = {
            "InfoBubble", "Runonce106", "Computer", "FolderExe",
            "FileText", "Folder", "Settings", "PowerOff"
        };
        for (int i = 0; i < DESKBAR_MAIN_ICON_COUNT; i++) {
            kprintf("[DESKBAR] icon %d/%d %s begin\n", i + 1,
                    DESKBAR_MAIN_ICON_COUNT, main_icon_names[i]);
            state->main_icon_loaded[i] =
                bk_graphics_icon_load_size(main_icon_names[i], 32,
                                      &state->main_icons[i]);
            kprintf("[DESKBAR] icon %d %s %s\n", i + 1,
                    main_icon_names[i],
                    state->main_icon_loaded[i] ? "OK" : "FAIL");
        }
        kprintf("[DESKBAR] icon Folder begin\n");
        state->folder_icon_loaded =
            bk_graphics_icon_load_size("Folder", 32, &state->folder_icon);
        kprintf("[DESKBAR] icon Folder %s\n",
                state->folder_icon_loaded ? "OK" : "FAIL");
        kprintf("[DESKBAR] icon Appwiz1500 begin\n");
        state->application_icon_loaded =
            bk_graphics_icon_load_size("Appwiz1500", 32, &state->application_icon);
        kprintf("[DESKBAR] icon Appwiz1500 %s\n",
                state->application_icon_loaded ? "OK" : "FAIL");
        kprintf("[DESKBAR] icon FileText begin\n");
        state->document_icon_loaded =
            bk_graphics_icon_load_size("FileText", 32, &state->document_icon);
        kprintf("[DESKBAR] icon FileText %s\n",
                state->document_icon_loaded ? "OK" : "FAIL");
    }
#ifdef BLES_VISION_SERVICE
    /* Deskbar used to be a second full-screen Ring-3 gui_program.  Each
     * gui_program owns a complete cached desktop image, so its black/old
     * snapshot overwrote DeskManager after composition.  Vision v3.2 keeps
     * one owner program and paints Deskbar as its top layer instead. */
    if (g_vision_deskbar_state)
        vision_deskbar_shutdown_layer();
    g_vision_deskbar_state = state;
    kprintf("[DESKBAR] embedded in Vision desktop layer\n");
#else
    kprintf("[DESKBAR] register program begin\n");
    program = gui_desktop_register_program(desktop, "deskbar", state,
                                           deskbar_paint,
                                           deskbar_handle_event,
                                           deskbar_destroy);
    kprintf("[DESKBAR] register program %s\n", program ? "OK" : "FAIL");
    if (!program) {
        gui_program_t temporary = {0};
        temporary.state = state;
        deskbar_destroy(&temporary);
    }
#endif
}
