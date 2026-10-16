#ifndef GUI_H
#define GUI_H

#include "../kernel/include/types.h"

#include "../sdk/include/bleskernos_font.h"

#define GUI_TITLEBAR_HEIGHT 24
#define GUI_BORDER_SIZE      5
#define GUI_MAX_EVENTS      32
#define GUI_MAX_MENUS        5
#define GUI_MAX_MENU_ITEMS   8
#define GUI_MAX_CONTEXT_ITEMS 10
#define GUI_MENU_HEIGHT     20
#define GUI_CURSOR_TRAIL_MAX 6
#define GUI_CURSOR_WIDTH    32
#define GUI_CURSOR_HEIGHT   32
#define GUI_SCROLLBAR_SIZE  16

typedef enum {
    GUI_CURSOR_ARROW = 0,
    GUI_CURSOR_WAIT,
    GUI_CURSOR_SIZE_WE,
    GUI_CURSOR_SIZE_NS,
    GUI_CURSOR_SIZE_NWSE,
    GUI_CURSOR_SIZE_NESW,
    GUI_CURSOR_PROGRESS,
    GUI_CURSOR_TEXT,
    GUI_CURSOR_POINTER,
    GUI_CURSOR_HELP,
    GUI_CURSOR_MOVE,
    GUI_CURSOR_NOT_ALLOWED,
    GUI_CURSOR_CROSSHAIR,
    GUI_CURSOR_ALIAS,
    GUI_CURSOR_ALL_SCROLL,
    GUI_CURSOR_BOTTOM_LEFT_CORNER,
    GUI_CURSOR_BOTTOM_RIGHT_CORNER,
    GUI_CURSOR_BOTTOM_SIDE,
    GUI_CURSOR_CELL,
    GUI_CURSOR_CENTER_PTR,
    GUI_CURSOR_CLOSEDHAND,
    GUI_CURSOR_COL_RESIZE,
    GUI_CURSOR_COLOR_PICKER,
    GUI_CURSOR_CONTEXT_MENU,
    GUI_CURSOR_COPY,
    GUI_CURSOR_DND_MOVE,
    GUI_CURSOR_DND_NO_DROP,
    GUI_CURSOR_DOWN_ARROW,
    GUI_CURSOR_DRAFT,
    GUI_CURSOR_LEFT_ARROW,
    GUI_CURSOR_LEFT_SIDE,
    GUI_CURSOR_NO_DROP,
    GUI_CURSOR_OPENHAND,
    GUI_CURSOR_PENCIL,
    GUI_CURSOR_PIRATE,
    GUI_CURSOR_RIGHT_ARROW,
    GUI_CURSOR_RIGHT_PTR,
    GUI_CURSOR_RIGHT_SIDE,
    GUI_CURSOR_ROW_RESIZE,
    GUI_CURSOR_TOP_LEFT_CORNER,
    GUI_CURSOR_TOP_RIGHT_CORNER,
    GUI_CURSOR_TOP_SIDE,
    GUI_CURSOR_UP_ARROW,
    GUI_CURSOR_VERTICAL_TEXT,
    GUI_CURSOR_WAYLAND_CURSOR,
    GUI_CURSOR_X_CURSOR,
    GUI_CURSOR_ZOOM_IN,
    GUI_CURSOR_ZOOM_OUT,
    GUI_CURSOR_COUNT
} gui_cursor_style_t;

typedef enum {
    GUI_ALERT_INFO = 0,
    GUI_ALERT_WARNING,
    GUI_ALERT_ERROR,
    GUI_ALERT_NETWORK
} gui_alert_kind_t;

#define GUI_ALERT_WIDTH  446
#define GUI_ALERT_HEIGHT 188

typedef enum {
    GUI_EVENT_NONE = 0,
    GUI_EVENT_MOUSE_MOVE,
    GUI_EVENT_MOUSE_DOWN,
    GUI_EVENT_MOUSE_UP,
    GUI_EVENT_MOUSE_WHEEL,
    GUI_EVENT_KEY,
    GUI_EVENT_KEY_UP,
    GUI_EVENT_FOCUS_GAIN,
    GUI_EVENT_FOCUS_LOST,
    GUI_EVENT_RESIZE,
    GUI_EVENT_CLOSE,
    GUI_EVENT_FILE_DROP,
    GUI_EVENT_FILES_CHANGED,
} gui_event_type_t;

typedef enum {
    GUI_WIDGET_LABEL = 1,
    GUI_WIDGET_BUTTON = 2,
    GUI_WIDGET_TEXTBOX = 3,
} gui_widget_type_t;

typedef enum {
    GUI_WIDGET_STYLE_BUTTON = 0,
    GUI_WIDGET_STYLE_SELECTABLE = 1,
    GUI_WIDGET_STYLE_LISTBOX = 2,
    GUI_WIDGET_STYLE_DROPDOWN = 3,
    GUI_WIDGET_STYLE_CHECKBOX = 4,
    GUI_WIDGET_STYLE_RADIO = 5,
    GUI_WIDGET_STYLE_SLIDER = 6,
} gui_widget_style_t;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} gui_rect_t;

