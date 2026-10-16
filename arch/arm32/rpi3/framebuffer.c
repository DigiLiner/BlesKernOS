#include "framebuffer.h"
#include "mailbox.h"
#include "cache.h"

#define TAG_SET_PHYS_WH      0x00048003U
#define TAG_SET_VIRT_WH      0x00048004U
#define TAG_SET_VIRT_OFFSET  0x00048009U
#define TAG_SET_DEPTH        0x00048005U
#define TAG_SET_PIXEL_ORDER  0x00048006U
#define TAG_ALLOCATE_BUFFER  0x00040001U
#define TAG_GET_PITCH        0x00040008U

static volatile uint32_t g_fb_msg[35] __attribute__((aligned(16)));
static rpi3_framebuffer_t g_fb;

/* BlesKernOS uses colors as 0x00RRGGBB.  The mailbox pixel-order tag,
 * however, describes the byte order in framebuffer memory.  On little-endian
 * ARM a 32-bit store reverses the apparent byte sequence of 0x00RRGGBB:
 *
 *   integer 0x00RRGGBB -> memory bytes BB GG RR 00
 *
 * Therefore an RGB framebuffer (R,G,B bytes) needs a BGR-shaped integer, and
 * a BGR framebuffer needs the native BlesKernOS 0x00RRGGBB value. */
static uint32_t pixel_value(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFFU;
    uint32_t g = (rgb >> 8) & 0xFFU;
    uint32_t b = rgb & 0xFFU;
    if (g_fb.pixel_order == 1U)
        return (b << 16) | (g << 8) | r; /* RGB byte order */
    return (r << 16) | (g << 8) | b;     /* BGR byte order */
}

int rpi3_framebuffer_init(uint32_t width, uint32_t height, uint32_t depth) {
    int rc;
    if (width == 0U || height == 0U || depth != 32U) return -1;

    g_fb_msg[0] = sizeof(g_fb_msg);
    g_fb_msg[1] = 0U;
    g_fb_msg[2] = TAG_SET_PHYS_WH; g_fb_msg[3] = 8U; g_fb_msg[4] = 8U;
    g_fb_msg[5] = width; g_fb_msg[6] = height;
    g_fb_msg[7] = TAG_SET_VIRT_WH; g_fb_msg[8] = 8U; g_fb_msg[9] = 8U;
    g_fb_msg[10] = width; g_fb_msg[11] = height;
    g_fb_msg[12] = TAG_SET_VIRT_OFFSET; g_fb_msg[13] = 8U; g_fb_msg[14] = 8U;
    g_fb_msg[15] = 0U; g_fb_msg[16] = 0U;
    g_fb_msg[17] = TAG_SET_DEPTH; g_fb_msg[18] = 4U; g_fb_msg[19] = 4U;
    g_fb_msg[20] = depth;
    g_fb_msg[21] = TAG_SET_PIXEL_ORDER; g_fb_msg[22] = 4U; g_fb_msg[23] = 4U;
    g_fb_msg[24] = 1U; /* request RGB, but honor firmware response */
    g_fb_msg[25] = TAG_ALLOCATE_BUFFER; g_fb_msg[26] = 8U; g_fb_msg[27] = 8U;
    g_fb_msg[28] = 4096U; g_fb_msg[29] = 0U;
    g_fb_msg[30] = TAG_GET_PITCH; g_fb_msg[31] = 4U; g_fb_msg[32] = 4U;
    g_fb_msg[33] = 0U;
    g_fb_msg[34] = 0U;

    rc = rpi3_mailbox_property_call(g_fb_msg, sizeof(g_fb_msg));
    if (rc != 0) return rc;
    if (g_fb_msg[28] == 0U || g_fb_msg[29] == 0U || g_fb_msg[33] == 0U) return -2;
    if (g_fb_msg[20] != 32U) return -3;

    g_fb.bus_address = g_fb_msg[28];
    g_fb.base = (volatile uint8_t *)(uintptr_t)rpi3_vc_bus_to_arm(g_fb.bus_address);
    g_fb.size = g_fb_msg[29];
    g_fb.width = g_fb_msg[5];
    g_fb.height = g_fb_msg[6];
    g_fb.depth = g_fb_msg[20];
    g_fb.pixel_order = g_fb_msg[24];
    g_fb.pitch = g_fb_msg[33];

    if ((uintptr_t)g_fb.base >= 0x3F000000U || g_fb.pitch < g_fb.width * 4U)
        return -4;
    if (g_fb.size < g_fb.pitch * g_fb.height)
        return -5;
    return 0;
}

const rpi3_framebuffer_t *rpi3_framebuffer_get(void) {
    return g_fb.base ? &g_fb : (const rpi3_framebuffer_t *)0;
}

void rpi3_framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t rgb) {
    volatile uint32_t *row;
    if (!g_fb.base || x >= g_fb.width || y >= g_fb.height) return;
    row = (volatile uint32_t *)(uintptr_t)((uintptr_t)g_fb.base + y * g_fb.pitch);
    row[x] = pixel_value(rgb);
}

void rpi3_framebuffer_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb) {
    uint32_t xx, yy, value;
    if (!g_fb.base || x >= g_fb.width || y >= g_fb.height) return;
    if (w > g_fb.width - x) w = g_fb.width - x;
    if (h > g_fb.height - y) h = g_fb.height - y;
    value = pixel_value(rgb);
    for (yy = 0U; yy < h; ++yy) {
        volatile uint32_t *row = (volatile uint32_t *)(uintptr_t)
            ((uintptr_t)g_fb.base + (y + yy) * g_fb.pitch);
        for (xx = 0U; xx < w; ++xx) row[x + xx] = value;
    }
}

void rpi3_framebuffer_clear(uint32_t rgb) {
    rpi3_framebuffer_fill_rect(0U, 0U, g_fb.width, g_fb.height, rgb);
}

void rpi3_framebuffer_flush(void) {
    if (!g_fb.base) return;
    rpi3_cache_clean_range((const void *)(uintptr_t)g_fb.base, g_fb.size);
}

void rpi3_framebuffer_test_pattern(void) {
    uint32_t bar_h;
    if (!g_fb.base) return;
    /* Deliberately resembles the old BlesKernOS desktop rather than a generic
       RGB test: teal desktop, light task bar, and three status tiles. */
    rpi3_framebuffer_clear(0x008080U);
    bar_h = g_fb.height >= 64U ? 40U : 20U;
    rpi3_framebuffer_fill_rect(0U, g_fb.height - bar_h, g_fb.width, bar_h, 0xC0C0C0U);
    rpi3_framebuffer_fill_rect(24U, 24U, 96U, 64U, 0x800000U);
    rpi3_framebuffer_fill_rect(136U, 24U, 96U, 64U, 0x008000U);
    rpi3_framebuffer_fill_rect(248U, 24U, 96U, 64U, 0x000080U);
    rpi3_framebuffer_flush();
}
