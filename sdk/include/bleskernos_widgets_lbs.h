#ifndef VIEWER_RETRO_UI_H
#define VIEWER_RETRO_UI_H

#include <bleskernos_api.h>

bool bk_vision_contains(bk_gui_rect_t rect, int x, int y);
void bk_vision_bevel_panel(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                      bool sunken, uint32_t face);
void bk_vision_button(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                 const char *label, bool pressed, bool enabled);
void bk_vision_icon_button(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                      const char *label, const bk_gui_image_t *icon,
                      bool pressed, bool enabled);
void bk_vision_progress(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                   uint32_t value, uint32_t maximum);
void bk_vision_slider(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                 uint32_t value, uint32_t maximum, bool enabled);
void bk_vision_inset_text(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                     const char *text, bool centered);
void bk_vision_status_bar(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                     const char *left, const char *right);

#endif