typedef struct {
    int x;
    int y;
} gui_point_t;

typedef struct {
    gui_rect_t bounds;
    uint32_t value;
    uint32_t visible;
    uint32_t total;
} gui_scrollbar_t;

typedef struct {
    bool active;
    int grab_offset;
} gui_scrollbar_drag_t;

typedef struct {
    uint32_t *pixels;
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    gui_rect_t clip;
    /* Una superficie de ventana puede conservar coordenadas de escritorio
       aunque su almacenamiento cubra solamente el rectangulo cliente. */
    int16_t origin_x;
    int16_t origin_y;
    uint16_t storage_width;
    uint16_t storage_height;
} gui_surface_t;

typedef enum {
    GUI_WINDOW_BUTTON_NONE = 0,
    GUI_WINDOW_BUTTON_MINIMIZE,
    GUI_WINDOW_BUTTON_CLOSE,
} gui_window_button_t;

typedef struct {
    gui_event_type_t type;
    int x;
    int y;
    int dx;
    int dy;
    uint8_t buttons;
    uint8_t button;
    uint8_t key;
    bool shift;
    bool ctrl;
    bool alt;
} gui_event_t;

struct gui_desktop;
struct gui_program;
struct gui_window;

typedef struct gui_desktop gui_desktop_t;
typedef struct gui_program gui_program_t;
typedef void (*gui_window_content_paint_t)(struct gui_window *window,
                                           gui_surface_t *surface,
                                           void *context);
typedef bool (*gui_window_event_t)(struct gui_window *window,
                                   const gui_event_t *event,
                                   void *context);
typedef void (*gui_widget_callback_t)(struct gui_window *window, uint32_t widget_id);
typedef void (*gui_program_paint_t)(gui_program_t *program, gui_desktop_t *desktop, gui_surface_t *surface);
typedef bool (*gui_program_event_t)(gui_program_t *program, gui_desktop_t *desktop, const gui_event_t *event);
typedef void (*gui_program_destroy_t)(gui_program_t *program);
typedef void (*gui_menu_callback_t)(struct gui_window *window,
                                    uint32_t item_id, void *context);

typedef struct {
    uint32_t id;
    char label[24];
    bool enabled;
    gui_menu_callback_t callback;
    void *context;
    uint32_t callback_pid;
} gui_menu_item_t;

typedef struct {
    char label[16];
    gui_menu_item_t items[GUI_MAX_MENU_ITEMS];
    uint8_t item_count;
} gui_menu_t;

typedef struct {
    gui_menu_item_t items[GUI_MAX_CONTEXT_ITEMS];
    uint8_t item_count;
    bool open;
    int x;
    int y;
    int width;
    int8_t pressed_item;
} gui_context_menu_t;

#define GUI_TEXT_ACTION_COPY  1U
#define GUI_TEXT_ACTION_PASTE 2U

typedef struct {
    gui_rect_t bounds; /* Coordenadas locales al area de contenido. */
    bool has_selection;
    bool editable;
    gui_menu_callback_t callback;
    void *context;
    uint32_t callback_pid;
} gui_text_context_t;

typedef struct gui_widget {
    uint32_t id;
    gui_widget_type_t type;
    gui_widget_style_t style;
    gui_rect_t bounds;
    char text[48];
    bool hovered;
    bool pressed;
    bool selected;
    bool enabled;
    bool visible;
    uint32_t *icon_pixels;
    uint16_t icon_width;
    uint16_t icon_height;
    gui_widget_callback_t callback;
    uint32_t callback_pid;
    void *payload;
    struct gui_widget *prev;
    struct gui_widget *next;
    /* Internal owner link. Appended so every historical field keeps its
       offset; the public SDK exposes widgets as an opaque type. */
    struct gui_window *window;
    int32_t range_min, range_max, range_step, range_value, range_start;
    bool range_changed;
} gui_widget_t;

