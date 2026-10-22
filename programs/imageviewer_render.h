#ifndef BLES_IMAGEVIEWER_RENDER_H
#define BLES_IMAGEVIEWER_RENDER_H
#include "../sdk/include/bleskernos_raster_lbs.h"
static bk_raster_rect_t viewer_raster_rect(gui_rect_t r) {
    return (bk_raster_rect_t){r.x,r.y,r.w,r.h};
}
static bk_raster_view_t viewer_raster_view(gui_surface_t *s) {
    return (bk_raster_view_t){sizeof(bk_raster_view_t),s->pixels,s->pitch,
        s->storage_width?s->storage_width:s->width,
        s->storage_height?s->storage_height:s->height,
        s->origin_x,s->origin_y,viewer_raster_rect(s->clip)};
}
static void viewer_draw_checker(gui_surface_t *s, gui_rect_t r) {
    if (!s) return;
    bk_raster_view_t view=viewer_raster_view(s);
    (void)bk_ngs_checker(&view,viewer_raster_rect(r));
}
static void viewer_blit_image(gui_surface_t *s, gui_rect_t area,
                              gui_rect_t destination, const gui_image_t *image) {
    if (!s || !image) return;
    bk_raster_view_t view=viewer_raster_view(s);
    (void)bk_ngs_image_blit(&view,viewer_raster_rect(area),
        viewer_raster_rect(destination),image->pixels,image->width,image->height);
}
#endif
