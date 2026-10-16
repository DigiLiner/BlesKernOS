#define BK_API_IMPLEMENTATION
#include "kernel/include/api.h"
#include "kernel/include/pit.h"
void bk_gui_surface_fill_rect(gui_surface_t *s,gui_rect_t r,uint32_t c){gui_gfx_fill_rect(s,r,c);}
void bk_gui_surface_draw_rect(gui_surface_t *s,gui_rect_t r,uint32_t c){gui_gfx_draw_rect(s,r,c);}
void bk_gui_surface_draw_line(gui_surface_t *s,int x,int y,int x1,int y1,uint32_t c){gui_gfx_draw_line(s,x,y,x1,y1,c);}
#include "kernel/api/gui_raster_api.inc"
