/* Include production Paint so the probe exercises its real renderer/events. */
#define bleskernos_program_main paint_application_main
#ifndef PAINT_SOURCE_PATH
#define PAINT_SOURCE_PATH "../../programs/paint.c"
#endif
#include PAINT_SOURCE_PATH
#undef bleskernos_program_main

static unsigned probe_failures;
static void probe_check(bool ok, const char *name) {
    kprintf("[PAINTPROBE] %s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) probe_failures++;
}

void bleskernos_program_main(gui_desktop_t *desktop) {
    paint_state_t *st = bk_sys_alloc_zero(sizeof(*st));
    uint32_t *pixels = bk_sys_alloc_zero(800U * 600U * sizeof(uint32_t));
    if (!st || !pixels) {
        kprintf("[PAINTPROBE] allocation: FAIL\n");
        return;
    }
    st->desktop = desktop;
    st->canvas = bk_sys_alloc_zero(PAINT_CANVAS_W * PAINT_CANVAS_H * sizeof(uint32_t));
    st->window = bk_gui_create_window(desktop, 85, 45, PAINT_WINDOW_W, PAINT_WINDOW_H, "Paint probe");
    if (!st->canvas || !st->window) {
        kprintf("[PAINTPROBE] setup: FAIL\n");
        return;
    }
    bk_gui_add_menu(st->window, "File");
    st->brush_size = 1;
    paint_set_default_filename(st);
    paint_canvas_clear(st, PAINT_COLOR_WHITE);
    static const char *icons[] = {"16/FilePencil", "16/Pbrush1", "16/Mspaint", "16/Delete", "16/Save"};
    for (unsigned i = 0; i < 5; i++)
        st->toolbar_icon_loaded[i] = bk_graphics_icon_load(icons[i], &st->toolbar_icons[i]);
    gui_surface_t surface = {.pixels=pixels, .width=800, .height=600, .pitch=800,
                            .clip={0,0,800,600}};
    gui_rect_t client = bk_gui_window_content_rect_raw(st->window);
    surface.origin_x = client.x; surface.origin_y = client.y;
    surface.storage_width = client.w; surface.storage_height = client.h;
    surface.pitch = client.w; surface.clip = client;
#ifdef PAINT_PROBE_BASELINE
    const unsigned frames = 1;
#else
    const unsigned frames = 10;
#endif
    kprintf("[PAINTPROBE] BEGIN frames=%u\n", frames);
    uint32_t start = bk_sys_uptime_ms();
    for (unsigned frame = 0; frame < frames; frame++) paint_content(st->window, &surface, st);
    uint32_t elapsed = bk_sys_uptime_ms() - start;
    unsigned icons_alive = 0;
    for (unsigned i = 0; i < 5; i++)
        if (st->toolbar_icon_loaded[i] && st->toolbar_icons[i].pixels) icons_alive++;
    kprintf("[PAINTPROBE] frames=%u render_ms=%u icons_alive=%u\n", frames, elapsed, icons_alive);
    probe_check(icons_alive == 5, "icons survive repaints");

#ifndef PAINT_PROBE_BASELINE
    /* Tight, offset storage and padded rows catch writes outside the clip. */
    uint32_t guarded[16 * 9 + 8];
    for (unsigned i = 0; i < sizeof(guarded)/sizeof(guarded[0]); i++) guarded[i] = 0x12345678U;
    for (unsigned i = 0; i < PAINT_CANVAS_W * PAINT_CANVAS_H; i++) st->canvas[i] = i;
    gui_surface_t tile = {.pixels=guarded+4,.width=800,.height=600,.pitch=16,
                         .clip={303,202,5,4},.origin_x=300,.origin_y=200,
                         .storage_width=11,.storage_height=9};
    paint_blit_canvas(st, &tile, (gui_rect_t){295,198,PAINT_CANVAS_W,PAINT_CANVAS_H});
    bool safe = true;
    for (unsigned i = 0; i < sizeof(guarded)/sizeof(guarded[0]); i++) {
        uint32_t expected = 0x12345678U;
        int row = ((int)i - 4) / 16, col = ((int)i - 4) % 16;
        if (i >= 4 && i < 4+16*9 && row >= 2 && row < 6 && col >= 3 && col < 8)
            expected = (row + 2) * PAINT_CANVAS_W + col + 5;
        if (guarded[i] != expected) safe = false;
    }
    probe_check(safe, "clipped blit respects origin pitch and guard pixels");
#endif

    paint_canvas_clear(st, PAINT_COLOR_WHITE);
    gui_rect_t canvas = paint_canvas_rect(st->window);
    gui_event_t event = {.type=GUI_EVENT_MOUSE_DOWN, .x=canvas.x+20, .y=canvas.y+20,
                         .button=MOUSE_LEFT_BUTTON, .buttons=MOUSE_LEFT_BUTTON};
    paint_event(st->window, &event, st);
    event.type=GUI_EVENT_MOUSE_UP;event.buttons=0;event.x=canvas.x+200;
    paint_event(st->window, &event, st);
    bool continuous = true;
    for (int x=20;x<=200;x++) if (st->canvas[20*PAINT_CANVAS_W+x] != PAINT_COLOR_BLACK) continuous=false;
    probe_check(continuous && !st->mouse_down, "release includes final pencil segment");

    event.type=GUI_EVENT_MOUSE_DOWN;event.buttons=MOUSE_LEFT_BUTTON;
    event.x=canvas.x+200;event.y=canvas.y+80;
    paint_event(st->window,&event,st);
    event.type=GUI_EVENT_MOUSE_MOVE;event.x=canvas.x+400;
    paint_event(st->window,&event,st);
    probe_check(st->canvas[80*PAINT_CANVAS_W+319] == PAINT_COLOR_BLACK, "drag reaches canvas edge");
    event.buttons=0; /* Recover a release whose mouse-up was coalesced/lost. */
    paint_event(st->window,&event,st);
    probe_check(!st->mouse_down, "released drag cannot keep painting");

    bk_sys_free(pixels);
    paint_cleanup(st);
    kprintf("[PAINTPROBE] DONE failures=%u\n", probe_failures);
    bk_app_launch("/SYSTEM/PROGRAMS/PAINT.BEX", NULL);
}
