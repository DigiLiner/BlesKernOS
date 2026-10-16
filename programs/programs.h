#ifndef PROGRAMS_H
#define PROGRAMS_H

#include "../gui/gui.h"

typedef struct {
    gui_window_t *window;
    uint32_t memory_bytes;
} program_runtime_info_t;

void deskmanager_install(gui_desktop_t *desktop);
void deskmanager_install_setup(gui_desktop_t *desktop, const char *wallpaper_path);
void deskbar_install(gui_desktop_t *desktop);
gui_rect_t deskbar_clock_rect(const gui_desktop_t *desktop);
bool deskbar_window_button_rect(const gui_desktop_t *desktop,
                                const gui_window_t *target, gui_rect_t *out);
#ifdef BLES_VISION_SERVICE
/* Vision owns one full-desktop Ring-3 gui_program.  Deskbar is embedded as a
 * layer inside DeskManager so two independent full-screen caches cannot
 * overwrite each other. */
void vision_deskbar_overlay_rects(gui_program_t *program, gui_desktop_t *desktop);
void vision_deskbar_paint_layer(gui_desktop_t *desktop, gui_surface_t *surface);
bool vision_deskbar_handle_event_layer(gui_desktop_t *desktop,
                                       const gui_event_t *event);
void vision_deskbar_shutdown_layer(void);
#endif

void filebrowser_install(gui_desktop_t *desktop);
void filebrowser_open_from_desktop(gui_desktop_t *desktop);
void filebrowser_open_path(gui_desktop_t *desktop, const char *path);
bool filebrowser_global_copy(void);
bool filebrowser_global_paste(void);

void shelllauncher_install(gui_desktop_t *desktop);
void shelllauncher_open_from_desktop(gui_desktop_t *desktop);

void texteditor_install(gui_desktop_t *desktop);
void texteditor_open(gui_desktop_t *desktop, const char *path);
void texteditor_open_from_desktop(gui_desktop_t *desktop);

void calculator_install(gui_desktop_t *desktop);
void calculator_open_from_desktop(gui_desktop_t *desktop);
void calendar_install(gui_desktop_t *desktop);
void calendar_open_from_desktop(gui_desktop_t *desktop);
void processmanager_install(gui_desktop_t *desktop);
void processmanager_open_from_desktop(gui_desktop_t *desktop);
void midamp_install(gui_desktop_t *desktop);
void midamp_open_from_desktop(gui_desktop_t *desktop);
void imageviewer_install(gui_desktop_t *desktop);
void imageviewer_open(gui_desktop_t *desktop, const char *path);
void imageviewer_open_from_desktop(gui_desktop_t *desktop);
void gears_install(gui_desktop_t *desktop);
void gears_open_from_desktop(gui_desktop_t *desktop);
void paint_install(gui_desktop_t *desktop);
void paint_open_from_desktop(gui_desktop_t *desktop);
void screensaverd_install(gui_desktop_t *desktop);
bool screensaver_is_enabled(void);
uint32_t screensaver_get_timeout_seconds(void);
void screensaver_set_enabled(bool enabled);
void screensaver_set_timeout_seconds(uint32_t seconds);
const char *screensaver_get_path(void);
bool screensaver_copy_path(char *buffer, uint32_t capacity);
void screensaver_set_path(const char *path);
bool screensaver_preview(gui_desktop_t *desktop);
void bk_input_mouse_set_sensitivity(uint8_t sensitivity);
uint8_t bk_input_mouse_get_sensitivity(void);

void deskmanager_set_background(uint32_t color);
uint32_t deskmanager_get_background(void);
const char *deskmanager_get_wallpaper_path(void);
bool deskmanager_set_wallpaper(const char *path);
#define DESK_WALLPAPER_STRETCH 0U
#define DESK_WALLPAPER_CENTER  1U
#define DESK_WALLPAPER_TILE    2U
#define DESK_WALLPAPER_FIT     3U
#define DESK_WALLPAPER_FILL    4U
void deskmanager_set_wallpaper_mode(uint8_t mode);
uint8_t deskmanager_get_wallpaper_mode(void);
void deskmanager_refresh_layout(void);

bool program_is_object(const char *path);
bool program_is_bex_executable(const char *path);
bool program_is_win32_executable(const char *path);
bool program_execute_path(gui_desktop_t *desktop, const char *path);
bool program_execute_path_arg(gui_desktop_t *desktop, const char *path,
                              const char *launch_arg);
/* Desktop/UI launches must not perform disk I/O and ELF/PE relocation while
 * the GUI event transaction is still active.  These helpers hand the load to
 * a short-lived kernel worker and return as soon as the request is queued. */
bool program_execute_path_async(gui_desktop_t *desktop, const char *path);
bool program_execute_path_async_arg(gui_desktop_t *desktop, const char *path,
                                    const char *launch_arg);
const char *program_launch_arg(void);
uint32_t *program_load_bmp_icon(const char *path);
uint32_t *program_load_bmp_icon_scaled(const char *path,
                                       uint16_t output_width,
                                       uint16_t output_height);
uint32_t *program_load_bmp_wallpaper_scaled(const char *path,
                                            uint16_t output_width,
                                            uint16_t output_height);
void program_draw_icon_pixels(gui_surface_t *surface, int x, int y,
                              const uint32_t *pixels,
                              uint16_t width, uint16_t height);
void about_install(gui_desktop_t *desktop);
void about_open(gui_desktop_t *desktop);

bool about_get_runtime_info(program_runtime_info_t *info);
bool filebrowser_get_runtime_info(program_runtime_info_t *info);
bool shelllauncher_get_runtime_info(program_runtime_info_t *info);
bool texteditor_get_runtime_info(program_runtime_info_t *info);
bool calculator_get_runtime_info(program_runtime_info_t *info);

void runbox_open_from_desktop(gui_desktop_t *desktop);
bool runbox_get_runtime_info(program_runtime_info_t *info);

bool calendar_get_runtime_info(program_runtime_info_t *info);
bool processmanager_get_runtime_info(program_runtime_info_t *info);
bool midamp_get_runtime_info(program_runtime_info_t *info);
bool gears_get_runtime_info(program_runtime_info_t *info);
bool paint_get_runtime_info(program_runtime_info_t *info);

#endif
