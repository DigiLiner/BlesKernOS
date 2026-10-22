#ifndef BLESKERNOS_NGS_H
#define BLESKERNOS_NGS_H
#include <bleskernos_api.h>
#include <bleskernos_lbs.h>
typedef struct bk_ngs_surface bk_ngs_surface_t;
bk_ngs_surface_t *bk_ngs_create(uint32_t width, uint32_t height);
void bk_ngs_destroy(bk_ngs_surface_t *);
/* After writing through this pointer, call bk_ngs_damage before presenting. */
uint32_t *bk_ngs_pixels(bk_ngs_surface_t *, uint32_t *pitch_bytes);
int bk_ngs_clip(bk_ngs_surface_t *, bk_gui_rect_t);
int bk_ngs_fill(bk_ngs_surface_t *, bk_gui_rect_t, uint32_t argb);
/* Straight-alpha source-over, nearest scaling, clipped destination. */
int bk_ngs_blit(bk_ngs_surface_t *, bk_gui_rect_t, const bk_ngs_surface_t *, bk_gui_rect_t);
int bk_ngs_present(bk_ngs_surface_t *, bk_gui_window_t *);
/* Select framebuffer presentation on the caller-owned Vision window. */
int bk_ngs_window_present(bk_gui_window_t *, const uint32_t *, uint32_t width, uint32_t height);
void bk_ngs_damage(bk_ngs_surface_t *, bk_gui_rect_t);
#endif
