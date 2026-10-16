/* Pure surface rasterization shared by the legacy ABI and Vision.
 * Hardware buffers, mode changes and presentation remain in gfx.c. */
static gui_rect_t gui_gfx_storage_bounds(const gui_surface_t *surface) {
    int width;
    int height;
    if (!surface) return (gui_rect_t){0, 0, 0, 0};
    width = surface->storage_width ? surface->storage_width : surface->width;
    height = surface->storage_height ? surface->storage_height : surface->height;
    return (gui_rect_t){surface->origin_x, surface->origin_y, width, height};
}


static void gui_gfx_fill_dwords(uint32_t *destination, uint32_t value,
                                uint32_t count) {
#ifdef BLES_ARCH_ARM32_RPI3
    while (count--) *destination++ = value;
#else
    uint32_t *dst = destination;
    __asm__ volatile ("cld; rep stosl"
                      : "+D"(dst), "+c"(count)
                      : "a"(value) : "memory");
#endif
}


static uint8_t gui_channel(uint32_t color, uint8_t shift) {
    return (uint8_t)((color >> shift) & 0xFF);
}


void gui_gfx_draw_xrgb(gui_surface_t *surface, gui_rect_t destination,
                       gui_rect_t clip, const uint32_t *pixels,
                       uint16_t width, uint16_t height) {
    gui_rect_t visible;
    gui_rect_t surface_clip;
    int x0, y0, x1, y1;

    if (!surface || !surface->pixels || !pixels || !width || !height ||
        destination.w <= 0 || destination.h <= 0 ||
        clip.w <= 0 || clip.h <= 0) return;
    if (!gui_rect_intersect(gui_gfx_get_clip(surface),
            gui_gfx_storage_bounds(surface), &surface_clip)) return;
    if (!gui_rect_intersect(destination, clip, &visible) ||
        !gui_rect_intersect(visible, surface_clip, &visible)) return;
    x0 = visible.x;
    y0 = visible.y;
    x1 = visible.x + visible.w;
    y1 = visible.y + visible.h;

    if (destination.w == (int)width && destination.h == (int)height) {
        uint32_t source_x = (uint32_t)(x0 - destination.x);
        for (int y = y0; y < y1; y++) {
            uint32_t source_y = (uint32_t)(y - destination.y);
            const uint32_t *src = &pixels[source_y * width + source_x];
            uint32_t *dst = &surface->pixels[(uint32_t)(y - surface->origin_y) * surface->pitch +
                                             (uint32_t)(x0 - surface->origin_x)];
            kmemcpy(dst, src, (size_t)(x1 - x0) * sizeof(uint32_t));
        }
        return;
    }

    /* Games commonly render 320x240 and display at 640x480.  Expand one row
       in a tight loop, then duplicate it with REP MOVSD instead of doing four
       scalar stores for every source pixel. */
    if (destination.w == (int)width * 2 && destination.h == (int)height * 2 &&
        x0 == destination.x && y0 == destination.y &&
        x1 == destination.x + destination.w &&
        y1 == destination.y + destination.h) {
        for (uint32_t sy = 0U; sy < height; sy++) {
            const uint32_t *src = &pixels[sy * width];
            uint32_t *dst0 = &surface->pixels[
                (uint32_t)(destination.y + (int)(sy * 2U) - surface->origin_y) * surface->pitch +
                (uint32_t)(destination.x - surface->origin_x)];
            uint32_t *dst1 = dst0 + surface->pitch;
            for (uint32_t sx = 0U; sx < width; sx++) {
                uint32_t color = src[sx];
                uint32_t dx = sx * 2U;
                dst0[dx] = color;
                dst0[dx + 1U] = color;
            }
            kmemcpy(dst1, dst0, (size_t)width * 2U * sizeof(uint32_t));
        }
        return;
    }

    {
        uint32_t step_x = ((uint32_t)width << 16) /
                          (uint32_t)destination.w;
        uint32_t step_y = ((uint32_t)height << 16) /
                          (uint32_t)destination.h;
        uint32_t start_x = (uint32_t)(x0 - destination.x) * step_x;
        uint32_t sy_fp = (uint32_t)(y0 - destination.y) * step_y;
        for (int y = y0; y < y1; y++, sy_fp += step_y) {
            uint32_t sy = sy_fp >> 16;
            uint32_t sx_fp = start_x;
            uint32_t *dst = &surface->pixels[(uint32_t)(y - surface->origin_y) * surface->pitch +
                                             (uint32_t)(x0 - surface->origin_x)];
            if (sy >= height) sy = height - 1U;
            for (int x = x0; x < x1; x++, dst++, sx_fp += step_x) {
                uint32_t sx = sx_fp >> 16;
                if (sx >= width) sx = width - 1U;
                *dst = pixels[sy * width + sx];
            }
        }
    }
}


