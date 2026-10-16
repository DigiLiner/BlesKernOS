#include "gui.h"
#include "../sdk/include/bleskernos_input.h"

#include "../kernel/include/gfx.h"
#include "../kernel/include/perfmon.h"
#include "../kernel/include/vfs.h"
#include "../kernel/include/memory.h"
#include "../kernel/include/graphics_resources.h"
#include "../kernel/include/vga.h"
#include "../kernel/libc/include/stdio.h"
#include "image.h"
#include "../kernel/include/pit.h"
#define GUI_DEFAULT_CURSOR_WIDTH  17
#define GUI_DEFAULT_CURSOR_HEIGHT 17

static const char *g_arrow_cursor[GUI_DEFAULT_CURSOR_HEIGHT] = {
    "XX                              ",
    "XoX                             ",
    "X.oX                            ",
    "X..oX                           ",
    "X...oX                          ",
    "X....oX                         ",
    "X.....oX                        ",
    "X......oX                       ",
    "X.......oX                      ",
    "X........oX                     ",
    "X.....oXXXX                     ",
    "X..oo.oX                        ",
    "X.oXX..X                        ",
    "XoX Xo.oX                       ",
    "XX   X..X                       ",
    "     XooX                       ",
    "      XX                        "
};

typedef struct {
    uint32_t pixels[GUI_CURSOR_WIDTH * GUI_CURSOR_HEIGHT];
    uint16_t width, height;
    int16_t hot_x, hot_y;
    bool loaded;
    uint8_t *animation;
    uint32_t animation_size, cycle_ms;
    uint16_t frame_count, frame_index;
} system_cursor_t;

/* Cache every stock cursor independently.  The old single-entry cache caused
 * a real VFS read each time the pointer crossed a resize border
 * (arrow->resize->arrow), which is very visible on old IDE disks. */
static system_cursor_t *g_system_cursors[GUI_CURSOR_COUNT];
static bool g_system_cursor_attempted[GUI_CURSOR_COUNT];

static const char *compositor_system_cursor_path(uint32_t index) {
    switch (index) {
        case GUI_CURSOR_ARROW: return "/SYSTEM/CURSORS/ARROW.CUR";
        case GUI_CURSOR_WAIT: return "/SYSTEM/CURSORS/WAIT.CUR";
        case GUI_CURSOR_SIZE_WE: return "/SYSTEM/CURSORS/HRESIZE.CUR";
        case GUI_CURSOR_SIZE_NS: return "/SYSTEM/CURSORS/VRESIZE.CUR";
        case GUI_CURSOR_SIZE_NWSE: return "/SYSTEM/CURSORS/DRESIZE1.CUR";
        case GUI_CURSOR_SIZE_NESW: return "/SYSTEM/CURSORS/DRESIZE2.CUR";
        case GUI_CURSOR_PROGRESS: return "/SYSTEM/CURSORS/PROGRESS.CUR";
        case GUI_CURSOR_TEXT: return "/SYSTEM/CURSORS/TEXT.CUR";
        case GUI_CURSOR_POINTER: return "/SYSTEM/CURSORS/POINTER.CUR";
        case GUI_CURSOR_HELP: return "/SYSTEM/CURSORS/HELP.CUR";
        case GUI_CURSOR_MOVE: return "/SYSTEM/CURSORS/FLEUR.CUR";
        case GUI_CURSOR_NOT_ALLOWED: return "/SYSTEM/CURSORS/NOT-ALLOWED.CUR";
        case GUI_CURSOR_CROSSHAIR: return "/SYSTEM/CURSORS/CROSSHAIR.CUR";
        case GUI_CURSOR_ALIAS: return "/SYSTEM/CURSORS/ALIAS.CUR";
        case GUI_CURSOR_ALL_SCROLL: return "/SYSTEM/CURSORS/ALL-SCROLL.CUR";
        case GUI_CURSOR_BOTTOM_LEFT_CORNER: return "/SYSTEM/CURSORS/BOTTOM_LEFT_CORNER.CUR";
        case GUI_CURSOR_BOTTOM_RIGHT_CORNER: return "/SYSTEM/CURSORS/BOTTOM_RIGHT_CORNER.CUR";
        case GUI_CURSOR_BOTTOM_SIDE: return "/SYSTEM/CURSORS/BOTTOM_SIDE.CUR";
        case GUI_CURSOR_CELL: return "/SYSTEM/CURSORS/CELL.CUR";
        case GUI_CURSOR_CENTER_PTR: return "/SYSTEM/CURSORS/CENTER_PTR.CUR";
        case GUI_CURSOR_CLOSEDHAND: return "/SYSTEM/CURSORS/CLOSEDHAND.CUR";
        case GUI_CURSOR_COL_RESIZE: return "/SYSTEM/CURSORS/COL-RESIZE.CUR";
        case GUI_CURSOR_COLOR_PICKER: return "/SYSTEM/CURSORS/COLOR-PICKER.CUR";
        case GUI_CURSOR_CONTEXT_MENU: return "/SYSTEM/CURSORS/CONTEXT-MENU.CUR";
        case GUI_CURSOR_COPY: return "/SYSTEM/CURSORS/COPY.CUR";
        case GUI_CURSOR_DND_MOVE: return "/SYSTEM/CURSORS/DND-MOVE.CUR";
        case GUI_CURSOR_DND_NO_DROP: return "/SYSTEM/CURSORS/DND-NO-DROP.CUR";
        case GUI_CURSOR_DOWN_ARROW: return "/SYSTEM/CURSORS/DOWN-ARROW.CUR";
        case GUI_CURSOR_DRAFT: return "/SYSTEM/CURSORS/DRAFT.CUR";
        case GUI_CURSOR_LEFT_ARROW: return "/SYSTEM/CURSORS/LEFT-ARROW.CUR";
        case GUI_CURSOR_LEFT_SIDE: return "/SYSTEM/CURSORS/LEFT_SIDE.CUR";
        case GUI_CURSOR_NO_DROP: return "/SYSTEM/CURSORS/NO-DROP.CUR";
        case GUI_CURSOR_OPENHAND: return "/SYSTEM/CURSORS/OPENHAND.CUR";
        case GUI_CURSOR_PENCIL: return "/SYSTEM/CURSORS/PENCIL.CUR";
        case GUI_CURSOR_PIRATE: return "/SYSTEM/CURSORS/PIRATE.CUR";
        case GUI_CURSOR_RIGHT_ARROW: return "/SYSTEM/CURSORS/RIGHT-ARROW.CUR";
        case GUI_CURSOR_RIGHT_PTR: return "/SYSTEM/CURSORS/RIGHT_PTR.CUR";
        case GUI_CURSOR_RIGHT_SIDE: return "/SYSTEM/CURSORS/RIGHT_SIDE.CUR";
        case GUI_CURSOR_ROW_RESIZE: return "/SYSTEM/CURSORS/ROW-RESIZE.CUR";
        case GUI_CURSOR_TOP_LEFT_CORNER: return "/SYSTEM/CURSORS/TOP_LEFT_CORNER.CUR";
        case GUI_CURSOR_TOP_RIGHT_CORNER: return "/SYSTEM/CURSORS/TOP_RIGHT_CORNER.CUR";
        case GUI_CURSOR_TOP_SIDE: return "/SYSTEM/CURSORS/TOP_SIDE.CUR";
        case GUI_CURSOR_UP_ARROW: return "/SYSTEM/CURSORS/UP-ARROW.CUR";
        case GUI_CURSOR_VERTICAL_TEXT: return "/SYSTEM/CURSORS/VERTICAL-TEXT.CUR";
        case GUI_CURSOR_WAYLAND_CURSOR: return "/SYSTEM/CURSORS/WAYLAND-CURSOR.CUR";
        case GUI_CURSOR_X_CURSOR: return "/SYSTEM/CURSORS/X-CURSOR.CUR";
        case GUI_CURSOR_ZOOM_IN: return "/SYSTEM/CURSORS/ZOOM-IN.CUR";
        case GUI_CURSOR_ZOOM_OUT: return "/SYSTEM/CURSORS/ZOOM-OUT.CUR";
        default: return NULL;
    }
}