typedef struct gui_window {
    uint32_t id;
    gui_rect_t bounds;
    int min_w;
    int min_h;
    char title[48];
    const uint32_t *icon_pixels;
    uint16_t icon_width;
    uint16_t icon_height;
    uint32_t bg_color;
    uint32_t title_color;
    uint32_t border_color;
    bool visible;
    bool listed;
    bool minimized;
    bool focused;
    /* A disabled native window remains visible but must not steal focus or
       z-order from an owned modal dialog. */
    bool input_enabled;
    bool dirty;
    bool borderless;
    bool resizable;
    uint8_t drag_height;
    gui_widget_t *widgets;
    gui_widget_t *focused_widget;
    gui_window_content_paint_t content_paint;
    void *content_context;
    uint32_t content_pid;
    bool content_pending;
    bool content_ready;
    bool content_repaint;
    /* 0=alive, 1=destruction requested, 2=finalizer owns the object.
       This is atomic because several Ring-3 callbacks can finish on different
       CPUs after the same window was closed. */
    volatile uint32_t destroy_state;
    uint32_t *content_cache;
    uint16_t content_cache_width;
    uint16_t content_cache_height;
    bool content_cache_rgb332;
    gui_surface_t content_staging;
    bool content_staging_active;
    int8_t content_staging_slot;
    gui_rect_t content_staging_rect;
    gui_window_event_t event_handler;
    void *event_context;
    uint32_t event_pid;
    uint32_t owner_pid;
    /* Optional GPU-owned client layer. Coordinates are desktop coordinates;
       the software window cache deliberately leaves this rectangle alone. */
    uint32_t gpu_view_surface;
    uint16_t gpu_view_width;
    uint16_t gpu_view_height;
    gui_rect_t gpu_view_rect;
    bool gpu_view_visible;
    bool gpu_view_frame_dirty;
    gui_menu_t menus[GUI_MAX_MENUS];
    uint8_t menu_count;
    int8_t open_menu;
    int8_t pressed_menu_item;
    gui_context_menu_t context_menu;
    gui_text_context_t text_context;
    struct gui_window *prev;
    struct gui_window *next;
    bool paint_bounds_valid;
    gui_rect_t paint_bounds;
    /* Appended fields preserve the offsets used by already compiled 0.8
       programs that receive gui_window_t pointers through the public SDK. */
    gui_desktop_t *desktop;
    gui_rect_t mode_restore_bounds;
    bool mode_restore_valid;
    /* Opt-in lifecycle/input extension for modern Ring-3 backends. Appended
       here so existing offsets and the default legacy behaviour stay intact. */
    bool extended_events;
    /* Full-frame clients (games/video) own a compact source surface.  The
       compositor can therefore reconstruct their client after unrelated
       damage without asking Ring 3 to repaint or flashing the CDE body. */
    bool streaming_content;
    uint32_t *stream_pixels;
    uint16_t stream_width;
    uint16_t stream_height;
    bool stream_ready;
    /* Global layout guard for fixed-layout native applications.  These fields
       are appended to preserve every historical gui_window_t offset.  Native
       windows keep their authored size as a safe minimum unless the program
       explicitly supplies another minimum or opts into extended resize events.
       Widget extents may raise the effective minimum further. */
    int layout_authored_w;
    int layout_authored_h;
    int layout_floor_w;
    int layout_floor_h;
    bool layout_guard_enabled;
    bool layout_min_explicit;
    /* Opt-in: paint through GUI helpers into client-sized storage. */
    bool compact_content;
    /* Controller can be Wine launcher while ownership belongs to TinyX.
     * Appending preserves offsets used by historical native binaries. */
    uint32_t controller_pid;
    struct {char name[48],version[24],description[128],copyright[64],icon_path[64];bool valid;} about;
} gui_window_t;

struct gui_program {
    uint32_t id;
    char name[32];
    void *state;
    gui_program_paint_t paint;
    gui_program_event_t handle_event;
    gui_program_destroy_t destroy;
    uint32_t callback_pid;
    bool paint_pending;
    bool paint_requested;
    bool paint_ready;
    /* Vision may own the complete desktop scene.  While that scene is being
       rebuilt, the compositor must never publish an older full-screen cache
       over newer application content. */
    bool scene_owner;
    gui_surface_t paint_cache;
    gui_surface_t paint_staging;
    /* Shell popups are cached with Vision, but composed above client windows.
       Publish their geometry together with the completed staging buffer. */
    uint32_t paint_overlay_count, staging_overlay_count;
    gui_rect_t paint_overlays[12], staging_overlays[12];
    gui_program_t *prev;
    gui_program_t *next;
    bool paint_cache_indexed;
};

struct gui_desktop {
    gui_surface_t surface;
    gui_program_t *first_program;
    gui_program_t *last_program;
    gui_window_t *first_window;
    gui_window_t *last_window;
    gui_window_t *focused_window;
    gui_window_t *drag_window;
    gui_window_t *resize_window;
    gui_rect_t resize_start_bounds;
    uint8_t resize_edges;
    int resize_start_x;
    int resize_start_y;
    int drag_off_x;
    int drag_off_y;
    bool drag_outline_enabled;
    bool drag_outline_visible;
    gui_rect_t drag_outline_bounds;
    bool minimize_animation_active;
    gui_rect_t minimize_animation_from;
    gui_rect_t minimize_animation_to;
    gui_rect_t minimize_animation_bounds;
    uint32_t minimize_animation_start_tick;
    uint32_t minimize_animation_duration_ticks;
    uint32_t minimize_animation_restore_window_id;
    int mouse_x;
    int mouse_y;
    uint8_t mouse_buttons;
    uint32_t next_program_id;
    uint32_t next_window_id;
    uint32_t next_widget_id;
    bool dirty_valid;
    gui_rect_t dirty_rect;
    uint32_t dirty_generation;
    bool cursor_valid;
    gui_rect_t cursor_rect;
    uint32_t cursor_backing[GUI_CURSOR_WIDTH * GUI_CURSOR_HEIGHT];
    uint32_t cursor_pixels[GUI_CURSOR_WIDTH * GUI_CURSOR_HEIGHT];
    uint16_t cursor_width;
    uint16_t cursor_height;
    int16_t cursor_hotspot_x;
    int16_t cursor_hotspot_y;
    uint32_t cursor_owner_pid;
    bool cursor_custom;
    gui_cursor_style_t cursor_style;
    bool cursor_trail_enabled;
    uint8_t cursor_trail_count;
    int cursor_trail_x[GUI_CURSOR_TRAIL_MAX];
    int cursor_trail_y[GUI_CURSOR_TRAIL_MAX];
    uint8_t cursor_paint_count;
    gui_rect_t cursor_paint_rects[GUI_CURSOR_TRAIL_MAX + 1];
    bool paint_valid;
    bool error_visible;
    gui_alert_kind_t error_kind;
    int32_t error_code;
    char error_title[48];
    char error_text[160];
    /* Shell transfers use bounded slots: an asynchronous upcall must not
     * borrow a pathname from the next drag. Event dx/dy carry slot/token. */
    bool file_drag_active;
    char file_drag_path[260];
    gui_window_t *file_drag_window;
    uint32_t file_drop_sequence, file_change_sequence;
    struct {
        uint32_t used, token, tick;
        char path[260];
    } file_drops[4];
    char (*file_drop_batches[4])[260];
    uint32_t file_drop_counts[4],file_drop_positions[4];
    gui_window_t *file_drop_windows[4];
    /* Stippled icon silhouette; no borrowed application image pointers. */
    bool drag_icon_ghost;
    uint8_t drag_icon_size, drag_icon_count;
    uint8_t drag_icon_mask[64 * 64];

};