void gui_gfx_clear(gui_surface_t *surface, uint32_t color) {
    if (!surface || !surface->pixels) return;
    gui_gfx_fill_rect(surface, gui_gfx_storage_bounds(surface), color);
}

void gui_gfx_set_clip(gui_surface_t *surface, gui_rect_t clip) {
    gui_rect_t screen;

    if (!surface) return;
    screen = gui_gfx_storage_bounds(surface);
    if (!gui_rect_intersect(screen, clip, &surface->clip))
        surface->clip = (gui_rect_t){0, 0, 0, 0};
}

void gui_gfx_reset_clip(gui_surface_t *surface) {
    if (!surface) return;
    surface->clip = gui_gfx_storage_bounds(surface);
}

gui_rect_t gui_gfx_get_clip(const gui_surface_t *surface) {
    if (!surface) return (gui_rect_t){0, 0, 0, 0};
    return surface->clip;
}

bool gui_gfx_point_visible(const gui_surface_t *surface, int x, int y) {
    gui_rect_t storage;
    if (!surface || !surface->pixels) return false;
    storage = gui_gfx_storage_bounds(surface);
    if (!gui_rect_contains(storage, x, y)) return false;
    return gui_rect_contains(surface->clip, x, y);
}

void gui_gfx_fill_rect(gui_surface_t *surface, gui_rect_t rect, uint32_t color) {
    int x2;
    int y2;
    gui_rect_t clipped;

    if (!surface || !surface->pixels || rect.w <= 0 || rect.h <= 0) return;
    if (!gui_rect_intersect(rect, surface->clip, &clipped)) return;
    rect = clipped;
    x2 = rect.x + rect.w;
    y2 = rect.y + rect.h;
    if (rect.x < surface->origin_x) rect.x = surface->origin_x;
    if (rect.y < surface->origin_y) rect.y = surface->origin_y;
    if (x2 > surface->origin_x + (surface->storage_width
            ? surface->storage_width : surface->width))
        x2 = surface->origin_x + (surface->storage_width
            ? surface->storage_width : surface->width);
    if (y2 > surface->origin_y + (surface->storage_height
            ? surface->storage_height : surface->height))
        y2 = surface->origin_y + (surface->storage_height
            ? surface->storage_height : surface->height);
    if (rect.x >= x2 || rect.y >= y2) return;

    for (int y = rect.y; y < y2; y++) {
        uint32_t *row = &surface->pixels[
            (uint32_t)(y - surface->origin_y) * surface->pitch +
            (uint32_t)(rect.x - surface->origin_x)];
        gui_gfx_fill_dwords(row, color, (uint32_t)(x2 - rect.x));
    }
}

void gui_gfx_putpixel(gui_surface_t *surface, int x, int y, uint32_t color) {
    if (!surface || !surface->pixels) return;
    if (!gui_gfx_point_visible(surface, x, y)) return;
    surface->pixels[(uint32_t)(y - surface->origin_y) * surface->pitch +
                    (uint32_t)(x - surface->origin_x)] = color;
}

