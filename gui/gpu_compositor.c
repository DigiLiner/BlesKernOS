#include "gui.h"

#include "../kernel/include/gfx.h"
#include "../kernel/include/gfx3d.h"
#include "../kernel/include/memory.h"
#include "../kernel/include/vga.h"
#include "../kernel/include/vision_ipc.h"

#define GPU_COMP_MAX_WINDOWS 12U
#define GPU_GLYPH_ATLAS_W 128U
#define GPU_GLYPH_ATLAS_H 128U
#define GPU_GLYPH_CELL 8U
#define GPU_CDE_INACTIVE_TITLE 0x00999999U

/* This compositor is deliberately an optional layer.  It is entered only
 * when the active driver exposes the fixed-function SVGA3D path.  QEMU's
 * legacy VMware VGA device therefore keeps the existing CPU compositor. */

typedef struct {
    gui_window_t *window;
    gfx3d_surface_handle_t surface;
    gfx3d_surface_handle_t viewport;
    uint16_t viewport_width;
    uint16_t viewport_height;
    uint16_t width;
    uint16_t height;
    bool seen;
} gpu_window_surface_t;

typedef struct {
    bool ready;
    bool frame_active;
    bool scene_valid;
    uint32_t generation;
    uint16_t width;
    uint16_t height;
    gfx3d_surface_handle_t background;
    gfx3d_surface_handle_t target;
    gfx3d_surface_handle_t overlay;
    gfx3d_surface_handle_t glyph_atlas;
    gfx3d_surface_handle_t cursor;
    gpu_window_surface_t windows[GPU_COMP_MAX_WINDOWS];
    uint32_t *scratch;
    uint32_t scratch_pixels;
    uint32_t *scene;
    uint32_t scene_pixels;
    uint32_t *overlay_pixels;
    uint32_t overlay_capacity;
    uint32_t cursor_pixels[GUI_CURSOR_WIDTH * GUI_CURSOR_HEIGHT];
    uint32_t cursor_hash;
    bool cursor_uploaded;
} gpu_compositor_state_t;

static gpu_compositor_state_t g_gpu;

static uint32_t gpu_required_caps(void) {
    return GFX3D_CAP_FIXED_FUNCTION | GFX3D_CAP_RENDER_TARGETS |
           GFX3D_CAP_PRESENT | GFX3D_CAP_ALPHA_BLEND |
           GFX3D_CAP_TEXTURES | GFX3D_CAP_SCALE |
           GFX3D_CAP_TRANSFORM | GFX3D_CAP_GLYPH_ATLAS |
           GFX3D_CAP_WINDOW_SURFACES;
}

static bool gpu_has_required_caps(void) {
    uint32_t caps = gfx3d_capabilities();
    return (caps & gpu_required_caps()) == gpu_required_caps();
}

static void gpu_destroy_handle(gfx3d_surface_handle_t *handle) {
    if (!handle || !*handle) return;
    (void)gfx3d_surface_destroy(*handle);
    *handle = GFX3D_SURFACE_INVALID;
}

static void gpu_release_resources(void) {
    for (uint32_t i = 0; i < GPU_COMP_MAX_WINDOWS; i++) {
        gpu_destroy_handle(&g_gpu.windows[i].surface);
        gpu_destroy_handle(&g_gpu.windows[i].viewport);
        g_gpu.windows[i].viewport_width = 0U;
        g_gpu.windows[i].viewport_height = 0U;
        g_gpu.windows[i].window = NULL;
        g_gpu.windows[i].width = 0U;
        g_gpu.windows[i].height = 0U;
        g_gpu.windows[i].seen = false;
    }
    gpu_destroy_handle(&g_gpu.background);
    gpu_destroy_handle(&g_gpu.target);
    gpu_destroy_handle(&g_gpu.overlay);
    gpu_destroy_handle(&g_gpu.glyph_atlas);
    gpu_destroy_handle(&g_gpu.cursor);
    if (g_gpu.scratch) kfree(g_gpu.scratch);
    if (g_gpu.scene) kfree(g_gpu.scene);
    if (g_gpu.overlay_pixels) kfree(g_gpu.overlay_pixels);
    g_gpu.scratch = NULL;
    g_gpu.scene = NULL;
    g_gpu.overlay_pixels = NULL;
    g_gpu.scratch_pixels = 0U;
    g_gpu.scene_pixels = 0U;
    g_gpu.overlay_capacity = 0U;
    g_gpu.cursor_hash = 0U;
    g_gpu.cursor_uploaded = false;
    g_gpu.ready = false;
    g_gpu.frame_active = false;
    g_gpu.scene_valid = false;
    g_gpu.width = 0U;
    g_gpu.height = 0U;
}