typedef struct {
    gui_event_t queue[GUI_MAX_EVENTS];
    uint8_t head;
    uint8_t tail;
    uint8_t last_mouse_buttons;
    int last_mouse_x;
    int last_mouse_y;
    int32_t last_mouse_wheel;
} gui_event_queue_t;

void gui_set_setup_mode(bool enabled);
bool gui_setup_mode(void);
void gui_init(void);
void gui_run(void);
bool gui_vision_claim_service(void);
bool gui_vision_initialize_engine(void);
bool gui_vision_start_shell(void);
void gui_vision_publish_ready(void);
bool gui_vision_step(void);
void gui_vision_release_service(void);
/* Called by the reaper under the GUI domain before releasing the image. */
bool syscall_gfx_window_event(gui_window_t *window,const gui_event_t *event,bool *handled);
bool bk_vision_driver_window_event(gui_window_t *window,const gui_event_t *event);
void gui_vision_abort_frame(uint32_t tid);
void gui_desktop_abandon_paint_lock(uint32_t tid);
void gui_vision_process_cleanup(uint32_t process_id);
bool gui_vision_ready(void);
uint32_t gui_vision_owner_pid(void);
gui_desktop_t *gui_vision_desktop(void);
bool gui_vision_setup_mode(void);
bool gui_vision_take_layout_refresh(void);
uint8_t gui_get_cpu_usage(void);
gui_desktop_t *gui_get_desktop(void);
uint32_t gui_get_last_input_tick(void);
void gui_request_paint(void);

bool gui_gfx_init(gui_surface_t *surface);
bool gui_gfx_reconfigure(gui_surface_t *surface);
void gui_gfx_shutdown(gui_surface_t *surface);
void gui_gfx_present(const gui_surface_t *surface);
void gui_gfx_present_rect(const gui_surface_t *surface, gui_rect_t rect);
void gui_gfx_present_rects(const gui_surface_t *surface,
                           const gui_rect_t *rects, uint8_t count);
void gui_gfx_draw_xrgb(gui_surface_t *surface, gui_rect_t destination,
                       gui_rect_t clip, const uint32_t *pixels,
                       uint16_t width, uint16_t height);
void gui_gfx_invalidate_front(void);

/* Compositor SVGA3D opcional. Permanece inactivo en QEMU/SVGA-II 2D. */
bool gui_gpu_compositor_enabled(void);
/* BLES_GRAPHICS_GUI_PERF_V1: the 3D compositor is only required when a visible
 * window imports a GPU render target. Ordinary 2D desktop damage must
 * stay on the native 2D driver. */
bool gui_gpu_compositor_needed(const gui_desktop_t *desktop);
void gui_gpu_compositor_begin_frame(gui_desktop_t *desktop,
                                    const gui_surface_t *surface);
void gui_gpu_compositor_capture_background(const gui_surface_t *surface);
void gui_gpu_compositor_capture_window(gui_window_t *window,
                                       const gui_surface_t *surface);
bool gui_gpu_compositor_publish_viewport(gui_window_t *window,
                                         uint32_t source,
                                         uint16_t width, uint16_t height);
void gui_gpu_compositor_clear_viewport(gui_window_t *window);
void gui_gpu_compositor_forget_window(gui_window_t *window);
bool gui_gpu_compositor_present(gui_desktop_t *desktop,
                                const gui_surface_t *final_surface,
                                bool draw_cursor);
bool gui_gpu_compositor_present_cached(gui_desktop_t *desktop,
                                       bool draw_cursor);
void gui_gpu_compositor_shutdown(void);
bool gui_compositor_cursor_argb(const gui_desktop_t *desktop,
                                uint32_t *pixels,
                                uint16_t *width, uint16_t *height,
                                int16_t *hot_x, int16_t *hot_y);
