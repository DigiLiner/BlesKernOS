#include <bleskernos_ngs3d.h>
#include <bleskernos_mesa.h>
#include <bleskernos_ngs.h>
#include <string.h>
#include "resources.h"
#include "backend.h"
#include <dlfcn.h>
extern const void *_glapi_get_proc_address(const char *);
struct bk_ngs3d_vertices {
    struct bk_ngs3d_vertices *next;
    uint32_t count;
    float xyz[];
};
struct bk_ngs3d_context {
    bk_mesa_context_t *mesa;
    bk_ngs3d_vertices_t *vertices;
    bk_gui_window_t *window;
    void *plugin, *plugin_context;
    const bk_ngs3d_backend_t *backend;
};
bk_ngs3d_context_t *bk_ngs3d_create(uint32_t w, uint32_t h, uint32_t backend) {
    bk_ngs3d_context_t *c;
    if (backend > BK_NGS3D_TINYGL_AUTO)
        return 0;
    c = bk_sys_alloc(sizeof(*c));
    if (!c)
        return 0;
    c->vertices = 0;
    c->window = 0;
    c->plugin = 0;
    c->backend = 0;
    c->plugin_context = 0;
    c->mesa = 0;
    if (backend >= BK_NGS3D_TINYGL_SOFTWARE) {
        const bk_ngs3d_backend_t *(*query)(void);
        c->plugin = dlopen(BK_LBS_DIRECTORY "TINYGL.LBS", RTLD_NOW | RTLD_LOCAL);
        if (!c->plugin) {
            bk_sys_free(c);
            return 0;
        }
        query = dlsym(c->plugin, "bk_ngs3d_backend_v1");
        c->backend = query ? query() : 0;
        if (!c->backend || c->backend->size < sizeof(*c->backend) || c->backend->abi != 1 ||
            !(c->plugin_context = c->backend->create(w, h, backend))) {
            dlclose(c->plugin);
            bk_sys_free(c);
            return 0;
        }
        bk_lbs_resource_acquire();
        return c;
    }
    c->mesa = bk_mesa_create_renderer(w, h, (bk_mesa_renderer_t)backend);
    if (!c->mesa) {
        bk_sys_free(c);
        return 0;
    }
    bk_lbs_resource_acquire();
    return c;
}
void bk_ngs3d_destroy(bk_ngs3d_context_t *c) {
    if (!c)
        return;
    while (c->vertices)
        bk_ngs3d_vertices_destroy(c, c->vertices);
    if (c->plugin) {
        c->backend->destroy(c->plugin_context);
        dlclose(c->plugin);
        bk_sys_free(c);
        bk_lbs_resource_release();
        return;
    }
    if (c->window)
        bk_mesa_detach_from_window(c->window);
    bk_mesa_destroy(c->mesa);
    bk_sys_free(c);
    bk_lbs_resource_release();
}
int bk_ngs3d_current(bk_ngs3d_context_t *c) {
    if (c && c->plugin)
        return c->backend->current(c->plugin_context);
    return c && bk_mesa_make_current(c->mesa) ? 0 : BK_LBS_INVALID;
}
int bk_ngs3d_capabilities(bk_ngs3d_context_t *c, bk_ngs3d_caps_t *o) {
    if (!c || !o || o->size < sizeof(*o))
        return BK_LBS_INVALID;
    if (c->plugin)
        return c->backend->capabilities(c->plugin_context, o);
    o->requested = bk_mesa_requested_renderer(c->mesa);
    o->active = bk_mesa_active_renderer(c->mesa);
    o->gpu_available = bk_mesa_gpu_available(c->mesa);
    o->depth_bits = c->mesa->depth_bits;
    o->renderer = bk_mesa_context_renderer_name(c->mesa);
    return 0;
}
int bk_ngs3d_present(bk_ngs3d_context_t *c, bk_gui_window_t *w) {
    if (!c)
        return BK_LBS_INVALID;
    if (c->plugin)
        return c->backend->present(c->plugin_context, w);
    if (c->window && c->window != w)
        bk_mesa_detach_from_window(c->window);
    c->window = 0;
    /* Zero-copy path only when both Mesa and Vision support it. */
    if (w && bk_mesa_active_renderer(c->mesa) == BK_MESA_RENDERER_GPU &&
        bk_mesa_present_gpu(c->mesa)) {
        bk_gui_rect_t r;
        if (bk_gui_window_content_rect(w, &r) && bk_mesa_attach_to_window(c->mesa, w, r)) {
            c->window = w;
            return 0;
        }
    }
    if (!bk_mesa_present(c->mesa))
        return -5;
    if (w) {
        bk_mesa_detach_from_window(w);
        if (bk_ngs_window_present(w, bk_mesa_pixels(c->mesa), bk_mesa_width(c->mesa),
                                  bk_mesa_height(c->mesa)))
            return -5;
    }
    return 0;
}
const uint32_t *bk_ngs3d_pixels(bk_ngs3d_context_t *c) {
    return c ? (c->plugin ? c->backend->pixels(c->plugin_context) : bk_mesa_pixels(c->mesa)) : 0;
}
int bk_ngs3d_sync(bk_ngs3d_context_t *c) {
    if (bk_ngs3d_current(c))
        return BK_LBS_INVALID;
    if (c->plugin)
        return c->backend->sync(c->plugin_context);
    glFinish();
    return bk_mesa_strict_failed(c->mesa) ? -5 : 0;
}
bk_ngs3d_vertices_t *bk_ngs3d_vertices_create(bk_ngs3d_context_t *c, const float *xyz, uint32_t n) {
    bk_ngs3d_vertices_t *v;
    if (!c || !xyz || !n || n > 1048576U)
        return 0;
    v = bk_sys_alloc(sizeof(*v) + n * 3 * sizeof(float));
    if (!v)
        return 0;
    memcpy(v->xyz, xyz, n * 3 * sizeof(float));
    v->count = n;
    v->next = c->vertices;
    c->vertices = v;
    return v;
}
void bk_ngs3d_vertices_destroy(bk_ngs3d_context_t *c, bk_ngs3d_vertices_t *v) {
    bk_ngs3d_vertices_t **p;
    if (!c || !v)
        return;
    for (p = &c->vertices; *p; p = &(*p)->next)
        if (*p == v) {
            *p = v->next;
            bk_sys_free(v);
            return;
        }
}
int bk_ngs3d_triangles(bk_ngs3d_context_t *c, bk_ngs3d_vertices_t *v) {
    bk_ngs3d_vertices_t *p;
    if (!c || !v)
        return BK_LBS_INVALID;
    for (p = c->vertices; p && p != v; p = p->next) {
    }
    if (!p || bk_ngs3d_current(c))
        return BK_LBS_INVALID;
    if (c->plugin)
        return c->backend->triangles(c->plugin_context, v->xyz, v->count);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, v->xyz);
    glDrawArrays(GL_TRIANGLES, 0, v->count);
    glDisableClientState(GL_VERTEX_ARRAY);
    GLenum error = glGetError();
    if (error == GL_OUT_OF_MEMORY)
        return BK_LBS_NOMEM;
    if (error != GL_NO_ERROR)
        return BK_LBS_INVALID;
    return bk_mesa_strict_failed(c->mesa) ? -5 : 0;
}

void *bk_ngs3d_get_proc(bk_ngs3d_context_t *c, const char *name) {
    if (!c || !name || strncmp(name, "gl", 2))
        return 0;
    if (c->plugin) {
        char prefixed[128];
        if (strlen(name) + 2 > sizeof(prefixed))
            return 0;
        prefixed[0] = 't';
        strcpy(prefixed + 1, name);
        return dlsym(c->plugin, prefixed);
    }
    return (void *)_glapi_get_proc_address(name);
}

int bk_ngs3d_stats(bk_ngs3d_context_t *c, bk_ngs3d_stats_t *out) {
    bk_mesa_gpu_stats_t s;
    if (!c || !out || out->size < sizeof(*out))
        return BK_LBS_INVALID;
    if (c->plugin)
        return c->backend->stats(c->plugin_context, out);
    bk_mesa_get_gpu_stats(c->mesa, &s);
    out->draw_calls = s.draw_calls;
    out->triangles = s.triangles;
    out->fallbacks = s.fallbacks;
    out->downloads = s.downloads;
    out->texture_uploads = s.texture_uploads;
    out->strict_failures = s.strict_failures;
    return 0;
}