static bool gpu_ensure_buffer(uint32_t **buffer, uint32_t *capacity,
                              uint32_t pixels) {
    uint32_t *new_buffer;
    if (!buffer || !capacity || !pixels) return false;
    if (*buffer && *capacity >= pixels) return true;
    new_buffer = (uint32_t *)krealloc(*buffer, pixels * sizeof(uint32_t));
    if (!new_buffer) return false;
    /* First publication can initialize the compositor from a user process.
     * Shared desktop caches must outlive that process's heap reclamation. */
#ifndef BLES_VISION_SERVICE
    (void)mm_set_allocation_owner(new_buffer, 0U);
#endif
    *buffer = new_buffer;
    *capacity = pixels;
    return true;
}

static bool gpu_create_surface(uint16_t width, uint16_t height,
                               gfx3d_format_t format, uint32_t flags,
                               gfx3d_surface_handle_t *handle) {
    gfx3d_surface_desc_t desc;
    if (!handle) return false;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.flags = flags;
    return gfx3d_surface_create(&desc, handle);
}

static bool gpu_upload_full(gfx3d_surface_handle_t handle,
                            const uint32_t *pixels,
                            uint16_t width, uint16_t height) {
    gfx_rect_t rect = {0, 0, width, height};
    return handle && pixels &&
           gfx3d_surface_upload(handle, pixels, width, &rect);
}

static bool gpu_build_glyph_atlas(void) {
    const uint32_t pixel_count = GPU_GLYPH_ATLAS_W * GPU_GLYPH_ATLAS_H;
    uint32_t *pixels = (uint32_t *)kmalloc(pixel_count * sizeof(uint32_t));
    uint8_t rows[8];
    bool ok = false;
    if (!pixels) return false;
    kmemset(pixels, 0, pixel_count * sizeof(uint32_t));
    for (uint32_t glyph = 0U; glyph < 256U; glyph++) {
        uint32_t cell_x = (glyph & 15U) * GPU_GLYPH_CELL;
        uint32_t cell_y = (glyph >> 4U) * GPU_GLYPH_CELL;
        if (!gui_font_get_glyph8((uint8_t)glyph, rows)) goto done;
        for (uint32_t y = 0U; y < 8U; y++) {
            for (uint32_t x = 0U; x < 8U; x++) {
                if (rows[y] & (0x80U >> x))
                    pixels[(cell_y + y) * GPU_GLYPH_ATLAS_W + cell_x + x] =
                        0xFFFFFFFFU;
            }
        }
    }
    ok = gpu_upload_full(g_gpu.glyph_atlas, pixels,
                         GPU_GLYPH_ATLAS_W, GPU_GLYPH_ATLAS_H);
done:
    kfree(pixels);
    return ok;
}

static bool gpu_prepare(uint16_t width, uint16_t height) {
    gfx3d_info_t info;
    uint32_t generation;
    uint32_t pixels;
    const char *phase;
    if (!gfx3d_get_info(&info)) return false;
    generation = info.transport_generation;
    pixels = (uint32_t)width * height;
    if (!gpu_has_required_caps() || !width || !height) {
        if (g_gpu.ready) gpu_release_resources();
        return false;
    }
    if (g_gpu.ready && g_gpu.generation == generation &&
        g_gpu.width == width && g_gpu.height == height) return true;

    gpu_release_resources();
    g_gpu.generation = generation;
    g_gpu.width = width;
    g_gpu.height = height;
    phase = "scene-buffer";
    if (!gpu_ensure_buffer(&g_gpu.scene, &g_gpu.scene_pixels, pixels)) goto failed;
    phase = "overlay-buffer";
    if (!gpu_ensure_buffer(&g_gpu.overlay_pixels, &g_gpu.overlay_capacity, pixels)) goto failed;
    phase = "background";
    if (!gpu_create_surface(width, height, GFX3D_FORMAT_XRGB8888,
            GFX3D_SURFACE_TEXTURE | GFX3D_SURFACE_DYNAMIC,
            &g_gpu.background)) goto failed;
    phase = "target";
    if (!gpu_create_surface(width, height, GFX3D_FORMAT_ARGB8888,
            GFX3D_SURFACE_RENDER_TARGET | GFX3D_SURFACE_TEXTURE |
            GFX3D_SURFACE_WINDOW,
            &g_gpu.target)) goto failed;
    phase = "overlay";
    if (!gpu_create_surface(width, height, GFX3D_FORMAT_ARGB8888,
            GFX3D_SURFACE_TEXTURE | GFX3D_SURFACE_DYNAMIC,
            &g_gpu.overlay)) goto failed;
    phase = "glyph-atlas";
    if (!gpu_create_surface(GPU_GLYPH_ATLAS_W, GPU_GLYPH_ATLAS_H,
            GFX3D_FORMAT_ARGB8888, GFX3D_SURFACE_TEXTURE,
            &g_gpu.glyph_atlas)) goto failed;
    phase = "cursor";
    if (!gpu_create_surface(GUI_CURSOR_WIDTH, GUI_CURSOR_HEIGHT,
            GFX3D_FORMAT_ARGB8888,
            GFX3D_SURFACE_TEXTURE | GFX3D_SURFACE_DYNAMIC,
            &g_gpu.cursor)) goto failed;
    phase = "glyph-upload";
    if (!gpu_build_glyph_atlas()) goto failed;
    g_gpu.ready = true;
    return true;
failed:
    {
        static bool reported;
        if (!reported) {
            reported = true;
            kprintf("[GUI:GPU] resource initialization failed phase=%s\n", phase);
        }
    }
    gpu_release_resources();
    return false;
}