void gui_gfx_set_16_color_mode(bool enabled);
void gui_gfx_set_output_depth(uint8_t depth);
uint8_t gui_gfx_output_depth(void);
bool gui_gfx_16_color_mode(void);
void gui_gfx_clear(gui_surface_t *surface, uint32_t color);
void gui_gfx_set_clip(gui_surface_t *surface, gui_rect_t clip);
void gui_gfx_reset_clip(gui_surface_t *surface);
gui_rect_t gui_gfx_get_clip(const gui_surface_t *surface);
bool gui_gfx_point_visible(const gui_surface_t *surface, int x, int y);
void gui_gfx_putpixel(gui_surface_t *surface, int x, int y, uint32_t color);
void gui_gfx_fill_rect(gui_surface_t *surface, gui_rect_t rect, uint32_t color);
void gui_gfx_draw_rect(gui_surface_t *surface, gui_rect_t rect, uint32_t color);
void gui_gfx_draw_line(gui_surface_t *surface, int x0, int y0, int x1, int y1, uint32_t color);
void gui_gfx_draw_polyline(gui_surface_t *surface, const gui_point_t *points,
                           uint32_t count, uint32_t color);
void gui_gfx_fill_gradient(gui_surface_t *surface, gui_rect_t rect, uint32_t top, uint32_t bottom);
void gui_gfx_fill_rounded_rect(gui_surface_t *surface, gui_rect_t rect, int radius, uint32_t color);
uint32_t gui_color_blend(uint32_t base, uint32_t top, uint8_t alpha);
uint32_t gui_color_lerp(uint32_t a, uint32_t b, uint8_t t);

/* Real fonts in /SYSTEM/FONTS. Text is UTF-8; length is bytes, at most 4096.
 * size is the TrueType em size in pixels (8..72), y is the top of the line.
 * IDs are stable during a boot; query names instead of hard-coding IDs.
 * Invalid/missing fonts return false/0; fonts are cached on demand.
 */
uint32_t bk_font_count(void);
bool bk_font_info(uint32_t id, bk_font_info_t *info);
uint16_t bk_font_line_height(uint32_t id, uint16_t size);
uint16_t bk_font_text_width(uint32_t id, uint16_t size,
                            const char *text, uint32_t length);
bool bk_font_draw_text(gui_surface_t *surface, uint32_t id, uint16_t size,
                       int x, int y, const char *text, uint32_t length,
                       uint32_t foreground, gui_rect_t clip);

void gui_font_draw_char(gui_surface_t *surface, int x, int y, char c, uint32_t fg, uint32_t bg, bool fill_bg);
void gui_font_draw_string(gui_surface_t *surface, int x, int y, const char *text, uint32_t fg, uint32_t bg, bool fill_bg);
void gui_font_draw_string_scaled(gui_surface_t *surface, int x, int y,
                                 const char *text, uint32_t fg, int scale);
void gui_font_draw_string_clipped(gui_surface_t *surface, int x, int y,
                                  const char *text, uint32_t fg,
                                  gui_rect_t clip);
void gui_font_draw_string_scaled_clipped(gui_surface_t *surface, int x, int y,
                                         const char *text, uint32_t fg,
                                         int scale, gui_rect_t clip);
bool gui_font_get_glyph8(uint8_t c, uint8_t rows[8]);
uint16_t gui_font_text_width(const char *text);
uint16_t gui_font_text_width_px(const char *text, uint32_t length,
                                int pixel_height, bool monospace, bool bold);
void gui_font_draw_string_px_clipped(gui_surface_t *surface, int x, int y,
                                     const char *text, uint32_t length,
                                     uint32_t fg, int pixel_height,
                                     bool bold, bool italic, bool monospace,
                                     gui_rect_t clip);

void gui_desktop_init(gui_desktop_t *desktop, gui_surface_t surface);
gui_program_t *gui_desktop_register_program(gui_desktop_t *desktop, const char *name, void *state, gui_program_paint_t paint, gui_program_event_t handle_event, gui_program_destroy_t destroy);
void gui_desktop_focus_window(gui_desktop_t *desktop, gui_window_t *window);
void gui_desktop_focus_top_window(gui_desktop_t *desktop,
                                  const gui_window_t *exclude);
void gui_desktop_window_hidden(gui_desktop_t *desktop, gui_window_t *window);
void gui_desktop_shell_window_state_changed(gui_desktop_t *desktop);
bool gui_window_is_focusable(const gui_window_t *window);
gui_window_t *gui_desktop_create_window(gui_desktop_t *desktop, int x, int y, int w, int h, const char *title);
void gui_desktop_add_window(gui_desktop_t *desktop, gui_window_t *window);
void gui_desktop_raise_window(gui_desktop_t *desktop, gui_window_t *window);
void gui_desktop_remove_window(gui_desktop_t *desktop, gui_window_t *window);
void gui_desktop_unregister_program(gui_desktop_t *desktop, gui_program_t *program);
void gui_desktop_cleanup_thread(gui_desktop_t *desktop, uint32_t tid);
void gui_desktop_cleanup_process(gui_desktop_t *desktop,
                                 uint32_t process_id);
