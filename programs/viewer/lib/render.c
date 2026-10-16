#include "render.h"

uint32_t vr_rgb565_to_xrgb(uint16_t color) {
    uint32_t r = (uint32_t)((color >> 11) & 0x1FU);
    uint32_t g = (uint32_t)((color >> 5) & 0x3FU);
    uint32_t b = (uint32_t)(color & 0x1FU);
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

const char *vr_quality_name(vr_quality_t quality) {
    switch (quality) {
        case VR_QUALITY_AUTO: return "Auto";
        case VR_QUALITY_EXACT: return "Exact";
        case VR_QUALITY_FINE: return "Fine";
        case VR_QUALITY_FAST: return "Fast";
        case VR_QUALITY_TURBO: return "Turbo";
        case VR_QUALITY_ULTRA: return "Ultra";
        default: return "Auto";
    }
}

uint8_t vr_quality_block(vr_quality_t quality, const vv_video_t *video,
                         bk_gui_rect_t target) {
    uint32_t fps = 0U;
    uint32_t pixels;
    if (quality == VR_QUALITY_EXACT) return 1U;
    if (quality == VR_QUALITY_FINE) return 4U;
    if (quality == VR_QUALITY_FAST) return 8U;
    if (quality == VR_QUALITY_TURBO) return 16U;
    if (quality == VR_QUALITY_ULTRA) return 32U;
    if (video && video->fps_den) fps = video->fps_num / video->fps_den;
    pixels = target.w > 0 && target.h > 0 ? (uint32_t)target.w * (uint32_t)target.h : 0U;
    if (vv_is_high_performance_native(video)) {
        uint32_t native = vv_native_block_width(video);
        if (fps >= 90U && native < 32U) return 32U;
        if (fps >= 50U && native < 16U) return 16U;
        if (fps >= 25U && native < 8U) return 8U;
        return native >= 4U && native <= 32U ? (uint8_t)native : 8U;
    }
    if (fps >= 90U) return 32U;
    if (fps >= 50U || pixels > 400000U) return 16U;
    if (fps >= 25U || pixels > 180000U) return 8U;
    return 4U;
}


static bool draw_full_color_image(bk_gui_surface_t *surface, vv_video_t *video,
                                  bk_gui_rect_t target, vr_quality_t quality,
                                  vr_stats_t *stats) {
    const bk_gui_image_t *image;
    uint32_t fps = 0U;
    uint32_t pixels;
    bool prefer = false;
    if (!surface || !video || target.w <= 0 || target.h <= 0) return false;
    if (video->fps_den) fps = video->fps_num / video->fps_den;
    pixels = (uint32_t)target.w * (uint32_t)target.h;

    if (quality == VR_QUALITY_EXACT || quality == VR_QUALITY_FINE) {
        prefer = true;
    } else if (quality == VR_QUALITY_AUTO) {
        if (video->format == VV_FORMAT_AVI && video->avi.mjpeg)
            prefer = pixels <= 800000U && fps <= 60U;
        else if (video->format == VV_FORMAT_MPEG1)
            prefer = pixels <= 500000U && fps <= 30U;
        else
            prefer = (pixels <= 320000U && fps <= 35U) ||
                     (pixels <= 90000U && fps <= 60U);
    }
    if (!prefer) return false;
    image = vv_frame_image(video);
    if (!image || !image->pixels) return false;
    /* Opaque movie frames do not need per-pixel alpha composition.
     * The OS chooses accelerated blit/scaling when its active driver can. */
    bk_gui_surface_draw_xrgb(surface, target, target, image->pixels,
                             image->width, image->height);
    if (stats) {
        stats->rectangles = 1U;
        stats->samples = pixels;
        stats->block_px = 1U;
    }
    return true;
}

static bool draw_native_blocks(bk_gui_surface_t *surface, vv_video_t *video,
                               bk_gui_rect_t target, vr_stats_t *stats) {
    uint32_t cols = vv_native_block_cols(video);
    uint32_t rows = vv_native_block_rows(video);
    uint32_t source_bw = vv_native_block_width(video);
    uint32_t source_bh = vv_native_block_height(video);
    uint32_t by;
    if (!cols || !rows) return false;
    for (by = 0; by < rows; by++) {
        uint32_t bx = 0;
        while (bx < cols) {
            uint16_t color = vv_native_block_color(video, by * cols + bx);
            uint32_t run = 1U;
            uint32_t x0, x1, y0, y1;
            while (bx + run < cols &&
                   vv_native_block_color(video, by * cols + bx + run) == color)
                run++;
            x0 = bx * source_bw;
            x1 = (bx + run) * source_bw;
            y0 = by * source_bh;
            y1 = (by + 1U) * source_bh;
            if (x1 > video->width) x1 = video->width;
            if (y1 > video->height) y1 = video->height;
            {
                int dx0 = target.x + (int)(x0 * (uint32_t)target.w / video->width);
                int dx1 = target.x + (int)(x1 * (uint32_t)target.w / video->width);
                int dy0 = target.y + (int)(y0 * (uint32_t)target.h / video->height);
                int dy1 = target.y + (int)(y1 * (uint32_t)target.h / video->height);
                if (dx1 <= dx0) dx1 = dx0 + 1;
                if (dy1 <= dy0) dy1 = dy0 + 1;
                bk_gui_surface_fill_rect(surface,
                    (bk_gui_rect_t){dx0, dy0, dx1 - dx0, dy1 - dy0},
                    vr_rgb565_to_xrgb(color));
                if (stats) stats->rectangles++;
            }
            bx += run;
        }
    }
    if (stats) {
        stats->samples = cols * rows;
        stats->block_px = (uint8_t)source_bw;
    }
    return true;
}

static bool draw_exact(bk_gui_surface_t *surface, vv_video_t *video,
                       bk_gui_rect_t target, vr_stats_t *stats) {
    int dy;
    if (target.w <= 0 || target.h <= 0) return false;
    for (dy = 0; dy < target.h; dy++) {
        uint32_t sy = (uint32_t)((uint32_t)dy * video->height / (uint32_t)target.h);
        int dx = 0;
        while (dx < target.w) {
            uint32_t sx = (uint32_t)((uint32_t)dx * video->width / (uint32_t)target.w);
            uint16_t color = vv_sample_rgb565(video, sx, sy);
            int run = 1;
            while (dx + run < target.w) {
                uint32_t nsx = (uint32_t)((uint32_t)(dx + run) * video->width /
                                          (uint32_t)target.w);
                if (vv_sample_rgb565(video, nsx, sy) != color) break;
                run++;
            }
            bk_gui_surface_fill_rect(surface,
                (bk_gui_rect_t){target.x + dx, target.y + dy, run, 1},
                vr_rgb565_to_xrgb(color));
            if (stats) { stats->rectangles++; stats->samples += (uint32_t)run; }
            dx += run;
        }
    }
    if (stats) stats->block_px = 1U;
    return true;
}

static bool draw_block_sampled(bk_gui_surface_t *surface, vv_video_t *video,
                               bk_gui_rect_t target, uint8_t block,
                               vr_stats_t *stats) {
    int y;
    if (!block) block = 8U;
    for (y = 0; y < target.h; y += block) {
        int cell_h = block;
        int x = 0;
        if (y + cell_h > target.h) cell_h = target.h - y;
        while (x < target.w) {
            int cell_w = block;
            int center_x, center_y;
            uint32_t sx, sy;
            uint16_t color;
            int run_cells = 1;
            if (x + cell_w > target.w) cell_w = target.w - x;
            center_x = x + cell_w / 2;
            center_y = y + cell_h / 2;
            sx = (uint32_t)((uint32_t)center_x * video->width / (uint32_t)target.w);
            sy = (uint32_t)((uint32_t)center_y * video->height / (uint32_t)target.h);
            if (sx >= video->width) sx = video->width - 1U;
            if (sy >= video->height) sy = video->height - 1U;
            color = vv_sample_rgb565(video, sx, sy);
            if (stats) stats->samples++;

            while (x + run_cells * block < target.w) {
                int nx = x + run_cells * block;
                int nw = block;
                int ncx;
                uint32_t nsx;
                uint16_t nc;
                if (nx + nw > target.w) nw = target.w - nx;
                ncx = nx + nw / 2;
                nsx = (uint32_t)((uint32_t)ncx * video->width / (uint32_t)target.w);
                if (nsx >= video->width) nsx = video->width - 1U;
                nc = vv_sample_rgb565(video, nsx, sy);
                if (stats) stats->samples++;
                if (nc != color) break;
                run_cells++;
            }
            {
                int rw = run_cells * block;
                if (x + rw > target.w) rw = target.w - x;
                bk_gui_surface_fill_rect(surface,
                    (bk_gui_rect_t){target.x + x, target.y + y, rw, cell_h},
                    vr_rgb565_to_xrgb(color));
                if (stats) stats->rectangles++;
                x += rw;
            }
        }
    }
    if (stats) stats->block_px = block;
    return true;
}

bool vr_draw_frame(bk_gui_surface_t *surface, vv_video_t *video,
                   bk_gui_rect_t target, vr_quality_t quality,
                   vr_stats_t *stats) {
    uint8_t block;
    if (!surface || !video || target.w <= 0 || target.h <= 0) return false;
    if (stats) { stats->rectangles = 0U; stats->samples = 0U; stats->block_px = 0U; }
    if (draw_full_color_image(surface, video, target, quality, stats))
        return true;
    block = vr_quality_block(quality, video, target);
    if (vv_is_high_performance_native(video)) {
        uint32_t native = vv_native_block_width(video);
        if (quality == VR_QUALITY_EXACT || block <= native)
            return draw_native_blocks(surface, video, target, stats);
        return draw_block_sampled(surface, video, target, block, stats);
    }
    if (block <= 1U) return draw_exact(surface, video, target, stats);
    return draw_block_sampled(surface, video, target, block, stats);
}
