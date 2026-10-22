#include <bleskernos_raster_lbs.h>
static int intersect(bk_raster_rect_t a, bk_raster_rect_t b, bk_raster_rect_t *out) {
    int x=a.x>b.x?a.x:b.x, y=a.y>b.y?a.y:b.y;
    int r=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w;
    int d=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;
    *out=(bk_raster_rect_t){x,y,r-x,d-y}; return r>x && d>y;
}
static int viewer_visible(bk_raster_view_t *surface, bk_raster_rect_t rect, bk_raster_rect_t *out) {
    if (!surface || surface->size < sizeof(*surface) || !surface->pixels ||
        !surface->storage_width || !surface->storage_height ||
        surface->pitch < surface->storage_width || surface->storage_width > 65535U ||
        surface->storage_height > 65535U) return 0;
    bk_raster_rect_t storage = {surface->origin_x, surface->origin_y,
        surface->storage_width,
        surface->storage_height};
    return intersect(rect, surface->clip, out) &&
           intersect(*out, storage, out);
}

int bk_ngs_checker(bk_raster_view_t *surface, bk_raster_rect_t rect) {
    bk_raster_rect_t visible;
    if (!viewer_visible(surface, rect, &visible)) return -22;
    for (int y=visible.y;y<visible.y+visible.h;y++) {
        uint32_t *dst=surface->pixels+(y-surface->origin_y)*surface->pitch+
                      visible.x-surface->origin_x;
        for (int x=visible.x;x<visible.x+visible.w;x++)
            *dst++=((((x-rect.x)/10)+((y-rect.y)/10))&1)?0x00383838:0x00484848;
    }
    return 0;
}

int bk_ngs_image_blit(bk_raster_view_t *surface, bk_raster_rect_t area,
                      bk_raster_rect_t destination, const uint32_t *pixels,
                      uint32_t width, uint32_t height) {
    bk_raster_rect_t visible;
    if (!pixels || !width || !height || width > 65535U || height > 65535U ||
        destination.w<=0 || destination.h<=0 ||
        !intersect(area,destination,&visible) ||
        !viewer_visible(surface,visible,&visible)) return -22;
    /* Exact nearest-neighbour sampling without division in the pixel loop. */
    int x_start=(visible.x-destination.x)*width;
    int x_step=width/destination.w, x_remainder=width%destination.w;
    for(int y=visible.y;y<visible.y+visible.h;y++) {
        int sy=((y-destination.y)*height)/destination.h;
        const uint32_t *src=pixels+sy*width;
        uint32_t *dst=surface->pixels+(y-surface->origin_y)*surface->pitch+
                      visible.x-surface->origin_x;
        int sx=x_start/destination.w, error=x_start%destination.w;
        for(int x=0;x<visible.w;x++) {
            uint32_t color=src[sx];
            if(color>>24)dst[x]=color&0x00FFFFFF;
            sx+=x_step;error+=x_remainder;
            if(error>=destination.w){error-=destination.w;sx++;}
        }
    }
    return 0;
}