void gui_desktop_cleanup_owner(gui_desktop_t *desktop, uint32_t owner_pid);
gui_window_t *gui_desktop_window_at(gui_desktop_t *desktop, int x, int y);
void gui_desktop_handle_event(gui_desktop_t *desktop, const gui_event_t *event);
void gui_desktop_paint(gui_desktop_t *desktop);
/* Serializa cambios de modo con el compositor. El repaint se difiere al
 * bucle GUI para no reentrar en callbacks Ring 3 durante la transaccion. */
void gui_desktop_paint_lock(void);
void gui_desktop_paint_unlock(void);
void gui_desktop_paint_programs(gui_desktop_t *desktop);
bool gui_desktop_scene_waiting(const gui_desktop_t *desktop);
void gui_desktop_publish_program_rect(gui_desktop_t *desktop,
                                      const gui_program_t *program,
                                      gui_rect_t rect);
bool gui_desktop_request_owner_paint(gui_desktop_t *desktop,
                                     uint32_t process_id,
                                     uint32_t thread_id);
bool gui_program_prepare_paint(gui_program_t *program,
                               const gui_surface_t *source,
                               gui_surface_t **staging_out);
void gui_program_composite_paint(const gui_program_t *program,
                                 gui_surface_t *destination);
void gui_program_finish_paint(gui_program_t *program);
void gui_program_release_paint(gui_program_t *program);
bool gui_desktop_has_dirty(const gui_desktop_t *desktop);
void gui_desktop_invalidate_rect(gui_desktop_t *desktop, gui_rect_t rect);
/* Damage generated by compositor-owned transient overlays (drag/resize
 * outlines and minimize/restore animation).  Unlike normal Vision damage this
 * must not request a new Ring-3 shell frame. */
void gui_desktop_invalidate_overlay_rect(gui_desktop_t *desktop, gui_rect_t rect);
void gui_desktop_invalidate_all(gui_desktop_t *desktop);
void gui_desktop_reflow(gui_desktop_t *desktop);
void gui_desktop_reset_after_mode_change(gui_desktop_t *desktop);
void gui_desktop_set_cursor_trail(gui_desktop_t *desktop, bool enabled);
bool gui_desktop_cursor_trail_enabled(const gui_desktop_t *desktop);
bool gui_desktop_set_cursor_image(gui_desktop_t *desktop,
                                  const uint32_t *pixels,
                                  uint16_t width, uint16_t height,
                                  int hotspot_x, int hotspot_y,
                                  uint32_t owner_pid);
void gui_desktop_reset_cursor(gui_desktop_t *desktop, uint32_t owner_pid);
void gui_compositor_busy_begin(void);
void gui_compositor_busy_end(void);
bool gui_compositor_tick_cursor(gui_desktop_t *desktop, uint32_t now_tick);
void gui_desktop_set_cursor_style(gui_desktop_t *desktop,
                                  gui_cursor_style_t style);
void gui_desktop_show_error(gui_desktop_t *desktop, const char *title,
                            const char *text);
void gui_desktop_show_alert(gui_desktop_t *desktop, gui_alert_kind_t kind,
                            const char *title, const char *text,
                            int32_t code);
void gui_alert_resources_init(void);
void gui_desktop_set_drag_outline(gui_desktop_t *desktop, bool enabled);
bool gui_desktop_drag_outline_enabled(const gui_desktop_t *desktop);
void gui_desktop_start_minimize_animation(gui_desktop_t *desktop,
                                          gui_rect_t from, gui_rect_t to,
                                          uint32_t start_tick,
                                          uint32_t duration_ticks);
void gui_desktop_start_restore_animation(gui_desktop_t *desktop,
                                         uint32_t window_id,
                                         gui_rect_t from, gui_rect_t to,
                                         uint32_t start_tick,
                                         uint32_t duration_ticks);
bool gui_desktop_tick_minimize_animation(gui_desktop_t *desktop,
                                         uint32_t now_tick);

bool gui_change_resolution(gui_desktop_t *desktop, uint16_t width,
                           uint16_t height, uint8_t bpp);
uint8_t gui_display_color_depth(void);

gui_window_t *gui_window_create(gui_desktop_t *desktop, int x, int y, int w, int h, const char *title);
void gui_window_destroy(gui_window_t *window);
void gui_window_paint(gui_surface_t *surface, gui_window_t *window, gui_rect_t clip);
void gui_window_paint_widgets(gui_surface_t *surface, gui_window_t *window,
                              gui_rect_t clip);
bool gui_window_contains(gui_window_t *window, int x, int y);
bool gui_window_titlebar_contains(gui_window_t *window, int x, int y);
gui_rect_t gui_window_minimize_button_rect(gui_window_t *window);
gui_rect_t gui_window_close_button_rect(gui_window_t *window);
gui_window_button_t gui_window_titlebar_button_at(gui_window_t *window, int x, int y);
void gui_window_minimize(gui_window_t *window);
void gui_window_close(gui_window_t *window);
void gui_window_restore(gui_window_t *window);
void gui_window_set_min_size(gui_window_t *window, int min_w, int min_h);
/* Recompute the effective minimum from the authored/user floor plus every
   native widget.  This is intentionally public inside the GUI core so widget
   creation and the desktop resizer share exactly the same constraints. */
