#include <bleskernos_ngs.h>
#include <string.h>
#include "resources.h"
struct bk_ngs_surface {
    uint32_t width, height;
    uint32_t *pixels;
    bk_gui_rect_t clip, dirty;
    bool damaged;
};
static bk_gui_rect_t intersect(bk_gui_rect_t a, bk_gui_rect_t b) {
    int64_t x = a.x > b.x ? a.x : b.x, y = a.y > b.y ? a.y : b.y;
    int64_t r = (int64_t)a.x + a.w, rb = (int64_t)b.x + b.w;
    int64_t d = (int64_t)a.y + a.h, db = (int64_t)b.y + b.h;
    bk_gui_rect_t c;
    if (rb < r)
        r = rb;
    if (db < d)
        d = db;
    c.x = x;
    c.y = y;
    c.w = r > x ? r - x : 0;
    c.h = d > y ? d - y : 0;
    return c;
}
bk_ngs_surface_t *bk_ngs_create(uint32_t w, uint32_t h) {
    bk_ngs_surface_t *s;
    if (!w || !h || w > 4096 || h > 4096)
        return 0;
    s = bk_sys_alloc(sizeof(*s));
    if (!s)
        return 0;
    memset(s, 0, sizeof(*s));
    s->pixels = bk_sys_alloc(w * h * 4);
    if (!s->pixels) {
        bk_sys_free(s);
        return 0;
    }
    s->width = w;
    s->height = h;
    s->clip = (bk_gui_rect_t){0, 0, w, h};
    memset(s->pixels, 0, w * h * 4);
    bk_ngs_damage(s, s->clip);
    bk_lbs_resource_acquire();
    return s;
}
void bk_ngs_destroy(bk_ngs_surface_t *s) {
    if (s) {
        bk_sys_free(s->pixels);
        bk_sys_free(s);
        bk_lbs_resource_release();
    }
}
uint32_t *bk_ngs_pixels(bk_ngs_surface_t *s, uint32_t *pitch) {
    if (!s)
        return 0;
    if (pitch)
        *pitch = s->width * 4;
    return s->pixels;
}
int bk_ngs_clip(bk_ngs_surface_t *s, bk_gui_rect_t r) {
    if (!s || r.w < 0 || r.h < 0)
        return BK_LBS_INVALID;
    s->clip = intersect(r, (bk_gui_rect_t){0, 0, s->width, s->height});
    return 0;
}
void bk_ngs_damage(bk_ngs_surface_t *s, bk_gui_rect_t r) {
    int right, bottom;
    if (!s)
        return;
    r = intersect(r, (bk_gui_rect_t){0, 0, s->width, s->height});
    if (!r.w || !r.h)
        return;
    if (!s->damaged) {
        s->dirty = r;
        s->damaged = true;
        return;
    }
    right = s->dirty.x + s->dirty.w;
    bottom = s->dirty.y + s->dirty.h;
    if (r.x + r.w > right)
        right = r.x + r.w;
    if (r.y + r.h > bottom)
        bottom = r.y + r.h;
    if (r.x < s->dirty.x)
        s->dirty.x = r.x;
    if (r.y < s->dirty.y)
        s->dirty.y = r.y;
    s->dirty.w = right - s->dirty.x;
    s->dirty.h = bottom - s->dirty.y;
}
int bk_ngs_fill(bk_ngs_surface_t *s, bk_gui_rect_t r, uint32_t color) {
    int x, y;
    if (!s || r.w < 0 || r.h < 0)
        return BK_LBS_INVALID;
    r = intersect(r, s->clip);
    for (y = r.y; y < r.y + r.h; y++)
        for (x = r.x; x < r.x + r.w; x++)
            s->pixels[y * s->width + x] = color;
    bk_ngs_damage(s, r);
    return 0;
}
static uint32_t blend(uint32_t d, uint32_t s) {
    uint32_t a = s >> 24, ia = 255 - a, out = a + ((d >> 24) * ia + 127) / 255, c;
    uint32_t v = out << 24;
    if (!out)
        return 0;
    for (c = 0; c < 24; c += 8) {
        uint32_t sc = (s >> c) & 255, dc = (d >> c) & 255;
        uint32_t n = sc * a * 255 + dc * (d >> 24) * ia;
        v |= ((n + out * 127) / (out * 255)) << c;
    }
    return v;
}
int bk_ngs_blit(bk_ngs_surface_t *d, bk_gui_rect_t dr, const bk_ngs_surface_t *s,
                bk_gui_rect_t sr) {
    bk_gui_rect_t r;
    int x, y;
    if (!d || !s || d == s || dr.w <= 0 || dr.h <= 0 || sr.w <= 0 || sr.h <= 0 || sr.x < 0 ||
        sr.y < 0 || (uint32_t)sr.x > s->width || (uint32_t)sr.y > s->height ||
        (uint32_t)sr.w > s->width - (uint32_t)sr.x || (uint32_t)sr.h > s->height - (uint32_t)sr.y)
        return BK_LBS_INVALID;
    r = intersect(dr, d->clip);
    for (y = r.y; y < r.y + r.h; y++)
        for (x = r.x; x < r.x + r.w; x++) {
            uint32_t sx = sr.x + ((int64_t)x - dr.x) * sr.w / dr.w;
            uint32_t sy = sr.y + ((int64_t)y - dr.y) * sr.h / dr.h;
            uint32_t *p = &d->pixels[y * d->width + x];
            *p = blend(*p, s->pixels[sy * s->width + sx]);
        }
    bk_ngs_damage(d, r);
    return 0;
}
int bk_ngs_present(bk_ngs_surface_t *s, bk_gui_window_t *w) {
    if (!s || !w)
        return BK_LBS_INVALID;
    if (!s->damaged)
        return 0;
    /* The existing API has full buffer presentation. Dirty state avoids idle
       submissions; per-rectangle upload awaits a public backend contract. */
    int result = bk_ngs_window_present(w, s->pixels, s->width, s->height);
    if (result)
        return result;
    s->damaged = false;
    return 0;
}

int bk_ngs_window_present(bk_gui_window_t *w, const uint32_t *pixels, uint32_t width,
                          uint32_t height) {
    if (!w || !pixels || !width || !height || width > 4096 || height > 4096)
        return BK_LBS_INVALID;
    if (bk_gui_window_present_xrgb(w, pixels, width, height))
        return 0;
    bk_gui_window_set_streaming(w, true);
    return bk_gui_window_present_xrgb(w, pixels, width, height) ? 0 : -5;
}