static uint16_t cur_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t cur_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool compositor_decode_cursor(system_cursor_t *cursor,
                                     const uint8_t *file, uint32_t size) {
    const uint8_t *entry;
    const uint8_t *dib;
    const uint8_t *palette;
    const uint8_t *xor_bits;
    const uint8_t *and_bits;
    uint32_t offset, dib_size, palette_count, xor_stride, and_stride;
    uint16_t width, height, bpp;

    if (!cursor || !file || size < 22U || cur_le16(file + 2) != 2U ||
        !cur_le16(file + 4)) return false;
    entry = file + 6;
    offset = cur_le32(entry + 12);
    if (offset > size || size - offset < 40U) return false;
    dib = file + offset;
    dib_size = cur_le32(dib);
    width = (uint16_t)cur_le32(dib + 4);
    height = (uint16_t)(cur_le32(dib + 8) / 2U);
    bpp = cur_le16(dib + 14);
    if (dib_size < 40U || dib_size > size - offset || width == 0U || height == 0U ||
        width > GUI_CURSOR_WIDTH || height > GUI_CURSOR_HEIGHT ||
        (bpp != 1U && bpp != 4U && bpp != 8U && bpp != 32U)) return false;
    palette_count = bpp <= 8U ? cur_le32(dib + 32) : 0U;
    if (bpp <= 8U && !palette_count) palette_count = 1U << bpp;
    if (palette_count > 256U || palette_count * 4U > size - offset - dib_size)
        return false;
    palette = dib + dib_size;
    xor_stride = ((uint32_t)width * bpp + 31U) / 32U * 4U;
    and_stride = ((uint32_t)width + 31U) / 32U * 4U;
    if (xor_stride * height + and_stride * height >
        size - offset - dib_size - palette_count * 4U) return false;
    xor_bits = palette + palette_count * 4U;
    and_bits = xor_bits + xor_stride * height;
    if (xor_bits > file + size || and_bits > file + size ||
        (uint32_t)(file + size - and_bits) < and_stride * height) return false;
    for (uint16_t y = 0U; y < height; y++) {
        const uint8_t *xor_row = xor_bits + (uint32_t)(height - 1U - y) * xor_stride;
        const uint8_t *and_row = and_bits + (uint32_t)(height - 1U - y) * and_stride;
        for (uint16_t x = 0U; x < width; x++) {
            uint32_t rgb, alpha = 0xFFU;
            if (bpp == 32U) {
                const uint8_t *px = xor_row + x * 4U;
                rgb = (uint32_t)px[2] << 16 | (uint32_t)px[1] << 8 | px[0];
                if (px[3]) alpha = px[3];
            } else {
                uint32_t index = bpp == 8U ? xor_row[x] :
                    (bpp == 4U ? ((x & 1U) ? (xor_row[x / 2U] & 15U) :
                                              (xor_row[x / 2U] >> 4)) :
                     ((xor_row[x / 8U] >> (7U - (x & 7U))) & 1U));
                const uint8_t *px;
                if (index >= palette_count) return false;
                px = palette + index * 4U;
                rgb = (uint32_t)px[2] << 16 | (uint32_t)px[1] << 8 | px[0];
            }
            if (and_row[x / 8U] & (uint8_t)(0x80U >> (x & 7U))) alpha = 0U;
            cursor->pixels[(uint32_t)y * GUI_CURSOR_WIDTH + x] =
                (alpha << 24) | rgb;
        }
    }
    cursor->width = width;
    cursor->height = height;
    cursor->hot_x = (int16_t)cur_le16(entry + 4);
    cursor->hot_y = (int16_t)cur_le16(entry + 6);
    cursor->loaded = cursor->hot_x >= 0 && cursor->hot_y >= 0 &&
        cursor->hot_x < width && cursor->hot_y < height;
    return cursor->loaded;
}

/* BKCA keeps indexed CUR frames in one OS-owned allocation. Only the active
 * frame is expanded to ARGB; no allocations or disk reads on animation ticks. */
static bool compositor_decode_animation(system_cursor_t *cursor,
                                         uint8_t *file, uint32_t size) {
    uint32_t cycle = 0U;
    uint16_t count;
    if (size < 8U || cur_le32(file) != 0x41434B42U ||
        cur_le16(file + 4) != 1U) return false;
    count = cur_le16(file + 6);
    if (!count || count > 64U || size < 8U + count * 12U) return false;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *entry = file + 8U + i * 12U;
        uint32_t delay = cur_le32(entry), offset = cur_le32(entry + 4);
        uint32_t length = cur_le32(entry + 8);
        if (!delay || delay > 10000U || offset < 8U + count * 12U ||
            offset > size || length > size - offset ||
            !compositor_decode_cursor(cursor, file + offset, length)) return false;
        cycle += delay;
    }
    cursor->animation = file;
    cursor->animation_size = size;
    cursor->cycle_ms = cycle;
    cursor->frame_count = count;
    cursor->frame_index = 0U;
    return compositor_decode_cursor(cursor, file + cur_le32(file + 12),
                                     cur_le32(file + 16));
}

static uint16_t compositor_cursor_frame(const system_cursor_t *cursor,
                                        uint32_t now_tick) {
    uint32_t hz = pit_get_frequency_hz();
    uint32_t phase;
    if (!cursor->animation || !hz || !cursor->cycle_ms) return 0U;
    phase = (uint32_t)(((uint64_t)now_tick * 1000U / hz) % cursor->cycle_ms);
    for (uint16_t i = 0U; i < cursor->frame_count; i++) {
        uint32_t delay = cur_le32(cursor->animation + 8U + i * 12U);
        if (phase < delay) return i;
        phase -= delay;
    }
    return 0U;
}

static const system_cursor_t *compositor_system_cursor(gui_cursor_style_t style) {
    system_cursor_t *cursor;
    const char *path;
    void *file = NULL;
    uint32_t size = 0U;
    if (style >= GUI_CURSOR_COUNT) style = GUI_CURSOR_ARROW;
    if (g_system_cursor_attempted[(uint32_t)style])
        return g_system_cursors[(uint32_t)style];
    g_system_cursor_attempted[(uint32_t)style] = true;
    cursor = (system_cursor_t *)kzalloc(sizeof(*cursor));
    if (!cursor) return NULL;
    path = compositor_system_cursor_path((uint32_t)style);
    const char *animation_path = style == GUI_CURSOR_WAIT ? "/SYSTEM/CURSORS/WAIT.BCA" :
        (style == GUI_CURSOR_PROGRESS ? "/SYSTEM/CURSORS/PROGRESS.BCA" : NULL);
    if (animation_path && vfs_read_all(animation_path, &file, &size)) {
        if (!compositor_decode_animation(cursor, file, size)) {
            kfree(file); file = NULL;
            cursor->animation = NULL;
        }
    }
    if (!cursor->animation) {
        if (!path || !vfs_read_all(path, &file, &size)) { kfree(cursor); return NULL; }
        if (!compositor_decode_cursor(cursor, file, size)) {
            kfree(file); kfree(cursor); return NULL;
        }
        kfree(file);
    }
#ifndef BLES_VISION_SERVICE
    if (!mm_set_allocation_owner(cursor, 0U) ||
        (cursor->animation && !mm_set_allocation_owner(cursor->animation, 0U))) {
        kfree(cursor->animation); kfree(cursor); return NULL;
    }
#endif
    g_system_cursors[(uint32_t)style] = cursor;
    return cursor;
}

static bool g_cursor_busy_was_active;

static bool compositor_busy(const gui_desktop_t *desktop) {
    return desktop && __atomic_load_n(&desktop->cursor_busy_count, __ATOMIC_RELAXED);
}

static bool compositor_custom_cursor(const gui_desktop_t *desktop) {
    return desktop && desktop->cursor_custom && !compositor_busy(desktop);
}

