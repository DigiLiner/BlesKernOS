/* Pure drawing helpers shared with the Ring-3 graphical server. */
static int bk_gui_max_int(int left, int right) {
    return left > right ? left : right;
}

static int bk_gui_min_int(int left, int right) {
    return left < right ? left : right;
}

static uint32_t bk_gui_image_blend(uint32_t background, uint32_t pixel) {
    uint32_t alpha = pixel >> 24;
    if (alpha == 255U) return pixel & 0x00FFFFFFU;
    if (alpha == 0U) return background;
    {
        uint32_t inverse = 255U - alpha;
        uint32_t red = (((pixel >> 16) & 0xFFU) * alpha +
            ((background >> 16) & 0xFFU) * inverse) / 255U;
        uint32_t green = (((pixel >> 8) & 0xFFU) * alpha +
            ((background >> 8) & 0xFFU) * inverse) / 255U;
        uint32_t blue = ((pixel & 0xFFU) * alpha +
            (background & 0xFFU) * inverse) / 255U;
        return (red << 16) | (green << 8) | blue;
    }
}

void bk_gui_surface_draw_image(gui_surface_t *surface, gui_rect_t destination,
                               gui_rect_t clip, const gui_image_t *image) {
    int x0, y0, x1, y1;
    gui_rect_t safe_clip, storage;
    if (!surface || !surface->pixels || !image || !image->pixels ||
        !image->width || !image->height || destination.w <= 0 ||
        destination.h <= 0 || clip.w <= 0 || clip.h <= 0) return;
    storage = (gui_rect_t){surface->origin_x, surface->origin_y,
        surface->storage_width ? surface->storage_width : surface->width,
        surface->storage_height ? surface->storage_height : surface->height};
    if (!gui_rect_intersect(surface->clip, storage, &safe_clip)) return;
    x0 = bk_gui_max_int(destination.x, clip.x);
    y0 = bk_gui_max_int(destination.y, clip.y);
    x1 = bk_gui_min_int(destination.x + destination.w, clip.x + clip.w);
    y1 = bk_gui_min_int(destination.y + destination.h, clip.y + clip.h);
    x0 = bk_gui_max_int(x0, safe_clip.x);
    y0 = bk_gui_max_int(y0, safe_clip.y);
    x1 = bk_gui_min_int(x1, safe_clip.x + safe_clip.w);
    y1 = bk_gui_min_int(y1, safe_clip.y + safe_clip.h);
    if (x0 >= x1 || y0 >= y1) return;

    /* Fast 1:1 copy.  Full-frame software GL clients spend enough time in
     * SWRAST already; never route a plain copy through the generic scaler or
     * gui_gfx_putpixel(). */
    if (destination.w == (int)image->width &&
        destination.h == (int)image->height) {
        for (int y = y0; y < y1; y++) {
            uint32_t source_y = (uint32_t)(y - destination.y);
            uint32_t source_x = (uint32_t)(x0 - destination.x);
            const uint32_t *src = &image->pixels[source_y * image->width + source_x];
            uint32_t *dst = &surface->pixels[(uint32_t)(y - surface->origin_y) * surface->pitch +
                                             (uint32_t)(x0 - surface->origin_x)];
            for (int x = x0; x < x1; x++, src++, dst++) {
                uint32_t pixel = *src;
                if ((pixel >> 24) == 255U) *dst = pixel & 0x00FFFFFFU;
                else if ((pixel >> 24) != 0U) *dst = bk_gui_image_blend(*dst, pixel);
            }
        }
        return;
    }

    /* Dedicated integer 2x scaler.  Tux Racer's software profile renders at
     * 400x300 and displays at 800x600, so this is a hot path.  The common
     * fully visible/opaque case converts one source texel into four stores and
     * avoids every multiply/divide in the inner loop. */
    if (destination.w == (int)image->width * 2 &&
        destination.h == (int)image->height * 2 &&
        x0 == destination.x && y0 == destination.y &&
        x1 == destination.x + destination.w &&
        y1 == destination.y + destination.h) {
        for (uint32_t sy = 0U; sy < image->height; sy++) {
            const uint32_t *src = &image->pixels[sy * image->width];
            uint32_t *dst0 = &surface->pixels[
                (uint32_t)(destination.y + (int)(sy * 2U) - surface->origin_y) * surface->pitch +
                (uint32_t)(destination.x - surface->origin_x)];
            uint32_t *dst1 = dst0 + surface->pitch;
            for (uint32_t sx = 0U; sx < image->width; sx++) {
                uint32_t pixel = src[sx];
                uint32_t alpha = pixel >> 24;
                uint32_t dx = sx * 2U;
                if (alpha == 255U) {
                    uint32_t color = pixel & 0x00FFFFFFU;
                    dst0[dx] = color;
                    dst0[dx + 1U] = color;
                    dst1[dx] = color;
                    dst1[dx + 1U] = color;
                } else if (alpha != 0U) {
                    dst0[dx] = bk_gui_image_blend(dst0[dx], pixel);
                    dst0[dx + 1U] = bk_gui_image_blend(dst0[dx + 1U], pixel);
                    dst1[dx] = bk_gui_image_blend(dst1[dx], pixel);
                    dst1[dx + 1U] = bk_gui_image_blend(dst1[dx + 1U], pixel);
                }
            }
        }
        return;
    }

    /* Generic nearest-neighbour scaler.  The old implementation performed an
     * integer division for every destination pixel and called putpixel() for
     * every store.  Use 16.16 source coordinates and direct clipped writes;
     * resizing a software-rendered window is now cheap enough to be useful. */
    {
        uint32_t step_x = (((uint32_t)image->width << 16) /
                           (uint32_t)destination.w);
        uint32_t step_y = (((uint32_t)image->height << 16) /
                           (uint32_t)destination.h);
        uint32_t start_x = (uint32_t)(x0 - destination.x) * step_x;
        uint32_t source_y_fp = (uint32_t)(y0 - destination.y) * step_y;

        for (int y = y0; y < y1; y++, source_y_fp += step_y) {
            uint32_t source_y = source_y_fp >> 16;
            uint32_t source_x_fp = start_x;
            uint32_t *dst = &surface->pixels[(uint32_t)(y - surface->origin_y) * surface->pitch +
                                             (uint32_t)(x0 - surface->origin_x)];
            if (source_y >= image->height) source_y = image->height - 1U;
            for (int x = x0; x < x1; x++, dst++, source_x_fp += step_x) {
                uint32_t source_x = source_x_fp >> 16;
                uint32_t pixel;
                if (source_x >= image->width) source_x = image->width - 1U;
                pixel = image->pixels[source_y * image->width + source_x];
                if ((pixel >> 24) == 255U) *dst = pixel & 0x00FFFFFFU;
                else if ((pixel >> 24) != 0U) *dst = bk_gui_image_blend(*dst, pixel);
            }
        }
    }
}


