#include "ui_stage7.h"
#include "gfx_rpi.h"
#include "../../../kernel/include/gfx.h"
#include "../../../kernel/include/memory.h"
#include "../../../kernel/include/vga.h"
#include "../../../gui/gui.h"

static gui_surface_t g_stage7_surface;

static void stage7_bevel(gui_surface_t *s, gui_rect_t r) {
    gui_gfx_fill_rect(s, r, 0x00C0C0C0U);
    gui_gfx_fill_rect(s, (gui_rect_t){r.x, r.y, r.w, 2}, 0x00FFFFFFU);
    gui_gfx_fill_rect(s, (gui_rect_t){r.x, r.y, 2, r.h}, 0x00FFFFFFU);
    gui_gfx_fill_rect(s, (gui_rect_t){r.x, r.y + r.h - 2, r.w, 2}, 0x00404040U);
    gui_gfx_fill_rect(s, (gui_rect_t){r.x + r.w - 2, r.y, 2, r.h}, 0x00404040U);
}

static void stage7_draw_desktop(gui_surface_t *s) {
    gui_rect_t screen;
    gui_rect_t taskbar;
    gui_rect_t window;
    gui_rect_t title;
    int cx;
    int cy;

    screen = (gui_rect_t){0, 0, s->width, s->height};
    gui_gfx_set_clip(s, screen);
    gui_gfx_fill_rect(s, screen, 0x00008080U);

    taskbar = (gui_rect_t){0, s->height - 40, s->width, 40};
    gui_gfx_fill_rect(s, taskbar, 0x00C0C0C0U);
    gui_gfx_fill_rect(s, (gui_rect_t){0, taskbar.y, taskbar.w, 2}, 0x00FFFFFFU);
    stage7_bevel(s, (gui_rect_t){6, taskbar.y + 6, 82, 28});
    gui_font_draw_string(s, 20, taskbar.y + 16, "BlesKern", 0x00000000U,
                         0x00C0C0C0U, false);

    cx = s->width / 2;
    cy = (s->height - 40) / 2;
    window = (gui_rect_t){cx - 250, cy - 150, 500, 300};
    stage7_bevel(s, window);
    title = (gui_rect_t){window.x + 5, window.y + 5, window.w - 10, 24};
    gui_gfx_fill_gradient(s, title, 0x00000080U, 0x000040A0U);
    gui_font_draw_string(s, title.x + 8, title.y + 8,
                         "BlesKernOS - ARM32", 0x00FFFFFFU, 0U, false);

    gui_font_draw_string_scaled(s, window.x + 24, window.y + 52,
                                "BlesKernOS", 0x00000000U, 2);
    gui_font_draw_string(s, window.x + 24, window.y + 92,
                         "Raspberry Pi graphics backend online", 0x00000000U,
                         0x00C0C0C0U, false);
    gui_font_draw_string(s, window.x + 24, window.y + 116,
                         "Shared kernel/drivers/core/gfx.c", 0x00000000U,
                         0x00C0C0C0U, false);
    gui_font_draw_string(s, window.x + 24, window.y + 136,
                         "Shared gui/gfx.c + gui/font.c", 0x00000000U,
                         0x00C0C0C0U, false);
    gui_font_draw_string(s, window.x + 24, window.y + 164,
                         "SD + FAT32 + VFS: online", 0x00008000U,
                         0x00C0C0C0U, false);

    stage7_bevel(s, (gui_rect_t){window.x + window.w - 118,
                                 window.y + window.h - 44, 92, 26});
    gui_font_draw_string(s, window.x + window.w - 92,
                         window.y + window.h - 35,
                         "OK", 0x00000000U, 0x00C0C0C0U, false);
}

int rpi3_stage7_ui_init(void) {
    heap_info_t heap;

    if (!rpi3_gfx_register()) {
        kprintf("[BLES:GFX] Raspberry framebuffer driver registration FAILED\n");
        return -1;
    }
    gfx_init();
    if (gfx_get_info()->mode != GFX_MODE_RPI_FRAMEBUFFER) {
        kprintf("[BLES:GFX] shared gfx core did not activate Raspberry backend\n");
        return -2;
    }
    kprintf("[BLES:GFX] shared gfx.c ONLINE driver=%s %ux%ux%u\n",
            gfx_driver_name(), (uint32_t)gfx_get_info()->width,
            (uint32_t)gfx_get_info()->height, (uint32_t)gfx_get_info()->bpp);

    if (!gui_gfx_init(&g_stage7_surface)) {
        mm_get_info(&heap);
        kprintf("[BLES:GUI] gui_gfx_init FAILED heap_free=%u KiB\n",
                (uint32_t)(heap.free_bytes / 1024U));
        return -3;
    }

    stage7_draw_desktop(&g_stage7_surface);
    gui_gfx_present(&g_stage7_surface);
    (void)gfx_flush();
    kprintf("[BLES:GUI] shared gui/gfx.c + gui/font.c ONLINE surface=%ux%u\n",
            (uint32_t)g_stage7_surface.width,
            (uint32_t)g_stage7_surface.height);
    return 0;
}
