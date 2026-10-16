#include "gui.h"
#include "color_palette.h"
#include "../kernel/include/system_sounds.h"
#include "../kernel/include/gfx.h"
#ifndef BLES_ARCH_ARM32_RPI3
#include "../kernel/include/gfx_vga.h"
#endif
#include "../kernel/include/memory.h"
#include "../kernel/include/mouse.h"
#ifndef BLES_ARCH_ARM32_RPI3
#include "../kernel/include/pic.h"
#include "../kernel/include/pit.h"
#endif
#include "../kernel/include/task.h"
#include "../kernel/include/compat_mode.h"

#define VGA_INPUT_STATUS_1 0x3DA
#define VGA_VERTICAL_RETRACE 0x08
#define GUI_GFX_BULK_PRESENT_PIXELS 16384U

static uint32_t *g_front_shadow;
static uint32_t g_shadow_pixels;
static int8_t g_retrace_state;
static bool g_page_flip;
static bool g_16_color_mode;
static uint8_t g_output_depth;
static bool g_driver_surface_mapped;
static gfx_surface_handle_t g_driver_surface_handle;

static void gui_gfx_copy_dwords(volatile uint32_t *destination,
                                const uint32_t *source, uint32_t count) {
#ifdef BLES_ARCH_ARM32_RPI3
    while (count--) *destination++ = *source++;
#else
    uint32_t *dst = (uint32_t *)(uintptr_t)destination;
    const uint32_t *src = source;
    __asm__ volatile ("cld; rep movsl"
                      : "+D"(dst), "+S"(src), "+c"(count)
                      : : "memory");
#endif
}

static void gui_gfx_copy_rect32(uint32_t framebuffer,
                                const gfx_info_t *info,
                                const gui_surface_t *surface,
                                gui_rect_t clip) {
    for (uint16_t y = (uint16_t)clip.y; y < clip.y + clip.h; y++) {
        volatile uint32_t *dst = (volatile uint32_t *)
            (framebuffer + (uint32_t)y * info->pitch) + clip.x;
        const uint32_t *src = &surface->pixels[(uint32_t)y *
            surface->pitch + (uint32_t)clip.x];
        gui_gfx_copy_dwords(dst, src, (uint32_t)clip.w);
    }
}

/*
 * El LFB no ofrece page flipping en todos los modos VBE que soportamos. Si la
 * copia empieza a mitad del barrido, las filas inferiores se ven en el cuadro
 * actual y las superiores recién en el siguiente, produciendo una diferencia
 * de fluidez según la altura. Esperar el inicio del retrazado reduce ese corte.
 * Los límites por ticks evitan bloquear el GUI en adaptadores que no reflejen
 * el estado VGA clásico por 0x3DA.
 */
static void gui_gfx_wait_vertical_retrace(void) {
#ifdef BLES_ARCH_ARM32_RPI3
    /* The firmware framebuffer has no VGA status register. The platform
       backend performs cache coherency during present_buffer(). */
    return;
#else
    uint32_t start = pit_get_ticks();
    uint32_t timeout = pit_get_frequency_hz() / 30U;
    uint32_t polls = 0U;
    const gfx_info_t *info = gfx_get_info();

    /* Phoenix/Intel 852GME: VBE 0x114 gives a working RGB565 LFB but
     * port 0x3DA need not report retrace in this firmware mode. Never make
     * display progress depend on that legacy register on this laptop. */
    if (info && info->mode == GFX_MODE_VESA_LFB && info->bpp == 16U &&
        mm_boot_vesa_read16(15U) == 0x0114U)
        return;
    if (g_retrace_state < 0) return;
    if (!timeout) timeout = 1U;
    while ((inb(VGA_INPUT_STATUS_1) & VGA_VERTICAL_RETRACE) &&
           pit_get_ticks() - start < timeout && polls++ < 20000U) {
        __asm__ volatile ("pause");
    }
    if (polls >= 20000U || pit_get_ticks() - start >= timeout) {
        g_retrace_state = -1;
        return;
    }

    start = pit_get_ticks();
    polls = 0U;
    while (!(inb(VGA_INPUT_STATUS_1) & VGA_VERTICAL_RETRACE) &&
           pit_get_ticks() - start < timeout && polls++ < 20000U) {
        __asm__ volatile ("pause");
    }
    if (polls >= 20000U || pit_get_ticks() - start >= timeout) {
        g_retrace_state = -1;
        return;
    }
    g_retrace_state = 1;
#endif
}