void bk_gui_surface_draw_xrgb(gui_surface_t *surface, gui_rect_t destination,
                              gui_rect_t clip, const uint32_t *pixels,
                              uint16_t width, uint16_t height) {
    gui_gfx_draw_xrgb(surface, destination, clip, pixels, width, height);
}

void bk_gui_surface_draw_progress(gui_surface_t *surface, gui_rect_t rect,
                                  uint32_t percent, uint32_t style,
                                  uint32_t animation_phase) {
    gui_rect_t inner;
    int inner_w;
    int inner_h;

    if (!surface || rect.w < 8 || rect.h < 8) return;
    if (percent > 100U) percent = 100U;

    /* Doble bisel hundido, igual al resto de controles del escritorio. */
    bk_gui_surface_fill_rect(surface, rect, 0x00808080U);
    bk_gui_surface_fill_rect(surface,
        (gui_rect_t){rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2},
        0x00404040U);
    bk_gui_surface_fill_rect(surface,
        (gui_rect_t){rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4},
        0x00FFFFFFU);
    bk_gui_surface_fill_rect(surface,
        (gui_rect_t){rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6},
        0x00D8D8D8U);
    inner = (gui_rect_t){rect.x + 5, rect.y + 5,
                         rect.w - 10, rect.h - 10};
    inner_w = inner.w;
    inner_h = inner.h;
    if (inner_w <= 0 || inner_h <= 0) return;

    if (style == BK_GUI_PROGRESS_INSTALLER) {
        const uint32_t segments = 24U;
        const int gap = 2;
        int segment_w = (inner_w - (int)(segments - 1U) * gap)
                        / (int)segments;
        uint32_t filled = percent
            ? (percent * segments + 99U) / 100U : 0U;
        if (segment_w < 2) segment_w = 2;
        if (filled > segments) filled = segments;
        for (uint32_t i = 0U; i < segments; i++) {
            int x = inner.x + (int)i * (segment_w + gap);
            gui_rect_t block;
            if (x >= inner.x + inner.w) break;
            block = (gui_rect_t){x, inner.y,
                bk_gui_min_int(segment_w, inner.x + inner.w - x), inner.h};
            bk_gui_surface_fill_rect(surface, block,
                i < filled ? 0x000000A8U : 0x00D8D8D8U);
            if (i < filled && block.w > 2 && block.h > 2) {
                bk_gui_surface_fill_rect(surface,
                    (gui_rect_t){block.x + 1, block.y + 1,
                                 block.w - 2, 1}, 0x006080FFU);
                bk_gui_surface_fill_rect(surface,
                    (gui_rect_t){block.x + 1, block.y + 2,
                                 1, block.h - 3}, 0x003050D0U);
            }
        }
    } else {
        int fill_w = (inner_w * (int)percent) / 100;
        if (fill_w > 0) {
            gui_rect_t fill = {inner.x, inner.y, fill_w, inner.h};
            int shine_w = bk_gui_max_int(3, fill_w / 9);
            int travel = bk_gui_max_int(1, fill_w + shine_w);
            int shine_x = inner.x - shine_w
                          + (int)(animation_phase % 100U) * travel / 99;
            gui_rect_t shine;
            bk_gui_surface_fill_rect(surface, fill, 0x00000080U);
            if (fill.h > 2) {
                bk_gui_surface_fill_rect(surface,
                    (gui_rect_t){fill.x, fill.y, fill.w, 1}, 0x004040C0U);
            }
            shine.x = bk_gui_max_int(fill.x, shine_x);
            shine.y = fill.y + 1;
            shine.w = bk_gui_min_int(fill.x + fill.w,
                                     shine_x + shine_w) - shine.x;
            shine.h = bk_gui_max_int(1, fill.h - 2);
            if (shine.w > 0) {
                bk_gui_surface_fill_rect(surface, shine, 0x002020C0U);
            }
        }
    }
}

uint16_t bk_gui_text_width_px(const char *text, uint32_t length,
                              uint16_t pixel_height, bool monospace, bool bold) {
    return gui_font_text_width_px(text, length, pixel_height, monospace, bold);
}

void bk_gui_surface_draw_text_px(gui_surface_t *surface, int x, int y,
                                 const char *text, uint32_t length,
                                 uint32_t foreground, uint16_t pixel_height,
                                 bool bold, bool italic, bool monospace,
                                 gui_rect_t clip) {
    gui_font_draw_string_px_clipped(surface, x, y, text, length, foreground,
                                    pixel_height, bold, italic, monospace, clip);
}

