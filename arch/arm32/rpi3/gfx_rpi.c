#include "gfx_rpi.h"
#include "framebuffer.h"
#include "cache.h"
#include "../../../kernel/include/gfx.h"
#include "../../../kernel/include/gfx_driver.h"

static uint32_t g_fence;

uint32_t rpi3_gfx_encode_rgb(uint32_t rgb) {
    const rpi3_framebuffer_t *fb = rpi3_framebuffer_get();
    uint32_t r = (rgb >> 16) & 0xFFU;
    uint32_t g = (rgb >> 8) & 0xFFU;
    uint32_t b = rgb & 0xFFU;
    /* pixel_order describes framebuffer bytes, not the numeric uint32_t.
       ARM is little-endian, so RGB bytes require 0x00BBGGRR stores. */
    if (!fb || fb->pixel_order == 1U)
        return (b << 16) | (g << 8) | r;
    return (r << 16) | (g << 8) | b;
}

uint32_t rpi3_gfx_decode_rgb(uint32_t native) {
    const rpi3_framebuffer_t *fb = rpi3_framebuffer_get();
    uint32_t r, g, b;
    native &= 0x00FFFFFFU;
    if (!fb || fb->pixel_order == 1U) {
        /* Native word 0x00BBGGRR -> canonical 0x00RRGGBB. */
        r = native & 0xFFU;
        g = (native >> 8) & 0xFFU;
        b = (native >> 16) & 0xFFU;
        return (r << 16) | (g << 8) | b;
    }
    return native;
}

static bool rpi_gfx_activate(gfx_info_t *info, uint16_t preferred_width,
                             uint16_t preferred_height) {
    const rpi3_framebuffer_t *fb = rpi3_framebuffer_get();
    (void)preferred_width;
    (void)preferred_height;
    if (!info || !fb || !fb->base || fb->depth != 32U ||
        !fb->width || !fb->height || fb->pitch > 0xFFFFU)
        return false;
    info->mode = GFX_MODE_RPI_FRAMEBUFFER;
    info->framebuffer = (uint32_t)(uintptr_t)fb->base;
    info->width = (uint16_t)fb->width;
    info->height = (uint16_t)fb->height;
    info->pitch = (uint16_t)fb->pitch;
    info->bpp = 32U;
    g_fence = 0U;
    return true;
}

static uint32_t rpi_gfx_caps(void) {
    return GFX_CAP_PRESENT_BUFFER | GFX_CAP_DIRTY_RECTS |
           GFX_CAP_PRESENT_CPU_COPY;
}

static void rpi_gfx_disable(void) { }

static bool rpi_gfx_list_modes(gfx_display_mode_t *modes, uint32_t max_modes,
                               uint32_t *count) {
    const rpi3_framebuffer_t *fb = rpi3_framebuffer_get();
    if (count) *count = fb ? 1U : 0U;
    if (!fb) return false;
    if (modes && max_modes) {
        modes[0].width = (uint16_t)fb->width;
        modes[0].height = (uint16_t)fb->height;
        modes[0].bpp = 32U;
    }
    return true;
}

static bool rpi_gfx_set_mode(gfx_info_t *info, uint16_t width,
                             uint16_t height, uint8_t bpp) {
    const rpi3_framebuffer_t *fb = rpi3_framebuffer_get();
    if (!fb || !info || width != fb->width || height != fb->height ||
        (bpp && bpp != 32U))
        return false;
    return rpi_gfx_activate(info, width, height);
}

static bool rpi_clip(const rpi3_framebuffer_t *fb, gfx_rect_t *r) {
    int32_t x2, y2;
    if (!fb || !r || r->w <= 0 || r->h <= 0) return false;
    x2 = r->x + r->w;
    y2 = r->y + r->h;
    if (r->x < 0) r->x = 0;
    if (r->y < 0) r->y = 0;
    if (x2 > (int32_t)fb->width) x2 = (int32_t)fb->width;
    if (y2 > (int32_t)fb->height) y2 = (int32_t)fb->height;
    r->w = x2 - r->x;
    r->h = y2 - r->y;
    return r->w > 0 && r->h > 0;
}

