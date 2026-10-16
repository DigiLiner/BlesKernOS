/* SPDX-License-Identifier: MIT
 * Firmware LFB fallback: identify historical PCI adapters, validate aperture,
 * keep the mode established by VBE. Never guess a scanout BAR or program PLLs.
 */
#include "../../include/driver.h"
#include "../../include/gfx_driver.h"
#include "../../include/memory.h"
#include "../../include/pci.h"
#include "../../include/vesa.h"
#include "../../include/vga.h"
static gfx_info_t boot;
static gfx_driver_ops_t ops;
typedef struct {
    uint16_t vendor, device;
    const char *name;
} family_t;
static const family_t families[] = {
#include "families/ati_vbe_ids.inc"
#include "families/nvidia_vbe_ids.inc"
#include "families/legacy_vbe_ids.inc"
};
static const family_t *identify(const pci_device_t *d) {
    if (!d || d->class_code != 3 || d->subclass != 0)
        return NULL;
    for (unsigned i = 0; i < sizeof(families) / sizeof(families[0]); i++)
        if (d->vendor_id == families[i].vendor && d->device_id == families[i].device)
            return &families[i];
    return NULL;
}
static bool valid_layout(const gfx_info_t *b) {
    if (!b->framebuffer || b->width < 320 || b->height < 200 || b->width > 4096 || b->height > 4096)
        return false;
    if (b->bpp != 8 && b->bpp != 16 && b->bpp != 24 && b->bpp != 32)
        return false;
    uint32_t row = (uint32_t)b->width * ((b->bpp + 7U) / 8U), size = (uint32_t)b->pitch * b->height;
    return b->pitch >= row && size <= 32U * 1024U * 1024U && b->framebuffer <= 0xffffffffU - size;
}
static bool aperture_contains(const pci_device_t *d, const gfx_info_t *b) {
    uint32_t length = (uint32_t)b->pitch * b->height;
    for (uint8_t i = 0; i < PCI_BAR_COUNT; i++) {
        pci_bar_info_t bar;
        if (!pci_get_bar_info(d, i, &bar) || bar.is_io || !bar.base || !bar.size)
            continue;
        if (bar.is_64 && (i + 1 >= PCI_BAR_COUNT || d->bars[i + 1] != 0))
            continue;
        if (b->framebuffer >= bar.base && b->framebuffer - bar.base <= bar.size &&
            length <= bar.size - (b->framebuffer - bar.base))
            return true;
    }
    return false;
}
static bool activate(gfx_info_t *info, uint16_t w, uint16_t h) {
    (void)w;
    (void)h;
    if (!info)
        return false;
    return vesa_attach_lfb(info, boot.framebuffer, boot.width, boot.height, boot.pitch, boot.bpp);
}
static bool modes(gfx_display_mode_t *out, uint32_t max, uint32_t *count) {
    if (!count)
        return false;
    *count = 1;
    if (out && max) {
        out[0].width = boot.width;
        out[0].height = boot.height;
        out[0].bpp = boot.bpp;
    }
    return true;
}
static bool set_mode(gfx_info_t *info, uint16_t w, uint16_t h, uint8_t bpp) {
    if (w != boot.width || h != boot.height || bpp != boot.bpp)
        return false;
    return activate(info, w, h);
}
/* The core performs its bounded CPU copy when this returns false. */
static bool present(const gfx_info_t *info, const uint32_t *pixels, uint32_t pitch,
                    const gfx_rect_t *rects, uint32_t count, uint32_t *fence) {
    (void)info;
    (void)pixels;
    (void)pitch;
    (void)rects;
    (void)count;
    (void)fence;
    return false;
}
static bool update(int x, int y, int w, int h) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    return true;
}
static bool flush(uint32_t *fence) {
    if (fence)
        *fence = 0;
    return true;
}
static bool wait_fence(uint32_t fence) { return fence == 0; }
static bool init(void) {
    if (mm_boot_vesa_read32(0) != VESA_BOOTINFO_MAGIC)
        return false;
    boot.mode = GFX_MODE_VESA_LFB;
    boot.framebuffer = mm_boot_vesa_read32(4);
    boot.width = mm_boot_vesa_read16(8);
    boot.height = mm_boot_vesa_read16(10);
    boot.pitch = mm_boot_vesa_read16(12);
    boot.bpp = mm_boot_vesa_read8(14);
    if (!valid_layout(&boot))
        return false;
    for (unsigned i = 0; i < pci_device_count(); i++) {
        const pci_device_t *d = pci_device_at(i);
        const family_t *f = identify(d);
        if (!f || !aperture_contains(d, &boot))
            continue;
        kmemset(&ops, 0, sizeof(ops));
        ops.abi_version = BK_GFX_DRIVER_ABI_VERSION;
        ops.descriptor_size = sizeof(ops);
        ops.name = f->name;
        ops.priority = 10;
        ops.activate = activate;
        ops.list_modes = modes;
        ops.set_mode = set_mode;
        ops.present_buffer = present;
        ops.update_rect = update;
        ops.flush = flush;
        ops.wait_fence = wait_fence;
        kprintf("[LEGACYFB] %s %x:%x firmware %ux%ux%u pitch=%u fb=%x\n", f->name, d->vendor_id,
                d->device_id, boot.width, boot.height, boot.bpp, boot.pitch, boot.framebuffer);
        return gfx_register_driver(&ops);
    }
    return false;
}
const bk_driver_module_t *bleskernos_driver_query(void) {
    static const bk_driver_module_t module = {
        BK_DRIVER_ABI_VERSION,
        sizeof(bk_driver_module_t),
        1,
        0,
        "Legacy firmware framebuffer",
        "Historical GPU families; exact PCI identities, verified firmware VBE aperture",
        init,
        NULL};
    return &module;
}
