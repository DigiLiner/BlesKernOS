#include "kernel/include/api.h"
#include "kernel/include/bootsplash.h"
#include "programs/programs.h"

bool vision_engine_step(gui_desktop_t *desktop);
void vision_about_restore(gui_desktop_t *desktop);
void bleskernos_program_main(gui_desktop_t *desktop) {
    gui_desktop_t *real_desktop;

    (void)desktop;

    bootsplash_show("VISION: ENTER RING3", 73);
    if (!bk_vision_claim()) {
        bk_proc_exit_status(1);
        return;
    }

    bootsplash_show("VISION: GRAPHICS ENGINE", 74);
    if (!bk_vision_initialize_engine()) {
        bk_vision_release();
        bk_proc_exit_status(2);
        return;
    }

    bootsplash_show("VISION: ENGINE READY", 75);
    real_desktop = bk_vision_desktop();
    if (!real_desktop) {
        bk_vision_release();
        bk_proc_exit_status(3);
        return;
    }

    /* Vision v3 owns desktop policy in Ring 3.  The kernel only exposes the
     * compositor/window/input substrate and a private trusted ABI while this
     * service is being loaded. */
    if (bk_vision_setup_mode()) {
        bootsplash_show("VISION: SETUP DESKTOP", 78);
        deskmanager_install_setup(real_desktop,
                                  "/SYSTEM/WALLPAPR/NOCHE.BMP");
    } else {
        bootsplash_show("VISION: DESKTOP", 78);
        deskmanager_install(real_desktop);
        bootsplash_show("VISION: DESKBAR", 86);
        deskbar_install(real_desktop);
        bootsplash_show("VISION: SCREENSAVER", 89);
        screensaverd_install(real_desktop);
    }

    vision_about_restore(real_desktop);
    bootsplash_show("VISION: START SHELL", 93);
    if (!bk_vision_start_shell()) {
        bk_vision_release();
        bk_proc_exit_status(4);
        return;
    }

    bk_vision_publish_desktop_preferences(deskmanager_get_background(),
        deskmanager_get_wallpaper_mode(), deskmanager_get_wallpaper_path());
    bootsplash_show("VISION: READY", 99);
    bk_vision_publish_ready();

    while (!bk_proc_exit_requested()) {
        uint32_t background;
        uint8_t mode;
        char wallpaper[VFS_MAX_PATH];
        if (bk_vision_take_desktop_preferences(&background, &mode,
                                               wallpaper, sizeof(wallpaper))) {
            if (!wallpaper[0]) deskmanager_set_background(background);
            else {
                deskmanager_set_wallpaper_mode(mode);
                (void)deskmanager_set_wallpaper(wallpaper);
            }
            bk_vision_publish_desktop_preferences(deskmanager_get_background(),
                deskmanager_get_wallpaper_mode(), deskmanager_get_wallpaper_path());
            kprintf("[VISION:PREFS] applied background=%x mode=%u wallpaper=%s\n",
                deskmanager_get_background(), deskmanager_get_wallpaper_mode(),
                deskmanager_get_wallpaper_path());
            bk_gui_invalidate_all(real_desktop);
            bk_gui_request_paint();
        }
        if (!vision_engine_step(real_desktop)) break;
        if (bk_vision_take_layout_refresh())
            deskmanager_refresh_layout();
    }

    bk_vision_release();
    bk_proc_exit_status(0);
}
