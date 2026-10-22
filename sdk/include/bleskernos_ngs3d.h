#ifndef BLESKERNOS_NGS3D_H
#define BLESKERNOS_NGS3D_H
#include <bleskernos_lbs.h>
#include <bleskernos_api.h>
typedef struct bk_ngs3d_context bk_ngs3d_context_t;
typedef struct bk_ngs3d_vertices bk_ngs3d_vertices_t;
enum {
    BK_NGS3D_AUTO = 0,
    BK_NGS3D_SOFTWARE = 1,
    BK_NGS3D_GPU = 2,
    BK_NGS3D_GPU_STRICT = 3,
    BK_NGS3D_TINYGL_SOFTWARE = 4,
    BK_NGS3D_TINYGL_AUTO = 5
};
typedef struct {
    uint32_t size, requested, active, gpu_available, depth_bits;
    const char *renderer;
} bk_ngs3d_caps_t;
bk_ngs3d_context_t *bk_ngs3d_create(uint32_t w, uint32_t h, uint32_t backend);
void bk_ngs3d_destroy(bk_ngs3d_context_t *);
int bk_ngs3d_current(bk_ngs3d_context_t *);
int bk_ngs3d_capabilities(bk_ngs3d_context_t *, bk_ngs3d_caps_t *);
int bk_ngs3d_present(bk_ngs3d_context_t *, bk_gui_window_t *);
const uint32_t *bk_ngs3d_pixels(bk_ngs3d_context_t *);
int bk_ngs3d_sync(bk_ngs3d_context_t *);
/* CPU vertex storage: GFX3D upload/draw is selected by Mesa, not claimed as
   persistent GPU VBO support. xyz is count*3 floats. */
bk_ngs3d_vertices_t *bk_ngs3d_vertices_create(bk_ngs3d_context_t *, const float *xyz,
                                              uint32_t count);
void bk_ngs3d_vertices_destroy(bk_ngs3d_context_t *, bk_ngs3d_vertices_t *);
int bk_ngs3d_triangles(bk_ngs3d_context_t *, bk_ngs3d_vertices_t *);
typedef struct {
    uint32_t size, draw_calls, triangles, fallbacks, downloads, texture_uploads, strict_failures;
} bk_ngs3d_stats_t;
int bk_ngs3d_stats(bk_ngs3d_context_t *, bk_ngs3d_stats_t *);
/* Resolve OpenGL entrypoints from the selected backend. Bind its context
   before invoking them; pointers expire when the context/library is released. */
void *bk_ngs3d_get_proc(bk_ngs3d_context_t *, const char *name);
/* OpenGL 1.x texture functions are exported by this same module. */
#endif