void gui_compositor_busy_begin(void) {
    gui_desktop_t *desktop = gui_get_desktop();
    if (!desktop) return;
    __atomic_add_fetch(&desktop->cursor_busy_count, 1U, __ATOMIC_RELAXED);
    gui_request_paint();
}

void gui_compositor_busy_end(void) {
    gui_desktop_t *desktop = gui_get_desktop();
    if (!desktop) return;
    uint32_t count = __atomic_load_n(&desktop->cursor_busy_count, __ATOMIC_RELAXED);
    while (count && !__atomic_compare_exchange_n(&desktop->cursor_busy_count,
            &count, count - 1U, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
    gui_request_paint();
}

static gui_cursor_style_t compositor_cursor_style(const gui_desktop_t *desktop) {
    if (compositor_busy(desktop)) return GUI_CURSOR_PROGRESS;
    return desktop ? desktop->cursor_style : GUI_CURSOR_ARROW;
}

bool gui_compositor_tick_cursor(gui_desktop_t *desktop, uint32_t now_tick) {
    bool busy = compositor_busy(desktop);
    bool changed = busy != g_cursor_busy_was_active;
    g_cursor_busy_was_active = busy;
    if (!desktop || compositor_custom_cursor(desktop)) return false;
    system_cursor_t *cursor = (system_cursor_t *)compositor_system_cursor(
        compositor_cursor_style(desktop));
    if (cursor && cursor->animation) {
        uint16_t frame = compositor_cursor_frame(cursor, now_tick);
        if (frame != cursor->frame_index) {
            const uint8_t *entry = cursor->animation + 8U + frame * 12U;
            compositor_decode_cursor(cursor, cursor->animation + cur_le32(entry + 4),
                                     cur_le32(entry + 8));
            cursor->frame_index = frame;
            changed = true;
        }
    }
    if (changed) {
        /* Include the previous hotspot and trail, without repainting the desktop. */
        gui_desktop_invalidate_rect(desktop, (gui_rect_t){
            desktop->mouse_x - GUI_CURSOR_WIDTH, desktop->mouse_y - GUI_CURSOR_HEIGHT,
            GUI_CURSOR_WIDTH * 2, GUI_CURSOR_HEIGHT * 2});
    }
    return changed;
}


/* BLESKERNOS_SVGA_HW_CURSOR_HELPER_BEGIN */
static bool g_hardware_cursor_ready;
static uint32_t g_hardware_cursor_generation;

/* BLESKERNOS_SVGA3D_PIPELINE_V6
 * While a direct GPU viewport is visible VMware uses a GPU-composited
 * software cursor. Track the last cursor position that actually reached the
 * scanout so mouse motion can request a cached GPU present without turning
 * every unrelated GUI paint into a full-screen 3D submission. */
static bool g_gpu_cursor_present_valid;
static int g_gpu_cursor_present_x;
static int g_gpu_cursor_present_y;

static bool compositor_prepare_hardware_cursor(
    const gui_desktop_t *desktop) {
    static uint32_t pixels[GUI_CURSOR_WIDTH * GUI_CURSOR_HEIGHT];
    const gfx_info_t *info = gfx_get_info();
    uint32_t generation = gfx_driver_generation();

    /* VMware's legacy cursor path has shown XOR artifacts, so keep it on the
     * software cursor. Rage128 and VirtIO GPU both expose native ARGB cursor
     * planes that are safe for the stock arrow. Moving those cursors must not
     * invalidate/present a desktop rectangle for every PS/2 packet. */
    if (bk_input_preference_get(BK_INPUT_CURSOR_SCHEME) || !desktop || compositor_custom_cursor(desktop) ||
        compositor_cursor_style(desktop) != GUI_CURSOR_ARROW || !info ||
        (info->mode != GFX_MODE_ATI_RAGE128 &&
         info->mode != GFX_MODE_VIRTIO_GPU)) {
        return false;
    }

    if (!gfx_cursor_supported()) {
        g_hardware_cursor_ready = false;
        g_hardware_cursor_generation = 0U;
        return false;
    }
    if (g_hardware_cursor_ready &&
        g_hardware_cursor_generation == generation) return true;

    g_hardware_cursor_ready = false;
    for (uint32_t i = 0U; i < GUI_CURSOR_WIDTH * GUI_CURSOR_HEIGHT; i++)
        pixels[i] = 0U;
    const system_cursor_t *stock = compositor_system_cursor(GUI_CURSOR_ARROW);
    if (!stock) return false;
    kmemcpy(pixels, stock->pixels, sizeof(pixels));
    g_hardware_cursor_ready = gfx_cursor_define(
        pixels, GUI_CURSOR_WIDTH, GUI_CURSOR_HEIGHT, stock->hot_x, stock->hot_y);
    if (g_hardware_cursor_ready) {
        g_hardware_cursor_generation = generation;
        /* Definir el bitmap no garantiza que UPDATE_CURSOR haya llegado al
           dispositivo. Si falla, conservar el cursor software en vez de
           ocultarlo durante toda la sesion. */
        if (!gfx_cursor_show(true)) {
            g_hardware_cursor_ready = false;
            g_hardware_cursor_generation = 0U;
        }
    } else {
        g_hardware_cursor_generation = 0U;
    }
    return g_hardware_cursor_ready;
}
/* BLESKERNOS_SVGA_HW_CURSOR_HELPER_END */

bool gui_compositor_move_hardware_cursor(gui_desktop_t *desktop) {
    const gfx_info_t *info = gfx_get_info();

    /* Do not initialize/transition the cursor here: those paths need one real
     * compositor frame to restore any previous software backing. This helper
     * is the hot path once Rage128/VirtIO's native arrow is live. */
    if (!desktop || desktop->cursor_trail_enabled || compositor_custom_cursor(desktop) ||
        compositor_cursor_style(desktop) != GUI_CURSOR_ARROW || !info ||
        (info->mode != GFX_MODE_ATI_RAGE128 &&
         info->mode != GFX_MODE_VIRTIO_GPU) ||
        !g_hardware_cursor_ready ||
        g_hardware_cursor_generation != gfx_driver_generation()) {
        return false;
    }

    if (!gfx_cursor_move(desktop->mouse_x, desktop->mouse_y) ||
        !gfx_cursor_show(true)) {
        /* Fall back to the normal compositor on the same event if the device
         * stops accepting cursor commands. */
        g_hardware_cursor_ready = false;
        g_hardware_cursor_generation = 0U;
        return false;
    }
    return true;
}

static gui_rect_t cursor_rect(const gui_desktop_t *desktop, int x, int y) {
    bool custom = desktop && compositor_custom_cursor(desktop);
    const system_cursor_t *system = custom ? NULL : compositor_system_cursor(
        compositor_cursor_style(desktop));
    int hot_x = custom ? desktop->cursor_hotspot_x :
                (system ? system->hot_x : 0);
    int hot_y = custom ? desktop->cursor_hotspot_y :
                (system ? system->hot_y : 0);
    int width = custom ? desktop->cursor_width :
                (system ? system->width : GUI_DEFAULT_CURSOR_WIDTH);
    int height = custom ? desktop->cursor_height :
                 (system ? system->height : GUI_DEFAULT_CURSOR_HEIGHT);
    return (gui_rect_t){x - hot_x, y - hot_y, width, height};
}

static uint8_t compositor_planned_cursor_rects(const gui_desktop_t *desktop,
                                               gui_rect_t *rects,
                                               uint8_t max_rects) {
    uint8_t count = 0;

    if (!desktop || !rects || !max_rects) return 0;
    if (desktop->cursor_trail_enabled) {
        for (uint8_t i = 0;
             i < desktop->cursor_trail_count && count < max_rects - 1;
             i++) {
            rects[count++] = cursor_rect(desktop, desktop->cursor_trail_x[i],
                                         desktop->cursor_trail_y[i]);
        }
    }
    rects[count++] = cursor_rect(desktop, desktop->mouse_x, desktop->mouse_y);
    return count;
}

static void dirty_union(gui_rect_t *dirty, bool *valid, gui_rect_t rect) {
    if (!dirty || !valid || rect.w <= 0 || rect.h <= 0) return;
    if (*valid) *dirty = gui_rect_union(*dirty, rect);
    else {
        *dirty = rect;
        *valid = true;
    }
}

static bool compositor_content_dirty_rect(gui_desktop_t *desktop,
                                           gui_rect_t *out) {
    gui_window_t *window;
    gui_rect_t screen;
    gui_rect_t dirty = {0, 0, 0, 0};
    gui_rect_t clipped;
    bool valid = false;

    if (!desktop || !out) return false;
    screen = (gui_rect_t){0, 0, desktop->surface.width,
                          desktop->surface.height};
    if (!desktop->paint_valid) {
        *out = screen;
        return true;
    }

    if (desktop->dirty_valid)
        dirty_union(&dirty, &valid, desktop->dirty_rect);

    window = desktop->first_window;
    while (window) {
        if (window->dirty) {
            if (window->paint_bounds_valid)
                dirty_union(&dirty, &valid, window->paint_bounds);
            if (window->visible)
                dirty_union(&dirty, &valid, window->bounds);
        }
        window = window->next;
    }

    if (!valid) return false;
    if (!gui_rect_intersect(screen, dirty, &clipped)) return false;
    *out = clipped;
    return true;
}

static void compositor_finish_windows(gui_desktop_t *desktop) {
    gui_window_t *window;

    if (!desktop) return;
    window = desktop->first_window;
    while (window) {
        if (!window->content_repaint) window->dirty = false;
        if (window->visible) {
            window->paint_bounds = window->bounds;
            window->paint_bounds_valid = true;
        } else {
            window->paint_bounds_valid = false;
        }
        window = window->next;
    }
}

static void paint_cursor_with_palette(gui_surface_t *surface, int x, int y,
                                      uint32_t dark, uint32_t mid,
                                      uint32_t light, gui_cursor_style_t style) {
    if (style == GUI_CURSOR_WAIT) {
        gui_gfx_draw_line(surface, x + 2, y + 1, x + 14, y + 1, dark);
        gui_gfx_draw_line(surface, x + 2, y + 15, x + 14, y + 15, dark);
        gui_gfx_draw_line(surface, x + 2, y + 1, x + 14, y + 15, dark);
        gui_gfx_draw_line(surface, x + 14, y + 1, x + 2, y + 15, dark);
        gui_gfx_fill_rect(surface, (gui_rect_t){x + 6, y + 5, 5, 6}, light);
        gui_gfx_putpixel(surface, x + 8, y + 3, mid);
        gui_gfx_putpixel(surface, x + 8, y + 12, mid);
        return;
    }
    if (style >= GUI_CURSOR_SIZE_WE && style <= GUI_CURSOR_SIZE_NESW) {
        int x0 = x + 2, y0 = y + 2, x1 = x + 14, y1 = y + 14;
        if (style == GUI_CURSOR_SIZE_WE) {
            gui_gfx_draw_line(surface, x0, y + 8, x1, y + 8, dark);
            gui_gfx_draw_line(surface, x0, y + 8, x0 + 4, y + 4, dark);
            gui_gfx_draw_line(surface, x0, y + 8, x0 + 4, y + 12, dark);
            gui_gfx_draw_line(surface, x1, y + 8, x1 - 4, y + 4, dark);
            gui_gfx_draw_line(surface, x1, y + 8, x1 - 4, y + 12, dark);
        } else if (style == GUI_CURSOR_SIZE_NS) {
            gui_gfx_draw_line(surface, x + 8, y0, x + 8, y1, dark);
            gui_gfx_draw_line(surface, x + 8, y0, x + 4, y0 + 4, dark);
            gui_gfx_draw_line(surface, x + 8, y0, x + 12, y0 + 4, dark);
            gui_gfx_draw_line(surface, x + 8, y1, x + 4, y1 - 4, dark);
            gui_gfx_draw_line(surface, x + 8, y1, x + 12, y1 - 4, dark);
        } else if (style == GUI_CURSOR_SIZE_NWSE) {
            gui_gfx_draw_line(surface, x0, y0, x1, y1, dark);
            gui_gfx_draw_line(surface, x0, y0, x0 + 5, y0, dark);
            gui_gfx_draw_line(surface, x0, y0, x0, y0 + 5, dark);
            gui_gfx_draw_line(surface, x1, y1, x1 - 5, y1, dark);
            gui_gfx_draw_line(surface, x1, y1, x1, y1 - 5, dark);
        } else {
            gui_gfx_draw_line(surface, x0, y1, x1, y0, dark);
            gui_gfx_draw_line(surface, x0, y1, x0 + 5, y1, dark);
            gui_gfx_draw_line(surface, x0, y1, x0, y1 - 5, dark);
            gui_gfx_draw_line(surface, x1, y0, x1 - 5, y0, dark);
            gui_gfx_draw_line(surface, x1, y0, x1, y0 + 5, dark);
        }
        return;
    }
    for (int row = 0; row < GUI_DEFAULT_CURSOR_HEIGHT; row++) {
        for (int col = 0; col < GUI_DEFAULT_CURSOR_WIDTH; col++) {
            char px = g_arrow_cursor[row][col];
            uint32_t color;

            /* Only the three glyph symbols are opaque.  Treat NUL and
             * every other byte as transparent too: older/stale objects could
             * contain short rows after the visible 17-pixel arrow and the
             * previous catch-all branch painted that tail as a 32x16 white
             * rectangle attached to the pointer. */
            if (px != 'X' && px != 'o' && px != '.') continue;
            if (px == 'X') color = dark;
            else if (px == 'o') color = mid;
            else color = light;

            gui_gfx_putpixel(surface, x + col, y + row, color);
        }
    }
}

static uint32_t cursor_alpha_blend(uint32_t destination, uint32_t source) {
    uint32_t alpha = source >> 24;
    uint32_t inverse;
    uint32_t rb;
    uint32_t g;
    if (!alpha) return destination;
    if (alpha >= 255U) return source & 0x00FFFFFFU;
    inverse = 255U - alpha;
    rb = ((((source & 0x00FF00FFU) * alpha) +
           ((destination & 0x00FF00FFU) * inverse)) >> 8) & 0x00FF00FFU;
    g = ((((source & 0x0000FF00U) * alpha) +
          ((destination & 0x0000FF00U) * inverse)) >> 8) & 0x0000FF00U;
    return rb | g;
}

static uint32_t compositor_stock_cursor_color(uint32_t color) {
    uint32_t scheme=bk_input_preference_get(BK_INPUT_CURSOR_SCHEME);
    uint32_t rgb=color&0xFFFFFFU,alpha=color&0xFF000000U;
    if (!scheme || !alpha) return color;
    if (scheme==1U) return alpha | (rgb^0xFFFFFFU);
    return alpha | (rgb==0U ? 0U : 0x004B93A8U);
}

static void paint_cursor(gui_desktop_t *desktop, gui_surface_t *surface,
                         int x, int y) {
    int origin_x;
    int origin_y;
    if (!desktop || !compositor_custom_cursor(desktop) || !desktop->cursor_width ||
        !desktop->cursor_height) {
        const system_cursor_t *system = compositor_system_cursor(
            compositor_cursor_style(desktop));
        if (system) {
            origin_x = x - system->hot_x;
            origin_y = y - system->hot_y;
            for (int row = 0; row < system->height; row++) {
                for (int col = 0; col < system->width; col++) {
                    uint32_t source = system->pixels[(uint32_t)row *
                                                     GUI_CURSOR_WIDTH + col];
                    source=compositor_stock_cursor_color(source);
                    int px = origin_x + col, py = origin_y + row;
                    if (!(source >> 24) || px < 0 || py < 0 ||
                        px >= surface->width || py >= surface->height) continue;
                    gui_gfx_putpixel(surface, px, py, cursor_alpha_blend(
                        surface->pixels[(uint32_t)py * surface->pitch + px], source));
                }
            }
            return;
        }
        paint_cursor_with_palette(surface, x, y,
                                  compositor_stock_cursor_color(0xFF000000U)&0xFFFFFFU,
                                  compositor_stock_cursor_color(0xFF808080U)&0xFFFFFFU,
                                  compositor_stock_cursor_color(0xFFFFFFFFU)&0xFFFFFFU,
                                  compositor_cursor_style(desktop));
        return;
    }
    origin_x = x - desktop->cursor_hotspot_x;
    origin_y = y - desktop->cursor_hotspot_y;
    for (int row = 0; row < desktop->cursor_height; row++) {
        int py = origin_y + row;
        if (py < 0 || py >= surface->height) continue;
        for (int col = 0; col < desktop->cursor_width; col++) {
            int px = origin_x + col;
            uint32_t source;
            uint32_t destination;
            if (px < 0 || px >= surface->width) continue;
            source = desktop->cursor_pixels[row * GUI_CURSOR_WIDTH + col];
            if (!(source >> 24)) continue;
            destination = surface->pixels[(uint32_t)py * surface->pitch +
                                          (uint32_t)px];
            gui_gfx_putpixel(surface, px, py,
                             cursor_alpha_blend(destination, source));
        }
    }
}

/* Exporta el bitmap ARGB real del cursor para que el compositor 3D pueda
 * mantenerlo como una capa GPU.  Esto evita leer de vuelta un viewport 3D
 * solo para restaurar los pixeles que quedan debajo del cursor software. */
bool gui_compositor_cursor_argb(const gui_desktop_t *desktop,
                                uint32_t *pixels,
                                uint16_t *width, uint16_t *height,
                                int16_t *hot_x, int16_t *hot_y) {
    const system_cursor_t *system;
    gui_surface_t temp;

    if (!desktop || !pixels || !width || !height || !hot_x || !hot_y)
        return false;
    kmemset(pixels, 0,
            GUI_CURSOR_WIDTH * GUI_CURSOR_HEIGHT * sizeof(uint32_t));

    if (compositor_custom_cursor(desktop) && desktop->cursor_width &&
        desktop->cursor_height &&
        desktop->cursor_width <= GUI_CURSOR_WIDTH &&
        desktop->cursor_height <= GUI_CURSOR_HEIGHT) {
        for (uint16_t row = 0U; row < desktop->cursor_height; row++)
            kmemcpy(pixels + (uint32_t)row * GUI_CURSOR_WIDTH,
                    desktop->cursor_pixels +
                        (uint32_t)row * GUI_CURSOR_WIDTH,
                    (size_t)desktop->cursor_width * sizeof(uint32_t));
        *width = desktop->cursor_width;
        *height = desktop->cursor_height;
        *hot_x = desktop->cursor_hotspot_x;
        *hot_y = desktop->cursor_hotspot_y;
        return true;
    }

    system = compositor_system_cursor(compositor_cursor_style(desktop));
    if (system && system->loaded) {
        kmemcpy(pixels, system->pixels,
                GUI_CURSOR_WIDTH * GUI_CURSOR_HEIGHT * sizeof(uint32_t));
        for (uint32_t i=0;i<GUI_CURSOR_WIDTH*GUI_CURSOR_HEIGHT;i++)
            pixels[i]=compositor_stock_cursor_color(pixels[i]);
        *width = system->width;
        *height = system->height;
        *hot_x = system->hot_x;
        *hot_y = system->hot_y;
        return true;
    }

    /* Fallback sin VFS: conserva las mismas formas que el cursor software. */
    temp = (gui_surface_t){
        pixels, GUI_CURSOR_WIDTH, GUI_CURSOR_HEIGHT, GUI_CURSOR_WIDTH,
        {0, 0, GUI_CURSOR_WIDTH, GUI_CURSOR_HEIGHT},
        0, 0, GUI_CURSOR_WIDTH, GUI_CURSOR_HEIGHT
    };
    paint_cursor_with_palette(&temp, 0, 0,
                              compositor_stock_cursor_color(0xFF000000U), compositor_stock_cursor_color(0xFF808080U),
                              compositor_stock_cursor_color(0xFFFFFFFFU),
                              desktop->cursor_style);
    *width = GUI_DEFAULT_CURSOR_WIDTH;
    *height = GUI_DEFAULT_CURSOR_HEIGHT;
    *hot_x = 0;
    *hot_y = 0;
    return true;
}

static gui_image_t g_alert_icons[4];

void gui_alert_resources_init(void) {
    static const char *const names[4] = {
        "Awfxex32Info", "Warning", "Forbidden", "Network"
    };
    static bool initialized;
    if (initialized) return;
    initialized = true;
    for (uint32_t i = 0; i < 4U; i++)
        (void)bk_graphics_icon_load(names[i], &g_alert_icons[i]);
}

static void alert_bevel(gui_surface_t *surface, gui_rect_t bounds,
                        bool sunken, uint32_t fill, int depth) {
    const uint32_t highlight = 0x00FFFFFFU;
    const uint32_t midlight = 0x00DFDFDFU;
    const uint32_t shadow = 0x00808080U;
    const uint32_t dark = 0x00404040U;
    uint32_t top = sunken ? dark : highlight;
    uint32_t top2 = sunken ? shadow : midlight;
    uint32_t bottom = sunken ? highlight : dark;
    uint32_t bottom2 = sunken ? midlight : shadow;
    gui_gfx_fill_rect(surface, bounds, fill);
    gui_gfx_fill_rect(surface, (gui_rect_t){bounds.x, bounds.y, bounds.w, 1}, top);
    gui_gfx_fill_rect(surface, (gui_rect_t){bounds.x, bounds.y, 1, bounds.h}, top);
    gui_gfx_fill_rect(surface, (gui_rect_t){bounds.x, bounds.y + bounds.h - 1,
                      bounds.w, 1}, bottom);
    gui_gfx_fill_rect(surface, (gui_rect_t){bounds.x + bounds.w - 1, bounds.y,
                      1, bounds.h}, bottom);
    if (depth > 1 && bounds.w > 4 && bounds.h > 4) {
        gui_gfx_fill_rect(surface, (gui_rect_t){bounds.x + 1, bounds.y + 1,
                          bounds.w - 2, 1}, top2);
        gui_gfx_fill_rect(surface, (gui_rect_t){bounds.x + 1, bounds.y + 1,
                          1, bounds.h - 2}, top2);
        gui_gfx_fill_rect(surface, (gui_rect_t){bounds.x + 1,
                          bounds.y + bounds.h - 2, bounds.w - 2, 1}, bottom2);
        gui_gfx_fill_rect(surface, (gui_rect_t){bounds.x + bounds.w - 2,
                          bounds.y + 1, 1, bounds.h - 2}, bottom2);
    }
}

static void alert_stipple(gui_surface_t *surface, gui_rect_t bounds,
                          uint32_t color) {
    for (int y = bounds.y + 1; y < bounds.y + bounds.h - 1; y += 3) {
        int x = bounds.x + 1 + ((y / 3) & 1);
        for (; x < bounds.x + bounds.w - 1; x += 4)
            gui_gfx_putpixel(surface, x, y, color);
    }
}

static void alert_draw_icon(gui_surface_t *surface, const gui_image_t *icon,
                            int x, int y) {
    if (!icon || !icon->pixels) return;
    for (uint16_t row = 0; row < icon->height; row++) {
        for (uint16_t col = 0; col < icon->width; col++) {
            uint32_t source = icon->pixels[(uint32_t)row * icon->width + col];
            uint32_t destination;
            if (!(source >> 24)) continue;
            destination = surface->pixels[(uint32_t)(y + row) * surface->pitch
                                         + (uint32_t)(x + col)];
            gui_gfx_putpixel(surface, x + col, y + row,
                             cursor_alpha_blend(destination, source));
        }
    }
}

static void compositor_paint_error(gui_desktop_t *desktop,
                                   gui_surface_t *surface) {
    gui_rect_t box;
    gui_rect_t body;
    gui_rect_t close_button;
    gui_rect_t ok_button;
    uint32_t accent_color = 0x00993434U;
    const char *kind_text = "Error";
    char line[58];
    char code[32];
    uint32_t source = 0U;
    int row = 0;

    if (!desktop || !surface || !desktop->error_visible) return;
    if (desktop->error_kind == GUI_ALERT_INFO) {
        accent_color = 0x002E6594U;
        kind_text = "Informacion";
    } else if (desktop->error_kind == GUI_ALERT_WARNING) {
        accent_color = 0x00A06000U;
        kind_text = "Advertencia";
    } else if (desktop->error_kind == GUI_ALERT_NETWORK) {
        accent_color = 0x003A5F89U;
        kind_text = "Red";
    }
    box.w = surface->width > GUI_ALERT_WIDTH ? GUI_ALERT_WIDTH : surface->width - 24;
    if (box.w < 220) box.w = 220;
    box.h = GUI_ALERT_HEIGHT;
    box.x = (surface->width - box.w) / 2;
    box.y = (surface->height - box.h) / 2;
    alert_bevel(surface, box, false, 0x00C0C0C0U, 2);
    alert_stipple(surface, (gui_rect_t){box.x + 3, box.y + 3,
                  box.w - 6, box.h - 6}, 0x00B8B8B8U);
    alert_bevel(surface, (gui_rect_t){box.x + 6, box.y + 5,
                box.w - 12, 24}, true, 0x004B93A8U, 1);
    alert_stipple(surface, (gui_rect_t){box.x + 7, box.y + 6,
                  box.w - 14, 22}, 0x0059A0B2U);
    gui_font_draw_string(surface, box.x + 14, box.y + 12,
                         desktop->error_title, 0x00FFFFFF, 0U, false);
    close_button = (gui_rect_t){box.x + box.w - 28, box.y + 7, 19, 18};
    alert_bevel(surface, close_button, false, 0x00C0C0C0U, 2);
    gui_gfx_draw_line(surface, close_button.x + 5, close_button.y + 5,
                      close_button.x + 13, close_button.y + 12, 0x00404040U);
    gui_gfx_draw_line(surface, close_button.x + 13, close_button.y + 5,
                      close_button.x + 5, close_button.y + 12, 0x00404040U);

    alert_bevel(surface, (gui_rect_t){box.x + 10, box.y + 35,
                box.w - 20, 103}, true, 0x00DFDFDFU, 2);
    gui_gfx_fill_rect(surface, (gui_rect_t){box.x + 14, box.y + 39, 5, 95},
                      accent_color);
    alert_draw_icon(surface, &g_alert_icons[desktop->error_kind],
                    box.x + 29, box.y + 54);
    gui_font_draw_string(surface, box.x + 25, box.y + 96, kind_text,
                         accent_color, 0U, false);
    body = (gui_rect_t){box.x + 82, box.y + 48, box.w - 105, 75};
    while (desktop->error_text[source] && row < 4) {
        uint32_t count = 0U;
        uint32_t last_space = 0U;
        while (desktop->error_text[source + count] && count < 48U) {
            char ch = desktop->error_text[source + count];
            if (ch == '\n') break;
            if (ch == ' ') last_space = count;
            line[count++] = ch;
        }
        if (desktop->error_text[source + count] && count == 48U
            && last_space > 0U) count = last_space;
        line[count] = '\0';
        gui_font_draw_string_clipped(surface, body.x, body.y + row * 16,
                                     line, 0x00181818U, body);
        source += count;
        while (desktop->error_text[source] == ' '
               || desktop->error_text[source] == '\n') source++;
        row++;
    }
    if (desktop->error_code != 0) {
        snprintf(code, sizeof(code), "Codigo: %d", desktop->error_code);
        gui_font_draw_string(surface, box.x + 82, box.y + 118, code,
                             0x00404040U, 0U, false);
    }
    ok_button = (gui_rect_t){box.x + box.w - 105, box.y + box.h - 43, 82, 27};
    alert_bevel(surface, ok_button, false, 0x00C0C0C0U, 2);
    gui_font_draw_string(surface, ok_button.x + 30, ok_button.y + 9,
                         "OK", 0x00101010U, 0U, false);
}

static void compositor_draw_outline(gui_surface_t *surface, gui_rect_t rect) {
    if (!surface || rect.w <= 0 || rect.h <= 0) return;
    gui_gfx_draw_rect(surface, rect, 0x00000000);
    if (rect.w > 2 && rect.h > 2) {
        rect.x++;
        rect.y++;
        rect.w -= 2;
        rect.h -= 2;
        gui_gfx_draw_rect(surface, rect, 0x00FFFFFF);
    }
}

static void compositor_draw_icon_ghost(gui_desktop_t *desktop,gui_surface_t *surface) {
    unsigned size=desktop->drag_icon_size;
    if(!size || size>64)return;
    gui_rect_t clip=gui_gfx_get_clip(surface);
    unsigned count=desktop->drag_icon_count>1?2:1;
    for(unsigned layer=count;layer>0;--layer) {
        int offset=(int)(layer-1)*6;
        for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
            unsigned value=desktop->drag_icon_mask[y*64+x];
            if(!value || (value==1 && ((x+y)&1)))continue;
            int px=desktop->drag_outline_bounds.x+(int)x+offset;
            int py=desktop->drag_outline_bounds.y+(int)y+offset;
            if(px<clip.x || py<clip.y || px>=clip.x+clip.w || py>=clip.y+clip.h ||
               px<0 || py<0 || px>=surface->width || py>=surface->height)continue;
            surface->pixels[py*surface->pitch+px]=value==2 ?
                (((x+y)&1)?0x00FFFFFF:0x00000000) : 0x00808080;
        }
    }
}

static void compositor_paint_interaction_overlay(gui_desktop_t *desktop,
                                                 gui_surface_t *surface) {
    if (!desktop || !surface) return;
    /* These are transient server-side overlays.  They intentionally live
     * above client windows and outside Vision's persistent shell cache. */
    if (desktop->drag_outline_visible) {
        if(desktop->drag_icon_ghost)compositor_draw_icon_ghost(desktop,surface);
        else compositor_draw_outline(surface, desktop->drag_outline_bounds);
    }
    if (desktop->minimize_animation_active)
        compositor_draw_outline(surface, desktop->minimize_animation_bounds);
}

static bool compositor_has_content_dirty(const gui_desktop_t *desktop) {
    const gui_window_t *window;
    if (!desktop || !desktop->paint_valid || desktop->dirty_valid) return true;
    for (window = desktop->first_window; window; window = window->next)
        if (window->dirty) return true;
    return false;
}

static void cursor_restore_background(gui_desktop_t *desktop) {
    gui_rect_t rect;
    if (!desktop || !desktop->cursor_valid ||
        desktop->cursor_trail_enabled || !desktop->surface.pixels) return;
    rect = desktop->cursor_rect;
    for (int row = 0; row < rect.h && row < GUI_CURSOR_HEIGHT; row++) {
        int y = rect.y + row;
        if (y < 0 || y >= desktop->surface.height) continue;
        for (int col = 0; col < rect.w && col < GUI_CURSOR_WIDTH; col++) {
            int x = rect.x + col;
            if (x < 0 || x >= desktop->surface.width) continue;
            desktop->surface.pixels[(uint32_t)y * desktop->surface.pitch +
                                    (uint32_t)x] =
                desktop->cursor_backing[row * GUI_CURSOR_WIDTH + col];
        }
    }
}

static void cursor_save_background(gui_desktop_t *desktop, gui_rect_t rect) {
    if (!desktop || !desktop->surface.pixels) return;
    for (int row = 0; row < rect.h && row < GUI_CURSOR_HEIGHT; row++) {
        int py = rect.y + row;
        for (int col = 0; col < rect.w && col < GUI_CURSOR_WIDTH; col++) {
            int px = rect.x + col;
            uint32_t color = 0U;
            if (px >= 0 && py >= 0 && px < desktop->surface.width &&
                py < desktop->surface.height)
                color = desktop->surface.pixels[(uint32_t)py *
                    desktop->surface.pitch + (uint32_t)px];
            desktop->cursor_backing[row * GUI_CURSOR_WIDTH + col] = color;
        }
    }
}

bool gui_compositor_stream_cursor_begin(gui_desktop_t *desktop) {
    if (!desktop || desktop->cursor_trail_enabled || !desktop->cursor_valid)
        return false;
    cursor_restore_background(desktop);
    desktop->cursor_valid = false;
    return true;
}

void gui_compositor_stream_cursor_end(gui_desktop_t *desktop, bool redraw) {
    gui_rect_t rect;
    if (!desktop || !redraw || desktop->cursor_trail_enabled) return;
    rect = cursor_rect(desktop, desktop->mouse_x, desktop->mouse_y);
    cursor_save_background(desktop, rect);
    paint_cursor(desktop, &desktop->surface,
                 desktop->mouse_x, desktop->mouse_y);
    desktop->cursor_rect = rect;
    desktop->cursor_valid = true;
}

static bool compositor_has_gpu_frame_dirty(const gui_desktop_t *desktop) {
    const gui_window_t *window;
    if (!desktop) return false;
    for (window = desktop->first_window; window; window = window->next)
        if (window->visible && window->gpu_view_visible &&
            window->gpu_view_frame_dirty) return true;
    return false;
}

static void compositor_clear_gpu_frame_dirty(gui_desktop_t *desktop) {
    gui_window_t *window;
    if (!desktop) return;
    for (window = desktop->first_window; window; window = window->next)
        window->gpu_view_frame_dirty = false;
}

void gui_compositor_paint(gui_desktop_t *desktop) {
    gui_rect_t content_rect = {0, 0, 0, 0};
    gui_rect_t present_rects[3];
    gui_rect_t new_cursor;
    gui_rect_t screen;
    gui_rect_t planned[GUI_CURSOR_TRAIL_MAX + 1];
    uint8_t planned_count;
    uint8_t present_count = 0;
    uint32_t dirty_generation;
    bool content_dirty;
    bool content_valid;
    bool old_cursor_valid;
    bool hardware_cursor;
    bool gpu_frame_dirty;
    bool gpu_cached_wanted = false;
    bool gpu_presented = false;

    if (!desktop) return;
    content_dirty = compositor_has_content_dirty(desktop);
    content_valid = compositor_content_dirty_rect(desktop, &content_rect);
    old_cursor_valid = desktop->cursor_valid;
    /* BLESKERNOS_SVGA_HW_CURSOR_INIT_BEGIN */
    hardware_cursor = !gui_gfx_output_depth() && !desktop->cursor_trail_enabled &&
                      compositor_prepare_hardware_cursor(desktop);
    if (!hardware_cursor && g_hardware_cursor_ready)
        (void)gfx_cursor_show(false);
    /* BLESKERNOS_SVGA_HW_CURSOR_INIT_END */

    screen = (gui_rect_t){0, 0, desktop->surface.width,
                          desktop->surface.height};
    gpu_frame_dirty = compositor_has_gpu_frame_dirty(desktop);
    if (!gui_gpu_compositor_needed(desktop))
        g_gpu_cursor_present_valid = false;

    /* A game/video frame on an unchanged GPU viewport is not desktop damage.
     * Re-submit the cached GPU scene directly.  VMware keeps its cursor in
     * software, so the cursor must become the last GPU layer while a direct
     * viewport is visible; writing a 2D cursor rectangle over a 3D surface
     * destroys the pixels below it and makes the game appear to update only
     * when the pointer or deskbar clock invalidates a region. */
    gpu_cached_wanted = !content_dirty && !desktop->cursor_trail_enabled &&
        gui_gpu_compositor_needed(desktop) &&
        (gpu_frame_dirty ||
         (!hardware_cursor &&
          (!g_gpu_cursor_present_valid ||
           g_gpu_cursor_present_x != desktop->mouse_x ||
           g_gpu_cursor_present_y != desktop->mouse_y)));
    if (gpu_cached_wanted &&
        gui_gpu_compositor_present_cached(desktop, !hardware_cursor)) {
        if (hardware_cursor) {
            if (!gfx_cursor_move(desktop->mouse_x, desktop->mouse_y) ||
                !gfx_cursor_show(true)) {
                g_hardware_cursor_ready = false;
                g_hardware_cursor_generation = 0U;
                hardware_cursor = false;
                /* The frame just presented did not contain a cursor. Re-submit
                 * once with the GPU cursor instead of falling through to the
                 * incompatible 2D software-cursor path. */
                if (!gui_gpu_compositor_present_cached(desktop, true))
                    goto gpu_cached_failed;
            }
        }
        if (!hardware_cursor) {
            g_gpu_cursor_present_valid = true;
            g_gpu_cursor_present_x = desktop->mouse_x;
            g_gpu_cursor_present_y = desktop->mouse_y;
        } else {
            g_gpu_cursor_present_valid = false;
        }
        compositor_clear_gpu_frame_dirty(desktop);
        desktop->cursor_valid = false;
        desktop->cursor_paint_count = 0U;
        desktop->paint_valid = true;
        perfmon_gui_frame(0U, (uint32_t)screen.w * (uint32_t)screen.h,
                          false, true, true);
        return;
    }
gpu_cached_failed:
    /* BLESKERNOS_GPU_CACHED_FALLTHROUGH_V9_1
     * Este label tambien se alcanza por flujo normal. Solo promueva a frame
     * completo si de verdad intentamos el cached present y fallo. Un click
     * sin dano visual debe ser un no-op de presentacion. */
    if (gpu_cached_wanted && gui_gpu_compositor_needed(desktop)) {
        content_dirty = true;
        content_rect = screen;
        content_valid = true;
    } else if (!content_dirty && gui_gpu_compositor_needed(desktop) &&
               !desktop->cursor_trail_enabled) {
        desktop->paint_valid = true;
        perfmon_gui_frame(0U, 0U, false, true, true);
        return;
    }
    /* BLESKERNOS_SVGA3D_FULL_FRAME_BEGIN
     * La primera ruta compuesta por GPU reconstruye sus capas completas
     * cuando cambia contenido. Los cuadros que sólo mueven el cursor
     * conservan el camino de rectángulos sucios existente. */
    if (content_dirty && gui_gpu_compositor_needed(desktop) &&
        gui_gpu_compositor_enabled()) {
        content_rect = screen;
        content_valid = true;
    }
    /* BLESKERNOS_SVGA3D_FULL_FRAME_END */
    planned_count = compositor_planned_cursor_rects(desktop, planned,
                                                    GUI_CURSOR_TRAIL_MAX + 1);
    /* Los trails sí forman parte temporal del surface y necesitan recomponer
       sus posiciones previas. Se mantiene este camino sólo cuando la opción
       está activada; el cursor normal usa backing store. */
    if (desktop->cursor_trail_enabled) {
        bool trail_valid = content_valid;
        for (uint8_t i = 0; i < desktop->cursor_paint_count; i++)
            dirty_union(&content_rect, &trail_valid,
                        desktop->cursor_paint_rects[i]);
        for (uint8_t i = 0; i < planned_count; i++)
            dirty_union(&content_rect, &trail_valid, planned[i]);
        if (trail_valid) {
            gui_rect_t clipped;
            if (gui_rect_intersect(screen, content_rect, &clipped)) {
                content_rect = clipped;
                content_valid = true;
                content_dirty = true;
            }
        }
    }
    /* El cursor no forma parte permanente del contenido. Restaurar su fondo
       permite moverlo sin volver a ejecutar todos los pintores de ventanas. */
    cursor_restore_background(desktop);
    dirty_generation = desktop->dirty_generation;
    if (content_valid) gui_gfx_set_clip(&desktop->surface, content_rect);
    if (content_valid &&
        (content_dirty || desktop->cursor_trail_enabled)) {
        /* El deskmanager, primer programa tanto en escritorio como en Setup,
         * ya repone por completo la region recortada (color o wallpaper).
         * Limpiar antes duplicaba todas las escrituras del dirty rect. */
        if (!desktop->first_program)
            gui_gfx_clear(&desktop->surface, 0x005080B0);
        {
            uint64_t perf_compose_started = perfmon_scope_begin();
            gui_desktop_paint_programs(desktop);
            compositor_paint_interaction_overlay(desktop, &desktop->surface);
            perfmon_scope_end(PERF_SCOPE_GUI_COMPOSE,
                              perf_compose_started);
        }
        /* A Ring-3 scene owner (Vision) is asynchronous.  If it is rebuilding
         * this dirty region, presenting now would publish the previous scene
         * and can erase a newly completed application frame.  Leave the last
         * good scanout untouched; the paint-completion upcall records the exact
         * clip and wakes the compositor again. */
        if (gui_desktop_scene_waiting(desktop)) {
            gui_gfx_reset_clip(&desktop->surface);
            return;
        }
    }
    compositor_paint_error(desktop, &desktop->surface);
    if (desktop->cursor_trail_enabled && planned_count > 1) {
        for (uint8_t i = 0; i + 1 < planned_count; i++) {
            uint8_t t = (uint8_t)(48 + (i * 128) /
                (planned_count > 1 ? planned_count - 1 : 1));
            uint32_t dark = gui_color_lerp(0x00B8B8B8, 0x00000000, t);
            uint32_t mid = gui_color_lerp(0x00E0E0E0, 0x00808080, t);
            paint_cursor_with_palette(&desktop->surface,
                                      planned[i].x, planned[i].y,
                                      dark, mid, 0x00FFFFFF, GUI_CURSOR_ARROW);
        }
    }
    gui_gfx_reset_clip(&desktop->surface);
    /* BLESKERNOS_SVGA3D_PRESENT_BEGIN */
    if (content_valid) {
        uint64_t perf_gpu_started = perfmon_scope_begin();
        gpu_presented = gui_gpu_compositor_present(
            desktop, &desktop->surface, !hardware_cursor);
        perfmon_scope_end(PERF_SCOPE_GPU_PRESENT, perf_gpu_started);
        if (gpu_presented) compositor_clear_gpu_frame_dirty(desktop);
    }
    /* BLESKERNOS_SVGA3D_PRESENT_END */

    /* A direct GPU viewport and a CPU software cursor cannot safely share the
     * scanout: the CPU backing store has no copy of the 3D pixels underneath
     * the pointer.  When the GPU compositor presented the frame with its cursor
     * layer, finish the dirty bookkeeping here and never issue 2D cursor
     * rectangles over that frame. */
    if (gpu_presented && !hardware_cursor) {
        g_gpu_cursor_present_valid = true;
        g_gpu_cursor_present_x = desktop->mouse_x;
        g_gpu_cursor_present_y = desktop->mouse_y;
        if (content_valid) compositor_finish_windows(desktop);
        if (content_valid && desktop->dirty_generation == dirty_generation)
            desktop->dirty_valid = false;
        desktop->cursor_valid = false;
        desktop->cursor_paint_count = 0U;
        desktop->paint_valid = true;
        perfmon_gui_frame(content_valid
                ? (uint32_t)content_rect.w * (uint32_t)content_rect.h : 0U,
            (uint32_t)screen.w * (uint32_t)screen.h,
            content_valid, true, true);
        return;
    }

    /* BLESKERNOS_SVGA_HW_CURSOR_BRANCH_BEGIN */
    if (hardware_cursor) {
        bool cursor_ok;
        cursor_ok = gfx_cursor_move(desktop->mouse_x, desktop->mouse_y) &&
                    gfx_cursor_show(true);
        if (!cursor_ok) {
            /* Una cola de cursor averiada no debe dejar al usuario sin
               puntero. Este mismo frame continua por el camino software. */
            g_hardware_cursor_ready = false;
            g_hardware_cursor_generation = 0U;
            hardware_cursor = false;
        }
    }
    if (hardware_cursor) {
        if (content_valid && !gpu_presented)
            present_rects[present_count++] = content_rect;
        /* Presentar una vez el rectángulo restaurado del cursor software
           anterior; luego el puntero vive como overlay del dispositivo. */
        if (old_cursor_valid &&
            (!content_valid || desktop->cursor_rect.x < content_rect.x ||
             desktop->cursor_rect.y < content_rect.y ||
             desktop->cursor_rect.x + desktop->cursor_rect.w >
                 content_rect.x + content_rect.w ||
             desktop->cursor_rect.y + desktop->cursor_rect.h >
                 content_rect.y + content_rect.h))
            present_rects[present_count++] = desktop->cursor_rect;
        if (present_count) {
            uint64_t perf_present_started = perfmon_scope_begin();
            gui_gfx_present_rects(&desktop->surface, present_rects,
                                  present_count);
            perfmon_scope_end(PERF_SCOPE_GFX_PRESENT,
                              perf_present_started);
        }
        if (content_valid) compositor_finish_windows(desktop);
        if (content_valid && desktop->dirty_generation == dirty_generation)
            desktop->dirty_valid = false;
        desktop->cursor_valid = false;
        desktop->cursor_paint_count = 0U;
        desktop->paint_valid = true;
        perfmon_gui_frame(content_valid
                ? (uint32_t)content_rect.w * (uint32_t)content_rect.h : 0U,
            (uint32_t)screen.w * (uint32_t)screen.h,
            content_valid, true, gpu_presented);
        return;
    }
    /* BLESKERNOS_SVGA_HW_CURSOR_BRANCH_END */

    {
        gui_rect_t cursor_area = cursor_rect(desktop, desktop->mouse_x,
                                             desktop->mouse_y);
        cursor_save_background(desktop, cursor_area);
        paint_cursor(desktop, &desktop->surface,
                     desktop->mouse_x, desktop->mouse_y);
        new_cursor = cursor_area;
    }
    if (content_valid && !gpu_presented)
        present_rects[present_count++] = content_rect;
    /* Nunca una el cursor con una región de contenido lejana (por ejemplo el
       reloj del deskbar). Ese bounding box era proporcional a la altura del
       mouse y explicaba por qué arriba se sentía mucho más lento. */
    if (old_cursor_valid &&
        (!content_valid || desktop->cursor_rect.x < content_rect.x ||
         desktop->cursor_rect.y < content_rect.y ||
         desktop->cursor_rect.x + desktop->cursor_rect.w >
             content_rect.x + content_rect.w ||
         desktop->cursor_rect.y + desktop->cursor_rect.h >
             content_rect.y + content_rect.h))
        present_rects[present_count++] = desktop->cursor_rect;
    if (!content_valid || new_cursor.x < content_rect.x ||
        new_cursor.y < content_rect.y ||
        new_cursor.x + new_cursor.w > content_rect.x + content_rect.w ||
        new_cursor.y + new_cursor.h > content_rect.y + content_rect.h)
        present_rects[present_count++] = new_cursor;
    {
        uint64_t perf_present_started = perfmon_scope_begin();
        gui_gfx_present_rects(&desktop->surface, present_rects, present_count);
        perfmon_scope_end(PERF_SCOPE_GFX_PRESENT, perf_present_started);
    }
    if (content_valid) compositor_finish_windows(desktop);
    /* No borre una invalidacion producida por una app Ring 3 mientras este
     * frame se estaba componiendo/presentando. */
    if (content_valid && desktop->dirty_generation == dirty_generation)
        desktop->dirty_valid = false;
    desktop->cursor_rect = new_cursor;
    desktop->cursor_valid = true;
    desktop->cursor_paint_count = planned_count;
    for (uint8_t i = 0; i < planned_count; i++)
        desktop->cursor_paint_rects[i] = planned[i];
    desktop->paint_valid = true;
    perfmon_gui_frame(content_valid
            ? (uint32_t)content_rect.w * (uint32_t)content_rect.h : 0U,
        (uint32_t)screen.w * (uint32_t)screen.h,
        content_valid, false, gpu_presented);
}
