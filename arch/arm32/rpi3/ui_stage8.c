#include "ui_stage8.h"
#include "gfx_rpi.h"
#include "input_stage9.h"
#include "../../../kernel/include/gfx.h"
#include "../../../kernel/include/bootsplash.h"
#include "../../../kernel/include/memory.h"
#include "../../../kernel/include/mouse.h"
#include "../../../kernel/libc/include/stdio.h"
#include "../../../gui/gui.h"
#include "../../../programs/programs.h"

static gui_surface_t g_stage8_surface;
static gui_desktop_t g_stage8_desktop;
static bool g_stage8_video_ready;
static bool g_stage8_shell_ready;
static volatile uint32_t g_stage8_paint_requested;
static gui_event_queue_t g_stage9_events;

/* These are normally owned by gui/gui.c. Stage 8 deliberately ports the
 * desktop/compositor without pulling the x86 scheduler-driven gui_run loop. */
gui_desktop_t *gui_get_desktop(void) {
    return g_stage8_shell_ready ? &g_stage8_desktop : (gui_desktop_t *)0;
}

void gui_request_paint(void) {
    g_stage8_paint_requested = 1U;
}

bool gui_setup_mode(void) { return false; }
void gui_set_setup_mode(bool enabled) { (void)enabled; }

static void stage8_status_paint(gui_window_t *window, gui_surface_t *surface,
                                void *context) {
    gui_rect_t client;
    (void)context;
    if (!window || !surface) return;
    client = gui_window_content_rect(window);
    gui_gfx_fill_rect(surface, client, 0x00C0C0C0U);
    gui_font_draw_string_clipped(surface, client.x + 18, client.y + 22,
                                 "BlesKernOS ARM32", 0x00000000U, client);
    gui_font_draw_string_clipped(surface, client.x + 18, client.y + 48,
                                 "Raspberry Pi platform online", 0x00000000U,
                                 client);
    gui_font_draw_string_clipped(surface, client.x + 18, client.y + 70,
                                 "SD / FAT32 / VFS: OK", 0x00008000U, client);
    gui_font_draw_string_clipped(surface, client.x + 18, client.y + 92,
                                 "Desktop + compositor: native", 0x00008000U,
                                 client);
    gui_font_draw_string_clipped(surface, client.x + 18, client.y + 126,
                                 "USB/keyboard/mouse input: native", 0x00404040U,
                                 client);
    gui_font_draw_string_clipped(surface, client.x + 18, client.y + 148,
                                 "Preemptive ARM scheduler: online", 0x00008000U,
                                 client);
    gui_font_draw_string_clipped(surface, client.x + 18, client.y + 170,
                                 "USR/SVC/EM_ARM loader: Stage 11", 0x00008000U,
                                 client);
}

int rpi3_stage8_video_bootstrap(void) {
    if (g_stage8_video_ready) return 0;
    if (!rpi3_gfx_register()) {
        kprintf("[BLES:GFX] Stage 8 framebuffer registration FAILED\n");
        return -1;
    }
    gfx_init();
    if (!gfx_get_info() || gfx_get_info()->mode != GFX_MODE_RPI_FRAMEBUFFER) {
        kprintf("[BLES:GFX] Stage 8 framebuffer activation FAILED\n");
        return -2;
    }
    g_stage8_video_ready = true;
    kprintf("[BLES:GFX] early shared gfx online for real bootsplash\n");
    bootsplash_show("Inicializando BlesKernOS ARM...", 12U);
    (void)gfx_flush();
    return 0;
}

void rpi3_stage8_boot_status(const char *text, uint8_t progress) {
    if (!g_stage8_video_ready) return;
    bootsplash_show(text, progress);
    (void)gfx_flush();
}

int rpi3_stage8_shell_init(void) {
    gui_window_t *window;
    heap_info_t heap;

    if (!g_stage8_video_ready) {
        int rc = rpi3_stage8_video_bootstrap();
        if (rc != 0) return rc;
    }

    rpi3_stage8_boot_status("Preparando escritorio...", 82U);
    if (!gui_gfx_init(&g_stage8_surface)) {
        mm_get_info(&heap);
        kprintf("[BLES:GUI] Stage 8 gui_gfx_init FAILED free=%u KiB\n",
                (uint32_t)(heap.free_bytes / 1024U));
        return -3;
    }

    gui_desktop_init(&g_stage8_desktop, g_stage8_surface);
    g_stage8_shell_ready = true;
    mouse_set_bounds(g_stage8_surface.width, g_stage8_surface.height);
    mouse_set_position(g_stage8_surface.width / 2, g_stage8_surface.height / 2);
    gui_event_init(&g_stage9_events);
    gui_desktop_set_cursor_trail(&g_stage8_desktop, false);
    gui_desktop_set_drag_outline(&g_stage8_desktop, true);

    rpi3_stage8_boot_status("Cargando escritorio...", 88U);
    deskmanager_install(&g_stage8_desktop);
    rpi3_stage8_boot_status("Cargando barra de tareas...", 94U);
    deskbar_install(&g_stage8_desktop);

    window = gui_desktop_create_window(&g_stage8_desktop,
        (g_stage8_surface.width - 470) / 2,
        (g_stage8_surface.height - 270) / 2 - 12,
        470, 270, "BlesKernOS - Raspberry Pi");
    if (window) {
        gui_window_set_content(window, stage8_status_paint, NULL);
        gui_desktop_focus_window(&g_stage8_desktop, window);
    }

    rpi3_stage8_boot_status("READY", 100U);
    bootsplash_disable();
    gui_desktop_invalidate_all(&g_stage8_desktop);
    g_stage8_paint_requested = 1U;
    rpi3_stage8_service();
    (void)gfx_flush();

    kprintf("[BLES:GUI] real desktop.c/windows.c/compositor.c ONLINE\n");
    kprintf("[BLES:SHELL] real deskmanager.c + deskbar.c ONLINE\n");
    return 0;
}

void rpi3_stage8_service(void) {
    gui_event_t event;
    bool needs_paint;
    if (!g_stage8_shell_ready) return;

    /* Stage 12.7: the desktop is no longer a single-threaded object.  Native
       kernel frontends (Terminal, Calculator and later GUI apps) create and
       retire windows from their own scheduler tasks while this task handles
       input and compositor traversal.  Hold the recursive desktop lock across
       one complete GUI transaction so a worker cannot free/mutate an object
       after hit-testing but before dispatch/paint. */
    gui_desktop_paint_lock();

    gui_event_poll(&g_stage9_events);
    while (gui_event_next(&g_stage9_events, &event)) {
        gui_desktop_handle_event(&g_stage8_desktop, &event);
        g_stage8_paint_requested = 1U;
    }

    needs_paint = g_stage8_paint_requested ||
                  gui_desktop_has_dirty(&g_stage8_desktop);
    if (needs_paint) {
        g_stage8_paint_requested = 0U;
        /* gui_desktop_paint() acquires the same mutex.  The real klock layer
           is recursive by task, matching the x86 implementation. */
        gui_desktop_paint(&g_stage8_desktop);
    }

    gui_desktop_paint_unlock();
}
