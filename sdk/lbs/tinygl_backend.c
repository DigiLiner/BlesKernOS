#include "backend.h"
#include "resources.h"
#include <bleskernos_tinygl.h>
#include <bleskernos_ngs.h>
static bk_tinygl_context_t *active;
static bk_gui_window_t *attached;
static void *create(uint32_t width, uint32_t height, uint32_t renderer) {
    if (active)
        return 0; /* Existing TinyGL supports one current context. */
    bk_tinygl_clear_fatal_error();
    active = bk_tinygl_create_ex(width, height,
                                 renderer == BK_NGS3D_TINYGL_SOFTWARE ? BK_TINYGL_RENDERER_SOFTWARE
                                                                      : BK_TINYGL_RENDERER_AUTO);
    if (active) {
        glViewport(0, 0, width, height);
        bk_lbs_resource_acquire();
    }
    return active;
}
static void destroy(void *context) {
    if (!context || context != active)
        return;
    if (attached) {
        bk_gui_window_clear_gpu_viewport(attached);
        attached = 0;
    }
    bk_tinygl_destroy(active);
    active = 0;
    bk_lbs_resource_release();
}
static int current(void *c) { return c && c == active ? 0 : BK_LBS_INVALID; }
static int capabilities(void *c, bk_ngs3d_caps_t *out) {
    if (current(c) || !out || out->size < sizeof(*out))
        return BK_LBS_INVALID;
    out->requested = bk_tinygl_requested_renderer(active) == BK_TINYGL_RENDERER_SOFTWARE
                         ? BK_NGS3D_TINYGL_SOFTWARE
                         : BK_NGS3D_TINYGL_AUTO;
    out->active = bk_tinygl_active_renderer(active);
    out->gpu_available = bk_tinygl_gpu_available();
    out->depth_bits = 16;
    out->renderer = bk_tinygl_renderer_name(active);
    return 0;
}
static int present(void *c, bk_gui_window_t *window) {
    bk_gui_rect_t r;
    if (current(c))
        return BK_LBS_INVALID;
    if (attached && attached != window)
        bk_gui_window_clear_gpu_viewport(attached);
    attached = 0;
    if (window && bk_tinygl_active_renderer(active) == BK_TINYGL_RENDERER_GPU &&
        bk_gui_gpu_viewport_supported() && bk_tinygl_present_gpu(active) &&
        bk_gui_window_content_rect(window, &r) &&
        bk_gui_window_set_gpu_viewport(window, bk_tinygl_gpu_surface(active), active->width,
                                       active->height, r)) {
        attached = window;
        return 0;
    }
    if (!bk_tinygl_present(active))
        return -5;
    if (window) {
        bk_gui_window_clear_gpu_viewport(window);
        if (bk_ngs_window_present(window, bk_tinygl_pixels(active), active->width, active->height))
            return -5;
    }
    return 0;
}
static const uint32_t *pixels(void *c) { return current(c) ? 0 : bk_tinygl_pixels(active); }
static int sync_frame(void *c) {
    if (current(c))
        return BK_LBS_INVALID;
    glFinish();
    return bk_tinygl_last_fatal_error() ? -5 : 0;
}
static int triangles(void *c, const float *xyz, uint32_t count) {
    if (current(c) || !xyz)
        return BK_LBS_INVALID;
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, xyz);
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDisableClientState(GL_VERTEX_ARRAY);
    return sync_frame(c);
}
static int stats(void *c, bk_ngs3d_stats_t *out) {
    bk_tinygl_gpu_stats_t s;
    if (current(c) || !out || out->size < sizeof(*out))
        return BK_LBS_INVALID;
    bk_tinygl_gpu_get_stats(&s);
    out->draw_calls = s.draw_calls;
    out->triangles = s.triangles_submitted;
    out->fallbacks = s.fallbacks;
    out->downloads = s.frames_resolved;
    out->texture_uploads = s.texture_uploads;
    out->strict_failures = 0;
    return 0;
}
static const bk_ngs3d_backend_t backend = {
    sizeof(backend), 1,      create,     destroy,   current, capabilities,
    present,         pixels, sync_frame, triangles, stats};
const bk_ngs3d_backend_t *bk_ngs3d_backend_v1(void) { return &backend; }