static uint32_t gui_rgb_to_vga(uint32_t rgb) {
    uint8_t r = (uint8_t)((rgb >> 16) & 0xFF);
    uint8_t g = (uint8_t)((rgb >> 8) & 0xFF);
    uint8_t b = (uint8_t)(rgb & 0xFF);
    uint8_t color = 0;
    if (r > 85) color |= 4;
    if (g > 85) color |= 2;
    if (b > 85) color |= 1;
    if (r > 170 || g > 170 || b > 170) color |= 8;
    return color;
}

static uint8_t gui_rgb_to_332(uint32_t rgb) {
    uint8_t r = (uint8_t)((rgb >> 16) & 0xE0);
    uint8_t g = (uint8_t)((rgb >> 8) & 0xE0);
    uint8_t b = (uint8_t)(rgb & 0xC0);
    return (uint8_t)(r | (g >> 3) | (b >> 6));
}

void gui_gfx_set_16_color_mode(bool enabled) {
    g_16_color_mode = enabled;
    if (enabled)
        gfx_set_default_palette();
    gui_gfx_invalidate_front();
}

bool gui_gfx_16_color_mode(void) {
    return g_16_color_mode;
}

/* A 32-bit-only adapter can still display a reduced color palette. Convert
 * damage in the existing desktop buffer, with no extra framebuffer. */
