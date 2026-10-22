/* Standalone native example: no test files, host services or private headers. */
#include <bleskernos_lbs.h>
#include <bleskernos_api.h>
#include <bleskernos_ngs3d.h>
#include <dlfcn.h>
#include <GL/gl.h>
static void *vision_library, *graphics_library;
const bk_lbs_requirement_t bk_lbs_requirements[] = {
    {"VISION.LBS", 1, &vision_library}, {"NGS3D.LBS", 1, &graphics_library}, {0, 0, 0}};
#define IMPORT(module, fn) __typeof__(&fn) p_##fn = (void *)dlsym(module, #fn)
void bk_lbs_program_main(void *argument) {
    (void)argument;
    IMPORT(vision_library, bk_gui_desktop);
    IMPORT(vision_library, bk_gui_create_window);
    IMPORT(vision_library, bk_gui_destroy_window);
    IMPORT(graphics_library, bk_ngs3d_create);
    IMPORT(graphics_library, bk_ngs3d_destroy);
    IMPORT(graphics_library, bk_ngs3d_vertices_create);
    IMPORT(graphics_library, bk_ngs3d_triangles);
    IMPORT(graphics_library, bk_ngs3d_present);
    IMPORT(graphics_library, bk_ngs3d_get_proc);
    if (!p_bk_gui_desktop || !p_bk_gui_create_window || !p_bk_gui_destroy_window ||
        !p_bk_ngs3d_create || !p_bk_ngs3d_destroy || !p_bk_ngs3d_vertices_create ||
        !p_bk_ngs3d_triangles || !p_bk_ngs3d_present || !p_bk_ngs3d_get_proc) {
        bk_sys_log("[LBSTRI] FAIL symbols\n");
        return;
    }
    bk_ngs3d_context_t *context = p_bk_ngs3d_create(320, 200, BK_NGS3D_AUTO);
    if (!context) {
        bk_sys_log("[LBSTRI] FAIL context\n");
        return;
    }
    bk_gui_desktop_t *desktop = p_bk_gui_desktop();
    bk_gui_window_t *window =
        p_bk_gui_create_window(desktop, 120, 100, 344, 240, "LBS 3D - Mesa AUTO");
    static const float triangle[] = {-.8f, -.7f, 0, .8f, -.7f, 0, 0, .8f, 0};
    bk_ngs3d_vertices_t *vertices = p_bk_ngs3d_vertices_create(context, triangle, 3);
    __typeof__(&glClearColor) color = p_bk_ngs3d_get_proc(context, "glClearColor");
    __typeof__(&glClear) clear = p_bk_ngs3d_get_proc(context, "glClear");
    bool ok = window && vertices && color && clear;
    if (ok) {
        color(.08f, .12f, .25f, 1);
        clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ok = !p_bk_ngs3d_triangles(context, vertices) && !p_bk_ngs3d_present(context, window);
        if (ok) {
            bk_sys_log("[LBSTRI] FRAME READY\n");
            bk_sys_sleep_ms(1800);
        }
    }
    p_bk_ngs3d_destroy(context); /* Includes vertex storage and GPU resources. */
    if (window)
        p_bk_gui_destroy_window(desktop, window);
    bk_sys_log(ok ? "[LBSTRI] ALL PASS\n" : "[LBSTRI] FAIL frame\n");
}