void gui_window_refresh_layout_limits(gui_window_t *window);
void gui_window_set_borderless(gui_window_t *window, bool borderless,
                               uint8_t drag_height);
void gui_window_set_content(gui_window_t *window,
                            gui_window_content_paint_t paint,
                            void *context);
bool gui_window_capture_content(gui_window_t *window,
                                const gui_surface_t *surface);
bool gui_window_begin_content_paint(gui_window_t *window,
                                    const gui_surface_t *source,
                                    gui_surface_t **staging_out);
void gui_window_end_content_paint(gui_window_t *window);
void gui_window_paint_cached_content(gui_surface_t *surface,
                                     const gui_window_t *window,
                                     gui_rect_t clip);
bool gui_window_update_stream(gui_window_t *window, const uint32_t *pixels,
                              uint16_t width, uint16_t height);
void gui_window_paint_stream_content(gui_surface_t *surface,
                                     const gui_window_t *window,
                                     gui_rect_t clip);
void gui_window_set_event_handler(gui_window_t *window,
                                  gui_window_event_t handler,
                                  void *context);
void gui_window_set_extended_events(gui_window_t *window, bool enabled);
bool gui_window_dispatch_event(gui_window_t *window,
                               const gui_event_t *event);
int gui_window_add_menu(gui_window_t *window, const char *label);
bool gui_window_add_menu_item(gui_window_t *window, int menu,
                              uint32_t id, const char *label,
                              gui_menu_callback_t callback, void *context);
void gui_window_paint_menus(gui_surface_t *surface, gui_window_t *window);
bool gui_window_handle_menu_event(gui_window_t *window,
                                  const gui_event_t *event);
void gui_window_context_clear(gui_window_t *window);
bool gui_window_context_add_item(gui_window_t *window, uint32_t id,
                                 const char *label, bool enabled,
                                 gui_menu_callback_t callback, void *context);
void gui_window_context_open(gui_window_t *window, int x, int y);
void gui_window_context_close(gui_window_t *window);
void gui_window_set_text_context(gui_window_t *window, gui_rect_t bounds,
                                 bool has_selection, bool editable,
                                 gui_menu_callback_t callback, void *context);
void gui_window_clear_text_context(gui_window_t *window);
bool gui_window_open_text_context_at(gui_window_t *window, int x, int y);
void gui_context_menu_clear(gui_context_menu_t *menu);
bool gui_context_menu_add_item(gui_context_menu_t *menu, uint32_t id,
                               const char *label, bool enabled,
                               gui_menu_callback_t callback, void *context);
void gui_context_menu_open(gui_context_menu_t *menu, int x, int y,
                           gui_rect_t limits);
void gui_context_menu_close(gui_context_menu_t *menu);
void gui_context_menu_paint(gui_surface_t *surface,
                            const gui_context_menu_t *menu);
bool gui_context_menu_handle_event(gui_context_menu_t *menu,
                                   gui_window_t *callback_window,
                                   const gui_event_t *event);
int gui_window_content_top(const gui_window_t *window);
gui_rect_t gui_window_content_rect(const gui_window_t *window);
gui_rect_t gui_window_content_rect_inset(const gui_window_t *window, int inset);
gui_rect_t gui_window_clamp_local_rect(const gui_window_t *window,
                                       gui_rect_t rect);

gui_widget_t *gui_widget_create(gui_desktop_t *desktop, gui_window_t *window, gui_widget_type_t type, gui_rect_t bounds, const char *text, gui_widget_callback_t callback);
void bk_gui_surface_draw_tab(gui_surface_t *surface, gui_rect_t tab,
    const char *label, bool selected, gui_rect_t page);
gui_widget_t *bk_gui_create_slider(gui_desktop_t *desktop, gui_window_t *window,
    gui_rect_t bounds, int32_t minimum, int32_t maximum, int32_t step,
    int32_t value, gui_widget_callback_t callback);
gui_widget_t *bk_gui_create_choice(gui_desktop_t *desktop, gui_window_t *window,
    gui_rect_t bounds, const char *text, bool radio, gui_widget_callback_t callback);
int32_t bk_gui_slider_get_value(const gui_widget_t *widget);
void bk_gui_slider_set_value(gui_widget_t *widget, int32_t value);
void bk_gui_choice_set_checked(gui_widget_t *widget, bool checked);
bool bk_gui_choice_is_checked(const gui_widget_t *widget);

gui_widget_t *gui_widget_create_button(gui_desktop_t *desktop,
                                       gui_window_t *window,
                                       gui_rect_t bounds,
                                       const char *text,
                                       gui_widget_callback_t callback);
gui_widget_t *gui_widget_create_selectable_button(gui_desktop_t *desktop,
                                                  gui_window_t *window,
                                                  gui_rect_t bounds,
                                                  const char *text,
                                                  gui_widget_callback_t callback);
