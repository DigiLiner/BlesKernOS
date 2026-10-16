#ifndef VIEWER_RETRO_UI_H
#define VIEWER_RETRO_UI_H

#include <bleskernos_api.h>

bool rvui_contains(bk_gui_rect_t rect, int x, int y);
void rvui_bevel_panel(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                      bool sunken, uint32_t face);
void rvui_button(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                 const char *label, bool pressed, bool enabled);
void rvui_icon_button(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                      const char *label, const bk_gui_image_t *icon,
                      bool pressed, bool enabled);
void rvui_progress(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                   uint32_t value, uint32_t maximum);
void rvui_slider(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                 uint32_t value, uint32_t maximum, bool enabled);
void rvui_inset_text(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                     const char *text, bool centered);
void rvui_status_bar(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                     const char *left, const char *right);

#endif