bool gui_gpu_compositor_needed(const gui_desktop_t *desktop) {
    const gui_window_t *window;
    if (!desktop) return false;
    for (window = desktop->first_window; window; window = window->next) {
        if (window->visible && window->gpu_view_visible &&
            window->gpu_view_surface) return true;
    }
    return false;
}

/* BLES_GRAPHICS_GUI_PERF_V1: keep normal desktop/window rendering on the 2D backend. */
bool gui_gpu_compositor_enabled(void) {
    const gfx_info_t *info = gfx_get_info();
    if (gui_gfx_output_depth() || !info || info->bpp != 32U || !gfx3d_available()) {
        if (g_gpu.ready) gpu_release_resources();
        return false;
    }
    return gpu_prepare(info->width, info->height);
}

void gui_gpu_compositor_begin_frame(gui_desktop_t *desktop,
                                    const gui_surface_t *surface) {
    /* BLES_GRAPHICS_GUI_PERF_V1: no imported GPU viewport -> no reason to rebuild/upload a
     * complete 3D desktop scene. The regular compositor will present only its
     * dirty rectangles through VirtIO 2D / Rage128 / SVGA. */
    if (!gui_gpu_compositor_needed(desktop)) {
        g_gpu.frame_active = false;
        return;
    }

    uint32_t pixels;
    if (!desktop || !surface || !surface->pixels ||
        !gui_gpu_compositor_needed(desktop) ||
        !gpu_prepare(surface->width, surface->height)) {
        g_gpu.frame_active = false;
        return;
    }
    pixels = (uint32_t)surface->width * surface->height;
    kmemset(g_gpu.scene, 0, pixels * sizeof(uint32_t));
    for (uint32_t i = 0U; i < GPU_COMP_MAX_WINDOWS; i++)
        g_gpu.windows[i].seen = false;
    g_gpu.frame_active = true;
}

void gui_gpu_compositor_capture_background(const gui_surface_t *surface) {
    uint32_t *dst;
    if (!g_gpu.frame_active || !surface || !surface->pixels) return;
    for (uint32_t y = 0U; y < surface->height; y++) {
        dst = g_gpu.scene + y * surface->width;
        for (uint32_t x = 0U; x < surface->width; x++)
            dst[x] = 0xFF000000U |
                surface->pixels[y * surface->pitch + x];
    }
    if (!gpu_upload_full(g_gpu.background, g_gpu.scene,
                         surface->width, surface->height))
        g_gpu.frame_active = false;
}

static gpu_window_surface_t *gpu_find_window_slot(gui_window_t *window) {
    gpu_window_surface_t *free_slot = NULL;
    for (uint32_t i = 0U; i < GPU_COMP_MAX_WINDOWS; i++) {
        if (g_gpu.windows[i].window == window) return &g_gpu.windows[i];
        if (!g_gpu.windows[i].window && !free_slot) free_slot = &g_gpu.windows[i];
    }
    return free_slot;
}

void gui_gpu_compositor_clear_viewport(gui_window_t *window) {
    gpu_window_surface_t *slot = gpu_find_window_slot(window);
    if (!window || !slot || slot->window != window) return;
    gpu_destroy_handle(&slot->viewport);
    slot->viewport_width = slot->viewport_height = 0U;
}

void gui_gpu_compositor_forget_window(gui_window_t *window) {
    gpu_window_surface_t *slot = gpu_find_window_slot(window);
    if (!window || !slot || slot->window != window) return;
    gpu_destroy_handle(&slot->viewport);
    gpu_destroy_handle(&slot->surface);
    kmemset(slot, 0, sizeof(*slot));
}