gui_widget_t *gui_widget_create_listbox(gui_desktop_t *desktop,
                                        gui_window_t *window,
                                        gui_rect_t bounds,
                                        const char *text);
gui_widget_t *gui_widget_create_dropdown(gui_desktop_t *desktop,
                                         gui_window_t *window,
                                         gui_rect_t bounds,
                                         gui_widget_callback_t callback);
gui_widget_t *gui_widget_create_textbox(gui_desktop_t *desktop,
                                        gui_window_t *window,
                                        gui_rect_t bounds,
                                        const char *text,
                                        uint16_t max_length,
                                        gui_widget_callback_t callback);
gui_rect_t gui_widget_screen_bounds(const gui_window_t *window,
                                    const gui_widget_t *widget);
void gui_widget_destroy(gui_widget_t *widget);
void gui_widget_set_style(gui_widget_t *widget, gui_widget_style_t style);
void gui_widget_set_selected(gui_widget_t *widget, bool selected);
void gui_widget_set_enabled(gui_widget_t *widget, bool enabled);
void gui_widget_set_visible(gui_window_t *window, gui_widget_t *widget,
                            bool visible);
void gui_widget_set_bounds(gui_window_t *window, gui_widget_t *widget,
                           gui_rect_t bounds);
void gui_widget_set_text(gui_widget_t *widget, const char *text);
void gui_widget_take_icon(gui_widget_t *widget, uint32_t *pixels,
                          uint16_t width, uint16_t height);
bool gui_widget_get_text(const gui_widget_t *widget, char *buffer,
                         uint32_t capacity);
void gui_widget_set_focus(gui_window_t *window, gui_widget_t *widget,
                          bool focused);
bool gui_widget_is_focused(const gui_window_t *window,
                           const gui_widget_t *widget);
void gui_widget_dropdown_clear(gui_widget_t *widget);
bool gui_widget_dropdown_add_item(gui_widget_t *widget,
                                  const char *label,
                                  const char *value);
int gui_widget_dropdown_get_selected(const gui_widget_t *widget);
void gui_widget_dropdown_set_selected(gui_widget_t *widget, int index);
bool gui_widget_dropdown_set_selected_by_value(gui_widget_t *widget,
                                               const char *value);
const char *gui_widget_dropdown_get_selected_label(const gui_widget_t *widget);
const char *gui_widget_dropdown_get_selected_value(const gui_widget_t *widget);
const char *gui_widget_dropdown_get_item_label(const gui_widget_t *widget,
                                               int index);
const char *gui_widget_dropdown_get_item_value(const gui_widget_t *widget,
                                               int index);
bool gui_widget_is_dropdown_expanded(const gui_widget_t *widget);
void gui_widget_paint(gui_surface_t *surface, gui_window_t *window, gui_widget_t *widget, gui_rect_t clip);
bool gui_widget_handle_event(gui_window_t *window, gui_widget_t *widget, const gui_event_t *event);
bool gui_widget_open_text_context_at(gui_window_t *window, int x, int y);

void gui_compositor_paint(gui_desktop_t *desktop);
bool gui_compositor_stream_cursor_begin(gui_desktop_t *desktop);
void gui_compositor_stream_cursor_end(gui_desktop_t *desktop, bool redraw);
/* Fast path for native hardware cursors (currently Rage128 arrow). Returns
 * true only when the pointer was moved without requiring a compositor frame. */
bool gui_compositor_move_hardware_cursor(gui_desktop_t *desktop);

void gui_event_init(gui_event_queue_t *queue);
void gui_widget_tick_caret(uint32_t now_ms);
void gui_event_poll(gui_event_queue_t *queue);
bool gui_event_next(gui_event_queue_t *queue, gui_event_t *event);
void gui_event_queue_reset(gui_event_queue_t *queue);
bool gui_event_queue_push(gui_event_queue_t *queue, const gui_event_t *event);
bool gui_event_queue_pop(gui_event_queue_t *queue, gui_event_t *event);

bool gui_rect_intersect(gui_rect_t a, gui_rect_t b, gui_rect_t *out);
bool gui_rect_contains(gui_rect_t rect, int x, int y);
gui_rect_t gui_rect_union(gui_rect_t a, gui_rect_t b);

void gui_scrollbar_init_vertical(gui_scrollbar_t *bar, gui_rect_t bounds,
                                 uint32_t value, uint32_t visible,
                                 uint32_t total);
gui_rect_t gui_scrollbar_thumb_rect(const gui_scrollbar_t *bar);
void gui_scrollbar_paint_vertical(gui_surface_t *surface,
                                  const gui_scrollbar_t *bar);
bool gui_scrollbar_handle_click_vertical(const gui_scrollbar_t *bar,
                                         int x, int y,
                                         uint32_t *new_value);
bool gui_scrollbar_handle_event_vertical(const gui_scrollbar_t *bar,
                                         gui_scrollbar_drag_t *drag,
                                         const gui_event_t *event,
                                         uint32_t wheel_step,
                                         uint32_t *new_value);

#endif