void gui_gfx_set_output_depth(uint8_t depth) { g_output_depth = depth; gui_gfx_invalidate_front(); }
uint8_t gui_gfx_output_depth(void) { return g_output_depth; }
static void gui_gfx_quantize_output(const gui_surface_t *surface, gui_rect_t clip) {
    if (!g_output_depth || g_output_depth >= 24U) return;
    static const uint32_t ega[16] = {0x000000,0x0000AA,0x00AA00,0x00AAAA,
        0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,0x555555,0x5555FF,0x55FF55,
        0x55FFFF,0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF};
    for (int y = clip.y; y < clip.y + clip.h; ++y) {
        uint32_t *row = surface->pixels + (uint32_t)y * surface->pitch;
        for (int x = clip.x; x < clip.x + clip.w; ++x) {
            uint32_t c = row[x], r = (c >> 16) & 255U, g = (c >> 8) & 255U, b = c & 255U;
            if (g_output_depth == 8U) c = content_rgb332_to_888(content_rgb888_to_332(c));
            else if (g_output_depth == 16U) {
                r >>= 3; g >>= 2; b >>= 3;
                c = (((r << 3) | (r >> 2)) << 16) |
                    (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
            } else {
                uint32_t best = 0, distance = 0xFFFFFFFFU;
                for (uint32_t i = 0; i < 16U; ++i) {
                    int dr = (int)r - (int)((ega[i] >> 16) & 255U);
                    int dg = (int)g - (int)((ega[i] >> 8) & 255U);
                    int db = (int)b - (int)(ega[i] & 255U);
                    uint32_t d = dr*dr + dg*dg + db*db;
                    if (d < distance) { distance = d; best = i; }
                }
                c = ega[best];
            }
            row[x] = c;
        }
    }
}

static bool gui_gfx_prepare_buffers(gui_surface_t *prepared,
                                    uint32_t **shadow_out,
                                    uint32_t *pixel_count_out,
                                    gfx_surface_handle_t *driver_handle_out,
                                    bool *driver_mapped_out) {
    const gfx_info_t *info;
    uint32_t pixel_count;
    uint32_t pixel_pitch;
    size_t bytes;
    uint32_t *pixels = NULL;
    uint32_t *shadow = NULL;
    gfx_surface_handle_t driver_handle = 0U;
    bool driver_created = false;
    bool driver_mapped = false;
    bool need_shadow;

    if (!prepared || !shadow_out || !pixel_count_out || !driver_handle_out ||
        !driver_mapped_out)
        return false;
    info = gfx_get_info();
    if (!info || !info->width || !info->height) return false;
    if ((uint32_t)info->width > 0xFFFFFFFFU / (uint32_t)info->height)
        return false;
    pixel_count = (uint32_t)info->width * (uint32_t)info->height;
    if (pixel_count > 0xFFFFFFFFU / (uint32_t)sizeof(uint32_t))
        return false;
    bytes = (size_t)pixel_count * sizeof(uint32_t);
    pixel_pitch = info->width;

    /* Un backbuffer mapeado por el driver solo se usa si el mapa CPU es apto
     * para rasterización frecuente y presentar no exige otra copia por CPU.
     * La Rage 128 compone en RAM y sube únicamente los dirty rects: escribir
     * cada primitiva GUI directamente sobre su VRAM sin write-combining era
     * mucho más lento que la presentación que intentaba ahorrar. */
    if ((gfx_driver_capabilities() & GFX_CAP_SURFACE_CPU_MAP) &&
        !(gfx_driver_capabilities() & GFX_CAP_PRESENT_CPU_COPY) &&
        gfx_surface_create(info->width, info->height, &driver_handle)) {
        driver_created = true;
        if (gfx_surface_map(driver_handle, &pixels, &pixel_pitch) &&
            pixels && pixel_pitch == info->width) {
            driver_mapped = true;
        } else {
            (void)gfx_surface_destroy(driver_handle);
            driver_created = false;
            driver_handle = 0U;
            pixels = NULL;
            pixel_pitch = info->width;
        }
    }

    if (!driver_mapped) {
        pixels = (uint32_t *)kmalloc(bytes);
        if (!pixels) return false;
        /* Estas superficies sobreviven al proceso que pide el cambio de modo.
         * Sin transferirlas al kernel, cerrar DISPLAY.CPL hacia que el reaper
         * liberase framebuffer y shadow mientras el compositor aun los usaba. */
        if (!mm_set_allocation_owner(pixels, 0U)) {
            kfree(pixels);
            return false;
        }
        /* A mode switch must never expose allocator contents while the first
           complete desktop frame is being rebuilt asynchronously. */
        (void)mm_set_allocation_subsystem(pixels,MM_SUBSYSTEM_GUI);
        kmemset(pixels, 0, bytes);
    } else {
        for (uint32_t i = 0; i < pixel_count; i++) pixels[i] = 0U;
    }

    /* Low-RAM presentation copies only damaged rectangles and converts
     * directly to scanout; it does not require a full-screen comparison copy. */
    need_shadow = mm_total_bytes() >= 64U*1024U*1024U &&
        compat_mode_use_front_shadow() &&
        info->mode != GFX_MODE_VGA_13H && info->mode != GFX_MODE_VGA_12H &&
        !(gfx_driver_capabilities() & GFX_CAP_PRESENT_BUFFER);
    if (need_shadow) {
        shadow = (uint32_t *)kmalloc(bytes);
        if (!shadow) {
            if (driver_created) (void)gfx_surface_destroy(driver_handle);
            else kfree(pixels);
            return false;
        }
        if (!mm_set_allocation_owner(shadow, 0U)) {
            kfree(shadow);
            if (driver_created) (void)gfx_surface_destroy(driver_handle);
            else kfree(pixels);
            return false;
        }
        (void)mm_set_allocation_subsystem(shadow,MM_SUBSYSTEM_GUI);
        for (uint32_t i = 0; i < pixel_count; i++)
            shadow[i] = 0xFFFFFFFFU;
    }

    *prepared = (gui_surface_t){
        .pixels = pixels,
        .width = info->width,
        .height = info->height,
        .pitch = (uint16_t)pixel_pitch,
        .clip = {0, 0, info->width, info->height}
    };
    *shadow_out = shadow;
    *pixel_count_out = pixel_count;
    *driver_handle_out = driver_handle;
    *driver_mapped_out = driver_mapped;
    return true;
}

static void gui_gfx_commit_buffers(gui_surface_t *surface,
                                   gui_surface_t prepared,
                                   uint32_t *shadow,
                                   uint32_t pixel_count,
                                   gfx_surface_handle_t driver_handle,
                                   bool driver_mapped) {
    *surface = prepared;
    g_front_shadow = shadow;
    g_shadow_pixels = pixel_count;
    g_driver_surface_handle = driver_handle;
    g_driver_surface_mapped = driver_mapped;
    g_retrace_state = 0;
    /* El page flip VBE/BGA no es seguro con dirty rects. La ruta directa ya
     * usa shadow, dirty rects y REP MOVSD para regiones grandes. */
    g_page_flip = false;
    mouse_set_bounds(surface->width, surface->height);
}

static bool gui_gfx_allocate(gui_surface_t *surface) {
    gui_surface_t prepared;
    uint32_t *shadow;
    uint32_t pixel_count;
    gfx_surface_handle_t driver_handle;
    bool driver_mapped;

    if (!surface || !gui_gfx_prepare_buffers(&prepared, &shadow, &pixel_count,
                                             &driver_handle, &driver_mapped))
        return false;
    gui_gfx_commit_buffers(surface, prepared, shadow, pixel_count,
                           driver_handle, driver_mapped);
    return true;
}

static bool gui_gfx_is_lfb_mode(const gfx_info_t *info) {
    return info && (info->mode == GFX_MODE_VESA_LFB ||
                    info->mode == GFX_MODE_ATI_RAGE128 ||
                    info->mode == GFX_MODE_INTEL_GMA9XX ||
                    info->mode == GFX_MODE_RPI_FRAMEBUFFER);
}

static void gui_gfx_discard_buffers(gui_surface_t *surface) {
    if (!surface) return;
    if (g_driver_surface_mapped)
        (void)gfx_surface_destroy(g_driver_surface_handle);
    else if (surface->pixels)
        kfree(surface->pixels);
    if (g_front_shadow) kfree(g_front_shadow);
    *surface = (gui_surface_t){0};
    g_front_shadow = NULL;
    g_shadow_pixels = 0U;
    g_driver_surface_mapped = false;
    g_driver_surface_handle = 0U;
    g_page_flip = false;
}

bool gui_gfx_init(gui_surface_t *surface) {
    const gfx_info_t *info;

    if (!surface) return false;
    surface->pixels = NULL;
    surface->width = 0;
    surface->height = 0;
    surface->pitch = 0;
    surface->clip = (gui_rect_t){0, 0, 0, 0};
    g_driver_surface_mapped = false;
    g_driver_surface_handle = 0U;

    info = gfx_get_info();
    if (!info) return false;

    /*
     * Respetar el modo que eligio Stage2/gfx_init().
     *
     * Antes, cualquier modo que no fuera VESA terminaba forzado a VGA 13h.
     * Por eso VGA 12h y VGA texto caian en 320x200x8.
     */
    if (info->mode == GFX_MODE_TEXT) {
        return false; /* No iniciar GUI en modo texto. */
    }

    if (!gui_gfx_is_lfb_mode(info) &&
        info->mode != GFX_MODE_VMWARE_SVGA &&
        info->mode != GFX_MODE_VIRTIO_GPU &&
        info->mode != GFX_MODE_VGA_13H &&
        info->mode != GFX_MODE_VGA_12H) {
        if (!gfx_set_mode13h()) return false;
    }

    return gui_gfx_allocate(surface);
}

bool gui_gfx_reconfigure(gui_surface_t *surface) {
    const gfx_info_t *current_info;
    gui_surface_t prepared;
    uint32_t *new_shadow;
    uint32_t new_pixel_count;
    gfx_surface_handle_t new_driver_handle;
    bool new_driver_mapped;
    uint32_t *old_pixels;
    uint32_t *old_shadow;
    gfx_surface_handle_t old_driver_handle;
    bool old_driver_mapped;

    current_info = gfx_get_info();
    if (surface && current_info && g_driver_surface_mapped &&
        (gfx_driver_capabilities() & GFX_CAP_PRESENT_VRAM_BLIT) &&
        surface->width == current_info->width &&
        surface->height == current_info->height &&
        surface->pitch == current_info->width) {
        gui_gfx_invalidate_front();
        mouse_set_bounds(surface->width, surface->height);
        return true;
    }

    if (!surface) return false;
    if (!gui_gfx_prepare_buffers(&prepared, &new_shadow, &new_pixel_count,
                                 &new_driver_handle, &new_driver_mapped)) {
        /* On <=16 MiB there is often enough room for either framebuffer but
           not both at once. The mode transaction owns the compositor lock,
           so release the old CPU buffers and retry in-place. gui_change_
           resolution restores the old mode/buffer if this retry also fails. */
        if (mm_physical_top() > 16U * 1024U * 1024U) return false;
        gui_gfx_discard_buffers(surface);
        if (!gui_gfx_prepare_buffers(&prepared, &new_shadow,
                                     &new_pixel_count, &new_driver_handle,
                                     &new_driver_mapped))
            return false;
    }

    /* Publicar el par nuevo de forma atomica desde el punto de vista del
     * compositor. Si la reserva falla, la superficie anterior sigue valida. */
    task_preempt_disable();
    old_pixels = surface->pixels;
    old_shadow = g_front_shadow;
    old_driver_handle = g_driver_surface_handle;
    old_driver_mapped = g_driver_surface_mapped;
    gui_gfx_commit_buffers(surface, prepared, new_shadow, new_pixel_count,
                           new_driver_handle, new_driver_mapped);
    task_preempt_enable();

    if (old_driver_mapped) {
        if (!new_driver_mapped || old_driver_handle != new_driver_handle)
            (void)gfx_surface_destroy(old_driver_handle);
    } else if (old_pixels && old_pixels != prepared.pixels) {
        kfree(old_pixels);
    }
    if (old_shadow) kfree(old_shadow);
    return true;
}

void gui_gfx_shutdown(gui_surface_t *surface) {
    gui_gfx_discard_buffers(surface);
}

void gui_gfx_invalidate_front(void) {
    if (!g_front_shadow) return;
    for (uint32_t i = 0; i < g_shadow_pixels; i++)
        g_front_shadow[i] = 0xFFFFFFFF;
}


void gui_gfx_present(const gui_surface_t *surface) {
    if (!surface) return;
    gui_gfx_present_rect(surface, (gui_rect_t){0, 0,
                                               surface->width,
                                               surface->height});
}

void gui_gfx_present_rects(const gui_surface_t *surface,
                           const gui_rect_t *rects, uint8_t count) {
    const gfx_info_t *info = gfx_get_info();
    gui_rect_t clipped[4];
    gui_rect_t screen;
    uint8_t clipped_count = 0;

    if (!surface || !surface->pixels || !rects || !count || !info) return;
    screen = (gui_rect_t){0, 0, surface->width, surface->height};
    if (count > 4U) count = 4U;
    for (uint8_t i = 0; i < count; i++) {
        if (gui_rect_intersect(screen, rects[i], &clipped[clipped_count]))
            clipped_count++;
    }
    if (!clipped_count) return;
    for (uint8_t i = 0; i < clipped_count; ++i)
        gui_gfx_quantize_output(surface, clipped[i]);

    /* Keep the small damage list useful all the way to the driver. Cursor,
     * chrome and content rectangles often overlap; sending the overlap twice
     * means copying the same VRAM rows twice and issuing redundant UPDATEs. */
    for (uint8_t i = 0; i < clipped_count; i++) {
        uint8_t j = (uint8_t)(i + 1U);
        while (j < clipped_count) {
            gui_rect_t a = clipped[i];
            gui_rect_t b = clipped[j];
            int ax1 = a.x + a.w, ay1 = a.y + a.h;
            int bx1 = b.x + b.w, by1 = b.y + b.h;
            bool overlap_or_touch = a.x <= bx1 && b.x <= ax1 &&
                                    a.y <= by1 && b.y <= ay1;
            if (overlap_or_touch) {
                int ux0 = a.x < b.x ? a.x : b.x;
                int uy0 = a.y < b.y ? a.y : b.y;
                int ux1 = ax1 > bx1 ? ax1 : bx1;
                int uy1 = ay1 > by1 ? ay1 : by1;
                uint32_t union_area = (uint32_t)(ux1 - ux0) * (uint32_t)(uy1 - uy0);
                uint32_t input_area = (uint32_t)a.w * (uint32_t)a.h +
                                      (uint32_t)b.w * (uint32_t)b.h;
                /* Merge when the bounding box adds at most 25% overdraw. */
                if (union_area <= input_area + input_area / 4U) {
                    clipped[i] = (gui_rect_t){ux0, uy0, ux1 - ux0, uy1 - uy0};
                    for (uint8_t k = j; k + 1U < clipped_count; k++)
                        clipped[k] = clipped[k + 1U];
                    clipped_count--;
                    continue;
                }
            }
            j++;
        }
    }

    if (gfx_driver_capabilities() & GFX_CAP_PRESENT_BUFFER) {
        gfx_rect_t dirty[4];
        uint32_t fence = 0;
        for (uint8_t i = 0; i < clipped_count; i++)
            dirty[i] = (gfx_rect_t){clipped[i].x, clipped[i].y,
                                    clipped[i].w, clipped[i].h};
        if (gfx_present_buffer(surface->pixels, surface->pitch,
                               dirty, clipped_count, &fence))
            return;
    }

    /* Prepare todas las regiones en la misma pagina y haga un solo flip. El
       cursor anterior, el nuevo y un deskbar distante ya no provocan varios
       VSync ni un rectangulo cuya altura dependa de la posicion del mouse. */
    if (gui_gfx_is_lfb_mode(info) && info->bpp == 32 && g_page_flip && g_front_shadow) {
        uint32_t draw_buffer = gfx_page_flip_draw_buffer();

        if (draw_buffer) {
            for (uint8_t i = 0; i < clipped_count; i++)
                gui_gfx_copy_rect32(draw_buffer, info, surface, clipped[i]);
            gui_gfx_wait_vertical_retrace();
            if (gfx_page_flip_commit()) {
                draw_buffer = gfx_page_flip_draw_buffer();
                for (uint8_t i = 0; i < clipped_count; i++) {
                    gui_rect_t clip = clipped[i];
                    if (draw_buffer)
                        gui_gfx_copy_rect32(draw_buffer, info, surface, clip);
                    for (uint16_t y = (uint16_t)clip.y;
                         y < clip.y + clip.h; y++) {
                        uint32_t index = (uint32_t)y * surface->pitch +
                                         (uint32_t)clip.x;
                        kmemcpy(&g_front_shadow[index],
                                &surface->pixels[index],
                                (size_t)clip.w * sizeof(uint32_t));
                    }
                }
                return;
            }
        }
        g_page_flip = false;
    }

    /* Los modos sin page flipping conservan sus conversiones existentes. */
    for (uint8_t i = 0; i < clipped_count; i++)
        gui_gfx_present_rect(surface, clipped[i]);
}

void gui_gfx_present_rect(const gui_surface_t *surface, gui_rect_t rect) {
    const gfx_info_t *info = gfx_get_info();
    gui_rect_t screen;
    gui_rect_t clip;
    if (!surface || !surface->pixels || !info) return;
    screen = (gui_rect_t){0, 0, surface->width, surface->height};
    if (!gui_rect_intersect(screen, rect, &clip)) return;
    gui_gfx_quantize_output(surface, clip);

    /*
     * No invalidar g_front_shadow en cada frame.
     *
     * El shadow existe para evitar copiar al framebuffer los píxeles que no
     * cambiaron. Si se invalida acá, gui_gfx_present() termina reescribiendo
     * casi toda la pantalla en cada repaint, justo lo que mata rendimiento al
     * mover el mouse o subir resolución.
     *
     * Invalidate solo debe llamarse cuando cambia el modo/resolución, al
     * arrancar, o si se necesita forzar un repaint completo.
     */
    if (gfx_driver_capabilities() & GFX_CAP_PRESENT_BUFFER) {
        gfx_rect_t dirty = {clip.x, clip.y, clip.w, clip.h};
        if (gfx_present_buffer(surface->pixels, surface->pitch,
                               &dirty, 1U, NULL))
            return;
    }

#ifndef BLES_ARCH_ARM32_RPI3
    if (info->mode == GFX_MODE_VGA_13H || info->mode == GFX_MODE_VGA_12H) {
        if (info->mode == GFX_MODE_VGA_12H) {
            gfx_vga_blit_12h(info, surface->pixels, surface->pitch,
                             clip.x, clip.y, clip.w, clip.h);
            return;
        }
        for (uint16_t y = (uint16_t)clip.y; y < clip.y + clip.h; y++) {
            for (uint16_t x = (uint16_t)clip.x; x < clip.x + clip.w; x++) {
                gfx_putpixel(x, y, (uint8_t)gui_rgb_to_vga(surface->pixels[y * surface->pitch + x]));
            }
        }
        return;
    }
#endif

    if (gui_gfx_is_lfb_mode(info) && !g_front_shadow && info->bpp != 8) {
        /* Dirty rectangles already bound the copy. Low-RAM desktops do not
         * need a second full-screen buffer just to compare individual pixels. */
        gui_gfx_wait_vertical_retrace();
        if (info->bpp == 32) {
            gui_gfx_copy_rect32(info->framebuffer, info, surface, clip);
        } else {
            for (uint16_t y = (uint16_t)clip.y; y < clip.y + clip.h; ++y) {
                volatile uint8_t *dst = (volatile uint8_t *)
                    (info->framebuffer + (uint32_t)y * info->pitch);
                const uint32_t *src = &surface->pixels[(uint32_t)y * surface->pitch];
                for (uint16_t x = (uint16_t)clip.x; x < clip.x + clip.w; ++x) {
                    uint32_t rgb = src[x];
                    if (info->bpp == 16) {
                        ((volatile uint16_t *)dst)[x] = (uint16_t)
                            (((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) |
                             ((rgb >> 3) & 0x001F));
                    } else if (info->bpp == 24) {
                        dst[x * 3] = (uint8_t)rgb;
                        dst[x * 3 + 1] = (uint8_t)(rgb >> 8);
                        dst[x * 3 + 2] = (uint8_t)(rgb >> 16);
                    }
                }
            }
        }
        return;
    }

    if (gui_gfx_is_lfb_mode(info) && info->bpp == 16) {
        gui_gfx_wait_vertical_retrace();
        for (uint16_t y = (uint16_t)clip.y; y < clip.y + clip.h; y++) {
            volatile uint16_t *dst = (volatile uint16_t *)
                (info->framebuffer + (uint32_t)y * info->pitch);
            const uint32_t *src = &surface->pixels[(uint32_t)y *
                                                  surface->pitch];
            for (uint16_t x = (uint16_t)clip.x; x < clip.x + clip.w; x++) {
                uint32_t rgb = src[x];
                uint32_t index = (uint32_t)y * surface->pitch + x;
                if (g_front_shadow[index] == rgb) continue;
                dst[x] = (uint16_t)(((rgb >> 8) & 0xF800) |
                                    ((rgb >> 5) & 0x07E0) |
                                    ((rgb >> 3) & 0x001F));
                g_front_shadow[index] = rgb;
            }
        }
        return;
    }

    if (gui_gfx_is_lfb_mode(info) && info->bpp == 8) {
        /* El backend ATI ya sincroniza sus operaciones. En el fallback de
         * firmware 8bpp, esperar por polling VGA en cada dirty rect mantiene
         * un Pentium III al 100% aun cuando no cambia casi nada. */
        if (info->mode != GFX_MODE_ATI_RAGE128)
            gui_gfx_wait_vertical_retrace();
        for (uint16_t y = (uint16_t)clip.y; y < clip.y + clip.h; y++) {
            volatile uint8_t *dst = (volatile uint8_t *)
                (info->framebuffer + (uint32_t)y * info->pitch);
            const uint32_t *src = &surface->pixels[(uint32_t)y *
                                                  surface->pitch];
            for (uint16_t x = (uint16_t)clip.x; x < clip.x + clip.w; x++) {
                uint32_t index = (uint32_t)y * surface->pitch + x;
                if (g_front_shadow && g_front_shadow[index] == src[x]) continue;
                dst[x] = g_16_color_mode
                    ? gui_rgb_to_vga(src[x])
                    : gui_rgb_to_332(src[x]);
                if (g_front_shadow) g_front_shadow[index] = src[x];
            }
        }
        return;
    }

    if (gui_gfx_is_lfb_mode(info) && info->bpp == 32) {
        uint32_t area = (uint32_t)clip.w * (uint32_t)clip.h;

        if (g_page_flip) {
            uint32_t draw_buffer = gfx_page_flip_draw_buffer();

            if (!draw_buffer) {
                g_page_flip = false;
            } else {
                /* El adaptador ve sólo la página anterior mientras se arma
                 * ésta. El cambio de Y offset hace visible el cuadro completo
                 * de forma atómica, eliminando la diferencia arriba/abajo. */
                gui_gfx_copy_rect32(draw_buffer, info, surface, clip);
                gui_gfx_wait_vertical_retrace();
                if (!gfx_page_flip_commit()) {
                    g_page_flip = false;
                } else {
                    /* Sincronice también la página que acaba de quedar oculta.
                     * Así el próximo dirty rect puede ser parcial sin arrastrar
                     * contenido de dos cuadros atrás. */
                    draw_buffer = gfx_page_flip_draw_buffer();
                    if (draw_buffer)
                        gui_gfx_copy_rect32(draw_buffer, info, surface, clip);
                    for (uint16_t y = (uint16_t)clip.y;
                         y < clip.y + clip.h; y++) {
                        uint32_t index = (uint32_t)y * surface->pitch +
                                         (uint32_t)clip.x;
                        kmemcpy(&g_front_shadow[index],
                                &surface->pixels[index],
                                (size_t)clip.w * sizeof(uint32_t));
                    }
                    return;
                }
            }
        }

        if (area >= GUI_GFX_BULK_PRESENT_PIXELS &&
            clip.x == 0 && clip.w == surface->width) {
            /* Prepare el shadow antes de VSync: ninguna comparación ni copia
             * RAM debe consumir la ventana de retrazado. REP MOVSD queda
             * limitado a filas completas: ciertos VBE/BGA presentan mal
             * ráfagas rectangulares grandes que comienzan dentro de la fila,
             * origen de las franjas vacías al abrir ventanas grandes. */
            for (uint16_t y = (uint16_t)clip.y; y < clip.y + clip.h; y++) {
                uint32_t index = (uint32_t)y * surface->pitch +
                                 (uint32_t)clip.x;
                const uint32_t *src = &surface->pixels[index];
                kmemcpy(&g_front_shadow[index], src,
                        (size_t)clip.w * sizeof(uint32_t));
            }
            gui_gfx_wait_vertical_retrace();
            gui_gfx_copy_rect32(info->framebuffer, info, surface, clip);
            return;
        }

        gui_gfx_wait_vertical_retrace();
        for (uint16_t y = (uint16_t)clip.y; y < clip.y + clip.h; y++) {
            volatile uint32_t *dst = (volatile uint32_t *)
                (info->framebuffer + (uint32_t)y * info->pitch);
            const uint32_t *src = &surface->pixels[(uint32_t)y *
                                                  surface->pitch];
            uint16_t end = (uint16_t)(clip.x + clip.w);

            /* Un dirty rect es la union de las posiciones anterior y nueva.
             * En movimientos de cursor o ventana gran parte de esa union no
             * cambia. Además, kmemcpy es byte a byte en el runtime del kernel:
             * escribir aquí DWORDs alineados reduce de cuatro a una las
             * transacciones de VRAM por píxel modificado. */
            for (uint16_t x = (uint16_t)clip.x; x < end; x++) {
                uint32_t index = (uint32_t)y * surface->pitch + x;
                uint32_t rgb = src[x];
                if (g_front_shadow[index] == rgb) continue;
                dst[x] = rgb;
                g_front_shadow[index] = rgb;
            }
        }
        return;
    }

    if (gui_gfx_is_lfb_mode(info) && info->bpp == 24) {
        gui_gfx_wait_vertical_retrace();
        for (uint16_t y = (uint16_t)clip.y; y < clip.y + clip.h; y++) {
            volatile uint8_t *dst = (volatile uint8_t *)
                (info->framebuffer + (uint32_t)y * info->pitch);
            const uint32_t *src = &surface->pixels[(uint32_t)y *
                                                  surface->pitch];
            for (uint16_t x = (uint16_t)clip.x; x < clip.x + clip.w; x++) {
                uint32_t rgb = src[x];
                uint32_t index = (uint32_t)y * surface->pitch + x;
                if (g_front_shadow[index] == rgb) continue;
                dst[x * 3] = (uint8_t)rgb;
                dst[x * 3 + 1] = (uint8_t)(rgb >> 8);
                dst[x * 3 + 2] = (uint8_t)(rgb >> 16);
                g_front_shadow[index] = rgb;
            }
        }
        return;
    }
}


#include "raster.inc"