void gui_gfx_draw_rect(gui_surface_t *surface, gui_rect_t rect, uint32_t color) {
    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x, rect.y, rect.w, 1}, color);
    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x, rect.y + rect.h - 1, rect.w, 1}, color);
    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x, rect.y, 1, rect.h}, color);
    gui_gfx_fill_rect(surface, (gui_rect_t){rect.x + rect.w - 1, rect.y, 1, rect.h}, color);
}

uint32_t gui_color_blend(uint32_t base, uint32_t top, uint8_t alpha) {
    uint32_t inv = (uint32_t)(255 - alpha);
    uint8_t r = (uint8_t)(((uint32_t)gui_channel(base, 16) * inv + (uint32_t)gui_channel(top, 16) * alpha) / 255);
    uint8_t g = (uint8_t)(((uint32_t)gui_channel(base, 8) * inv + (uint32_t)gui_channel(top, 8) * alpha) / 255);
    uint8_t b = (uint8_t)(((uint32_t)gui_channel(base, 0) * inv + (uint32_t)gui_channel(top, 0) * alpha) / 255);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t gui_color_lerp(uint32_t a, uint32_t b, uint8_t t) {
    return gui_color_blend(a, b, t);
}

void gui_gfx_fill_gradient(gui_surface_t *surface, gui_rect_t rect, uint32_t top, uint32_t bottom) {
    int span;

    if (!surface || !surface->pixels || rect.w <= 0 || rect.h <= 0) return;
    if (rect.h == 1) {
        gui_gfx_fill_rect(surface, rect, top);
        return;
    }

    span = rect.h - 1;
    for (int row = 0; row < rect.h; row++) {
        uint8_t t = (uint8_t)((row * 255) / span);
        gui_gfx_fill_rect(surface, (gui_rect_t){rect.x, rect.y + row, rect.w, 1}, gui_color_lerp(top, bottom, t));
    }
}

static int gui_rounded_inset(int radius, int y) {
    int dy = radius - 1 - y;
    int inset = 0;

    while (inset < radius) {
        int dx = radius - 1 - inset;
        if ((dx * dx) + (dy * dy) < (radius * radius)) break;
        inset++;
    }

    return inset;
}

void gui_gfx_fill_rounded_rect(gui_surface_t *surface, gui_rect_t rect, int radius, uint32_t color) {
    if (!surface || !surface->pixels || rect.w <= 0 || rect.h <= 0) return;
    if (radius <= 0) {
        gui_gfx_fill_rect(surface, rect, color);
        return;
    }

    if (radius * 2 > rect.w) radius = rect.w / 2;
    if (radius * 2 > rect.h) radius = rect.h / 2;
    if (radius <= 0) {
        gui_gfx_fill_rect(surface, rect, color);
        return;
    }

    for (int row = 0; row < rect.h; row++) {
        int inset = 0;
        if (row < radius) inset = gui_rounded_inset(radius, row);
        else if (row >= rect.h - radius) inset = gui_rounded_inset(radius, rect.h - 1 - row);
        gui_gfx_fill_rect(surface, (gui_rect_t){rect.x + inset, rect.y + row, rect.w - (inset * 2), 1}, color);
    }
}

void gui_gfx_draw_line(gui_surface_t *surface, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        int e2;
        gui_gfx_putpixel(surface, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gui_gfx_draw_polyline(gui_surface_t *surface,
                           const gui_point_t *points, uint32_t count,
                           uint32_t color) {
    if (!surface || !points || count < 2U || count > 1024U) return;
    for (uint32_t i = 1U; i < count; i++)
        gui_gfx_draw_line(surface, points[i - 1U].x, points[i - 1U].y,
                          points[i].x, points[i].y, color);
}

bool gui_rect_intersect(gui_rect_t a, gui_rect_t b, gui_rect_t *out) {
    int x1 = a.x > b.x ? a.x : b.x;
    int y1 = a.y > b.y ? a.y : b.y;
    int x2 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y2 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    if (x2 <= x1 || y2 <= y1) return false;
    if (out) *out = (gui_rect_t){x1, y1, x2 - x1, y2 - y1};
    return true;
}

gui_rect_t gui_rect_union(gui_rect_t a, gui_rect_t b) {
    int x1;
    int y1;
    int x2;
    int y2;

    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    x1 = a.x < b.x ? a.x : b.x;
    y1 = a.y < b.y ? a.y : b.y;
    x2 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    y2 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return (gui_rect_t){x1, y1, x2 - x1, y2 - y1};
}

bool gui_rect_contains(gui_rect_t rect, int x, int y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

void gui_scrollbar_init_vertical(gui_scrollbar_t *bar, gui_rect_t bounds,
                                 uint32_t value, uint32_t visible,
                                 uint32_t total) {
    uint32_t max_value;

    if (!bar) return;
    if (visible > total) visible = total;
    max_value = total > visible ? total - visible : 0;
    if (value > max_value) value = max_value;
    bar->bounds = bounds;
    bar->value = value;
    bar->visible = visible;
    bar->total = total;
}

gui_rect_t gui_scrollbar_thumb_rect(const gui_scrollbar_t *bar) {
    gui_rect_t b;
    int track_y;
    int track_h;
    int thumb_h;
    int thumb_y;
    uint32_t max_value;

    if (!bar) return (gui_rect_t){0, 0, 0, 0};
    b = bar->bounds;
    if (b.w <= 0 || b.h <= GUI_SCROLLBAR_SIZE * 2)
        return (gui_rect_t){b.x, b.y, b.w, b.h};
    track_y = b.y + GUI_SCROLLBAR_SIZE;
    track_h = b.h - GUI_SCROLLBAR_SIZE * 2;
    if (!bar->total || bar->visible >= bar->total) {
        return (gui_rect_t){b.x + 2, track_y + 2, b.w - 4, track_h - 4};
    }
#if defined(BLES_ARCH_ARM32_RPI3)
    /* Stage 12 ARM userland pulls the real terminal scrollbar into the ARM
       kernel.  Its track and scrollback are bounded (<= a few thousand), so
       a 32-bit product is sufficient and avoids a compiler-runtime 64-bit
       division dependency in the freestanding kernel. */
    thumb_h = (int)(((uint32_t)track_h * bar->visible) / bar->total);
#else
    thumb_h = (int)(((uint64_t)track_h * bar->visible) / bar->total);
#endif
    if (thumb_h < 12) thumb_h = 12;
    if (thumb_h > track_h) thumb_h = track_h;
    max_value = bar->total - bar->visible;
#if defined(BLES_ARCH_ARM32_RPI3)
    thumb_y = track_y + (int)(((uint32_t)(track_h - thumb_h) * bar->value) /
                              (max_value ? max_value : 1U));
#else
    thumb_y = track_y + (int)(((uint64_t)(track_h - thumb_h) * bar->value) /
                              (max_value ? max_value : 1U));
#endif
    return (gui_rect_t){b.x + 2, thumb_y, b.w - 4, thumb_h};
}

static void gui_scrollbar_draw_arrow(gui_surface_t *surface, gui_rect_t r,
                                     bool down) {
    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;

    gui_gfx_fill_rect(surface, r, 0x00C0C0C0);
    gui_gfx_draw_line(surface, r.x, r.y, r.x + r.w - 1, r.y, 0x00FFFFFF);
    gui_gfx_draw_line(surface, r.x, r.y, r.x, r.y + r.h - 1, 0x00FFFFFF);
    gui_gfx_draw_line(surface, r.x, r.y + r.h - 1, r.x + r.w - 1,
                      r.y + r.h - 1, 0x00404040);
    gui_gfx_draw_line(surface, r.x + r.w - 1, r.y, r.x + r.w - 1,
                      r.y + r.h - 1, 0x00404040);
    if (down) {
        gui_gfx_draw_line(surface, cx - 4, cy - 2, cx + 4, cy - 2, 0x00101010);
        gui_gfx_draw_line(surface, cx - 3, cy - 1, cx + 3, cy - 1, 0x00101010);
        gui_gfx_draw_line(surface, cx - 2, cy, cx + 2, cy, 0x00101010);
        gui_gfx_draw_line(surface, cx - 1, cy + 1, cx + 1, cy + 1, 0x00101010);
    } else {
        gui_gfx_draw_line(surface, cx - 1, cy - 2, cx + 1, cy - 2, 0x00101010);
        gui_gfx_draw_line(surface, cx - 2, cy - 1, cx + 2, cy - 1, 0x00101010);
        gui_gfx_draw_line(surface, cx - 3, cy, cx + 3, cy, 0x00101010);
        gui_gfx_draw_line(surface, cx - 4, cy + 1, cx + 4, cy + 1, 0x00101010);
    }
}

void gui_scrollbar_paint_vertical(gui_surface_t *surface,
                                  const gui_scrollbar_t *bar) {
    gui_rect_t b;
    gui_rect_t thumb;

    if (!surface || !bar) return;
    b = bar->bounds;
    if (b.w <= 0 || b.h <= 0) return;
    gui_gfx_fill_rect(surface, b, 0x00D0D0D0);
    gui_gfx_draw_rect(surface, b, 0x00404040);
    if (b.w > 4 && b.h > 4)
        gui_gfx_draw_rect(surface,
            (gui_rect_t){b.x + 1, b.y + 1, b.w - 2, b.h - 2}, 0x00808080);
    if (b.h >= GUI_SCROLLBAR_SIZE * 2) {
        gui_scrollbar_draw_arrow(surface,
            (gui_rect_t){b.x, b.y, b.w, GUI_SCROLLBAR_SIZE}, false);
        gui_scrollbar_draw_arrow(surface,
            (gui_rect_t){b.x, b.y + b.h - GUI_SCROLLBAR_SIZE,
                         b.w, GUI_SCROLLBAR_SIZE}, true);
    }
    thumb = gui_scrollbar_thumb_rect(bar);
    gui_gfx_fill_rect(surface, thumb, 0x00C0C0C0);
    gui_gfx_draw_line(surface, thumb.x, thumb.y,
                      thumb.x + thumb.w - 1, thumb.y, 0x00FFFFFF);
    gui_gfx_draw_line(surface, thumb.x, thumb.y,
                      thumb.x, thumb.y + thumb.h - 1, 0x00FFFFFF);
    gui_gfx_draw_line(surface, thumb.x, thumb.y + thumb.h - 1,
                      thumb.x + thumb.w - 1, thumb.y + thumb.h - 1,
                      0x00404040);
    gui_gfx_draw_line(surface, thumb.x + thumb.w - 1, thumb.y,
                      thumb.x + thumb.w - 1, thumb.y + thumb.h - 1,
                      0x00404040);
    if (thumb.w > 4 && thumb.h > 4) {
        gui_gfx_draw_line(surface, thumb.x + 1, thumb.y + 1,
                          thumb.x + thumb.w - 2, thumb.y + 1, 0x00DFDFDF);
        gui_gfx_draw_line(surface, thumb.x + 1, thumb.y + 1,
                          thumb.x + 1, thumb.y + thumb.h - 2, 0x00DFDFDF);
        gui_gfx_draw_line(surface, thumb.x + 1, thumb.y + thumb.h - 2,
                          thumb.x + thumb.w - 2, thumb.y + thumb.h - 2,
                          0x00808080);
    }
}

bool gui_scrollbar_handle_click_vertical(const gui_scrollbar_t *bar,
                                         int x, int y,
                                         uint32_t *new_value) {
    gui_rect_t b;
    gui_rect_t thumb;
    uint32_t value;
    uint32_t max_value;

    if (!bar || !new_value || !gui_rect_contains(bar->bounds, x, y))
        return false;
    if (bar->visible >= bar->total) {
        *new_value = 0;
        return true;
    }
    b = bar->bounds;
    value = bar->value;
    max_value = bar->total - bar->visible;
    thumb = gui_scrollbar_thumb_rect(bar);
    if (y < b.y + GUI_SCROLLBAR_SIZE) {
        if (value) value--;
    } else if (y >= b.y + b.h - GUI_SCROLLBAR_SIZE) {
        if (value < max_value) value++;
    } else if (y < thumb.y) {
        value = value > bar->visible ? value - bar->visible : 0;
    } else if (y >= thumb.y + thumb.h) {
        value += bar->visible;
        if (value > max_value) value = max_value;
    }
    *new_value = value;
    return true;
}

bool gui_scrollbar_handle_event_vertical(const gui_scrollbar_t *bar,
                                         gui_scrollbar_drag_t *drag,
                                         const gui_event_t *event,
                                         uint32_t wheel_step,
                                         uint32_t *new_value) {
    gui_rect_t thumb;
    uint32_t max_value;
    int track_y;
    int track_h;
    int thumb_top;
    uint32_t old_value;

    if (!bar || !drag || !event || !new_value) return false;
    old_value = bar->value;
    max_value = bar->total > bar->visible ? bar->total - bar->visible : 0;

    if (event->type == GUI_EVENT_MOUSE_WHEEL) {
        int64_t value;
        if (!wheel_step) wheel_step = 1;
        value = (int64_t)bar->value - (int64_t)event->dy * wheel_step;
        if (value < 0) value = 0;
        if ((uint64_t)value > max_value) value = max_value;
        *new_value = (uint32_t)value;
        if (*new_value != old_value)
            (void)system_sound_event_play(SYSTEM_SOUND_SCROLL);
        return true;
    }

    if (event->type == GUI_EVENT_MOUSE_DOWN &&
        event->button == 1 && gui_rect_contains(bar->bounds, event->x, event->y)) {
        thumb = gui_scrollbar_thumb_rect(bar);
        if (gui_rect_contains(thumb, event->x, event->y) && max_value) {
            drag->active = true;
            drag->grab_offset = event->y - thumb.y;
            *new_value = bar->value;
            return true;
        }
        {
            bool handled = gui_scrollbar_handle_click_vertical(
                bar, event->x, event->y, new_value);
            if (handled && *new_value != old_value)
                (void)system_sound_event_play(SYSTEM_SOUND_SCROLL);
            return handled;
        }
    }

    if (event->type == GUI_EVENT_MOUSE_MOVE && drag->active) {
        if (!(event->buttons & 1)) {
            drag->active = false;
            return false;
        }
        thumb = gui_scrollbar_thumb_rect(bar);
        track_y = bar->bounds.y + GUI_SCROLLBAR_SIZE;
        track_h = bar->bounds.h - GUI_SCROLLBAR_SIZE * 2 - thumb.h;
        if (track_h <= 0) {
            *new_value = 0;
            return true;
        }
        thumb_top = event->y - drag->grab_offset - track_y;
        if (thumb_top < 0) thumb_top = 0;
        if (thumb_top > track_h) thumb_top = track_h;
        *new_value = track_h > 0
#if defined(BLES_ARCH_ARM32_RPI3)
            ? (uint32_t)(((uint32_t)thumb_top * max_value) / (uint32_t)track_h)
#else
            ? (uint32_t)(((uint64_t)thumb_top * max_value) / (uint32_t)track_h)
#endif
            : 0;
        if (*new_value != old_value)
            (void)system_sound_event_play(SYSTEM_SOUND_SCROLL);
        return true;
    }

    if (event->type == GUI_EVENT_MOUSE_UP && event->button == 1 &&
        drag->active) {
        drag->active = false;
        *new_value = bar->value;
        return true;
    }
    return false;
}