bool gui_gpu_compositor_publish_viewport(gui_window_t *window,
                                         uint32_t source,
                                         uint16_t width, uint16_t height) {
    gui_desktop_t *desktop = gui_get_desktop();
    gpu_window_surface_t *slot;
    gfx3d_composite_t copy;
    if (!window || !source || !width || !height || !desktop) return false;
    if (!gpu_prepare(desktop->surface.width, desktop->surface.height)) {
        static bool reported;
        if (!reported) {
            reported = true;
            kprintf("[GUI:GPU] prepare failed size=%ux%u caps=%x ready=%u scene=%u overlay=%u\n",
                (uint32_t)desktop->surface.width, (uint32_t)desktop->surface.height,
                gfx3d_capabilities(), g_gpu.ready, g_gpu.scene_pixels, g_gpu.overlay_capacity);
        }
        return false;
    }
    slot = gpu_find_window_slot(window);
    if (!slot) return false;
    if (!slot->viewport || slot->viewport_width != width ||
        slot->viewport_height != height) {
        gpu_destroy_handle(&slot->viewport);
        if (!gpu_create_surface(width, height, GFX3D_FORMAT_ARGB8888,
                GFX3D_SURFACE_TEXTURE | GFX3D_SURFACE_RENDER_TARGET |
                GFX3D_SURFACE_WINDOW, &slot->viewport)) return false;
        slot->viewport_width = width;
        slot->viewport_height = height;
    }
    slot->window = window;
    /* Publish an immutable frame for the compositor. The application may
     * clear/draw its original render target as soon as this call returns.
     * FIFO order protects this GPU copy without a CPU readback or wait. */
    kmemset(&copy, 0, sizeof(copy));
    copy.source = (gfx_rect_t){0, 0, width, height};
    copy.clip = copy.source;
    copy.transform = (gfx3d_transform2d_t){1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    copy.opacity = 255U;
    copy.modulation_color = 0xFFFFFFFFU;
    copy.filter = GFX3D_FILTER_NEAREST;
    /* ONE source factor over transparent black copies RGBA unchanged,
     * including straight-alpha pixels. Later composition blends normally. */
    copy.source_premultiplied = true;
    return gfx3d_surface_clear(slot->viewport, 0U, NULL) &&
           gfx3d_surface_composite(source, slot->viewport, &copy, NULL) &&
           gfx3d_end(slot->viewport, NULL);
}

uint32_t gui_gpu_compositor_native_viewport(gui_window_t *window,uint16_t *width,uint16_t *height) {
    uint32_t w=0,h=0;
    if(!window || bk_vision_native_frame(window,0,&w,&h)<=0)return 0;
    gui_desktop_t *desktop=gui_get_desktop();
    if(!desktop || !gpu_prepare(desktop->surface.width,desktop->surface.height))return 0;
    gpu_window_surface_t *slot=gpu_find_window_slot(window);
    if(!slot)return 0;
    if(!slot->viewport || slot->viewport_width!=w || slot->viewport_height!=h) {
        gpu_destroy_handle(&slot->viewport);
        if(!gpu_create_surface(w,h,GFX3D_FORMAT_ARGB8888,
            GFX3D_SURFACE_TEXTURE|GFX3D_SURFACE_RENDER_TARGET|GFX3D_SURFACE_WINDOW,&slot->viewport))return 0;
        slot->viewport_width=w;slot->viewport_height=h;
    }
    /* Bind ownership even if the producer closes between query and copy;
     * its queued clear can then reclaim this newly allocated cache. */
    slot->window=window;
    if(bk_vision_native_frame(window,slot->viewport,&w,&h)<=0)return 0;
    *width=(uint16_t)w;*height=(uint16_t)h;
    static uint32_t consumed;
    if(++consumed==1U || consumed%600U==0U)
        kprintf("[VISION:GALLIUM] consumed_frames=%u size=%ux%u\n",consumed,w,h);
    return slot->viewport;
}

void gui_gpu_compositor_capture_window(gui_window_t *window,
                                       const gui_surface_t *surface) {
    gpu_window_surface_t *slot;
    uint32_t pixels;
    if (!g_gpu.frame_active || !window || !window->visible ||
        !surface || !surface->pixels || window->bounds.w <= 0 ||
        window->bounds.h <= 0 || window->bounds.w > 65535 ||
        window->bounds.h > 65535) return;
    slot = gpu_find_window_slot(window);
    if (!slot) return;
    if (!slot->surface || slot->width != (uint16_t)window->bounds.w ||
        slot->height != (uint16_t)window->bounds.h) {
        gpu_destroy_handle(&slot->surface);
        slot->width = (uint16_t)window->bounds.w;
        slot->height = (uint16_t)window->bounds.h;
        if (!gpu_create_surface(slot->width, slot->height,
                GFX3D_FORMAT_ARGB8888,
                GFX3D_SURFACE_TEXTURE | GFX3D_SURFACE_DYNAMIC |
                GFX3D_SURFACE_WINDOW, &slot->surface)) {
            slot->window = NULL;
            return;
        }
    }
    slot->window = window;
    slot->seen = true;
    pixels = (uint32_t)slot->width * slot->height;
    if (!gpu_ensure_buffer(&g_gpu.scratch, &g_gpu.scratch_pixels, pixels)) {
        g_gpu.frame_active = false;
        return;
    }
    kmemset(g_gpu.scratch, 0, pixels * sizeof(uint32_t));
    for (uint32_t ly = 0U; ly < slot->height; ly++) {
        int sy = window->bounds.y + (int)ly;
        if (sy < 0 || sy >= surface->height) continue;
        for (uint32_t lx = 0U; lx < slot->width; lx++) {
            int sx = window->bounds.x + (int)lx;
            uint32_t pixel;
            if (sx < 0 || sx >= surface->width) continue;
            pixel = 0xFF000000U |
                surface->pixels[(uint32_t)sy * surface->pitch + (uint32_t)sx];
            g_gpu.scratch[ly * slot->width + lx] = pixel;
            g_gpu.scene[(uint32_t)sy * g_gpu.width + (uint32_t)sx] = pixel;
        }
    }
    if (!gpu_upload_full(slot->surface, g_gpu.scratch,
                         slot->width, slot->height))
        g_gpu.frame_active = false;
}

static bool gpu_composite(gfx3d_surface_handle_t source,
                          gfx3d_surface_handle_t destination,
                          gfx_rect_t source_rect,
                          gfx3d_transform2d_t transform,
                          gfx_rect_t clip, uint8_t opacity,
                          uint32_t modulation, gfx3d_filter_t filter,
                          uint32_t *fence_out) {
    gfx3d_composite_t operation;
    operation.source = source_rect;
    operation.transform = transform;
    operation.clip = clip;
    operation.opacity = opacity;
    operation.filter = filter;
    operation.source_premultiplied = false;
    operation.modulation_color = modulation;
    return gfx3d_surface_composite(source, destination, &operation, fence_out);
}

static uint32_t gpu_cursor_hash(const uint32_t *pixels,
                                uint16_t width, uint16_t height,
                                int16_t hot_x, int16_t hot_y) {
    uint32_t hash = 2166136261U;
    if (!pixels) return 0U;
    for (uint32_t i = 0U;
         i < GUI_CURSOR_WIDTH * GUI_CURSOR_HEIGHT; i++) {
        hash ^= pixels[i];
        hash *= 16777619U;
    }
    hash ^= (uint32_t)width | ((uint32_t)height << 16);
    hash *= 16777619U;
    hash ^= (uint16_t)hot_x | ((uint32_t)(uint16_t)hot_y << 16);
    hash *= 16777619U;
    return hash ? hash : 1U;
}

static bool gpu_draw_cursor(gui_desktop_t *desktop, gfx_rect_t clip) {
    gfx3d_transform2d_t transform = {1.0f, 0.0f, 0.0f,
                                    0.0f, 1.0f, 0.0f};
    gfx_rect_t source;
    uint16_t width = 0U, height = 0U;
    int16_t hot_x = 0, hot_y = 0;
    uint32_t hash;

    if (!desktop || !g_gpu.cursor ||
        !gui_compositor_cursor_argb(desktop, g_gpu.cursor_pixels,
                                    &width, &height, &hot_x, &hot_y) ||
        !width || !height || width > GUI_CURSOR_WIDTH ||
        height > GUI_CURSOR_HEIGHT)
        return false;

    hash = gpu_cursor_hash(g_gpu.cursor_pixels, width, height, hot_x, hot_y);
    if (!g_gpu.cursor_uploaded || g_gpu.cursor_hash != hash) {
        if (!gpu_upload_full(g_gpu.cursor, g_gpu.cursor_pixels,
                             GUI_CURSOR_WIDTH, GUI_CURSOR_HEIGHT))
            return false;
        g_gpu.cursor_hash = hash;
        g_gpu.cursor_uploaded = true;
    }

    source = (gfx_rect_t){0, 0, width, height};
    transform.tx = (float)(desktop->mouse_x - hot_x);
    transform.ty = (float)(desktop->mouse_y - hot_y);
    return gpu_composite(g_gpu.cursor, g_gpu.target, source, transform,
                         clip, 255U, 0xFFFFFFFFU,
                         GFX3D_FILTER_NEAREST, NULL);
}

static bool gpu_draw_text(gfx3d_surface_handle_t destination,
                          int x, int y, const char *text,
                          uint32_t rgb, gfx_rect_t clip) {
    gfx3d_transform2d_t transform = {1.0f, 0.0f, 0.0f,
                                   0.0f, 1.0f, 0.0f};
    while (text && *text) {
        uint8_t c = (uint8_t)*text++;
        gfx_rect_t source = {(c & 15U) * 8, (c >> 4U) * 8, 8, 8};
        transform.tx = (float)x;
        transform.ty = (float)y;
        if (!gpu_composite(g_gpu.glyph_atlas, destination, source,
                           transform, clip, 255U,
                           0xFF000000U | (rgb & 0x00FFFFFFU),
                           GFX3D_FILTER_NEAREST, NULL)) return false;
        x += 7;
    }
    return true;
}

static bool gpu_draw_window_title(gui_window_t *window) {
    gui_rect_t frame;
    gui_rect_t minimize;
    gfx_rect_t clip;
    uint32_t title_bg, title_text;
    int title_left, title_right, title_w, title_x;
    if (!window || window->borderless || !window->visible) return true;
    frame = window->bounds;
    title_bg = window->focused ? window->title_color : GPU_CDE_INACTIVE_TITLE;
    title_text = (((title_bg >> 16) & 0xFFU) +
                  ((title_bg >> 8) & 0xFFU) +
                  (title_bg & 0xFFU)) < 400U ? 0x00FFFFFFU : 0x001A1716U;
    title_left = frame.x + GUI_BORDER_SIZE + 21;
    minimize = gui_window_minimize_button_rect(window);
    title_right = gui_setup_mode()
        ? frame.x + frame.w - GUI_BORDER_SIZE - 4
        : minimize.x - 5;
    if (title_right < title_left) title_right = title_left;
    clip.x = title_left;
    clip.y = frame.y + 5;
    clip.w = title_right - title_left;
    clip.h = GUI_TITLEBAR_HEIGHT - 8;
    title_w = (int)gui_font_text_width(window->title);
    title_x = frame.x + (frame.w - title_w) / 2;
    if (title_x < title_left) title_x = title_left;
    if (title_x + title_w > title_right) title_x = title_right - title_w;
    if (title_x < title_left) title_x = title_left;
    return gpu_draw_text(g_gpu.target, title_x, frame.y + 8,
                         window->title, title_text, clip);
}

static bool gpu_build_overlay(const gui_surface_t *surface) {
    uint32_t pixels;
    if (!surface || !surface->pixels) return false;
    pixels = (uint32_t)surface->width * surface->height;
    for (uint32_t y = 0U; y < surface->height; y++) {
        for (uint32_t x = 0U; x < surface->width; x++) {
            uint32_t final_pixel = 0xFF000000U |
                surface->pixels[y * surface->pitch + x];
            uint32_t scene_pixel = g_gpu.scene[y * surface->width + x];
            g_gpu.overlay_pixels[y * surface->width + x] =
                final_pixel == scene_pixel ? 0U : final_pixel;
        }
    }
    return gpu_upload_full(g_gpu.overlay, g_gpu.overlay_pixels,
                           surface->width, surface->height) && pixels != 0U;
}

/* A native Gallium screensaver owns the *final* scanout.  The general
 * desktop compositor intentionally puts its shell overlay (including the
 * Deskbar) above ordinary application windows.  Reusing that route for
 * fullscreen Jelly makes an opaque CPU placeholder or the Deskbar hide the
 * GPU frame, leaving only intermittent flashes.  Render the already captured
 * GPU viewport as the final layer, without a CPU readback or GUI overlay.
 * Other GL applications continue through the normal layered compositor. */
static gui_window_t *gpu_native_saver_window(gui_desktop_t *desktop) {
    if (!desktop) return NULL;
    for (gui_window_t *w=desktop->first_window; w; w=w->next)
        if (w->native_gpu_saver && w->visible && w->gpu_view_visible &&
            w->gpu_view_surface) return w;
    return NULL;
}

static bool gpu_present_native_saver(gui_desktop_t *desktop) {
    gui_window_t *window=gpu_native_saver_window(desktop);
    gpu_window_surface_t *slot=window?gpu_find_window_slot(window):NULL;
    if (!window || !slot || slot->window!=window || !slot->viewport ||
        !window->gpu_view_width || !window->gpu_view_height) return false;
    gfx_rect_t full={0,0,g_gpu.width,g_gpu.height};
    gfx_rect_t source={0,0,window->gpu_view_width,window->gpu_view_height};
    gfx3d_transform2d_t transform={
        (float)g_gpu.width/window->gpu_view_width,0.0f,0.0f,
        0.0f,(float)g_gpu.height/window->gpu_view_height,0.0f};
    uint32_t fence=0U;
    if (!gfx3d_begin(g_gpu.target,0xFF000000U,0.0f,GFX3D_DRAW_CLEAR_COLOR) ||
        !gpu_composite(slot->viewport,g_gpu.target,source,transform,full,
                       255U,0xFFFFFFFFU,GFX3D_FILTER_LINEAR,NULL) ||
        !gfx3d_end(g_gpu.target,&fence) ||
        !gfx3d_surface_present(g_gpu.target,&full,&fence)) return false;
    g_gpu.scene_valid=true;
    g_gpu.frame_active=false;
    static bool reported;
    if (!reported) {
        kprintf("[VISION:SAVER:GPU] exclusive viewport %ux%u -> %ux%u\n",
            window->gpu_view_width,window->gpu_view_height,g_gpu.width,g_gpu.height);
        reported=true;
    }
    return true;
}

/* Unlike an ordinary GPU window, a screensaver is the only visible scene.
 * Called before the CPU compositor can schedule a conflicting 2D present.
 * Keep the old scanout when a GPU copy is not yet available: uploading a grey
 * placeholder would permanently replace the active VirGL scanout resource. */
bool gui_gpu_compositor_saver_active(const gui_desktop_t *desktop) {
    return gpu_native_saver_window((gui_desktop_t *)desktop) != NULL;
}

bool gui_gpu_compositor_present_saver(gui_desktop_t *desktop) {
    static uint32_t failed,delivered;
    if (!desktop || !gui_gpu_compositor_saver_active(desktop)) return false;
    bool ok=g_gpu.ready && gpu_present_native_saver(desktop);
    if(ok) {
        if(++delivered==1U || delivered%600U==0U)
            kprintf("[VISION:SAVER:GPU] v4 presented=%u exclusive=1\n",delivered);
    } else if(++failed==1U || failed%120U==0U)
        kprintf("[VISION:SAVER:GPU] present unavailable ready=%u active=%u scene=%u count=%u\n",
            g_gpu.ready,g_gpu.frame_active,g_gpu.scene_valid,failed);
    return ok;
}

bool gui_gpu_compositor_present(gui_desktop_t *desktop,
                                const gui_surface_t *final_surface,
                                bool draw_cursor) {
    gfx_rect_t full;
    gfx3d_transform2d_t identity = {1.0f, 0.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f};
    uint32_t fence = 0U;
    if (!g_gpu.frame_active || !desktop || !final_surface ||
        !gui_gpu_compositor_needed(desktop)) return false;
    if (gpu_native_saver_window(desktop))
        return gpu_present_native_saver(desktop);
    if (!gpu_build_overlay(final_surface)) return false;
    full = (gfx_rect_t){0, 0, g_gpu.width, g_gpu.height};
    if (!gfx3d_begin(g_gpu.target, 0xFF000000U, 0.0f,
                     GFX3D_DRAW_CLEAR_COLOR) ||
        !gpu_composite(g_gpu.background, g_gpu.target, full, identity,
                       full, 255U, 0xFFFFFFFFU, GFX3D_FILTER_NEAREST, NULL))
        return false;

    for (gui_window_t *window = desktop->first_window;
         window; window = window->next) {
        gpu_window_surface_t *slot = gpu_find_window_slot(window);
        gfx3d_transform2d_t transform = identity;
        gfx_rect_t source;
        if (!slot || slot->window != window || !slot->seen ||
            !slot->surface || !window->visible) continue;
        source = (gfx_rect_t){0, 0, slot->width, slot->height};
        transform.tx = (float)window->bounds.x;
        transform.ty = (float)window->bounds.y;
        if (!gpu_composite(slot->surface, g_gpu.target, source, transform,
                           full, 255U, 0xFFFFFFFFU,
                           GFX3D_FILTER_NEAREST, NULL) ||
            !gpu_draw_window_title(window)) return false;

        /* The 3D viewport is a separate GPU layer. It is inserted after the
         * window's software client and before later windows, preserving the
         * normal z-order without ever reading its pixels back to the CPU. */
        if (window->gpu_view_visible && slot->viewport) {
            gfx_rect_t viewport_source = {0, 0,
                window->gpu_view_width, window->gpu_view_height};
            gfx_rect_t viewport_clip;
            gfx3d_transform2d_t viewport_transform = identity;
            gui_rect_t clipped;
            gui_rect_t gpu_rect = window->gpu_view_rect;
            if (gui_rect_intersect(window->bounds, gpu_rect, &clipped) &&
                gui_rect_intersect(clipped,
                    (gui_rect_t){0, 0, g_gpu.width, g_gpu.height}, &clipped)) {
                viewport_clip = (gfx_rect_t){clipped.x, clipped.y,
                                             clipped.w, clipped.h};
                viewport_transform.tx = (float)gpu_rect.x;
                viewport_transform.ty = (float)gpu_rect.y;
                viewport_transform.m00 = (float)gpu_rect.w /
                                         window->gpu_view_width;
                viewport_transform.m11 = (float)gpu_rect.h /
                                         window->gpu_view_height;
                if (!gpu_composite(slot->viewport, g_gpu.target,
                                   viewport_source, viewport_transform,
                                   viewport_clip, 255U, 0xFFFFFFFFU,
                                   GFX3D_FILTER_LINEAR, NULL)) return false;
            }
        }
    }
    if (!gpu_composite(g_gpu.overlay, g_gpu.target, full, identity,
                       full, 255U, 0xFFFFFFFFU, GFX3D_FILTER_NEAREST, NULL) ||
        (draw_cursor && !gpu_draw_cursor(desktop, full)) ||
        !gfx3d_end(g_gpu.target, &fence) ||
        !gfx3d_surface_present(g_gpu.target, &full, &fence)) return false;
    /* BLESKERNOS_SVGA3D_PIPELINE_V6: PRESENT is ordered with following
     * SVGA3D commands on the same FIFO. Waiting here stalls the GUI thread
     * and makes the mouse lag behind a busy game. CPU readback/upload paths
     * retain their own synchronization. */
    g_gpu.scene_valid = true;

    for (uint32_t i = 0U; i < GPU_COMP_MAX_WINDOWS; i++) {
        if (g_gpu.windows[i].window && !g_gpu.windows[i].seen) {
            gpu_destroy_handle(&g_gpu.windows[i].surface);
            gpu_destroy_handle(&g_gpu.windows[i].viewport);
            g_gpu.windows[i].viewport_width = 0U;
            g_gpu.windows[i].viewport_height = 0U;
            g_gpu.windows[i].window = NULL;
            g_gpu.windows[i].width = 0U;
            g_gpu.windows[i].height = 0U;
        }
    }
    g_gpu.frame_active = false;
    return true;
}

bool gui_gpu_compositor_present_cached(gui_desktop_t *desktop,
                                       bool draw_cursor) {
    gfx_rect_t full;
    gfx3d_transform2d_t identity = {1.0f, 0.0f, 0.0f,
                                   0.0f, 1.0f, 0.0f};
    uint32_t fence = 0U;

    /* Fast path for a new frame inside an already-attached GPU viewport.
     * Background, CPU window surfaces and overlay are unchanged; only the
     * contents of the application's render target changed.  Reusing those
     * cached layers avoids repainting/uploading the whole 1024x768 desktop
     * for every game frame. */
    if (!desktop || !gui_gpu_compositor_needed(desktop) ||
        !g_gpu.ready || !g_gpu.scene_valid || g_gpu.frame_active) return false;
    if (gpu_native_saver_window(desktop))
        return gpu_present_native_saver(desktop);

    full = (gfx_rect_t){0, 0, g_gpu.width, g_gpu.height};
    if (!gfx3d_begin(g_gpu.target, 0xFF000000U, 0.0f,
                     GFX3D_DRAW_CLEAR_COLOR) ||
        !gpu_composite(g_gpu.background, g_gpu.target, full, identity,
                       full, 255U, 0xFFFFFFFFU,
                       GFX3D_FILTER_NEAREST, NULL))
        return false;

    for (gui_window_t *window = desktop->first_window;
         window; window = window->next) {
        gpu_window_surface_t *slot = gpu_find_window_slot(window);
        gfx3d_transform2d_t transform = identity;
        gfx_rect_t source;
        if (!slot || slot->window != window || !slot->surface ||
            !window->visible) continue;
        source = (gfx_rect_t){0, 0, slot->width, slot->height};
        transform.tx = (float)window->bounds.x;
        transform.ty = (float)window->bounds.y;
        /* The captured window already contains its title. Focus/title
         * changes recapture that layer; ordinary frames need no glyph draws. */
        if (!gpu_composite(slot->surface, g_gpu.target, source, transform,
                           full, 255U, 0xFFFFFFFFU,
                           GFX3D_FILTER_NEAREST, NULL)) return false;

        if (window->gpu_view_visible && slot->viewport) {
            gfx_rect_t viewport_source = {0, 0,
                window->gpu_view_width, window->gpu_view_height};
            gfx_rect_t viewport_clip;
            gfx3d_transform2d_t viewport_transform = identity;
            gui_rect_t clipped;
            gui_rect_t gpu_rect = window->gpu_view_rect;
            if (gui_rect_intersect(window->bounds, gpu_rect, &clipped) &&
                gui_rect_intersect(clipped,
                    (gui_rect_t){0, 0, g_gpu.width, g_gpu.height}, &clipped)) {
                viewport_clip = (gfx_rect_t){clipped.x, clipped.y,
                                             clipped.w, clipped.h};
                viewport_transform.tx = (float)gpu_rect.x;
                viewport_transform.ty = (float)gpu_rect.y;
                viewport_transform.m00 = (float)gpu_rect.w /
                                         window->gpu_view_width;
                viewport_transform.m11 = (float)gpu_rect.h /
                                         window->gpu_view_height;
                if (!gpu_composite(slot->viewport, g_gpu.target,
                                   viewport_source, viewport_transform,
                                   viewport_clip, 255U, 0xFFFFFFFFU,
                                   GFX3D_FILTER_LINEAR, NULL)) return false;
            }
        }
    }

    if (!gpu_composite(g_gpu.overlay, g_gpu.target, full, identity,
                       full, 255U, 0xFFFFFFFFU,
                       GFX3D_FILTER_NEAREST, NULL) ||
        (draw_cursor && !gpu_draw_cursor(desktop, full)) ||
        !gfx3d_end(g_gpu.target, &fence) ||
        !gfx3d_surface_present(g_gpu.target, &full, &fence)) return false;
    /* BLESKERNOS_SVGA3D_PIPELINE_V6: keep cached presentation async. */
    return true;
}

void gui_gpu_compositor_shutdown(void) {
    gpu_release_resources();
}
