#ifndef BLESKERNOS_SS_MESA_H
#define BLESKERNOS_SS_MESA_H

/* Shared Mesa 3.5 / OSMesa surface used by BlesKernOS screen savers.
 *
 * TinyGL owns a single process-global context and its glInit/glClose path is
 * not suitable for the screen-saver preview, which can be painted while the
 * desktop is also servicing other GUI programs.  OSMesa gives every saver an
 * explicit context and lets us make it current before every frame.
 */

#include <GL/gl.h>
#include <GL/osmesa.h>

typedef struct {
    OSMesaContext context;
    uint32_t *pixels;
    uint16_t width;
    uint16_t height;
    bool ready;
} ss_mesa_surface_t;

static bool ss_mesa_surface_make_current(ss_mesa_surface_t *surface) {
    if (!surface || !surface->ready || !surface->context || !surface->pixels)
        return false;
    /* Rebinding also resets driver state; retain the active pipeline. */
    if (OSMesaGetCurrentContext() == surface->context) return true;
    if (!OSMesaMakeCurrent(surface->context, surface->pixels,
                           GL_UNSIGNED_BYTE,
                           (GLsizei)surface->width,
                           (GLsizei)surface->height))
        return false;
    OSMesaPixelStore(OSMESA_ROW_LENGTH, (GLint)surface->width);
    /* GUI surfaces have y=0 at the top, so keep OSMesa in top-down order. */
    OSMesaPixelStore(OSMESA_Y_UP, 0);
    return true;
}

static bool ss_mesa_surface_create(ss_mesa_surface_t *surface,
                                   uint16_t width, uint16_t height,
                                   bool allow_gpu) {
    uint32_t count;
    if (!surface || width < 16U || height < 16U) return false;

    surface->context = NULL;
    surface->pixels = NULL;
    surface->width = width;
    surface->height = height;
    surface->ready = false;

    count = (uint32_t)width * (uint32_t)height;
    if (count > 0xFFFFFFFFU / 4U) return false;
    surface->pixels = (uint32_t *)bk_sys_alloc_zero(count * 4U);
    if (!surface->pixels) return false;

    surface->context = OSMesaCreateContextExt(OSMESA_BGRA,
                                               16, 0, 0, NULL);
    if (!surface->context) {
        bk_sys_free(surface->pixels);
        surface->pixels = NULL;
        return false;
    }

    if (!OSMesaBleskSetRenderer(surface->context,
            allow_gpu ? OSMESA_BLESK_RENDERER_AUTO
                      : OSMESA_BLESK_RENDERER_SOFTWARE)) {
        OSMesaDestroyContext(surface->context);
        surface->context = NULL;
        bk_sys_free(surface->pixels);
        surface->pixels = NULL;
        return false;
    }

    surface->ready = true;
    if (!ss_mesa_surface_make_current(surface)) {
        OSMesaDestroyContext(surface->context);
        surface->context = NULL;
        bk_sys_free(surface->pixels);
        surface->pixels = NULL;
        surface->ready = false;
        return false;
    }
    return true;
}

static bool ss_mesa_surface_present(ss_mesa_surface_t *surface) {
    if (!ss_mesa_surface_make_current(surface)) return false;
    glFinish();
    return OSMesaBleskResolveColor(surface->context) ? true : false;
}

static void ss_mesa_surface_destroy(ss_mesa_surface_t *surface) {
    if (!surface) return;
    if (surface->context) OSMesaDestroyContext(surface->context);
    if (surface->pixels) bk_sys_free(surface->pixels);
    surface->context = NULL;
    surface->pixels = NULL;
    surface->width = 0;
    surface->height = 0;
    surface->ready = false;
}

static inline uint32_t ss_mesa_pixel_to_rgb(uint32_t pixel) {
    /* OSMESA_BGRA on little-endian x86 maps to 0xAARRGGBB. */
    return pixel & 0x00FFFFFFU;
}

#endif