static void rpi_clean_rect(const rpi3_framebuffer_t *fb, const gfx_rect_t *r) {
    uint32_t row;
    if (!fb || !r || !fb->base) return;
    for (row = 0U; row < (uint32_t)r->h; row++) {
        const void *p = (const void *)(uintptr_t)
            ((uintptr_t)fb->base + ((uint32_t)r->y + row) * fb->pitch +
             (uint32_t)r->x * 4U);
        rpi3_cache_clean_range(p, (uint32_t)r->w * 4U);
    }
}

static bool rpi_gfx_present_buffer(const gfx_info_t *info,
                                   const uint32_t *pixels,
                                   uint32_t source_pitch,
                                   const gfx_rect_t *rects,
                                   uint32_t rect_count,
                                   uint32_t *fence_out) {
    const rpi3_framebuffer_t *fb = rpi3_framebuffer_get();
    uint32_t i;
    (void)info;
    if (!fb || !pixels || !source_pitch || !rects || !rect_count) return false;
    for (i = 0U; i < rect_count; i++) {
        gfx_rect_t r = rects[i];
        uint32_t y;
        if (!rpi_clip(fb, &r)) continue;
        for (y = 0U; y < (uint32_t)r.h; y++) {
            volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)
                ((uintptr_t)fb->base + ((uint32_t)r.y + y) * fb->pitch) + r.x;
            const uint32_t *src = pixels +
                ((uint32_t)r.y + y) * source_pitch + (uint32_t)r.x;
            uint32_t x;
            /* The GUI backbuffer is always 0x00RRGGBB. Convert every pixel
               to the byte order requested by VideoCore/QEMU. */
            for (x = 0U; x < (uint32_t)r.w; x++)
                dst[x] = rpi3_gfx_encode_rgb(src[x] & 0x00FFFFFFU);
        }
        rpi_clean_rect(fb, &r);
    }
    g_fence++;
    if (!g_fence) g_fence = 1U;
    if (fence_out) *fence_out = g_fence;
    return true;
}

static bool rpi_gfx_update_rect(int x, int y, int w, int h) {
    const rpi3_framebuffer_t *fb = rpi3_framebuffer_get();
    gfx_rect_t r = {x, y, w, h};
    if (!rpi_clip(fb, &r)) return true;
    rpi_clean_rect(fb, &r);
    return true;
}

static bool rpi_gfx_flush(uint32_t *fence_out) {
    rpi3_framebuffer_flush();
    g_fence++;
    if (!g_fence) g_fence = 1U;
    if (fence_out) *fence_out = g_fence;
    return true;
}

static bool rpi_gfx_wait_fence(uint32_t fence) {
    (void)fence;
    __asm__ volatile("dsb sy" ::: "memory");
    return true;
}

static const gfx_driver_ops_t g_rpi_gfx_ops = {
    .abi_version = BK_GFX_DRIVER_ABI_VERSION,
    .descriptor_size = sizeof(gfx_driver_ops_t),
    .name = "rpi-mailbox-fb",
    .priority = 1000U,
    .capabilities = GFX_CAP_PRESENT_BUFFER | GFX_CAP_DIRTY_RECTS |
                    GFX_CAP_PRESENT_CPU_COPY,
    .activate = rpi_gfx_activate,
    .get_capabilities = rpi_gfx_caps,
    .disable = rpi_gfx_disable,
    .list_modes = rpi_gfx_list_modes,
    .set_mode = rpi_gfx_set_mode,
    .present_buffer = rpi_gfx_present_buffer,
    .update_rect = rpi_gfx_update_rect,
    .flush = rpi_gfx_flush,
    .wait_fence = rpi_gfx_wait_fence,
};

bool rpi3_gfx_register(void) {
    return gfx_register_driver(&g_rpi_gfx_ops);
}
