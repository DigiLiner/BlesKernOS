#ifndef BK_NGS3D_BACKEND_INTERNAL_H
#define BK_NGS3D_BACKEND_INTERNAL_H
#include <bleskernos_ngs3d.h>
/* Append-only plugin interface; implementation pointers never cross Ring 0. */
typedef struct {
    uint32_t size, abi;
    void *(*create)(uint32_t, uint32_t, uint32_t);
    void (*destroy)(void *);
    int (*current)(void *);
    int (*capabilities)(void *, bk_ngs3d_caps_t *);
    int (*present)(void *, bk_gui_window_t *);
    const uint32_t *(*pixels)(void *);
    int (*sync)(void *);
    int (*triangles)(void *, const float *, uint32_t);
    int (*stats)(void *, bk_ngs3d_stats_t *);
} bk_ngs3d_backend_t;
#endif
