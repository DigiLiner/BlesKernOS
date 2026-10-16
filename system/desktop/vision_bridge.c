#include "programs/programs.h"
#include "kernel/include/vfs.h"
#include "kernel/include/memory.h"
#include "kernel/include/klock.h"
#include "gui/gui.h"

/*
 * Small kernel-side compatibility bridge left behind after DeskManager moved
 * into VISION.BEX.  Legacy kernel and public-API callers still need stable
 * desktop preference symbols, but desktop painting/policy itself belongs to
 * Vision from v3 onward.
 */
#define VISION_DESK_DEFAULT_BACKGROUND 0x00204070U
#define VISION_DESK_DEFAULT_WALLPAPER  "/SYSTEM/WALLPAPR/CLASSIC.BMP"

static uint32_t g_vision_background = VISION_DESK_DEFAULT_BACKGROUND;
static uint8_t g_vision_wallpaper_mode = DESK_WALLPAPER_TILE;
static char g_vision_wallpaper[VFS_MAX_PATH] = VISION_DESK_DEFAULT_WALLPAPER;

static kspinlock_t g_preferences_lock = KSPINLOCK_INITIALIZER;
static bool g_preferences_pending;

bool bk_vision_take_desktop_preferences(uint32_t *background, uint8_t *mode,
                                       char *path, uint32_t capacity) {
    if (!background || !mode || !path || !capacity) return false;
    uint32_t flags = kspin_lock_irqsave(&g_preferences_lock);
    bool pending = g_preferences_pending;
    if (pending) {
        *background = g_vision_background;
        *mode = g_vision_wallpaper_mode;
        kstrncpy(path, g_vision_wallpaper, capacity - 1U);
        path[capacity - 1U] = '\0';
        g_preferences_pending = false;
    }
    kspin_unlock_irqrestore(&g_preferences_lock, flags);
    return pending;
}

void bk_vision_publish_desktop_preferences(uint32_t background, uint8_t mode,
                                           const char *path) {
    uint32_t flags = kspin_lock_irqsave(&g_preferences_lock);
    /* Do not overwrite a newer request while Vision is decoding a bitmap. */
    if (!g_preferences_pending) {
        g_vision_background = background;
        g_vision_wallpaper_mode = mode;
        kstrncpy(g_vision_wallpaper, path ? path : "", sizeof(g_vision_wallpaper) - 1U);
        g_vision_wallpaper[sizeof(g_vision_wallpaper) - 1U] = '\0';
    }
    kspin_unlock_irqrestore(&g_preferences_lock, flags);
}

void deskmanager_set_background(uint32_t color) {
    uint32_t flags = kspin_lock_irqsave(&g_preferences_lock);
    g_vision_background = color & 0x00FFFFFFU;
    if (!g_vision_background)
        g_vision_background = VISION_DESK_DEFAULT_BACKGROUND;
    g_vision_wallpaper[0] = '\0';
    g_preferences_pending = true;
    kspin_unlock_irqrestore(&g_preferences_lock, flags);
    gui_request_paint();
}

uint32_t deskmanager_get_background(void) {
    return g_vision_background;
}

const char *deskmanager_get_wallpaper_path(void) {
    return g_vision_wallpaper;
}

bool deskmanager_set_wallpaper(const char *path) {
    vfs_dir_entry_t info;
    if (!path || !path[0]) return false;
    if (!vfs_stat(path, &info) || info.type != VFS_NODE_FILE) return false;
    uint32_t flags = kspin_lock_irqsave(&g_preferences_lock);
    kstrncpy(g_vision_wallpaper, path, sizeof(g_vision_wallpaper) - 1U);
    g_vision_wallpaper[sizeof(g_vision_wallpaper) - 1U] = '\0';
    g_preferences_pending = true;
    kspin_unlock_irqrestore(&g_preferences_lock, flags);
    gui_request_paint();
    return true;
}

void deskmanager_set_wallpaper_mode(uint8_t mode) {
    if (mode > DESK_WALLPAPER_FILL) mode = DESK_WALLPAPER_STRETCH;
    uint32_t flags = kspin_lock_irqsave(&g_preferences_lock);
    g_vision_wallpaper_mode = mode;
    g_preferences_pending = true;
    kspin_unlock_irqrestore(&g_preferences_lock, flags);
    gui_request_paint();
}

uint8_t deskmanager_get_wallpaper_mode(void) {
    return g_vision_wallpaper_mode;
}

void deskmanager_refresh_layout(void) {
    gui_request_paint();
}
