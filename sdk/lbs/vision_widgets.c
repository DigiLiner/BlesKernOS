#include <bleskernos_theme.h>
#include <bleskernos_widgets_lbs.h>

#define RV_FACE BK_THEME_FACE
#define RV_LIGHT BK_THEME_LIGHT
#define RV_HILITE BK_THEME_MIDLIGHT
#define RV_SHADOW BK_THEME_SHADOW
#define RV_DARK BK_THEME_DARK
#define RV_TEXT BK_THEME_TEXT
#define RV_DISABLED BK_THEME_MUTED
#define RV_BLUE BK_THEME_SELECTION

bool bk_vision_contains(bk_gui_rect_t rect, int x, int y) {
    return x >= rect.x && y >= rect.y &&
           x < rect.x + rect.w && y < rect.y + rect.h;
}

static void line_h(bk_gui_surface_t *surface, int x, int y, int w,
                   uint32_t color) {
    if (w > 0)
        bk_gui_surface_fill_rect(surface, (bk_gui_rect_t){x, y, w, 1}, color);
}

static void line_v(bk_gui_surface_t *surface, int x, int y, int h,
                   uint32_t color) {
    if (h > 0)
        bk_gui_surface_fill_rect(surface, (bk_gui_rect_t){x, y, 1, h}, color);
}

void bk_vision_bevel_panel(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                      bool sunken, uint32_t face) {
    uint32_t tl1 = sunken ? RV_DARK : RV_LIGHT;
    uint32_t tl2 = sunken ? RV_SHADOW : RV_HILITE;
    uint32_t br1 = sunken ? RV_LIGHT : RV_DARK;
    uint32_t br2 = sunken ? RV_HILITE : RV_SHADOW;

    if (!surface || rect.w < 4 || rect.h < 4) return;
    bk_gui_surface_fill_rect(surface, rect, face);
    line_h(surface, rect.x, rect.y, rect.w, tl1);
    line_v(surface, rect.x, rect.y, rect.h, tl1);
    line_h(surface, rect.x + 1, rect.y + 1, rect.w - 2, tl2);
    line_v(surface, rect.x + 1, rect.y + 1, rect.h - 2, tl2);
    line_h(surface, rect.x, rect.y + rect.h - 1, rect.w, br1);
    line_v(surface, rect.x + rect.w - 1, rect.y, rect.h, br1);
    line_h(surface, rect.x + 1, rect.y + rect.h - 2, rect.w - 2, br2);
    line_v(surface, rect.x + rect.w - 2, rect.y + 1, rect.h - 2, br2);
}

void bk_vision_button(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                 const char *label, bool pressed, bool enabled) {
    bk_vision_icon_button(surface, rect, label, NULL, pressed, enabled);
}

void bk_vision_icon_button(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                      const char *label, const bk_gui_image_t *icon,
                      bool pressed, bool enabled) {
    int tx;
    int ty;
    int icon_size = icon && icon->pixels ? 16 : 0;
    int gap = icon_size && label && label[0] ? 3 : 0;
    int group_width;
    uint32_t fg = enabled ? RV_TEXT : RV_DISABLED;
    if (!surface || !label) return;
    bk_vision_bevel_panel(surface, rect, pressed, RV_FACE);
    group_width = icon_size + gap + (int)bk_gui_text_width(label);
    tx = rect.x + (rect.w - group_width) / 2;
    ty = rect.y + (rect.h - 8) / 2;
    if (pressed) { tx++; ty++; }
    if (icon_size) {
        bk_gui_surface_draw_image(surface,
            (bk_gui_rect_t){tx, rect.y + (rect.h - icon_size) / 2 +
                            (pressed ? 1 : 0), icon_size, icon_size},
            rect, icon);
        tx += icon_size + gap;
    }
    if (!enabled) {
        bk_gui_surface_draw_text(surface, tx + 1, ty + 1, label,
                                 RV_LIGHT, RV_FACE, false);
    }
    bk_gui_surface_draw_text(surface, tx, ty, label, fg, RV_FACE, false);
}

void bk_vision_progress(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                   uint32_t value, uint32_t maximum) {
    int inner_w;
    int fill_w;
    bk_vision_bevel_panel(surface, rect, true, RV_FACE);
    if (!surface || rect.w < 6 || rect.h < 6 || maximum == 0U) return;
    if (value > maximum) value = maximum;
    inner_w = rect.w - 4;
    fill_w = (int)((uint32_t)inner_w * value / maximum);
    bk_gui_surface_fill_rect(surface,
        (bk_gui_rect_t){rect.x + 2, rect.y + 2, inner_w, rect.h - 4},
        0x00FFFFFFU);
    if (fill_w > 0)
        bk_gui_surface_fill_rect(surface,
            (bk_gui_rect_t){rect.x + 2, rect.y + 2, fill_w, rect.h - 4},
            RV_BLUE);
}

void bk_vision_status_bar(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                     const char *left, const char *right) {
    int rx;
    bk_vision_bevel_panel(surface, rect, true, RV_FACE);
    if (left)
        bk_gui_surface_draw_text(surface, rect.x + 5, rect.y + 5,
                                 left, RV_TEXT, RV_FACE, false);
    if (right) {
        rx = rect.x + rect.w - 5 - (int)bk_gui_text_width(right);
        bk_gui_surface_draw_text(surface, rx, rect.y + 5,
                                 right, RV_TEXT, RV_FACE, false);
    }
}

void bk_vision_slider(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                 uint32_t value, uint32_t maximum, bool enabled) {
    int track_x, track_y, track_w, thumb_x;
    bk_gui_rect_t thumb;
    if (!surface || rect.w < 14 || rect.h < 10) return;
    if (!maximum) maximum = 1U;
    if (value > maximum) value = maximum;
    track_x = rect.x + 5;
    track_w = rect.w - 10;
    track_y = rect.y + rect.h / 2 - 2;
    bk_vision_bevel_panel(surface, (bk_gui_rect_t){track_x, track_y, track_w, 4},
                     true, enabled ? 0x00D8D8D8U : RV_FACE);
    thumb_x = track_x + (int)((uint32_t)(track_w - 8) * value / maximum);
    thumb = (bk_gui_rect_t){thumb_x, rect.y + 2, 8, rect.h - 4};
    bk_vision_bevel_panel(surface, thumb, false,
                     enabled ? RV_FACE : 0x00B0B0B0U);
}

void bk_vision_inset_text(bk_gui_surface_t *surface, bk_gui_rect_t rect,
                     const char *text, bool centered) {
    int tx;
    if (!surface || !text || rect.w < 6 || rect.h < 6) return;
    bk_vision_bevel_panel(surface, rect, true, 0x00E8E8E8U);
    tx = rect.x + 4;
    if (centered)
        tx = rect.x + (rect.w - (int)bk_gui_text_width(text)) / 2;
    bk_gui_surface_draw_text(surface, tx, rect.y + (rect.h - 8) / 2,
                             text, RV_TEXT, 0x00E8E8E8U, false);
}
