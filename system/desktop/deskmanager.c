#include "programs.h"
#include "kernel/include/memory.h"   // Para kzalloc, kfree, kstrncpy
#include "kernel/include/api.h"
#include "kernel/include/task.h"
#include "kernel/include/pit.h"
#include "kernel/include/vfs.h"
#include "kernel/include/block.h"
#include "kernel/include/mouse.h"
#include "kernel/include/iso9660.h"
#include "kernel/include/bootsplash.h"
#include "kernel/include/compat_mode.h"
#include "kernel/include/user_config.h"
#include "../../kernel/libc/include/stdio.h"
#include "system/desktop/file_progress.h"

/* ══════════════════════════════════════════════════════════════════════════
 *  Iconos de escritorio
 *
 *  Cada icono de escritorio tiene:
 *    - Posición y tamaño (bounds)
 *    - Etiqueta visible
 *    - Estado hover / press
 *    - Registro de doble clic
 *    - Callback que se dispara al hacer doble clic
 * ══════════════════════════════════════════════════════════════════════════ */

static int g_desktop_icon_size=32;
#include "kernel/include/browser_preferences.h"

#define DESK_ICON_W          (g_desktop_icon_size+32)
#define DESK_ICON_H          (g_desktop_icon_size+28)
#define DESK_ICON_IMG_W      g_desktop_icon_size
#define DESK_ICON_IMG_H      g_desktop_icon_size
#define DESK_DBLCLICK_MS bk_input_preference_get(BK_INPUT_DOUBLE_CLICK)
#define DESK_MAX_ICONS      128
#define RESIZE_MARGIN         5
#define RESIZE_LEFT           0x01
#define RESIZE_RIGHT          0x02
#define RESIZE_TOP            0x04
#define RESIZE_BOTTOM         0x08

static uint32_t desk_ticks_from_ms(uint32_t milliseconds) {
    uint32_t hz = pit_get_frequency_hz();
    uint32_t ticks;
    if (!hz) hz = 100U;
    ticks = (hz * milliseconds + 999U) / 1000U;
    return ticks ? ticks : 1U;
}

typedef void (*desk_icon_open_fn)(gui_desktop_t *desktop);

typedef struct {
    int      x, y;
    int      drag_x, drag_y;
    bool hidden;
    char path[VFS_MAX_PATH];
    char     label[24];
    bool     selected;
    bool     hovered;
    bool     pressed;
    uint32_t last_click_tick;
    int      last_click_x;
    int      last_click_y;
    desk_icon_open_fn open;
    uint32_t *pixels;
    uint32_t *original_pixels; /* Immutable source, never resampled in place. */
    uint16_t original_width,original_height;
} desk_icon_t;

typedef struct {
    desk_icon_t icons[DESK_MAX_ICONS];
    int         icon_count;
    int         base_icon_count;
    gui_context_menu_t context_menu, desktop_menu;
    bool auto_arrange, align_grid, selecting;
    int select_x, select_y;
    bool selection_before[DESK_MAX_ICONS];
    int drag_dx, drag_dy;
    int         context_icon;
    uint32_t    context_pressed;
    int         selected_icon, drag_icon;
    int         drag_start_x, drag_start_y, drag_offset_x, drag_offset_y;
    bool        icon_dragged;
    uint32_t    desktop_files_hash;
    uint32_t    last_drive_poll,file_change_seen;
    uint16_t    last_surface_width;
    bool        show_cdrom;
    bool        show_usb;
    bool        show_floppy;
    bool        setup_only;
} deskmanager_state_t;

#define DESK_DEFAULT_BACKGROUND 0x00204070U
#define DESK_DEFAULT_WALLPAPER  "/SYSTEM/WALLPAPR/CLASSIC.BMP"
#define DESK_BMP_MAX_DIMENSION   4096U
#define DESK_BMP_MAX_PIXELS      (16U * 1024U * 1024U)
static uint32_t g_desktop_background = DESK_DEFAULT_BACKGROUND;
static uint32_t *g_desktop_wallpaper;
static uint16_t g_wallpaper_w;
static uint16_t g_wallpaper_h;
static bool g_wallpaper_tiled_source;
static uint8_t g_wallpaper_mode = DESK_WALLPAPER_TILE;
static gui_desktop_t *g_desk_desktop;
static deskmanager_state_t *g_desk_state;
static char g_wallpaper_path[VFS_MAX_PATH];
static void desk_arrange_icons(deskmanager_state_t *st,gui_desktop_t *desktop);
static void desk_layout_preferences(deskmanager_state_t *st,bool save) {
    struct {uint32_t magic,automatic,grid,size;} value={0x33594C44,st->auto_arrange,st->align_grid,g_desktop_icon_size};
    const char *path=BK_DESKTOP_FILES "/.bk-layout";
    if(save) {(void)bk_file_write_all(path,&value,sizeof(value));return;}
    int fd=bk_file_open(path,VFS_O_RDONLY);
    if(fd<0)return;
    int n=bk_file_read(fd,&value,sizeof(value));bk_file_close(fd);
    if(n!=(int)sizeof(value) || (value.magic!=0x32594C44 && value.magic!=0x33594C44) || value.automatic>1 || value.grid>1 ||
       (value.size!=32 && value.size!=48 && value.size!=64))return;
    st->auto_arrange=value.automatic;st->align_grid=value.grid;
    g_desktop_icon_size=value.magic==0x32594C44 ? 32 : value.size;
}

static uint16_t desk_le16(const uint8_t *p);
static uint32_t desk_le32(const uint8_t *p);

static void desk_wallpaper_release(void) {
    if (g_desktop_wallpaper) kfree(g_desktop_wallpaper);
    g_desktop_wallpaper = NULL;
    g_wallpaper_w = 0U;
    g_wallpaper_h = 0U;
    g_wallpaper_tiled_source = false;
}

/* Compone una sola vez el fondo completo. El repintado posterior se limita a
 * copiar las filas del dirty rect; no vuelve a escalar, mezclar alfa ni
 * recorrer el BMP entero al mover una ventana. */
static uint32_t *desk_wallpaper_build_cache(const uint32_t *source,
                                             uint16_t source_w,
                                             uint16_t source_h,
                                             uint16_t screen_w,
                                             uint16_t screen_h,
                                             uint8_t mode) {
    uint32_t count;
    uint32_t *cache;
    uint32_t background;
    int origin_x = 0;
    int origin_y = 0;

    if (!source || !source_w || !source_h || !screen_w || !screen_h)
        return NULL;
    count = (uint32_t)screen_w * (uint32_t)screen_h;
    /* Full-screen caches are useful for scaled/centered artwork.  Keep a
       generous ceiling on normal machines, while the tile mode below retains
       the tiny source image and never reaches this allocation at all. */
    if (!count || count >
        (mm_physical_top() > 32U * 1024U * 1024U ? 4U * 1024U * 1024U
                                                 : 1024U * 1024U))
        return NULL;
    cache = (uint32_t *)kmalloc((size_t)count * sizeof(uint32_t));
    if (!cache) return NULL;
    background = g_desktop_background ? g_desktop_background
                                      : DESK_DEFAULT_BACKGROUND;
    for (uint32_t i = 0U; i < count; i++) cache[i] = background;

    if (mode == DESK_WALLPAPER_TILE) {
        for (uint16_t y = 0U; y < screen_h; y++) {
            const uint32_t *src_row = source +
                (uint32_t)(y % source_h) * source_w;
            uint32_t *dst_row = cache + (uint32_t)y * screen_w;
            for (uint16_t x = 0U; x < screen_w; x++)
                dst_row[x] = src_row[x % source_w] & 0x00FFFFFFU;
        }
        return cache;
    }

    if (mode != DESK_WALLPAPER_STRETCH) {
        origin_x = ((int)screen_w - (int)source_w) / 2;
        origin_y = ((int)screen_h - (int)source_h) / 2;
    }
    for (uint16_t sy = 0U; sy < source_h; sy++) {
        int dy = origin_y + (int)sy;
        int sx_first = 0;
        int sx_last = source_w;
        if (dy < 0 || dy >= screen_h) continue;
        if (origin_x < 0) sx_first = -origin_x;
        if (origin_x + sx_last > screen_w) sx_last = screen_w - origin_x;
        if (sx_first < 0) sx_first = 0;
        if (sx_last > source_w) sx_last = source_w;
        if (sx_first >= sx_last) continue;
        for (int sx = sx_first; sx < sx_last; sx++)
            cache[(uint32_t)dy * screen_w + (uint32_t)(origin_x + sx)] =
                source[(uint32_t)sy * source_w + (uint32_t)sx] & 0x00FFFFFFU;
    }
    return cache;
}

static void desk_wallpaper_copy_dirty(gui_surface_t *surface) {
    gui_rect_t visible;
    gui_rect_t screen;
    if (!surface || !surface->pixels || !g_desktop_wallpaper ||
        (!g_wallpaper_tiled_source &&
         (g_wallpaper_w != surface->width || g_wallpaper_h != surface->height)))
        return;
    screen = (gui_rect_t){0, 0, surface->width, surface->height};
    if (!gui_rect_intersect(screen, gui_gfx_get_clip(surface), &visible))
        return;
    if (g_wallpaper_tiled_source) {
        for (int y = visible.y; y < visible.y + visible.h; y++) {
            uint32_t *dst = &surface->pixels[(uint32_t)y * surface->pitch +
                                             (uint32_t)visible.x];
            const uint32_t *src = g_desktop_wallpaper +
                (uint32_t)(y % g_wallpaper_h) * g_wallpaper_w;
            for (int x = 0; x < visible.w; x++)
                dst[x] = src[(visible.x + x) % g_wallpaper_w] & 0x00FFFFFFU;
        }
        return;
    }
    for (int y = visible.y; y < visible.y + visible.h; y++) {
        kmemcpy(&surface->pixels[(uint32_t)y * surface->pitch +
                                 (uint32_t)visible.x],
                &g_desktop_wallpaper[(uint32_t)y * g_wallpaper_w +
                                     (uint32_t)visible.x],
                (size_t)visible.w * sizeof(uint32_t));
    }
}

void deskmanager_set_background(uint32_t color) {
    g_desktop_background = color & 0x00FFFFFF;
    /* Negro no forma parte de la paleta del panel. Un cero puede llegar al
       iniciar el CPL antes de sincronizar su estado: conservar el fondo
       clasico en vez de dejar el escritorio completamente negro. */
    if (!g_desktop_background)
        g_desktop_background = DESK_DEFAULT_BACKGROUND;
    desk_wallpaper_release();
    g_wallpaper_path[0] = '\0';
}

uint32_t deskmanager_get_background(void) {
    return g_desktop_background;
}

const char *deskmanager_get_wallpaper_path(void) {
    return g_wallpaper_path;
}

uint8_t deskmanager_get_wallpaper_mode(void) {
    return g_wallpaper_mode;
}

static bool desk_wallpaper_dimensions(const char *path, uint16_t *width,
                                      uint16_t *height) {
    void *file = NULL;
    uint32_t size = 0;
    uint8_t *data;
    int32_t w, raw_h, h;
    uint16_t planes, bpp;
    uint32_t dib_size, compression;

    if (!path || !width || !height) return false;
    if (!vfs_read_all(path, &file, &size) || !file) {
        kprintf("[WALLPAPER] no se pudo leer %s (%s)\n", path,
                vfs_last_error_text());
        return false;
    }
    if (size < 54U) {
        kprintf("[WALLPAPER] BMP demasiado corto: %s size=%u\n", path, size);
        kfree(file);
        return false;
    }

    data = (uint8_t *)file;
    dib_size = desk_le32(data + 14);
    w = (int32_t)desk_le32(data + 18);
    raw_h = (int32_t)desk_le32(data + 22);
    planes = desk_le16(data + 26);
    bpp = desk_le16(data + 28);
    compression = desk_le32(data + 30);
    if (raw_h == (int32_t)0x80000000U) raw_h = 0;
    h = raw_h < 0 ? -raw_h : raw_h;

    if (data[0] != 'B' || data[1] != 'M' || dib_size < 40U ||
        planes != 1U || w <= 0 || h <= 0 ||
        (uint32_t)w > DESK_BMP_MAX_DIMENSION ||
        (uint32_t)h > DESK_BMP_MAX_DIMENSION ||
        (uint32_t)w > DESK_BMP_MAX_PIXELS / (uint32_t)h ||
        !(((bpp == 24U || bpp == 32U) && compression == 0U) ||
          ((bpp == 1U || bpp == 4U || bpp == 8U) && compression == 0U) ||
          (bpp == 8U && compression == 1U)) ||
        (compression == 1U && raw_h < 0)) {
        kprintf("[WALLPAPER] BMP no compatible: %s %dx%d bpp=%u comp=%u\n",
                path, w, h, bpp, compression);
        kfree(file);
        return false;
    }
    *width = (uint16_t)w;
    *height = (uint16_t)h;
    kfree(file);
    return true;
}

void deskmanager_set_wallpaper_mode(uint8_t mode) {
    char path[VFS_MAX_PATH];
    if (mode > DESK_WALLPAPER_FILL) mode = DESK_WALLPAPER_STRETCH;
    if (g_wallpaper_mode == mode) return;
    g_wallpaper_mode = mode;
    if (!g_wallpaper_path[0]) return;
    kstrncpy(path, g_wallpaper_path, sizeof(path) - 1U);
    path[sizeof(path) - 1U] = '\0';
    (void)deskmanager_set_wallpaper(path);
}

bool deskmanager_set_wallpaper(const char *path) {
    if (!compat_mode_allow_wallpaper()) {
        desk_wallpaper_release();
        g_wallpaper_path[0] = '\0';
        return false;
    }
    uint32_t *pixels;
    uint32_t *cache;
    uint16_t source_w, source_h, output_w, output_h;
    uint16_t screen_w, screen_h;
    if (!g_desk_desktop || !path) return false;
    screen_w = g_desk_desktop->surface.width;
    screen_h = g_desk_desktop->surface.height;
    output_w = screen_w;
    output_h = screen_h;
    if (g_wallpaper_mode != DESK_WALLPAPER_STRETCH) {
        if (!desk_wallpaper_dimensions(path, &source_w, &source_h)) return false;
        output_w = source_w;
        output_h = source_h;
        if (g_wallpaper_mode == DESK_WALLPAPER_FIT ||
            g_wallpaper_mode == DESK_WALLPAPER_FILL) {
            bool use_width = g_wallpaper_mode == DESK_WALLPAPER_FIT
                ? (uint32_t)screen_w * source_h <= (uint32_t)screen_h * source_w
                : (uint32_t)screen_w * source_h >= (uint32_t)screen_h * source_w;
            if (use_width) {
                output_w = screen_w;
                output_h = (uint16_t)(((uint32_t)source_h * screen_w) / source_w);
            } else {
                output_h = screen_h;
                output_w = (uint16_t)(((uint32_t)source_w * screen_h) / source_h);
            }
            if (!output_w) output_w = 1U;
            if (!output_h) output_h = 1U;
        }
    }
    pixels = program_load_bmp_wallpaper_scaled(path, output_w, output_h);
    if (!pixels) return false;
    if (g_wallpaper_mode == DESK_WALLPAPER_TILE &&
        (uint32_t)output_w * output_h <= 65536U) {
        /* El BMP CLASSIC ocupa apenas 10 KiB decodificado. Repetirlo por el
           rectángulo sucio evita un cache de varios MiB y mantiene el mosaico
           al pasar a resoluciones mayores que el limite del cache completo. */
        cache = pixels;
    } else {
        cache = desk_wallpaper_build_cache(pixels, output_w, output_h,
                                           screen_w, screen_h,
                                           g_wallpaper_mode);
        kfree(pixels);
    }
    if (!cache) return false;
    /* Vision allocated this cache in its own user heap. It already has the
     * correct service lifetime: requesting an owner PID through the public
     * metadata API is forbidden and used to discard every decoded wallpaper.
     * Only the legacy kernel-side image loader needs an ownership transfer. */
#ifndef BLES_VISION_SERVICE
    if (!mm_set_allocation_owner(cache, 0U)) {
        kfree(cache);
        return false;
    }
#endif
    desk_wallpaper_release();
    g_desktop_wallpaper = cache;
    g_wallpaper_tiled_source = g_wallpaper_mode == DESK_WALLPAPER_TILE &&
                              cache == pixels;
    g_wallpaper_w = g_wallpaper_tiled_source ? output_w : screen_w;
    g_wallpaper_h = g_wallpaper_tiled_source ? output_h : screen_h;
    kstrncpy(g_wallpaper_path, path, sizeof(g_wallpaper_path) - 1);
    g_wallpaper_path[sizeof(g_wallpaper_path) - 1] = '\0';
    kprintf("[WALLPAPER] installed %s %ux%u tiled=%u\n",g_wallpaper_path,
        g_wallpaper_w,g_wallpaper_h,g_wallpaper_tiled_source);
    return true;
}

/* ── utilidad: leer ticks PIT (declarada en pit.h, incluida via programs.h→gui.h) ── */

/* ──────────────────────────────────────────────────────────────────────────
 *  Callbacks de apertura de cada icono
 * ────────────────────────────────────────────────────────────────────────── */

/* Forward-declare las funciones de apertura que viven en sus módulos */
static void desk_open_trash(gui_desktop_t *desktop) {
    (void)bk_file_mkdir(BK_TRASH_FILES);
    (void)program_execute_path_async_arg(desktop,"/SYSTEM/PROGRAMS/FILE.BEX",BK_TRASH_FILES);
}

static void desk_open_icon(gui_desktop_t *desktop, desk_icon_t *icon) {
    if (icon->path[0]) {
        vfs_dir_entry_t info;
        if (bk_file_stat(icon->path,&info) && info.type == VFS_NODE_DIR)
            (void)program_execute_path_async_arg(desktop,"/SYSTEM/PROGRAMS/FILE.BEX",icon->path);
        else (void)bk_app_launch(icon->path,NULL);
    } else if (icon->open) icon->open(desktop);
}

static uint32_t desk_position_key(const desk_icon_t *icon) {
    const char *key = icon->path[0] ? icon->path : icon->label;
    uint32_t hash=2166136261U;
    while (*key) { hash ^= (uint8_t)*key++; hash *= 16777619U; }
    return hash;
}

static void desk_icon_position(desk_icon_t *icon, bool save) {
    char path[VFS_MAX_PATH];
    struct { uint32_t magic; int32_t x,y; uint32_t hidden; } record;
    snprintf(path,sizeof(path),BK_DESKTOP_FILES "/.bk-pos-%08x",desk_position_key(icon));
    if (save) {
        record.magic=0x31534f50; record.x=icon->x; record.y=icon->y; record.hidden=icon->hidden;
        (void)bk_file_write_all(path,&record,sizeof(record));
    } else {
        int fd=bk_file_open(path,VFS_O_RDONLY);
        if (fd<0) return;
        int bytes=bk_file_read(fd,&record,sizeof(record)); bk_file_close(fd);
        if (bytes == sizeof(record) && record.magic==0x31534f50 && record.x>=0 && record.y>=0) {
            icon->x=record.x; icon->y=record.y; icon->hidden=record.hidden!=0;
        }
    }
}

static void desk_open_files_app(gui_desktop_t *desktop) {
    (void)program_execute_path_async(desktop, "/SYSTEM/PROGRAMS/FILE.BEX");
}

static void desk_open_shell_app(gui_desktop_t *desktop) {
    (void)program_execute_path_async(desktop, "/SYSTEM/PROGRAMS/SHELL.BEX");
}

static void desk_open_editor_app(gui_desktop_t *desktop) {
    (void)program_execute_path_async(desktop, "/SYSTEM/PROGRAMS/TEXTEDITOR.BEX");
}

static void desk_open_netsurf_app(gui_desktop_t *desktop) {
    char path[260];bk_browser_default_path(path,sizeof(path));
    (void)program_execute_path_async(desktop,path);
}

static void desk_open_font_app(gui_desktop_t *desktop) {
    (void)program_execute_path_async(desktop, "/SYSTEM/PROGRAMS/FONT.BEX");
}

static void desk_open_calculator_app(gui_desktop_t *desktop) {
    (void)program_execute_path_async(desktop, "/SYSTEM/PROGRAMS/CALCULATOR.BEX");
}

static void desk_open_processmanager_app(gui_desktop_t *desktop) {
    (void)program_execute_path_async(desktop, "/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX");
}

static void desk_open_events_app(gui_desktop_t *desktop) {
    (void)program_execute_path_async(desktop,"/SYSTEM/PROGRAMS/EVENT_VIEWER.BEX");
}

static void desk_open_performance_app(gui_desktop_t *desktop) {
    (void)program_execute_path_async(desktop,
        "/SYSTEM/PROGRAMS/PERFORMANCE_MONITOR.BEX");
}

static void desk_open_midamp_app(gui_desktop_t *desktop) {
    (void)program_execute_path_async(desktop, "/SYSTEM/PROGRAMS/MIDAMP.BEX");
}

static void desk_open_viewer_app(gui_desktop_t *desktop) {
    (void)program_execute_path_async(desktop, "/SYSTEM/PROGRAMS/IMAGEVIEWER.BEX");
}



static void desk_open_control_panel(gui_desktop_t *desktop) {
    (void)program_execute_path_async(desktop, "/SYSTEM/CONTROL/CONTROL.BEX");
}

static void desk_open_cdrom(gui_desktop_t *desktop) {
    /* El acceso ATAPI puede ser lento: solo se intenta por accion del usuario. */
    if (vfs_has_cdrom() || iso9660_mount_default())
        (void)program_execute_path_async_arg(desktop, "/SYSTEM/PROGRAMS/FILE.BEX",
                                       "/CDROM");
}

static void desk_open_floppy(gui_desktop_t *desktop) {
    if (block_get("fd0"))
        (void)program_execute_path_async_arg(desktop, "/SYSTEM/PROGRAMS/FILE.BEX",
                                       "/fd0");
}

static void desk_open_usb(gui_desktop_t *desktop) {
    if (block_get("usb0"))
        (void)program_execute_path_async_arg(desktop, "/SYSTEM/PROGRAMS/FILE.BEX",
                                       "/usb0");
}

static uint16_t desk_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t desk_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* BI_RLE8 (compression=1) stores runs in bottom-up scanline order.
 * Decode once to an indexed, top-down buffer so the scaling loop remains
 * simple and does not need to interpret the variable-length stream per pixel. */
static bool desk_bmp_decode_rle8(const uint8_t *data, uint32_t size,
                                 uint32_t offset, uint32_t image_size,
                                 uint16_t width, uint16_t height,
                                 uint8_t *indices) {
    uint32_t pos = offset;
    uint32_t limit = size;
    uint32_t x = 0U;
    int32_t y = (int32_t)height - 1;

    if (!data || !indices || !width || !height || offset >= size) return false;
    if (image_size && image_size <= size - offset) limit = offset + image_size;
    kmemset(indices, 0, (size_t)width * height);

    while (pos < limit && y >= 0) {
        uint8_t count = data[pos++];
        uint8_t value;
        if (pos >= limit) return false;
        value = data[pos++];

        if (count != 0U) {
            if (x + count > width) return false;
            kmemset(indices + (uint32_t)y * width + x, value, count);
            x += count;
            continue;
        }

        if (value == 0U) {              /* end of line */
            x = 0U;
            y--;
        } else if (value == 1U) {       /* end of bitmap */
            return true;
        } else if (value == 2U) {       /* delta */
            uint8_t dx, dy;
            if (pos + 2U > limit) return false;
            dx = data[pos++];
            dy = data[pos++];
            if (x + dx > width || dy > (uint32_t)y) return false;
            x += dx;
            y -= dy;
        } else {                        /* absolute run */
            uint32_t literal = value;
            if (x + literal > width || pos + literal > limit) return false;
            kmemcpy(indices + (uint32_t)y * width + x, data + pos, literal);
            pos += literal;
            x += literal;
            if (literal & 1U) {
                if (pos >= limit) return false;
                pos++;                  /* word alignment padding */
            }
        }
    }
    /* Some encoders finish the final row with EOL and omit EOB. */
    return y < 0;
}

static uint32_t desk_bmp_palette_rgb(const uint8_t *palette,
                                     uint16_t palette_count,
                                     uint8_t index) {
    const uint8_t *entry;
    if (!palette || index >= palette_count) return 0U;
    entry = palette + (uint32_t)index * 4U;
    return ((uint32_t)entry[2] << 16) |
           ((uint32_t)entry[1] << 8) | entry[0];
}

/* Read one palette index from a BI_RGB row.  BMP stores 4-bpp pixels high
 * nibble first and 1-bpp pixels most-significant bit first. */
static uint8_t desk_bmp_index_at(const uint8_t *row, uint16_t bpp,
                                 uint32_t x) {
    if (!row) return 0U;
    if (bpp == 8U) return row[x];
    if (bpp == 4U) {
        uint8_t packed = row[x >> 1];
        return (x & 1U) ? (uint8_t)(packed & 0x0FU)
                        : (uint8_t)(packed >> 4);
    }
    if (bpp == 1U)
        return (uint8_t)((row[x >> 3] >> (7U - (x & 7U))) & 1U);
    return 0U;
}

static bool desk_color_matches_key(uint32_t rgb, uint32_t key) {
    int red = (int)((rgb >> 16) & 0xFF) - (int)((key >> 16) & 0xFF);
    int green = (int)((rgb >> 8) & 0xFF) - (int)((key >> 8) & 0xFF);
    int blue = (int)(rgb & 0xFF) - (int)(key & 0xFF);
    if (red < 0) red = -red;
    if (green < 0) green = -green;
    if (blue < 0) blue = -blue;
    return red <= 8 && green <= 8 && blue <= 8;
}

static bool desk_use_exact_key(const char *path) {
    return path && kstrcmp(path, "/ICONS/MONITOR.BMP") == 0;
}

static uint32_t *desk_load_bmp_scaled(const char *path,
                                      uint16_t output_width,
                                      uint16_t output_height,
                                      bool transparent) {
    void *file = NULL;
    uint32_t size = 0;
    uint8_t *data;
    uint32_t offset, dib_size, compression, image_size;
    uint32_t stride = 0U;
    uint32_t palette_offset = 0U;
    uint32_t colors_used = 0U;
    uint16_t palette_count = 0U;
    const uint8_t *palette = NULL;
    uint8_t *rle_indices = NULL;
    int32_t width, raw_height, height;
    uint16_t planes, bpp;
    uint32_t pixel_count;
    uint32_t *pixels = NULL;
    uint16_t *source_x = NULL;
    uint32_t key_rgb = 0U;
    bool exact_key;

    if (!path || output_width == 0U || output_height == 0U) return NULL;
    if (!vfs_read_all(path, &file, &size) || !file || size < 54U) {
        if (!transparent)
            kprintf("[WALLPAPER] no se pudo leer %s (%s)\n", path,
                    vfs_last_error_text());
        return NULL;
    }
    data = (uint8_t *)file;
    offset = desk_le32(data + 10);
    dib_size = desk_le32(data + 14);
    width = (int32_t)desk_le32(data + 18);
    raw_height = (int32_t)desk_le32(data + 22);
    planes = desk_le16(data + 26);
    bpp = desk_le16(data + 28);
    compression = desk_le32(data + 30);
    image_size = desk_le32(data + 34);
    if (raw_height == (int32_t)0x80000000U) raw_height = 0;
    height = raw_height < 0 ? -raw_height : raw_height;

    if (data[0] != 'B' || data[1] != 'M' || dib_size < 40U ||
        planes != 1U || width <= 0 || height <= 0 ||
        (uint32_t)width > DESK_BMP_MAX_DIMENSION ||
        (uint32_t)height > DESK_BMP_MAX_DIMENSION ||
        (uint32_t)width > DESK_BMP_MAX_PIXELS / (uint32_t)height ||
        !(((bpp == 24U || bpp == 32U) && compression == 0U) ||
          ((bpp == 1U || bpp == 4U || bpp == 8U) && compression == 0U) ||
          (bpp == 8U && compression == 1U)) ||
        (compression == 1U && raw_height < 0) || offset >= size) {
        if (!transparent)
            kprintf("[WALLPAPER] BMP no compatible: %s %dx%d bpp=%u comp=%u\n",
                    path, width, height, bpp, compression);
        kfree(file);
        return NULL;
    }

    if (bpp == 1U || bpp == 4U || bpp == 8U) {
        uint32_t palette_limit = 1U << bpp;
        palette_offset = 14U + dib_size;
        colors_used = desk_le32(data + 46);
        if (!colors_used) colors_used = palette_limit;
        if (colors_used > palette_limit || palette_offset > offset ||
            colors_used > (offset - palette_offset) / 4U ||
            palette_offset + colors_used * 4U > size) {
            if (!transparent)
                kprintf("[WALLPAPER] paleta BMP%u invalida: %s colors=%u\n",
                        bpp, path, colors_used);
            kfree(file);
            return NULL;
        }
        palette = data + palette_offset;
        palette_count = (uint16_t)colors_used;
        if (compression == 0U) {
            uint32_t row_bits = (uint32_t)width * bpp;
            stride = ((row_bits + 31U) / 32U) * 4U;
            if (!stride || stride > size - offset ||
                (uint32_t)height > (size - offset) / stride) {
                if (!transparent)
                    kprintf("[WALLPAPER] pixeles BMP%u truncados: %s\n",
                            bpp, path);
                kfree(file);
                return NULL;
            }
        } else {
            uint32_t source_count = (uint32_t)width * (uint32_t)height;
            rle_indices = (uint8_t *)kmalloc(source_count);
            if (!rle_indices ||
                !desk_bmp_decode_rle8(data, size, offset, image_size,
                                      (uint16_t)width, (uint16_t)height,
                                      rle_indices)) {
                if (!transparent)
                    kprintf("[WALLPAPER] flujo BI_RLE8 invalido: %s\n", path);
                if (rle_indices) kfree(rle_indices);
                kfree(file);
                return NULL;
            }
        }
    } else {
        stride = ((uint32_t)width * (uint32_t)(bpp / 8U) + 3U) & ~3U;
        if (stride > size - offset ||
            (uint32_t)height > (size - offset) / stride) {
            if (!transparent)
                kprintf("[WALLPAPER] pixeles BMP truncados: %s\n", path);
            kfree(file);
            return NULL;
        }
    }

    pixel_count = (uint32_t)output_width * output_height;
    if (!pixel_count || pixel_count > DESK_BMP_MAX_PIXELS) {
        if (rle_indices) kfree(rle_indices);
        kfree(file);
        return NULL;
    }
    pixels = (uint32_t *)kmalloc((size_t)pixel_count * sizeof(uint32_t));
    source_x = (uint16_t *)kmalloc((size_t)output_width * sizeof(uint16_t));
    if (!pixels || !source_x) {
        if (source_x) kfree(source_x);
        if (pixels) kfree(pixels);
        if (rle_indices) kfree(rle_indices);
        kfree(file);
        return NULL;
    }
    exact_key = desk_use_exact_key(path);
    for (uint16_t x = 0U; x < output_width; x++)
        source_x[x] = (uint16_t)(((uint32_t)x * (uint32_t)width) /
                                 output_width);

    if (bpp == 1U || bpp == 4U || bpp == 8U) {
        uint8_t key_index;
        if (compression == 1U) {
            key_index = rle_indices[0];
        } else {
            uint32_t key_row = raw_height > 0 ? (uint32_t)height - 1U : 0U;
            key_index = desk_bmp_index_at(data + offset + key_row * stride,
                                          bpp, 0U);
        }
        key_rgb = desk_bmp_palette_rgb(palette, palette_count, key_index);
    } else {
        uint32_t key_row = raw_height > 0 ? (uint32_t)height - 1U : 0U;
        const uint8_t *key_src = data + offset + key_row * stride;
        key_rgb = ((uint32_t)key_src[2] << 16) |
                  ((uint32_t)key_src[1] << 8) | key_src[0];
    }

    for (uint16_t y = 0U; y < output_height; y++) {
        uint32_t top_y = ((uint32_t)y * (uint32_t)height) / output_height;
        uint32_t file_y = raw_height > 0
            ? (uint32_t)height - 1U - top_y : top_y;
        for (uint16_t x = 0U; x < output_width; x++) {
            uint32_t sx = source_x[x];
            uint32_t rgb;
            uint8_t alpha = 0xFFU;

            if (bpp == 1U || bpp == 4U || bpp == 8U) {
                uint8_t index = compression == 1U
                    ? rle_indices[top_y * (uint32_t)width + sx]
                    : desk_bmp_index_at(data + offset + file_y * stride,
                                        bpp, sx);
                rgb = desk_bmp_palette_rgb(palette, palette_count, index);
            } else {
                const uint8_t *src = data + offset + file_y * stride +
                    sx * (uint32_t)(bpp / 8U);
                rgb = ((uint32_t)src[2] << 16) |
                      ((uint32_t)src[1] << 8) | src[0];
                if (bpp == 32U) alpha = src[3];
            }
            if (!transparent) alpha = 0xFFU;
            if ((transparent && alpha == 0U) ||
                (transparent && bpp != 32U &&
                 ((exact_key ? rgb == key_rgb
                             : desk_color_matches_key(rgb, key_rgb)) ||
                  rgb == 0x00FF00FFU))) {
                pixels[(uint32_t)y * output_width + x] = 0U;
            } else {
                pixels[(uint32_t)y * output_width + x] =
                    ((uint32_t)alpha << 24) | rgb;
            }
        }
    }

    kfree(source_x);
    if (rle_indices) kfree(rle_indices);
    if (!transparent && bpp == 8U && compression == 1U)
        kprintf("[WALLPAPER] BI_RLE8 OK: %s %dx%d -> %ux%u\n",
                path, width, height, output_width, output_height);
    else if (!transparent && (bpp == 1U || bpp == 4U || bpp == 8U))
        kprintf("[WALLPAPER] BMP%u OK: %s %dx%d -> %ux%u modo=%u\n",
                bpp, path, width, height, output_width, output_height,
                g_wallpaper_mode);
    bootsplash_pulse();
    kfree(file);
    return pixels;
}

/* ------------------------------------------------------------------------- */
/* BVI1 scalable icon loader.
 *
 * Applications still request their historical /ICONS/NAME.BMP path.  The
 * central icon loader transparently replaces the extension with .BVI and
 * renders the vector rectangles at the requested size.  This keeps every
 * existing caller compatible and leaves ICONS.PAK/BMP as a safe fallback.
 */

#define BK_BVI_MAX_COLORS 256U
#define BK_BVI_PACK_ENTRY_SIZE 24U

static void *bk_bvi_pack_data;
static uint32_t bk_bvi_pack_size;
static bool bk_bvi_pack_attempted;
static bool bk_bvi_pack_table_only;
static char bk_bvi_pack_path[32];

static bool bk_bvi_read_range(uint32_t offset, void *buffer, uint32_t size) {
    int fd;
    int got;
    if (!bk_bvi_pack_path[0] || !buffer || !size || offset > 0x7FFFFFFFU)
        return false;
    fd = vfs_open(bk_bvi_pack_path, VFS_O_RDONLY);
    if (fd < 0) return false;
    if (vfs_seek(fd, (int32_t)offset, 0U) != (int32_t)offset) {
        vfs_close(fd);
        return false;
    }
    got = vfs_read(fd, buffer, size);
    vfs_close(fd);
    return got >= 0 && (uint32_t)got == size;
}

static uint32_t bk_bvi_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool bk_bvi_make_name(const char *path, char out[16]) {
    const char *base = path;
    uint32_t used = 0U;
    if (!path || !out) return false;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    while (base[used] && base[used] != '.' && used < 15U) {
        char ch = base[used];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        out[used++] = ch;
    }
    out[used] = '\0';
    return used > 0U;
}

static bool bk_bvi_pack_load_once(void) {
    uint8_t *data;
    vfs_dir_entry_t entry;
    if (bk_bvi_pack_attempted) return bk_bvi_pack_data != NULL;
    bk_bvi_pack_attempted = true;
    if (compat_mode_is_low_memory()) {
        uint8_t header[12];
        uint32_t count;
        uint32_t table_size;
        const char *path = NULL;
        if (vfs_stat("/ICONS/ICONS.PAK", &entry) &&
            entry.type == VFS_NODE_FILE) path = "/ICONS/ICONS.PAK";
        else if (vfs_stat("/ICONS.PAK", &entry) &&
                 entry.type == VFS_NODE_FILE) path = "/ICONS.PAK";
        if (!path) return false;
        kstrncpy(bk_bvi_pack_path, path, sizeof(bk_bvi_pack_path) - 1U);
        if (!bk_bvi_read_range(0U, header, sizeof(header)) ||
            header[0] != 'B' || header[1] != 'K' ||
            header[2] != 'V' || header[3] != 'P' ||
            bk_bvi_rd32(header + 4U) != 1U) return false;
        count = bk_bvi_rd32(header + 8U);
        if (count > (entry.size - 12U) / BK_BVI_PACK_ENTRY_SIZE)
            return false;
        table_size = 12U + count * BK_BVI_PACK_ENTRY_SIZE;
        bk_bvi_pack_data = kmalloc(table_size);
        if (!bk_bvi_pack_data ||
            !bk_bvi_read_range(0U, bk_bvi_pack_data, table_size)) {
            if (bk_bvi_pack_data) kfree(bk_bvi_pack_data);
            bk_bvi_pack_data = NULL;
            return false;
        }
        bk_bvi_pack_size = entry.size;
        bk_bvi_pack_table_only = true;
    } else if (!vfs_read_all("/ICONS/ICONS.PAK", &bk_bvi_pack_data,
                             &bk_bvi_pack_size) &&
               !vfs_read_all("/ICONS.PAK", &bk_bvi_pack_data,
                             &bk_bvi_pack_size)) return false;
    data = (uint8_t *)bk_bvi_pack_data;
    if (bk_bvi_pack_size < 12U || data[0] != 'B' || data[1] != 'K' ||
        data[2] != 'V' || data[3] != 'P' || bk_bvi_rd32(data + 4U) != 1U) {
        kfree(bk_bvi_pack_data);
        bk_bvi_pack_data = NULL;
        bk_bvi_pack_size = 0U;
        return false;
    }
    uint32_t count = bk_bvi_rd32(data + 8U);
    if (count > (bk_bvi_pack_size - 12U) / BK_BVI_PACK_ENTRY_SIZE) {
        kfree(bk_bvi_pack_data);
        bk_bvi_pack_data = NULL;
        bk_bvi_pack_size = 0U;
        return false;
    }
    return true;
}

static bool bk_bvi_pack_find(const char *path, const uint8_t **document,
                             uint32_t *document_size, void **owned_document) {
    char wanted[16];
    uint8_t *data;
    uint32_t count;
    if (owned_document) *owned_document = NULL;
    if (!document || !document_size || !bk_bvi_make_name(path, wanted) ||
        !bk_bvi_pack_load_once()) return false;
    data = (uint8_t *)bk_bvi_pack_data;
    count = bk_bvi_rd32(data + 8U);
    for (uint32_t i = 0U; i < count; i++) {
        uint8_t *entry = data + 12U + i * BK_BVI_PACK_ENTRY_SIZE;
        bool equal_name = true;
        for (uint32_t ch = 0U; ch < 16U; ch++) {
            if ((uint8_t)wanted[ch] != entry[ch]) {
                equal_name = false;
                break;
            }
            if (!wanted[ch]) break;
        }
        if (!equal_name) continue;
        uint32_t offset = bk_bvi_rd32(entry + 16U);
        uint32_t size = bk_bvi_rd32(entry + 20U);
        if (offset > bk_bvi_pack_size || size > bk_bvi_pack_size - offset)
            return false;
        if (bk_bvi_pack_table_only) {
            uint8_t *copy = (uint8_t *)kmalloc(size);
            if (!copy || !bk_bvi_read_range(offset, copy, size)) {
                if (copy) kfree(copy);
                return false;
            }
            *document = copy;
            if (owned_document) *owned_document = copy;
        } else {
            *document = data + offset;
        }
        *document_size = size;
        return true;
    }
    return false;
}

static bool bk_bvi_icon_path(const char *path, char *out, uint32_t capacity) {
    const char *dot = NULL;
    uint32_t length = 0U;
    if (!path || !out || capacity < 6U) return false;
    for (const char *p = path; *p; p++) {
        if (*p == '.') dot = p;
        length++;
    }
    if (!dot || length + 1U > capacity) return false;
    if (!((dot[1] == 'B' || dot[1] == 'b') &&
          (dot[2] == 'M' || dot[2] == 'm') &&
          (dot[3] == 'P' || dot[3] == 'p') && dot[4] == '\0'))
        return false;
    uint32_t prefix = (uint32_t)(dot - path);
    if (prefix + 5U > capacity) return false;
    for (uint32_t i = 0U; i < prefix; i++) out[i] = path[i];
    out[prefix + 0U] = '.';
    out[prefix + 1U] = 'B';
    out[prefix + 2U] = 'V';
    out[prefix + 3U] = 'I';
    out[prefix + 4U] = '\0';
    return true;
}

static const uint8_t *bk_bvi_skip_space(const uint8_t *cursor,
                                         const uint8_t *end) {
    while (cursor < end && (*cursor == ' ' || *cursor == '\t' ||
                            *cursor == '\r' || *cursor == '\n')) cursor++;
    return cursor;
}

static const uint8_t *bk_bvi_next_line(const uint8_t *cursor,
                                        const uint8_t *end) {
    while (cursor < end && *cursor != '\n') cursor++;
    return cursor < end ? cursor + 1U : end;
}

static bool bk_bvi_word(const uint8_t *cursor, const uint8_t *end,
                        const char *word, const uint8_t **after) {
    while (*word && cursor < end && *cursor == (uint8_t)*word) {
        cursor++;
        word++;
    }
    if (*word || (cursor < end && *cursor != ' ' && *cursor != '\t' &&
                  *cursor != '\r' && *cursor != '\n')) return false;
    if (after) *after = cursor;
    return true;
}

static bool bk_bvi_uint(const uint8_t **cursor, const uint8_t *end,
                        uint32_t *value) {
    const uint8_t *p = bk_bvi_skip_space(*cursor, end);
    uint32_t result = 0U;
    bool any = false;
    while (p < end && *p >= '0' && *p <= '9') {
        uint32_t digit = (uint32_t)(*p - '0');
        if (result > 429496729U ||
            (result == 429496729U && digit > 5U)) return false;
        result = result * 10U + digit;
        p++;
        any = true;
    }
    if (!any) return false;
    *cursor = p;
    *value = result;
    return true;
}

static bool bk_bvi_fringe_color(uint32_t pixel) {
    uint32_t red = (pixel >> 16) & 0xFFU;
    uint32_t green = (pixel >> 8) & 0xFFU;
    uint32_t blue = pixel & 0xFFU;
    uint32_t difference = red > blue ? red - blue : blue - red;
    uint32_t magenta = red < blue ? red : blue;
    return (pixel >> 24) != 0U && magenta >= 238U && difference <= 12U &&
           green + 60U < magenta;
}

static void bk_bvi_cleanup_fringe(uint32_t *pixels, uint32_t width,
                                  uint32_t height) {
    if (!pixels || width < 2U || height < 2U) return;
    /* Una sola capa de magenta fuerte limpia el fondo residual sin comerse
       sombras rosadas ni detalles que pertenecen al dibujo. Alfa=1 se usa
       como marca temporal para no propagar el borrado durante el recorrido. */
    for (uint32_t pass = 0U; pass < 1U; pass++) {
        bool changed = false;
        for (uint32_t y = 0U; y < height; y++) {
            for (uint32_t x = 0U; x < width; x++) {
                uint32_t index = y * width + x;
                if (!bk_bvi_fringe_color(pixels[index])) continue;
                bool touches_clear = false;
                uint32_t y0 = y ? y - 1U : y;
                uint32_t y1 = y + 1U < height ? y + 1U : y;
                uint32_t x0 = x ? x - 1U : x;
                uint32_t x1 = x + 1U < width ? x + 1U : x;
                for (uint32_t ny = y0; ny <= y1 && !touches_clear; ny++)
                    for (uint32_t nx = x0; nx <= x1; nx++)
                        if ((pixels[ny * width + nx] >> 24) == 0U) {
                            touches_clear = true;
                            break;
                        }
                if (touches_clear) {
                    pixels[index] = (pixels[index] & 0x00FFFFFFU) | 0x01000000U;
                    changed = true;
                }
            }
        }
        for (uint32_t i = 0U; i < width * height; i++)
            if ((pixels[i] >> 24) == 1U) pixels[i] = 0U;
        if (!changed) break;
    }
}

static uint32_t *bk_bvi_load_bmp_path(const char *path, int out_w, int out_h) {
    char bvi_path[VFS_MAX_PATH];
    void *owned_raw = NULL;
    const uint8_t *raw = NULL;
    uint32_t raw_size = 0U;
    uint32_t colors[BK_BVI_MAX_COLORS];
    uint8_t color_defined[BK_BVI_MAX_COLORS];
    uint32_t canvas_w = 0U, canvas_h = 0U;
    uint32_t *pixels = NULL;
    bool saw_header = false;

    if (out_w <= 0 || out_h <= 0 || out_w > 1024 || out_h > 1024 ||
        !bk_bvi_icon_path(path, bvi_path, sizeof(bvi_path)))
        return NULL;
    if (!bk_bvi_pack_find(path, &raw, &raw_size, &owned_raw)) {
        if (!vfs_read_all(bvi_path, &owned_raw, &raw_size) || !owned_raw)
            return NULL;
        raw = (const uint8_t *)owned_raw;
    }
    if (raw_size < 5U) goto fail;

    kmemset(colors, 0, sizeof(colors));
    kmemset(color_defined, 0, sizeof(color_defined));
    const uint8_t *cursor = raw;
    const uint8_t *end = cursor + raw_size;

    while (cursor < end) {
        const uint8_t *line = bk_bvi_skip_space(cursor, end);
        const uint8_t *args = NULL;
        if (line >= end) break;
        if (*line == '#') {
            cursor = bk_bvi_next_line(line, end);
            continue;
        }
        if (!saw_header) {
            if ((uint32_t)(end - line) < 4U || line[0] != 'B' ||
                line[1] != 'V' || line[2] != 'I' || line[3] != '1')
                goto fail;
            saw_header = true;
            cursor = bk_bvi_next_line(line, end);
            continue;
        }
        if (bk_bvi_word(line, end, "canvas", &args)) {
            if (!bk_bvi_uint(&args, end, &canvas_w) ||
                !bk_bvi_uint(&args, end, &canvas_h) || !canvas_w || !canvas_h ||
                canvas_w > 4096U || canvas_h > 4096U) goto fail;
            if (!pixels) {
                uint32_t count = (uint32_t)out_w * (uint32_t)out_h;
                pixels = (uint32_t *)kmalloc(count * sizeof(uint32_t));
                if (!pixels) goto fail;
                kmemset(pixels, 0, count * sizeof(uint32_t));
            }
        } else if (bk_bvi_word(line, end, "color", &args)) {
            uint32_t id, red, green, blue, alpha;
            if (!bk_bvi_uint(&args, end, &id) ||
                !bk_bvi_uint(&args, end, &red) ||
                !bk_bvi_uint(&args, end, &green) ||
                !bk_bvi_uint(&args, end, &blue) ||
                !bk_bvi_uint(&args, end, &alpha) ||
                id >= BK_BVI_MAX_COLORS || red > 255U || green > 255U ||
                blue > 255U || alpha > 255U) goto fail;
            colors[id] = (alpha << 24) | (red << 16) | (green << 8) | blue;
            color_defined[id] = 1U;
        } else if (bk_bvi_word(line, end, "rect", &args)) {
            uint32_t x, y, width, height, color;
            if (!pixels || !canvas_w || !canvas_h ||
                !bk_bvi_uint(&args, end, &x) ||
                !bk_bvi_uint(&args, end, &y) ||
                !bk_bvi_uint(&args, end, &width) ||
                !bk_bvi_uint(&args, end, &height) ||
                !bk_bvi_uint(&args, end, &color) ||
                color >= BK_BVI_MAX_COLORS || !color_defined[color] ||
                x > canvas_w || y > canvas_h || width > canvas_w - x ||
                height > canvas_h - y) goto fail;
            uint32_t pixel = colors[color];
            if (pixel >> 24) {
                uint32_t x0 = (x * (uint32_t)out_w) / canvas_w;
                uint32_t y0 = (y * (uint32_t)out_h) / canvas_h;
                uint32_t x1 = ((x + width) * (uint32_t)out_w + canvas_w - 1U) /
                              canvas_w;
                uint32_t y1 = ((y + height) * (uint32_t)out_h + canvas_h - 1U) /
                              canvas_h;
                if (x1 > (uint32_t)out_w) x1 = (uint32_t)out_w;
                if (y1 > (uint32_t)out_h) y1 = (uint32_t)out_h;
                for (uint32_t py = y0; py < y1; py++)
                    for (uint32_t px = x0; px < x1; px++)
                        pixels[py * (uint32_t)out_w + px] = pixel;
            }
        }
        cursor = bk_bvi_next_line(line, end);
    }

    if (owned_raw) kfree(owned_raw);
    if (saw_header && pixels && canvas_w && canvas_h) {
        bk_bvi_cleanup_fringe(pixels, (uint32_t)out_w, (uint32_t)out_h);
        return pixels;
    }
    if (pixels) kfree(pixels);
    return NULL;

fail:
    if (pixels) kfree(pixels);
    if (owned_raw) kfree(owned_raw);
    return NULL;
}

#ifndef BK_ICONPAK_CENTRAL_LOADER
#define BK_ICONPAK_CENTRAL_LOADER 1

static void *bk_iconpak_data = NULL;
static uint32_t bk_iconpak_size = 0;

static uint32_t bk_iconpak_rd32(const uint8_t *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool bk_iconpak_load_once(void) {
    if (bk_iconpak_data) return true;

    /* El paquete instalado actualmente es BKVP (vectorial). Intentar abrirlo
       tambien como el antiguo BKIP dejaba sus 278 KiB completos residentes
       aun despues de rechazar la firma. */
    if (compat_mode_is_low_memory()) return false;

    bootsplash_show("@H33FF5DFA", 78);
    bootsplash_pulse();

    if (vfs_read_all("/ICONS/ICONS.PAK", &bk_iconpak_data, &bk_iconpak_size)) {
        bootsplash_pulse();
        return true;
    }

    bootsplash_pulse();
    if (vfs_read_all("/ICONS.PAK", &bk_iconpak_data, &bk_iconpak_size)) {
        bootsplash_pulse();
        return true;
    }

    return false;
}

static bool bk_iconpak_make_name_from_bmp_path(const char *path, char out[16]) {
    const char *base;
    uint32_t i = 0;

    if (!path || !out) return false;

    base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    while (base[i] && base[i] != '.' && i < 15) {
        char c = base[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i++] = c;
    }

    out[i] = '\0';
    return i > 0;
}

static bool bk_iconpak_name16_eq(const uint8_t *name16, const char *name) {
    uint32_t i = 0;

    if (!name16 || !name) return false;

    while (i < 16 && name[i]) {
        if ((char)name16[i] != name[i]) return false;
        i++;
    }

    return i < 16 && name16[i] == '\0';
}

static bool bk_iconpak_path_is_icon_bmp(const char *path) {
    const char *dot = NULL;
    const char *base;

    if (!path) return false;

    base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
        if (*p == '.') dot = p;
    }

    if (!dot) return false;

    return (dot[0] == '.' &&
           (dot[1] == 'B' || dot[1] == 'b') &&
           (dot[2] == 'M' || dot[2] == 'm') &&
           (dot[3] == 'P' || dot[3] == 'p') &&
            dot[4] == '\0' &&
            base[0] != '\0');
}

static uint32_t *bk_iconpak_load_bmp_path(const char *path,
                                          int out_w,
                                          int out_h) {
    char wanted[16];
    uint8_t *data;
    uint32_t count;

    if (!bk_iconpak_path_is_icon_bmp(path)) return NULL;
    if (out_w <= 0 || out_h <= 0) return NULL;
    if (!bk_iconpak_make_name_from_bmp_path(path, wanted)) return NULL;
    if (!bk_iconpak_load_once()) return NULL;

    data = (uint8_t *)bk_iconpak_data;

    if (!data || bk_iconpak_size < 12) return NULL;
    if (data[0] != 'B' || data[1] != 'K' ||
        data[2] != 'I' || data[3] != 'P') return NULL;
    if (bk_iconpak_rd32(data + 4) != 1) return NULL;

    count = bk_iconpak_rd32(data + 8);
    if (12U + count * 32U > bk_iconpak_size) return NULL;

    for (uint32_t i = 0; i < count; i++) {
        if ((i & 3U) == 0) bootsplash_pulse();
        uint8_t *e = data + 12U + i * 32U;
        uint32_t w = bk_iconpak_rd32(e + 16);
        uint32_t h = bk_iconpak_rd32(e + 20);
        uint32_t off = bk_iconpak_rd32(e + 24);
        uint32_t size = bk_iconpak_rd32(e + 28);
        uint32_t need;
        uint32_t *src;
        uint32_t *out;

        if (!bk_iconpak_name16_eq(e, wanted)) continue;
        if (!w || !h) return NULL;

        need = w * h * sizeof(uint32_t);
        if (size < need) return NULL;
        if (off > bk_iconpak_size || off + need > bk_iconpak_size)
            return NULL;

        src = (uint32_t *)(data + off);
        out = (uint32_t *)kmalloc((uint32_t)out_w *
                                  (uint32_t)out_h *
                                  sizeof(uint32_t));
        if (!out) return NULL;

        for (int y = 0; y < out_h; y++) {
            uint32_t sy = ((uint32_t)y * h) / (uint32_t)out_h;
            if ((y & 3) == 0) bootsplash_pulse();
            for (int x = 0; x < out_w; x++) {
                uint32_t sx = ((uint32_t)x * w) / (uint32_t)out_w;
                out[y * out_w + x] = src[sy * w + sx];
            }
        }

        return out;
    }

    return NULL;
}

#endif /* BK_ICONPAK_CENTRAL_LOADER */


static void bk_iconpak_preload_on_gui_start(void) {
    /*
     * Precarga temprana. Si VFS todavía no está listo, falla barato;
     * la primera carga real volverá a intentar.
     */
    bootsplash_debug("@H6021B258");
    (void)bk_iconpak_load_once();
    bootsplash_debug("@H8D532FD4");
}



typedef struct {
    const char *legacy_path;
    const char *graphics_name;
} desk_graphics_alias_t;

/*
 * Mientras BlesKernOS termina su set propio de iconos, todas las rutas
 * historicas bajo /ICONS se resuelven primero contra GRAPHICS.PAK.
 * Esto mantiene una sola familia visual (KDE 1) sin obligar a reescribir
 * cada programa.  BLESKERNOS.BMP queda fuera a proposito: es el primer
 * icono propio y se usa en el boton central del Deskbar.
 */
static const desk_graphics_alias_t g_desk_graphics_aliases[] = {
    {"/ICONS/TRASH.BMP",     "RecycleEmpty"},
    {"/ICONS/FOLDER.BMP",    "Folder"},
    {"/ICONS/FILE.BMP",      "FileText"},
    {"/ICONS/TEXT.BMP",      "FileText"},
    {"/ICONS/CONFIG.BMP",    "Settings"},
    {"/ICONS/IMAGE.BMP",     "Camera"},
    {"/ICONS/OBJECT.BMP",    "ExecutableWindow"},
    {"/ICONS/ABOUT.BMP",     "InfoBubble"},
    {"/ICONS/MIDI.BMP",      "MidiFile"},
    {"/ICONS/HDD.BMP",       "Format16"},
    {"/ICONS/CD.BMP",        "ReaderCd"},
    {"/ICONS/CDROM.BMP",     "ReaderCd"},
    {"/ICONS/USB.BMP",       "ReaderOpened"},
    {"/ICONS/FLOPPY.BMP",    "ReaderDisket"},
    {"/ICONS/FILES.BMP",     "WindowsExplorer"},
    {"/ICONS/SHELL.BMP",     "MsDos"},
    {"/ICONS/EDITOR.BMP",    "Notepad"},
    {"/ICONS/CALC.BMP",      "Calculator"},
    {"/ICONS/MIDAMP.BMP",    "MediaAudio"},
    {"/ICONS/NETSCAPE.BMP",  "Globe"},
    {"/ICONS/PROCESOS.BMP",  "Taskman100"},
    {"/ICONS/MONITOR.BMP",   "DisplaySettings"},
    {"/ICONS/DISPLAY.BMP",   "DisplaySettings"},
    {"/ICONS/SOUND.BMP",     "Mmsys100"},
    {"/ICONS/MOUSE.BMP",     "Mouse"},
    {"/ICONS/KEYBOARD.BMP",  "Keyboard"},
    {"/ICONS/DATETIME.BMP",  "Timedate"},
    {"/ICONS/CPU.BMP",       "Processor"},
    {"/ICONS/MEMORY.BMP",    "Memory"},
    {"/ICONS/PCI.BMP",       "PciBus"},
    {"/ICONS/DEVICES.BMP",   "HardwareDiag"},
    {"/ICONS/SYSTEM.BMP",    "FileManager"},
    {"/ICONS/INTERNET.BMP",  "InternetSettings"},
    {"/ICONS/MODEM.BMP",     "ModemFax"},
    {"/ICONS/LANGUAGE.BMP",  "Intl101"},
    {"/ICONS/CONTROL.BMP",   "Controls3000"},
    {"/ICONS/WARNING.BMP",   "Warning"},
};

static const char *desk_graphics_alias_for_path(const char *path) {
    uint32_t count = (uint32_t)(sizeof(g_desk_graphics_aliases) /
                                sizeof(g_desk_graphics_aliases[0]));
    if (!path) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (kstrcmp(path, g_desk_graphics_aliases[i].legacy_path) == 0)
            return g_desk_graphics_aliases[i].graphics_name;
    }
    return NULL;
}

static uint32_t *desk_load_graphics_scaled(const char *resource_name,
                                           uint16_t output_width,
                                           uint16_t output_height) {
    gui_image_t image;
    uint32_t *pixels;

    if (!resource_name || !output_width || !output_height) return NULL;
    kmemset(&image, 0, sizeof(image));
    bool loaded = output_width == output_height && (output_width == 16 || output_width == 32)
        ? bk_graphics_icon_load_size(resource_name, output_width, &image)
        : bk_graphics_icon_load(resource_name, &image);
    if (!loaded ||
        !image.pixels || !image.width || !image.height) {
        if (image.pixels) bk_gui_image_free(&image);
        return NULL;
    }

    pixels = (uint32_t *)kmalloc((uint32_t)output_width *
                                 (uint32_t)output_height *
                                 sizeof(uint32_t));
    if (!pixels) {
        bk_gui_image_free(&image);
        return NULL;
    }

    /* Nearest-neighbour: los iconos KDE 1 conservan bordes de pixel
     * limpios aun cuando el escritorio pide 28, 44 o 64 px. */
    for (uint32_t y = 0; y < output_height; y++) {
        uint32_t sy = (y * image.height) / output_height;
        for (uint32_t x = 0; x < output_width; x++) {
            uint32_t sx = (x * image.width) / output_width;
            pixels[y * output_width + x] =
                image.pixels[sy * image.width + sx];
        }
    }
    bk_gui_image_free(&image);
    return pixels;
}

uint32_t *program_load_bmp_icon_scaled(const char *path,
                                       uint16_t output_width,
                                       uint16_t output_height) {
    const char *graphics_name = desk_graphics_alias_for_path(path);
    if (graphics_name) {
        /* Para rutas migradas no volver al BMP viejo: asi la sesion usa una
         * sola familia visual y un recurso faltante se detecta enseguida. */
        return desk_load_graphics_scaled(
            graphics_name, output_width, output_height);
    }

    /* Iconos propios/no migrados conservan la ruta clasica. */
    {
        uint32_t *bvi_icon = bk_bvi_load_bmp_path(
            path, (int)output_width, (int)output_height);
        if (bvi_icon) return bvi_icon;
    }

    {
        uint32_t *bk_pak_icon = bk_iconpak_load_bmp_path(
            path, (int)output_width, (int)output_height);
        if (bk_pak_icon) return bk_pak_icon;
    }

    return desk_load_bmp_scaled(path, output_width, output_height, true);
}

uint32_t *program_load_bmp_wallpaper_scaled(const char *path,
                                            uint16_t output_width,
                                            uint16_t output_height) {
    return desk_load_bmp_scaled(path, output_width, output_height, false);
}

uint32_t *program_load_bmp_icon(const char *path) {
    return program_load_bmp_icon_scaled(path, 28, 28);
}

void program_draw_icon_pixels(gui_surface_t *surface, int x, int y,
                              const uint32_t *pixels,
                              uint16_t width, uint16_t height) {
    gui_rect_t visible;
    gui_rect_t image;
    int start_x, start_y, end_x, end_y;

    if (!surface || !surface->pixels || !pixels) return;
    image = (gui_rect_t){x, y, width, height};
    if (!gui_rect_intersect(image, gui_gfx_get_clip(surface), &visible))
        return;
    start_x = visible.x - x;
    start_y = visible.y - y;
    end_x = start_x + visible.w;
    end_y = start_y + visible.h;

    /* El escritorio se repinta con un clip pequeño. Antes se recorría el BMP
     * completo (hasta 800x600) para cada movimiento del cursor aunque casi
     * todos los píxeles fueran descartados por gui_gfx_point_visible(). */
    for (int py = start_y; py < end_y; py++) {
        for (int px = start_x; px < end_x; px++) {
            uint32_t color = pixels[(uint32_t)py * width + px];
            uint8_t alpha = (uint8_t)(color >> 24);
            uint32_t rgb = color & 0x00FFFFFF;
            int dx = x + px;
            int dy = y + py;
            uint32_t *dst;

            if (alpha == 0) continue;
            if (alpha == 0xFF) {
                surface->pixels[(uint32_t)dy * surface->pitch +
                                (uint32_t)dx] = rgb;
                continue;
            }

            dst = &surface->pixels[(uint32_t)dy * surface->pitch +
                                   (uint32_t)dx];
            *dst = gui_color_blend(*dst, rgb, alpha);
        }
    }
}

static void desk_load_icon_image(desk_icon_t *icon,const char *path) {
    gui_image_t image={0};
    const char *resource=desk_graphics_alias_for_path(path);
    bool loaded = resource && (g_desktop_icon_size == 32
        ? bk_graphics_icon_load_size(resource, 32, &image)
        : bk_graphics_icon_load(resource, &image));
    if(loaded && image.pixels && image.width && image.height) {
        icon->original_pixels=image.pixels;
        icon->original_width=image.width;icon->original_height=image.height;
    } else {
        if(image.pixels)bk_gui_image_free(&image);
        icon->original_pixels=program_load_bmp_icon_scaled(path,64,64);
        icon->original_width=64;icon->original_height=64;
    }
    if(!icon->original_pixels)return;
    int size=g_desktop_icon_size;
    icon->pixels=bk_sys_alloc_zero(size*size*sizeof(uint32_t));
    if(!icon->pixels)return;
    for(int y=0;y<size;++y)for(int x=0;x<size;++x)
        icon->pixels[y*size+x]=icon->original_pixels[(y*icon->original_height/size)*icon->original_width+x*icon->original_width/size];
}

static void desk_clear_icon(desk_icon_t *icon) {
    if (!icon) return;
    if (icon->pixels) kfree(icon->pixels);
    if (icon->original_pixels) kfree(icon->original_pixels);
    kmemset(icon, 0, sizeof(*icon));
}

static void desk_remove_icons_from(deskmanager_state_t *st, int first) {
    if (!st) return;
    if (first < 0) first = 0;
    if (first > st->icon_count) first = st->icon_count;
    for (int i = first; i < st->icon_count; i++) {
        desk_clear_icon(&st->icons[i]);
    }
    st->icon_count = first;
}

static bool desk_cdrom_available(void) {
    /*
     * Esta funcion corre desde el repintado del escritorio. No debe emitir
     * comandos ATAPI ni tratar de montar una unidad vacia: en el PIT de
     * 300 Hz el primer sondeo ocurre cerca de los 3,33 segundos y bloqueaba
     * la GUI en algunas unidades reales.
     */
    return vfs_has_cdrom();
}

static bool desk_usb_available(void) {
    const char *mount = vfs_get_mount_name();

    if (mount && kstrcmp(mount, "usb0") == 0) return false;
    return block_get("usb0") != NULL;
}

static bool desk_floppy_available(void) {
    const char *mount = vfs_get_mount_name();

    if (!block_get("fd0")) return false;
    return mount && kstrcmp(mount, "fd0") == 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Dibujo de iconos
 * ────────────────────────────────────────────────────────────────────────── */

/*
 * Iconos integrados eliminados.
 * El escritorio ahora dibuja solo BMP externos cargados desde /ICONS.
 */

static void desk_draw_icon(gui_surface_t *surface, const desk_icon_t *icon) {
    if (icon->hidden) return;
    int x = icon->x;
    int y = icon->y;

    /* Selección / hover: fondo resaltado */
    if (icon->selected || icon->hovered || icon->pressed) {
        gui_gfx_fill_rect(surface,
            (gui_rect_t){x - 2, y - 2, DESK_ICON_W + 4, DESK_ICON_H + 4},
            icon->pressed ? 0x00204060 : 0x002A5080);
    }

    /* Imagen del icono (centrada horizontalmente en DESK_ICON_W) */
    int img_x = x + (DESK_ICON_W - DESK_ICON_IMG_W) / 2;
    int img_y = y + 2;

    if (icon->pixels) {
        program_draw_icon_pixels(surface, img_x, img_y,
                                 icon->pixels,
                                 DESK_ICON_IMG_W, DESK_ICON_IMG_H);
    }

    /* Etiqueta debajo */
    uint32_t fg = (icon->selected || icon->hovered || icon->pressed) ? 0x00FFFFFF : 0x00EEEEFF;
    int label_w = (int)bk_gui_text_width_px(icon->label,kstrlen(icon->label),8,false,false);
    gui_rect_t label_clip = {x - 2, y + DESK_ICON_IMG_H + 5,
                             DESK_ICON_W + 4, 24};
    if (label_w <= DESK_ICON_W) {
        int label_x = x + (DESK_ICON_W - label_w) / 2;
        bk_gui_surface_draw_text_px(surface, label_x,
                                     y + DESK_ICON_IMG_H + 8,
                                     icon->label, kstrlen(icon->label), fg, 8, false, false, false, label_clip);
    } else {
        char first[10] = {0};
        char second[16];
        int split = 0;
        while (icon->label[split] && split < 9) {
            first[split] = icon->label[split]; first[split + 1] = 0;
            if ((int)bk_gui_text_width_px(first,kstrlen(first),8,false,false) > DESK_ICON_W) { first[split] = 0; break; }
            split++;
        }
        first[split] = '\0';
        kstrncpy(second, icon->label + split, sizeof(second) - 1);
        second[sizeof(second) - 1] = '\0';
        int first_x = x + (DESK_ICON_W - (int)bk_gui_text_width_px(first,kstrlen(first),8,false,false)) / 2;
        int second_x = x + (DESK_ICON_W - (int)bk_gui_text_width_px(second,kstrlen(second),8,false,false)) / 2;
        bk_gui_surface_draw_text_px(surface, first_x,
                                     y + DESK_ICON_IMG_H + 5,
                                     first, kstrlen(first), fg, 8, false, false, false, label_clip);
        bk_gui_surface_draw_text_px(surface, second_x,
                                     y + DESK_ICON_IMG_H + 15,
                                     second, kstrlen(second), fg, 8, false, false, false, label_clip);
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Registro de iconos
 * ────────────────────────────────────────────────────────────────────────── */

static void desk_add_icon(deskmanager_state_t *st,
                           int x, int y,
                           const char *label,
                           desk_icon_open_fn open_fn) {
    if (st->icon_count >= DESK_MAX_ICONS) return;
    desk_icon_t *ic = &st->icons[st->icon_count++];
    ic->x           = x;
    ic->y           = y;
    ic->hovered     = false;
    ic->pressed     = false;
    ic->last_click_tick = 0;
    ic->last_click_x    = -999;
    ic->last_click_y    = -999;
    ic->open        = open_fn;
    const char *icon_path = NULL;
    if (open_fn == desk_open_trash) icon_path = "/ICONS/TRASH.BMP";
    else if (open_fn == desk_open_files_app) icon_path = "/ICONS/FILES.BMP";
    else if (open_fn == desk_open_shell_app) icon_path = "/ICONS/SHELL.BMP";
    else if (open_fn == desk_open_editor_app) icon_path = "/ICONS/EDITOR.BMP";
    else if (open_fn == desk_open_netsurf_app) icon_path = "/ICONS/NETSCAPE.BMP";
    else if (open_fn == desk_open_font_app) icon_path = "/ICONS/TEXT.BMP";
    else if (open_fn == desk_open_calculator_app) icon_path = "/ICONS/CALC.BMP";
    else if (open_fn == desk_open_events_app) icon_path = "/ICONS/ABOUT.BMP";
    else if (open_fn == desk_open_processmanager_app) icon_path = "/ICONS/PROCESOS.BMP";
    else if (open_fn == desk_open_performance_app) icon_path = "/ICONS/PROCESOS.BMP";
    else if (open_fn == desk_open_midamp_app) icon_path = "/ICONS/MIDAMP.BMP";
    else if (open_fn == desk_open_viewer_app) icon_path = "/ICONS/IMAGE.BMP";
    else if (open_fn == desk_open_control_panel) icon_path = "/ICONS/CONFIG.BMP";
    else if (open_fn == desk_open_cdrom) icon_path = "/ICONS/CDROM.BMP";
    else if (open_fn == desk_open_floppy) icon_path = "/ICONS/FLOPPY.BMP";
    else if (open_fn == desk_open_usb) icon_path = "/ICONS/USB.BMP";
    if(compat_mode_allow_icon_images() && icon_path)desk_load_icon_image(ic,icon_path);
    if (label && kstrcmp(label, "Editor de texto") == 0) label = "Editor";
    if (label && kstrcmp(label, "Rendimiento") == 0) label = "Rendim.";
    if (label && kstrcmp(label, "Visor de eventos") == 0) label = "Eventos";
    kstrncpy(ic->label, label ? label : "", sizeof(ic->label) - 1);
    ic->label[sizeof(ic->label) - 1] = '\0';
    desk_icon_position(ic,false);
}

static bool desk_has_icon_open_fn(const deskmanager_state_t *st,
                                  desk_icon_open_fn open_fn) {
    int i;
    if (!st || !open_fn) return false;
    for (i = 0; i < st->icon_count; i++) {
        if (st->icons[i].open == open_fn) return true;
    }
    return false;
}

static void desk_find_free_default_slot(const deskmanager_state_t *st,
                                        const gui_desktop_t *desktop,
                                        int *x_out, int *y_out) {
    int usable_h;
    int rows;
    int column;
    int row;

    if (!x_out || !y_out) return;
    usable_h = desktop ? desktop->surface.height - 48 : 552;
    rows = usable_h / (DESK_ICON_H+8);
    if (rows < 1) rows = 1;
    

    for (column = 0; column < (desktop ? (desktop->surface.width-24)/(DESK_ICON_W+12) : 8); column++) {
        for (row = 0; row < rows; row++) {
            int x = 12 + column * (DESK_ICON_W+12);
            int y = 12 + row * (DESK_ICON_H+8);
            bool occupied = false;
            int i;
            for (i = 0; st && i < st->icon_count; i++) {
                if (st->icons[i].x == x && st->icons[i].y == y) {
                    occupied = true;
                    break;
                }
            }
            if (!occupied) {
                *x_out = x;
                *y_out = y;
                return;
            }
        }
    }
    *x_out = 12;
    *y_out = 12 + (st ? st->icon_count : 0) * (DESK_ICON_H+8);
}

static void desk_add_default_icon_if_missing(deskmanager_state_t *st,
                                             gui_desktop_t *desktop,
                                             const char *label,
                                             desk_icon_open_fn open_fn) {
    int x;
    int y;
    if (!st || !open_fn || desk_has_icon_open_fn(st, open_fn)) return;
    desk_find_free_default_slot(st, desktop, &x, &y);
    desk_add_icon(st, x, y, label, open_fn);
}

static void desk_ensure_default_program_icons(deskmanager_state_t *st,
                                              gui_desktop_t *desktop) {
    desk_add_default_icon_if_missing(st, desktop, "Archivos",
                                     desk_open_files_app);
    desk_add_default_icon_if_missing(st, desktop, "Editor de texto",
                                     desk_open_editor_app);
    desk_add_default_icon_if_missing(st, desktop, "NetSurf",
                                     desk_open_netsurf_app);
}

static void desk_sync_drive_icons(deskmanager_state_t *st,
                                  gui_desktop_t *desktop,
                                  bool force) {
    bool show_cdrom;
    bool show_usb;
    bool show_floppy;
    uint32_t now;
    int drive_x;
    int drive_y;

    if (!st || !desktop) return;

    if (st->drag_icon >= 0 || st->selecting || desktop->file_drag_active) return;
    now = bk_sys_ticks();
    uint32_t sequence=bk_gui_files_sequence();
    if (!force && st->file_change_seen==sequence &&
        now-st->last_drive_poll<desk_ticks_from_ms(2000))return;
    st->file_change_seen=sequence;
    st->last_drive_poll=now;

    vfs_dir_entry_t *files = NULL;
    uint32_t file_count=0, hash=2166136261U;
    (void)bk_shell_read_directory(BK_DESKTOP_FILES,&files,&file_count);
    for (uint32_t i=0;i<file_count;++i) {
        if (files[i].name[0]=='.') continue;
        for (const char *n=files[i].name; *n; ++n) { hash ^= (uint8_t)*n; hash *= 16777619U; }
    }
    show_cdrom = desk_cdrom_available();
    show_usb = desk_usb_available();
    show_floppy = desk_floppy_available();
    if (!force &&
        st->last_surface_width == desktop->surface.width &&
        st->show_cdrom == show_cdrom &&
        st->show_usb == show_usb &&
        st->show_floppy == show_floppy && st->desktop_files_hash == hash) {
        bk_sys_free(files); return;
    }
    st->desktop_files_hash=hash;

    if(st->selected_icon>=st->base_icon_count)st->selected_icon=-1;
    desk_remove_icons_from(st, st->base_icon_count);
    st->show_cdrom = show_cdrom;
    st->show_usb = show_usb;
    st->show_floppy = show_floppy;
    st->last_surface_width = desktop->surface.width;

    drive_x = desktop->surface.width - DESK_ICON_W - 16;
    drive_y = 16;
    if (show_cdrom && st->icon_count < DESK_MAX_ICONS) {
        desk_add_icon(st, drive_x, drive_y, "CD-ROM",
                      desk_open_cdrom);
        drive_y += 80;
    }
    if (show_usb && st->icon_count < DESK_MAX_ICONS) {
        desk_add_icon(st, drive_x, drive_y, "USB",
                      desk_open_usb);
        drive_y += 80;
    }
    if (show_floppy && st->icon_count < DESK_MAX_ICONS) {
        desk_add_icon(st, drive_x, drive_y, "@H3737CE2B",
                      desk_open_floppy);
    }
    for (uint32_t i=0;i<file_count && st->icon_count<DESK_MAX_ICONS;++i) {
        if (files[i].name[0]=='.') continue;
        int x,y; desk_find_free_default_slot(st,desktop,&x,&y);
        desk_add_icon(st,x,y,files[i].name,NULL);
        desk_icon_t *icon=&st->icons[st->icon_count-1];
        snprintf(icon->path,sizeof(icon->path),BK_DESKTOP_FILES "/%s",files[i].name);
        desk_load_icon_image(icon,files[i].type==VFS_NODE_DIR ?
            "/ICONS/FOLDER.BMP" : "/ICONS/FILE.BMP");
        desk_icon_position(icon,false);
    }
    bk_sys_free(files);
    if(st->auto_arrange)desk_arrange_icons(st,desktop);
    gui_desktop_invalidate_all(desktop);

}

/* ──────────────────────────────────────────────────────────────────────────
 *  Hit-test
 * ────────────────────────────────────────────────────────────────────────── */

static int desk_hit_icon(const deskmanager_state_t *st, int mx, int my) {
    for (int i = 0; i < st->icon_count; i++) {
        if (st->icons[i].hidden) continue;
        gui_rect_t r = {st->icons[i].x - 2, st->icons[i].y - 2,
                        DESK_ICON_W + 4, DESK_ICON_H + 4};
        if (gui_rect_contains(r, mx, my)) return i;
    }
    return -1;
}

static gui_rect_t desk_icon_rect(const desk_icon_t *icon) {
    if (!icon) return (gui_rect_t){0, 0, 0, 0};
    return (gui_rect_t){icon->x - 2, icon->y - 2,
                        DESK_ICON_W + 4, DESK_ICON_H + 4};
}

static void desk_clear_selection(deskmanager_state_t *st) {
    for(int i=0;i<st->icon_count;++i)st->icons[i].selected=false;
    st->selected_icon=-1;
}
static void desk_select_area(deskmanager_state_t *st,gui_desktop_t *desktop,gui_rect_t area) {
    st->selected_icon=-1;
    for(int i=0;i<st->icon_count;++i) {
        gui_rect_t overlap;desk_icon_t *icon=&st->icons[i];
        bool selected=!icon->hidden && (st->selection_before[i] || gui_rect_intersect(area,desk_icon_rect(icon),&overlap));
        if(selected!=icon->selected) {icon->selected=selected;gui_desktop_invalidate_rect(desktop,desk_icon_rect(icon));}
        if(selected && st->selected_icon<0)st->selected_icon=i;
    }
}
static void desk_arrange_icons(deskmanager_state_t *st,gui_desktop_t *desktop) {
    int rows=(desktop->surface.height-104)/(DESK_ICON_H+8);
    if(rows<1)rows=1;
    unsigned slot=0;
    for(int i=0;i<st->icon_count;++i) {
        desk_icon_t *icon=&st->icons[i];if(icon->hidden)continue;
        icon->x=12+(slot/(unsigned)rows)*(DESK_ICON_W+12);
        icon->y=12+(slot%(unsigned)rows)*(DESK_ICON_H+8);++slot;
    }
}
static bool desk_resize_icons(deskmanager_state_t *st,int size) {
    if(size==g_desktop_icon_size)return true;
    uint32_t *images[DESK_MAX_ICONS]={0};
    for(int i=0;i<st->icon_count;++i)if(st->icons[i].original_pixels) {
        int width=st->icons[i].original_width,height=st->icons[i].original_height;
        images[i]=bk_sys_alloc_zero(size*size*sizeof(uint32_t));
        if(!images[i]) {for(int j=0;j<i;++j)bk_sys_free(images[j]);return false;}
        for(int y=0;y<size;++y)for(int x=0;x<size;++x)
            images[i][y*size+x]=st->icons[i].original_pixels[(y*height/size)*width+x*width/size];
    }
    for(int i=0;i<st->icon_count;++i)if(images[i]) {
        bk_sys_free(st->icons[i].pixels);st->icons[i].pixels=images[i];
    }
    g_desktop_icon_size=size;return true;
}
static void desk_icon_ghost(gui_desktop_t *desktop,const uint32_t *pixels,int size,unsigned count) {
    kmemset(desktop->drag_icon_mask,0,sizeof(desktop->drag_icon_mask));
    if(!pixels || size<1 || size>64)return;
    for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
        if(!(pixels[y*size+x]>>24))continue;
        bool edge=!x || !y || x==size-1 || y==size-1 ||
            !(pixels[y*size+x-1]>>24) || !(pixels[y*size+x+1]>>24) ||
            !(pixels[(y-1)*size+x]>>24) || !(pixels[(y+1)*size+x]>>24);
        desktop->drag_icon_mask[y*64+x]=edge?2:1;
    }
    desktop->drag_icon_size=size;desktop->drag_icon_count=count;
    desktop->drag_icon_ghost=true;
}

static gui_rect_t desk_context_menu_rect(const gui_context_menu_t *menu) {
    if (!menu) return (gui_rect_t){0, 0, 0, 0};
    return (gui_rect_t){menu->x, menu->y, menu->width,
                        menu->item_count * 22 + 4};
}

enum {
    DESK_CONTEXT_OPEN = 1,
    DESK_CONTEXT_FILES,
    DESK_CONTEXT_NEW_FOLDER,
    DESK_CONTEXT_NEW_TEXT,
    DESK_CONTEXT_CONTROL,
    DESK_CONTEXT_DISPLAY,
    DESK_CONTEXT_REFRESH,
    DESK_CONTEXT_DELETE,
    DESK_CONTEXT_DESKTOP,
    DESK_CONTEXT_AUTO,
    DESK_CONTEXT_GRID,
    DESK_CONTEXT_SMALL,
    DESK_CONTEXT_MEDIUM,
    DESK_CONTEXT_LARGE,
    DESK_CONTEXT_ARRANGE,

};

static void desk_context_callback(gui_window_t *window UNUSED,
                                  uint32_t item_id, void *context) {
    deskmanager_state_t *st = (deskmanager_state_t *)context;
    gui_desktop_t *desktop = g_desk_desktop;
    char path[VFS_MAX_PATH];
    if (!st || !desktop) return;
    if (item_id == DESK_CONTEXT_OPEN && st->context_icon >= 0 &&
        st->context_icon < st->icon_count) {
        desk_open_icon(desktop,&st->icons[st->context_icon]);
    } else if (item_id == DESK_CONTEXT_DELETE && st->context_icon >= 0 && st->context_icon < st->icon_count) {
        bool failed=false;
        bk_shell_job_t *job=bk_shell_job_create(desktop,BK_SHELL_JOB_TRASH,NULL);
        for(int i=0;i<st->icon_count;++i) {
            desk_icon_t *icon=&st->icons[i];
            if(icon->hidden || (!icon->selected && i!=st->context_icon))continue;
            if(icon->path[0]) {if(!bk_shell_job_add(job,icon->path))failed=true;}
            else {icon->hidden=true;icon->selected=false;desk_icon_position(icon,true);}
        }
        if(job && job->count && !failed)failed=!bk_shell_job_start(job);
        else bk_sys_free(job);
        if(st->auto_arrange)desk_arrange_icons(st,desktop);
        if(failed)gui_desktop_show_error(desktop,"Papelera","Algunos archivos no se pudieron enviar a la papelera.");
        gui_desktop_invalidate_all(desktop);
    } else if(item_id>=DESK_CONTEXT_AUTO && item_id<=DESK_CONTEXT_ARRANGE) {
        if(item_id==DESK_CONTEXT_AUTO)st->auto_arrange=!st->auto_arrange;
        else if(item_id==DESK_CONTEXT_GRID)st->align_grid=!st->align_grid;
        else if(item_id==DESK_CONTEXT_SMALL || item_id==DESK_CONTEXT_MEDIUM || item_id==DESK_CONTEXT_LARGE) {
            int size=item_id==DESK_CONTEXT_SMALL?32:item_id==DESK_CONTEXT_MEDIUM?48:64;
            if(!desk_resize_icons(st,size))return;
        }
        if(st->auto_arrange || item_id==DESK_CONTEXT_ARRANGE ||
           (item_id>=DESK_CONTEXT_SMALL && item_id<=DESK_CONTEXT_LARGE))desk_arrange_icons(st,desktop);
        for(int i=0;i<st->icon_count;++i)desk_icon_position(&st->icons[i],true);
        desk_layout_preferences(st,true);gui_desktop_invalidate_all(desktop);
    } else if (item_id == DESK_CONTEXT_FILES) {
        desk_open_files_app(desktop);
    } else if (item_id == DESK_CONTEXT_NEW_FOLDER) {
        for (int n = 1; n < 100; n++) {
            snprintf(path, sizeof(path), BK_DESKTOP_FILES "/Nueva carpeta %u", (uint32_t)n);
            if (vfs_mkdir(path)) break;
        }
    } else if (item_id == DESK_CONTEXT_NEW_TEXT) {
        for (int n = 1; n < 100; n++) {
            snprintf(path, sizeof(path), BK_DESKTOP_FILES "/Nuevo documento %u.txt", (uint32_t)n);
            if (vfs_write_all(path, NULL, 0)) {
                (void)program_execute_path_async_arg(desktop,
                    "/SYSTEM/PROGRAMS/TEXTEDITOR.BEX", path);
                break;
            }
        }
    } else if (item_id == DESK_CONTEXT_CONTROL) {
        desk_open_control_panel(desktop);
    } else if (item_id == DESK_CONTEXT_DISPLAY) {
        (void)program_execute_path_async(desktop, "/SYSTEM/CONTROL/DISPLAY.CPL");
    } else if (item_id == DESK_CONTEXT_REFRESH) {
        desk_sync_drive_icons(st, desktop, true);
        /* Drive icons may have appeared/disappeared, so this explicit refresh
         * is the one context action that still needs desktop-wide damage. */
        gui_desktop_invalidate_all(desktop);
    }
}

static void desk_open_context(deskmanager_state_t *st, gui_desktop_t *desktop,
                              int x, int y, int icon) {
    gui_rect_t limits;
    if (!st || !desktop) return;
    st->context_icon = icon;st->context_pressed=0;
    gui_context_menu_clear(&st->desktop_menu);
    gui_context_menu_clear(&st->context_menu);
    if (icon >= 0)
        (void)gui_context_menu_add_item(&st->context_menu,
            DESK_CONTEXT_OPEN, "@H5A216C13", true, desk_context_callback, st);
    if (icon >= 0)
        (void)gui_context_menu_add_item(&st->context_menu,DESK_CONTEXT_DELETE,
            st->icons[icon].path[0] ? "Enviar a papelera" : "Quitar icono",
            true,desk_context_callback,st);
    (void)gui_context_menu_add_item(&st->context_menu,DESK_CONTEXT_DESKTOP,
        "Escritorio",true,NULL,st);
    (void)gui_context_menu_add_item(&st->context_menu,
        DESK_CONTEXT_FILES, "@H3D400190", true, desk_context_callback, st);
    (void)gui_context_menu_add_item(&st->context_menu,
        DESK_CONTEXT_NEW_FOLDER, "@HAAAADA68", true,
        desk_context_callback, st);
    (void)gui_context_menu_add_item(&st->context_menu,
        DESK_CONTEXT_NEW_TEXT, "@HB9A3132A", true,
        desk_context_callback, st);
    (void)gui_context_menu_add_item(&st->context_menu,
        DESK_CONTEXT_CONTROL, "@H59EF0B79", true,
        desk_context_callback, st);
    (void)gui_context_menu_add_item(&st->context_menu,
        DESK_CONTEXT_DISPLAY, "@H898CDA5D", true,
        desk_context_callback, st);
    (void)gui_context_menu_add_item(&st->context_menu,
        DESK_CONTEXT_REFRESH, "@H7CC1E2B3", true, desk_context_callback, st);
    limits = (gui_rect_t){0, 0, desktop->surface.width,
                          desktop->surface.height - 80};
    gui_context_menu_open(&st->context_menu, x, y, limits);
    gui_desktop_invalidate_rect(desktop,
                                desk_context_menu_rect(&st->context_menu));
    gui_request_paint();
}

static void desk_open_desktop_menu(deskmanager_state_t *st,gui_desktop_t *desktop,int row) {
    gui_context_menu_t *menu=&st->desktop_menu;gui_context_menu_clear(menu);
    (void)gui_context_menu_add_item(menu,DESK_CONTEXT_AUTO,st->auto_arrange?"[x] Organizar auto":"[ ] Organizar auto",true,desk_context_callback,st);
    (void)gui_context_menu_add_item(menu,DESK_CONTEXT_GRID,st->align_grid?"[x] Alinear cuadricula":"[ ] Alinear cuadricula",true,desk_context_callback,st);
    (void)gui_context_menu_add_item(menu,DESK_CONTEXT_SMALL,g_desktop_icon_size==32?"(*) Chicos":"( ) Chicos",true,desk_context_callback,st);
    (void)gui_context_menu_add_item(menu,DESK_CONTEXT_MEDIUM,g_desktop_icon_size==48?"(*) Medianos":"( ) Medianos",true,desk_context_callback,st);
    (void)gui_context_menu_add_item(menu,DESK_CONTEXT_LARGE,g_desktop_icon_size==64?"(*) Grandes":"( ) Grandes",true,desk_context_callback,st);
    (void)gui_context_menu_add_item(menu,DESK_CONTEXT_ARRANGE,"Organizar ahora",true,desk_context_callback,st);
    int x=st->context_menu.x+st->context_menu.width-2;
    if(x+menu->width>desktop->surface.width)x=st->context_menu.x-menu->width+2;
    gui_context_menu_open(menu,x,st->context_menu.y+2+row*22,
        (gui_rect_t){0,0,desktop->surface.width,desktop->surface.height-80});
}
static bool desk_context_event(deskmanager_state_t *st,gui_desktop_t *desktop,const gui_event_t *event) {
    if(!st->context_menu.open)return false;
    if(event->type==GUI_EVENT_KEY && event->key==KEY_ESCAPE) {
        gui_context_menu_close(&st->context_menu);gui_context_menu_close(&st->desktop_menu);
        gui_desktop_invalidate_all(desktop);return true;
    }
    if(event->type!=GUI_EVENT_MOUSE_MOVE && event->type!=GUI_EVENT_MOUSE_DOWN && event->type!=GUI_EVENT_MOUSE_UP)return false;
    gui_rect_t parent=desk_context_menu_rect(&st->context_menu);
    gui_rect_t child=desk_context_menu_rect(&st->desktop_menu);
    gui_context_menu_t *menu=NULL;
    if(st->desktop_menu.open && gui_rect_contains(child,event->x,event->y))menu=&st->desktop_menu;
    else if(gui_rect_contains(parent,event->x,event->y))menu=&st->context_menu;
    if(menu) {
        int row=(event->y-menu->y-3)/22;
        if(event->y<menu->y+3 || row<0 || row>=menu->item_count)return true;
        if(event->type==GUI_EVENT_MOUSE_MOVE) {
            menu->pressed_item=row;
            if(menu==&st->context_menu) {
                if(menu->items[row].id==DESK_CONTEXT_DESKTOP) {
                    if(!st->desktop_menu.open)desk_open_desktop_menu(st,desktop,row);
                } else gui_context_menu_close(&st->desktop_menu);
            }
        } else if(menu->items[row].id==DESK_CONTEXT_DESKTOP) {
            desk_open_desktop_menu(st,desktop,row);
        } else if(event->type==GUI_EVENT_MOUSE_DOWN && event->button==MOUSE_LEFT_BUTTON) {
            st->context_pressed=menu->items[row].id;
        } else if(event->type==GUI_EVENT_MOUSE_UP && event->button==MOUSE_LEFT_BUTTON) {
            uint32_t id=menu->items[row].id;
            if(st->context_pressed==id) {
                gui_context_menu_close(&st->context_menu);gui_context_menu_close(&st->desktop_menu);
                desk_context_callback(NULL,id,st);
            }
            st->context_pressed=0;
        }
    } else if(event->type==GUI_EVENT_MOUSE_DOWN) {
        gui_context_menu_close(&st->context_menu);gui_context_menu_close(&st->desktop_menu);st->context_pressed=0;
    }
    if(!menu && event->type==GUI_EVENT_MOUSE_UP)st->context_pressed=0;
    gui_desktop_invalidate_rect(desktop,parent);
    if(child.w && child.h)gui_desktop_invalidate_rect(desktop,child);
    if(st->desktop_menu.open)gui_desktop_invalidate_rect(desktop,desk_context_menu_rect(&st->desktop_menu));
    gui_request_paint();return menu!=NULL || event->type==GUI_EVENT_MOUSE_MOVE;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Pintar ventanas (igual que antes)
 * ────────────────────────────────────────────────────────────────────────── */

#ifndef BLES_VISION_SERVICE
static void deskmanager_paint_windows(gui_desktop_t *desktop,
                                       gui_surface_t *surface,
                                       gui_rect_t screen) {
    gui_rect_t win_clip;
    gui_rect_t content_clip;
    gui_rect_t saved_clip;

    /* gui_desktop_paint() already owns the desktop paint mutex for this whole
       traversal. Destruction/widget-list mutation therefore cannot free what
       we are walking, even if the scheduler runs a Ring-3 callback meanwhile.
       Keeping preemption disabled across all window/content/widget drawing
       caused long scheduler stalls on single-core Pentium II/III machines. */
    gui_window_t *window = desktop->first_window;
    while (window) {
        bool intersects = window->visible &&
                          gui_rect_intersect(window->bounds, screen,
                                             &win_clip);
        if (intersects)
            gui_window_paint(surface, window, screen);
        if (intersects && window->streaming_content && window->stream_ready) {
            /* Video/games publish a compact frame backing directly.  Never
             * bounce through a Ring-3 paint callback: that callback races the
             * producer and was the source of gray/old-frame flashes. */
            gui_window_paint_stream_content(surface, window, screen);
        } else if (intersects && window->content_paint) {
            if (window->content_pid) {
                bool dragging = desktop->drag_window == window;
                /* Los callbacks Ring 3 terminan despues de este frame. Se
                 * compone siempre la ultima imagen completa para no alternar
                 * el interior entre fondo vacio y contenido nuevo. */
                gui_window_paint_cached_content(surface, window, screen);
                if (window->dirty && window->content_pending) {
                    if (!dragging) window->content_repaint = true;
                } else if (window->dirty && !dragging) {
                    gui_surface_t *staging = NULL;
                    uint32_t arguments[3] = {
                        (uint32_t)(uintptr_t)window,
                        0U,
                        (uint32_t)(uintptr_t)window->content_context};
                    if (gui_window_begin_content_paint(window, surface,
                            &staging)) {
                        arguments[1] = (uint32_t)(uintptr_t)staging;
                    }
                    if (staging && task_queue_window_upcall(window,
                            window->content_pid,
                            (uint32_t)(uintptr_t)window->content_paint,
                            arguments, 3, NULL, 0, -2)) {
                        window->content_pending = true;
                        window->content_repaint = false;
                    } else {
                        if (staging)
                            gui_window_end_content_paint(window);
                        window->content_repaint = true;
                    }
                }
                /* Durante un arrastre el cache terminado se traslada junto
                 * con el marco. Pedir además un callback Ring 3 por cada
                 * paquete PS/2 sólo crea una cola de cuadros atrasados. */
            } else {
                saved_clip = gui_gfx_get_clip(surface);
                if (gui_rect_intersect(gui_window_content_rect(window),
                                       saved_clip, &content_clip)) {
                    gui_gfx_set_clip(surface, content_clip);
                    window->content_paint(window, surface,
                                          window->content_context);
                }
                gui_gfx_set_clip(surface, saved_clip);
            }
        }
        if (intersects)
            gui_window_paint_widgets(surface, window, screen);
        /* BLESKERNOS_SVGA3D_WINDOW_SURFACE_BEGIN */
        if (intersects)
            gui_gpu_compositor_capture_window(window, surface);
        /* BLESKERNOS_SVGA3D_WINDOW_SURFACE_END */
        if (intersects)
            gui_window_paint_menus(surface, window);
        window = window->next;
    }
}
#endif

static bool deskmanager_dispatch_widgets(gui_window_t *window,
                                          const gui_event_t *event) {
    gui_widget_t *widget;
    bool handled = false;
    if (!window) return false;
    for(widget=window->widgets;widget;widget=widget->next)
        if(gui_widget_is_dropdown_expanded(widget)) {
            (void)gui_widget_handle_event(window,widget,event);
            return true;
        }

    widget = window->widgets;
    while (widget) {
        if (gui_widget_handle_event(window, widget, event))
            handled = true;
        widget = widget->next;
    }
    return handled;
}

static uint8_t desk_resize_edges(gui_window_t *window, int x, int y) {
    uint8_t edges = 0;
    if (!window || !gui_window_contains(window, x, y)) return 0;
    if (!window->resizable) return 0;
    if (x < window->bounds.x + RESIZE_MARGIN) edges |= RESIZE_LEFT;
    if (x >= window->bounds.x + window->bounds.w - RESIZE_MARGIN) edges |= RESIZE_RIGHT;
    if (y < window->bounds.y + RESIZE_MARGIN) edges |= RESIZE_TOP;
    if (y >= window->bounds.y + window->bounds.h - RESIZE_MARGIN) edges |= RESIZE_BOTTOM;
    return edges;
}

static gui_cursor_style_t desk_resize_cursor(uint8_t edges) {
    if ((edges & (RESIZE_LEFT | RESIZE_RIGHT)) &&
        (edges & (RESIZE_TOP | RESIZE_BOTTOM)))
        return ((edges & RESIZE_LEFT && edges & RESIZE_TOP) ||
                (edges & RESIZE_RIGHT && edges & RESIZE_BOTTOM))
             ? GUI_CURSOR_SIZE_NWSE : GUI_CURSOR_SIZE_NESW;
    if (edges & (RESIZE_LEFT | RESIZE_RIGHT)) return GUI_CURSOR_SIZE_WE;
    if (edges & (RESIZE_TOP | RESIZE_BOTTOM)) return GUI_CURSOR_SIZE_NS;
    return GUI_CURSOR_ARROW;
}

static gui_rect_t desk_resize_bounds(gui_desktop_t *desktop, int x, int y) {
    gui_window_t *window = desktop ? desktop->resize_window : NULL;
    gui_rect_t bounds = desktop ? desktop->resize_start_bounds
                                : (gui_rect_t){0, 0, 0, 0};
    int dx;
    int dy;
    int min_w;
    int min_h;
    if (!desktop || !window) return bounds;
    dx = x - desktop->resize_start_x;
    dy = y - desktop->resize_start_y;

    if (desktop->resize_edges & RESIZE_LEFT) {
        bounds.x += dx;
        bounds.w -= dx;
    }
    if (desktop->resize_edges & RESIZE_RIGHT) bounds.w += dx;
    if (desktop->resize_edges & RESIZE_TOP) {
        bounds.y += dy;
        bounds.h -= dy;
    }
    if (desktop->resize_edges & RESIZE_BOTTOM) bounds.h += dy;

    min_w = window->min_w > 0 ? window->min_w : 160;
    min_h = window->min_h > 0 ? window->min_h : 90;
    if (bounds.w < min_w) {
        if (desktop->resize_edges & RESIZE_LEFT) bounds.x -= min_w - bounds.w;
        bounds.w = min_w;
    }
    if (bounds.h < min_h) {
        if (desktop->resize_edges & RESIZE_TOP) bounds.y -= min_h - bounds.h;
        bounds.h = min_h;
    }
    /* Do not satisfy screen-edge constraints by squeezing below min_w/min_h.
       That was able to defeat the minimum computed above and reintroduce the
       exact overlap the minimum was meant to prevent.  Preserve the size and
       slide the window back into the work area; only a physically smaller
       desktop is allowed to cap it. */
    {
        int work_w = (int)desktop->surface.width;
        int work_h = (int)desktop->surface.height - 52;
        if (work_w < 1) work_w = 1;
        if (work_h < 1) work_h = 1;
        if (bounds.w > work_w) {
            bounds.x = 0;
            bounds.w = work_w;
        } else {
            if (bounds.x < 0) bounds.x = 0;
            if (bounds.x + bounds.w > work_w) bounds.x = work_w - bounds.w;
        }
        if (bounds.h > work_h) {
            bounds.y = 0;
            bounds.h = work_h;
        } else {
            if (bounds.y < 0) bounds.y = 0;
            if (bounds.y + bounds.h > work_h) bounds.y = work_h - bounds.h;
        }
    }
    return bounds;
}

static void desk_resize_window(gui_desktop_t *desktop, int x, int y) {
    gui_window_t *window = desktop ? desktop->resize_window : NULL;
    gui_rect_t bounds;
    if (!desktop || !window) return;
    bounds = desk_resize_bounds(desktop, x, y);
    if (desktop->drag_outline_enabled) {
        desktop->drag_icon_ghost=false;
        gui_rect_t old_outline = desktop->drag_outline_bounds;
        if (old_outline.x != bounds.x || old_outline.y != bounds.y ||
            old_outline.w != bounds.w || old_outline.h != bounds.h) {
            desktop->drag_outline_bounds = bounds;
            desktop->drag_outline_visible = true;
            gui_desktop_invalidate_overlay_rect(desktop, old_outline);
            gui_desktop_invalidate_overlay_rect(desktop, bounds);
            (void)bk_system_sound_event_play(BK_SYSTEM_SOUND_WINDOW_RESIZE);
        }
        return;
    }
    if (window->bounds.x != bounds.x || window->bounds.y != bounds.y ||
        window->bounds.w != bounds.w || window->bounds.h != bounds.h) {
        gui_rect_t old_bounds = window->bounds;
        window->bounds = bounds;
        window->mode_restore_valid = false;
        window->dirty = true;
        gui_desktop_invalidate_rect(desktop, old_bounds);
        gui_desktop_invalidate_rect(desktop, bounds);
        (void)bk_system_sound_event_play(BK_SYSTEM_SOUND_WINDOW_RESIZE);
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Paint principal
 * ────────────────────────────────────────────────────────────────────────── */

static void deskmanager_paint(gui_program_t *program,
                               gui_desktop_t *desktop,
                               gui_surface_t *surface) {
    deskmanager_state_t *st = (deskmanager_state_t *)program->state;
    gui_rect_t full_screen;
    gui_rect_t screen;

    if (!desktop || !surface) return;

    /* Vision receives a staging surface whose clip is exactly the damaged
     * region selected by the compositor.  Older code threw that information
     * away and traversed/repainted every visible window for every tiny change
     * (clock tick, button hover, caret, etc.).  Once DeskManager moved to
     * Ring 3 that became catastrophically expensive because every window
     * painter also crosses the userspace/kernel boundary.  Keep all shell work
     * bounded to the clip that actually needs rebuilding. */
    full_screen = (gui_rect_t){0, 0, surface->width, surface->height};
    if (!gui_rect_intersect(full_screen, gui_gfx_get_clip(surface), &screen))
        return;

    /* BLESKERNOS_SVGA3D_FRAME_BEGIN */
    gui_gpu_compositor_begin_frame(desktop, surface);
    /* BLESKERNOS_SVGA3D_FRAME_END */
    if (st && !st->setup_only) desk_sync_drive_icons(st, desktop, false);

#ifdef BLES_VISION_SERVICE
    /* Input may open a popup after this callback was queued with a smaller
     * clip. Capture its geometry BEFORE painting and draw every new popup in
     * full; publish that expanded clip together with the cached pixels. */
    program->staging_overlay_count = 0;
    if (st && !st->setup_only) {
        vision_deskbar_overlay_rects(program, desktop);
        if (st->context_menu.open && program->staging_overlay_count < 12U)
            program->staging_overlays[program->staging_overlay_count++] = desk_context_menu_rect(&st->context_menu);
        if (st->desktop_menu.open && program->staging_overlay_count < 12U)
            program->staging_overlays[program->staging_overlay_count++] = desk_context_menu_rect(&st->desktop_menu);
        for (uint32_t i=1;i<program->staging_overlay_count;i++) {
            gui_rect_t r=program->staging_overlays[i];
            int left=screen.x<r.x ? screen.x : r.x;
            int top=screen.y<r.y ? screen.y : r.y;
            int right=screen.x+screen.w>r.x+r.w ? screen.x+screen.w : r.x+r.w;
            int bottom=screen.y+screen.h>r.y+r.h ? screen.y+screen.h : r.y+r.h;
            screen=(gui_rect_t){left,top,right-left,bottom-top};
        }
        if (!gui_rect_intersect(full_screen,screen,&screen)) return;
        gui_gfx_set_clip(surface,screen);
    }
#endif

    if (g_desktop_wallpaper && (g_wallpaper_tiled_source ||
        (g_wallpaper_w == surface->width && g_wallpaper_h == surface->height))) {
        desk_wallpaper_copy_dirty(surface);
    } else {
        gui_gfx_fill_rect(surface, screen,
            g_desktop_background ? g_desktop_background
                                 : DESK_DEFAULT_BACKGROUND);
    }

    /* Only desktop icons touching the damaged region need to be redrawn. */
    if (st && !st->setup_only) {
        for (int i = 0; i < st->icon_count; i++) {
            gui_rect_t icon_rect = desk_icon_rect(&st->icons[i]);
            gui_rect_t ignored;
            if (gui_rect_intersect(icon_rect, screen, &ignored))
                desk_draw_icon(surface, &st->icons[i]);
        }
    }

    /* BLESKERNOS_SVGA3D_BACKGROUND_BEGIN */
    gui_gpu_compositor_capture_background(surface);
    /* BLESKERNOS_SVGA3D_BACKGROUND_END */
#ifndef BLES_VISION_SERVICE
    deskmanager_paint_windows(desktop, surface, screen);
#endif
    /* Drag/resize and minimize/restore outlines are compositor overlays.
       Keeping them out of the Vision shell cache avoids a Ring-3 repaint on
       every mouse packet and keeps the animation above application windows. */
    if (st && !st->setup_only) {
        gui_context_menu_paint(surface, &st->context_menu);
        if(st->context_menu.open)for(unsigned i=0;i<st->context_menu.item_count;++i)
            if(st->context_menu.items[i].id==DESK_CONTEXT_DESKTOP) {
                int x=st->context_menu.x+st->context_menu.width-13,y=st->context_menu.y+9+i*22;
                for(int n=0;n<5;++n)gui_gfx_draw_line(surface,x+n,y+n,x+n,y+8-n,0x00181818);
            }
        gui_context_menu_paint(surface,&st->desktop_menu);
#ifdef BLES_VISION_SERVICE
        /* One Ring-3 cache for the complete shell: background, windows and
         * Deskbar are rendered atomically into the same staging surface. */
        vision_deskbar_paint_layer(desktop, surface);

#endif
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Handle events
 * ────────────────────────────────────────────────────────────────────────── */

static gui_window_t *desk_focus_fallback(gui_desktop_t *desktop,
                                         gui_window_t *excluded) {
    gui_window_t *candidate;
    if (!desktop) return NULL;
    candidate = desktop->last_window;
    while (candidate) {
        if (candidate != excluded && candidate->visible && candidate->listed &&
            candidate->input_enabled && candidate->destroy_state == 0U)
            return candidate;
        candidate = candidate->prev;
    }
    return NULL;
}

static bool desk_queue_files_drop(gui_desktop_t *desktop,gui_window_t *target,
                                 const gui_event_t *event,const char (*paths)[260],unsigned count) {
    if(!count || count>DESK_MAX_ICONS)return false;
    uint32_t now=bk_sys_ticks();
    for(unsigned i=0;i<4;++i) {
        if(desktop->file_drops[i].used && now-desktop->file_drops[i].tick>desk_ticks_from_ms(10000)) {
            bk_sys_free(desktop->file_drop_batches[i]);desktop->file_drop_batches[i]=NULL;
            __sync_lock_release(&desktop->file_drops[i].used);
        }
        if(!__sync_bool_compare_and_swap(&desktop->file_drops[i].used,0,1))continue;
        desktop->file_drop_batches[i]=NULL;desktop->file_drop_counts[i]=count;desktop->file_drop_positions[i]=0;
        if(count>1) {
            desktop->file_drop_batches[i]=bk_sys_alloc_zero(count*260);
            if(!desktop->file_drop_batches[i]) {__sync_lock_release(&desktop->file_drops[i].used);return false;}
            kmemcpy(desktop->file_drop_batches[i],paths,count*260);
        }
        desktop->file_drop_windows[i]=target;
        uint32_t token=++desktop->file_drop_sequence;
        bk_runtime_strncpy(desktop->file_drops[i].path,paths[0],259);desktop->file_drops[i].path[259]=0;
        desktop->file_drops[i].token=token;desktop->file_drops[i].tick=now;
        __sync_synchronize();
        gui_event_t drop=*event;drop.type=GUI_EVENT_FILE_DROP;drop.dx=i;drop.dy=token;
        bool queued=gui_window_dispatch_event(target,&drop);
        if(!queued) {
            bk_sys_free(desktop->file_drop_batches[i]);desktop->file_drop_batches[i]=NULL;
            __sync_lock_release(&desktop->file_drops[i].used);
        }
        return queued;
    }
    return false;
}
static bool desk_queue_file_drop(gui_desktop_t *desktop,gui_window_t *target,
                                 const gui_event_t *event,const char *path) {
    char paths[1][260];bk_runtime_strncpy(paths[0],path,259);paths[0][259]=0;
    return desk_queue_files_drop(desktop,target,event,paths,1);
}

static void desk_drop_file(deskmanager_state_t *st,gui_desktop_t *desktop,
                           const gui_event_t *event,const char *path,int ignored_icon) {
    gui_window_t *target=gui_desktop_window_at(desktop,event->x,event->y);
    if (target) {
        if (target->input_enabled) (void)desk_queue_file_drop(desktop,target,event,path);
        return;
    }
    bool hidden=false;
    if(ignored_icon>=0) {hidden=st->icons[ignored_icon].hidden;st->icons[ignored_icon].hidden=true;}
    int hit=desk_hit_icon(st,event->x,event->y);
    if(ignored_icon>=0) st->icons[ignored_icon].hidden=hidden;
    const char *directory=BK_DESKTOP_FILES;
    unsigned action=BK_SHELL_JOB_MOVE;
    vfs_dir_entry_t info;
    if(hit>=0 && st->icons[hit].open==desk_open_trash)action=BK_SHELL_JOB_TRASH;
    else if(hit>=0 && st->icons[hit].path[0] && bk_file_stat(st->icons[hit].path,&info) && info.type==VFS_NODE_DIR)
        directory=st->icons[hit].path;
    if(action==BK_SHELL_JOB_MOVE && !st->auto_arrange && !bk_runtime_strcmp(directory,BK_DESKTOP_FILES)) {
        desk_icon_t placed={0};
        snprintf(placed.path,sizeof(placed.path),BK_DESKTOP_FILES "/%s",bk_shell_basename(path));
        placed.x=event->x-5;placed.y=event->y-2;
        if(placed.x<0)placed.x=0;
        if(placed.y<0)placed.y=0;
        if(placed.x>desktop->surface.width-DESK_ICON_W)placed.x=desktop->surface.width-DESK_ICON_W;
        if(placed.y>desktop->surface.height-80-DESK_ICON_H)placed.y=desktop->surface.height-80-DESK_ICON_H;
        desk_icon_position(&placed,true);
    }
    if(!bk_shell_job_one(desktop,action,path,directory))
        gui_desktop_show_error(desktop,"Mover archivo","No se pudo iniciar la transferencia.");

}

static bool deskmanager_handle_event(gui_program_t *program,
                                      gui_desktop_t *desktop,
                                      const gui_event_t *event) {
    deskmanager_state_t *st = (deskmanager_state_t *)program->state;
    gui_window_t *hit;
    bool handled = false;
    bool left_click;
    bool right_click;

    if (!desktop || !event) return false;
    if (desktop->file_drag_active) {
        if (event->type==GUI_EVENT_KEY && event->key==KEY_ESCAPE) {
            desktop->file_drag_active=false; desktop->file_drag_window=NULL;desktop->drag_outline_visible=false;desktop->drag_icon_ghost=false;
            gui_desktop_invalidate_all(desktop); return true;
        }
        if (event->type==GUI_EVENT_MOUSE_MOVE) {
            gui_rect_t previous=desktop->drag_outline_bounds;
            if(!desktop->drag_icon_ghost) {
                vfs_dir_entry_t info;
                const char *resource=bk_file_stat(desktop->file_drag_path,&info) && info.type==VFS_NODE_DIR ? "Folder" : "FileText";
                uint32_t *pixels=desk_load_graphics_scaled(resource,g_desktop_icon_size,g_desktop_icon_size);
                desk_icon_ghost(desktop,pixels,g_desktop_icon_size,1);bk_sys_free(pixels);
            }
            desktop->drag_outline_bounds=(gui_rect_t){event->x+8,event->y+8,desktop->drag_icon_size,desktop->drag_icon_size};
            desktop->drag_outline_visible=true;
            gui_desktop_invalidate_overlay_rect(desktop,previous);
            gui_desktop_invalidate_overlay_rect(desktop,desktop->drag_outline_bounds);
            gui_request_paint(); return true;
        }
        if (event->type==GUI_EVENT_MOUSE_UP && event->button==MOUSE_LEFT_BUTTON) {
            char source[VFS_MAX_PATH]; bk_runtime_strcpy(source,desktop->file_drag_path);
            desktop->file_drag_active=false; desktop->drag_outline_visible=false;desktop->drag_icon_ghost=false;
            desk_drop_file(st,desktop,event,source,-1);
            gui_window_t *window=desktop->first_window;
            gui_event_t refresh={0}; refresh.type=GUI_EVENT_FILES_CHANGED;
            while (window) { if(window==desktop->file_drag_window) { gui_window_dispatch_event(window,&refresh); break; } window=window->next; }
            desktop->file_drag_window=NULL;
            gui_desktop_invalidate_all(desktop); return true;
        }
    }
    if(st && !st->setup_only && event->type==GUI_EVENT_KEY && event->key==KEY_ESCAPE &&
       (st->selecting || st->drag_icon>=0)) {
        st->selecting=false;st->drag_icon=-1;st->icon_dragged=false;
        for(int i=0;i<st->icon_count;++i)st->icons[i].pressed=false;
        desktop->drag_outline_visible=false;desktop->drag_icon_ghost=false;
        gui_desktop_invalidate_all(desktop);return true;
    }
    if(st && !st->setup_only && st->selecting) {
        if(event->type==GUI_EVENT_MOUSE_MOVE) {
            int x=event->x,y=event->y;
            if(x<0)x=0;
            if(y<0)y=0;
            if(x>=desktop->surface.width)x=desktop->surface.width-1;
            if(y>=desktop->surface.height-80)y=desktop->surface.height-81;
            gui_rect_t previous=desktop->drag_outline_bounds;
            gui_rect_t area={x<st->select_x?x:st->select_x,y<st->select_y?y:st->select_y,
                x<st->select_x?st->select_x-x:x-st->select_x,y<st->select_y?st->select_y-y:y-st->select_y};
            desktop->drag_outline_bounds=area;desktop->drag_outline_visible=true;desktop->drag_icon_ghost=false;
            desk_select_area(st,desktop,area);
            gui_desktop_invalidate_overlay_rect(desktop,previous);
            gui_desktop_invalidate_overlay_rect(desktop,area);gui_request_paint();return true;
        }
        if(event->type==GUI_EVENT_MOUSE_UP && event->button==MOUSE_LEFT_BUTTON) {
            st->selecting=false;desktop->drag_outline_visible=false;
            gui_desktop_invalidate_overlay_rect(desktop,desktop->drag_outline_bounds);gui_request_paint();return true;
        }
    }
    if(st && !st->setup_only && st->drag_icon>=0 && event->type==GUI_EVENT_MOUSE_MOVE &&
       (event->buttons&MOUSE_LEFT_BUTTON)) {
        int dx=event->x-st->drag_start_x,dy=event->y-st->drag_start_y;
        if(st->icon_dragged || dx*dx+dy*dy>=36) {
            unsigned count=0;
            for(int i=0;i<st->icon_count;++i)if(st->icons[i].selected && !st->icons[i].hidden)++count;
            desk_icon_t *icon=&st->icons[st->drag_icon];
            if(!st->icon_dragged)desk_icon_ghost(desktop,icon->pixels,g_desktop_icon_size,count);
            st->icon_dragged=true;st->drag_dx=dx;st->drag_dy=dy;icon->last_click_tick=0;
            gui_rect_t previous=desktop->drag_outline_bounds;
            desktop->drag_outline_bounds=(gui_rect_t){icon->drag_x+dx+5,icon->drag_y+dy+2,
                g_desktop_icon_size+(count>1?6:0),g_desktop_icon_size+(count>1?6:0)};
            desktop->drag_outline_visible=true;
            gui_desktop_invalidate_overlay_rect(desktop,previous);
            gui_desktop_invalidate_overlay_rect(desktop,desktop->drag_outline_bounds);gui_request_paint();return true;
        }
    }
    if(st && !st->setup_only && st->drag_icon>=0 && event->type==GUI_EVENT_MOUSE_UP && event->button==MOUSE_LEFT_BUTTON) {
        st->drag_icon=-1;
        if(st->icon_dragged) {
            st->icon_dragged=false;desktop->drag_icon_ghost=false;desktop->drag_outline_visible=false;
            gui_window_t *window=gui_desktop_window_at(desktop,event->x,event->y);
            int target=-1;
            for(int i=0;i<st->icon_count;++i)if(!st->icons[i].selected && !st->icons[i].hidden &&
                gui_rect_contains(desk_icon_rect(&st->icons[i]),event->x,event->y)){target=i;break;}
            char destination[260]={0};bool trash=target>=0 && st->icons[target].open==desk_open_trash;
            vfs_dir_entry_t info;
            if(target>=0 && st->icons[target].path[0] && bk_file_stat(st->icons[target].path,&info) && info.type==VFS_NODE_DIR)
                bk_runtime_strcpy(destination,st->icons[target].path);
            bool changed=false,failed=false;
            if(window) {
                char (*paths)[260]=bk_sys_alloc_zero(st->icon_count*260);unsigned count=0;
                if(paths) {
                    for(int i=0;i<st->icon_count;++i)if(st->icons[i].selected && st->icons[i].path[0])
                        bk_runtime_strcpy(paths[count++],st->icons[i].path);
                    if(count)failed=!desk_queue_files_drop(desktop,window,event,paths,count);
                    bk_sys_free(paths);
                } else failed=true;
            } else if(trash || destination[0]) {
                bk_shell_job_t *job=bk_shell_job_create(desktop,trash?BK_SHELL_JOB_TRASH:BK_SHELL_JOB_MOVE,destination);
                for(int i=0;i<st->icon_count;++i) {
                    desk_icon_t *icon=&st->icons[i];if(!icon->selected || icon->hidden)continue;
                    if(icon->path[0]) {if(!bk_shell_job_add(job,icon->path))failed=true;}
                    else if(trash) {icon->hidden=true;icon->selected=false;desk_icon_position(icon,true);}
                }
                if(job && job->count && !failed)failed=!bk_shell_job_start(job);
                else bk_sys_free(job);
            } else if(!st->auto_arrange) {
                int dx=st->drag_dx,dy=st->drag_dy,min_x=desktop->surface.width,min_y=desktop->surface.height,max_x=0,max_y=0;
                for(int i=0;i<st->icon_count;++i)if(st->icons[i].selected && !st->icons[i].hidden) {
                    desk_icon_t *icon=&st->icons[i];
                    if(icon->drag_x<min_x)min_x=icon->drag_x;
                    if(icon->drag_y<min_y)min_y=icon->drag_y;
                    if(icon->drag_x+DESK_ICON_W>max_x)max_x=icon->drag_x+DESK_ICON_W;
                    if(icon->drag_y+DESK_ICON_H>max_y)max_y=icon->drag_y+DESK_ICON_H;
                }
                if(dx< -min_x)dx=-min_x;
                if(dy< -min_y)dy=-min_y;
                if(max_x+dx>desktop->surface.width)dx=desktop->surface.width-max_x;
                if(max_y+dy>desktop->surface.height-80)dy=desktop->surface.height-80-max_y;
                for(int i=0;i<st->icon_count;++i)if(st->icons[i].selected && !st->icons[i].hidden) {
                    desk_icon_t *icon=&st->icons[i];icon->x=icon->drag_x+dx;icon->y=icon->drag_y+dy;
                    if(st->align_grid) {icon->x=12+((icon->x-12+(DESK_ICON_W+12)/2)/(DESK_ICON_W+12))*(DESK_ICON_W+12);
                        icon->y=12+((icon->y-12+(DESK_ICON_H+8)/2)/(DESK_ICON_H+8))*(DESK_ICON_H+8);}
                    if(icon->x<0)icon->x=0;
                    if(icon->y<0)icon->y=0;
                    if(icon->x>desktop->surface.width-DESK_ICON_W)icon->x=desktop->surface.width-DESK_ICON_W;
                    if(icon->y>desktop->surface.height-80-DESK_ICON_H)icon->y=desktop->surface.height-80-DESK_ICON_H;
                    desk_icon_position(icon,true);
                }
            }
            for(int i=0;i<st->icon_count;++i)st->icons[i].pressed=false;
            if(changed){bk_gui_files_changed();desk_sync_drive_icons(st,desktop,true);}
            if(st->auto_arrange)desk_arrange_icons(st,desktop);
            if(failed)gui_desktop_show_error(desktop,"Mover archivos","Algunos archivos no se pudieron transferir. Revisa el destino.");
            gui_desktop_invalidate_all(desktop);return true;
        }
    }
    if(st && !st->setup_only && event->type==GUI_EVENT_KEY && !desktop->focused_window) {
        kbd_modifiers_t mods={0};(void)bk_input_key_modifiers(&mods);
        if(mods.ctrl && (event->key=='a' || event->key=='A')) {
            for(int i=0;i<st->icon_count;++i)if(!st->icons[i].hidden){st->icons[i].selected=true;st->selected_icon=i;}
            gui_desktop_invalidate_all(desktop);return true;
        }
    }
    if(st && !st->setup_only && event->type==GUI_EVENT_KEY && event->key==KEY_DELETE && !desktop->focused_window && st->selected_icon>=0) {
        st->context_icon=st->selected_icon;
        desk_context_callback(NULL,DESK_CONTEXT_DELETE,st); return true;
    }
    if(st && !st->setup_only && desk_context_event(st,desktop,event))return true;
#ifdef BLES_VISION_SERVICE
    /* Preserve the old z-order semantics: Deskbar was the last registered
     * program, therefore it had first chance to consume input. */
    if (st && !st->setup_only &&
        vision_deskbar_handle_event_layer(desktop, event))
        return true;
#endif
    left_click = event->button == MOUSE_LEFT_BUTTON;
    right_click = event->button == MOUSE_RIGHT_BUTTON;


    /* ── Primero probar si el clic cae sobre una ventana existente ── */
    if (event->type == GUI_EVENT_MOUSE_DOWN) {
        hit = gui_desktop_window_at(desktop, event->x, event->y);
        if (hit) {
            /*
             * Win32 keeps a disabled owner below its active modal child.
             * Raising it here made nested WinZip dialogs cover the license
             * window and left every visible button inert.
             */
            if (!hit->input_enabled) {
                gui_window_t *modal = desktop->last_window;
                while (modal && (!modal->visible || !modal->input_enabled ||
                                 modal->owner_pid != hit->owner_pid))
                    modal = modal->prev;
                if (modal) {
                    gui_desktop_raise_window(desktop, modal);
                    gui_desktop_focus_window(desktop, modal);
                }
                return true;
            }
            gui_desktop_raise_window(desktop, hit);
            gui_desktop_focus_window(desktop, hit);
            if (!left_click) {
                if (right_click &&
                    gui_window_open_text_context_at(hit, event->x, event->y)) {
                    hit->dirty = true;
                    gui_request_paint();
                    return true;
                }
                handled = gui_window_dispatch_event(hit, event) || handled;
                return handled || right_click;
            }
            if (gui_window_handle_menu_event(hit, event)) {
                return true;
            }
            gui_window_button_t button =
                gui_window_titlebar_button_at(hit, event->x, event->y);
            if (!st || !st->setup_only) {
                if (button == GUI_WINDOW_BUTTON_CLOSE) {
                    if (hit->extended_events) {
                        gui_event_t close_event = {0};
                        close_event.type = GUI_EVENT_CLOSE;
                        /* A queued Ring-3 callback counts as handled. VCL owns
                           the close policy; if queueing fails, fall back to the
                           historical immediate close. */
                        if (gui_window_dispatch_event(hit, &close_event))
                            return true;
                    }
                    gui_window_t *fallback = desk_focus_fallback(desktop, hit);
                    gui_window_close(hit);
                    gui_desktop_focus_window(desktop, fallback);
                    return true;
                }
                if (button == GUI_WINDOW_BUTTON_MINIMIZE) {
                    gui_window_t *fallback = desk_focus_fallback(desktop, hit);
                    gui_rect_t target;
                    gui_rect_t from = hit->bounds;
                    if (!deskbar_window_button_rect(desktop, hit, &target)) {
                        target = (gui_rect_t){
                            desktop->surface.width / 2 - 24,
                            desktop->surface.height - 27,
                            48, 24
                        };
                    }
                    gui_desktop_start_minimize_animation(
                        desktop, from, target, pit_get_ticks(),
                        desk_ticks_from_ms(180U));
                    gui_window_minimize(hit);
                    gui_desktop_focus_window(desktop, fallback);
                    return true;
                }
                uint8_t edges = desk_resize_edges(hit, event->x, event->y);
                if (edges) {
                    desktop->resize_window = hit;
                    desktop->resize_edges = edges;
                    desktop->resize_start_bounds = hit->bounds;
                    desktop->resize_start_x = event->x;
                    desktop->resize_start_y = event->y;
                    bk_system_sound_resize_begin();
                    if (desktop->drag_outline_enabled) {
        desktop->drag_icon_ghost=false;
                        desktop->drag_outline_bounds = hit->bounds;
                        desktop->drag_outline_visible = true;
                        gui_desktop_invalidate_overlay_rect(desktop, hit->bounds);
                    }
                    return true;
                }
                if (gui_window_titlebar_contains(hit, event->x, event->y)) {
                    desktop->drag_window  = hit;
                    desktop->drag_off_x   = event->x - hit->bounds.x;
                    desktop->drag_off_y   = event->y - hit->bounds.y;
                    if (desktop->drag_outline_enabled) {
        desktop->drag_icon_ghost=false;
                        desktop->drag_outline_bounds = hit->bounds;
                        desktop->drag_outline_visible = true;
                        gui_desktop_invalidate_overlay_rect(desktop,
                                                            hit->bounds);
                    }
                }
            } else if (button != GUI_WINDOW_BUTTON_NONE ||
                       gui_window_titlebar_contains(hit, event->x, event->y)) {
                /* SETUP.BEX es modal durante el primer arranque: su ventana no
                   puede cerrarse, minimizarse, moverse ni redimensionarse. */
                return true;
            }
            handled = deskmanager_dispatch_widgets(hit, event) || handled;
            handled = gui_window_dispatch_event(hit, event) || handled;

            /* Quitar hover/press de todos los iconos */
            if (st) {
                for (int i = 0; i < st->icon_count; i++) {
                    st->icons[i].pressed = false;
                }
            }
            return true;
        }

        /* En modo Setup el fondo es decorativo: todo el input permanece
           capturado por la única ventana del asistente. */
        if (st && st->setup_only) {
            if (desktop->focused_window)
                return gui_window_dispatch_event(desktop->focused_window, event);
            return true;
        }

        /* No hay ventana → probar icono */
        gui_desktop_focus_window(desktop, NULL);

        if (st) {
            int idx = desk_hit_icon(st, event->x, event->y);
            if (idx >= 0) {
                if (left_click) {
                    kbd_modifiers_t mods={0};(void)bk_input_key_modifiers(&mods);
                    if(!mods.ctrl && !mods.shift && !st->icons[idx].selected)desk_clear_selection(st);
                    if(mods.ctrl) {st->icons[idx].selected=!st->icons[idx].selected;st->icons[idx].last_click_tick=0;}
                    else st->icons[idx].selected=true;
                    st->selected_icon=st->icons[idx].selected?idx:-1;
                    st->drag_icon=st->icons[idx].selected?idx:-1;st->icon_dragged=false;
                    if(st->selected_icon<0)for(int i=0;i<st->icon_count;++i)
                        if(st->icons[i].selected && !st->icons[i].hidden){st->selected_icon=i;break;}
                    for(int i=0;i<st->icon_count;++i){st->icons[i].drag_x=st->icons[i].x;st->icons[i].drag_y=st->icons[i].y;}
                    gui_desktop_invalidate_all(desktop);
                    st->drag_start_x=event->x; st->drag_start_y=event->y;
                    st->drag_offset_x=event->x-st->icons[idx].x;
                    st->drag_offset_y=event->y-st->icons[idx].y;
                    st->icons[idx].pressed = st->icons[idx].selected && !mods.ctrl && !mods.shift;
                    gui_desktop_invalidate_rect(desktop,
                                                desk_icon_rect(&st->icons[idx]));
                    return true;
                }
                if (right_click) {
                    if(!st->icons[idx].selected) {desk_clear_selection(st);st->icons[idx].selected=true;}
                    st->selected_icon=idx;gui_desktop_invalidate_all(desktop);
                    desk_open_context(st, desktop, event->x, event->y, idx);
                    return true;
                }
                return false;
            }
            if(left_click) {
                kbd_modifiers_t mods={0};(void)bk_input_key_modifiers(&mods);
                if(!mods.ctrl && !mods.shift)desk_clear_selection(st);
                for(int i=0;i<st->icon_count;++i)st->selection_before[i]=st->icons[i].selected;
                st->selecting=true;st->select_x=event->x;st->select_y=event->y;
                desktop->drag_icon_ghost=false;desktop->drag_outline_visible=false;
                gui_desktop_invalidate_all(desktop);return true;
            }
            if(!right_click)return false;
            desk_open_context(st, desktop, event->x, event->y, -1);
            return true;
        }
        return false;
    }

    if (event->type == GUI_EVENT_MOUSE_UP) {
        if (desktop->focused_window) {
            if (gui_window_handle_menu_event(desktop->focused_window, event)) {
                return true;
            }
            handled = deskmanager_dispatch_widgets(desktop->focused_window, event);
            handled = gui_window_dispatch_event(desktop->focused_window, event) || handled;
        }
        if (desktop->drag_window) {
            if (desktop->drag_outline_enabled &&
                desktop->drag_outline_visible) {
                gui_window_t *dragged = desktop->drag_window;
                gui_rect_t old_bounds = dragged->bounds;
                gui_rect_t new_bounds = desktop->drag_outline_bounds;

                desktop->drag_outline_visible = false;
                dragged->bounds = new_bounds;
                dragged->mode_restore_valid = false;
                dragged->dirty = true;
                gui_desktop_invalidate_rect(desktop, old_bounds);
                gui_desktop_invalidate_rect(desktop, new_bounds);
            }
            handled = true;
        }
        desktop->drag_window = NULL;
        if (desktop->resize_window) {
            gui_window_t *resized = desktop->resize_window;
            handled = true;
            bk_system_sound_resize_end();
            if (desktop->drag_outline_enabled &&
                desktop->drag_outline_visible) {
                gui_rect_t old_bounds = resized->bounds;
                gui_rect_t new_bounds = desktop->drag_outline_bounds;
                desktop->drag_outline_visible = false;
                resized->bounds = new_bounds;
                resized->mode_restore_valid = false;
                resized->dirty = true;
                gui_desktop_invalidate_rect(desktop, old_bounds);
                gui_desktop_invalidate_rect(desktop, new_bounds);
            }
            /* Resize is useful to native control panels too: fixed-layout
             * windows now use it to reflow their controls inside safe groups.
             * Extended clients still receive exactly the same event as before. */
            if (resized->event_handler || resized->extended_events) {
                gui_event_t resize_event = {0};
                resize_event.type = GUI_EVENT_RESIZE;
                (void)gui_window_dispatch_event(resized, &resize_event);
            }
        }
        desktop->resize_window = NULL;
        desktop->resize_edges = 0;

        /* Soltar icono → detectar doble clic */
        if (st && !st->setup_only) {
            for (int i = 0; i < st->icon_count; i++) {
                desk_icon_t *ic = &st->icons[i];
                if (!ic->pressed) continue;
                ic->pressed = false;
                gui_desktop_invalidate_rect(desktop, desk_icon_rect(ic));

                gui_rect_t r = {ic->x - 2, ic->y - 2,
                                DESK_ICON_W + 4, DESK_ICON_H + 4};
                if (!gui_rect_contains(r, event->x, event->y)) continue;

                uint32_t now   = bk_sys_ticks();
                uint32_t delta = now - ic->last_click_tick;
                bool same = (event->x >= ic->last_click_x - 4 &&
                             event->x <= ic->last_click_x + 4 &&
                             event->y >= ic->last_click_y - 4 &&
                             event->y <= ic->last_click_y + 4);

                if (delta < desk_ticks_from_ms(DESK_DBLCLICK_MS) && same && ic->last_click_tick != 0) {
                    /* ¡Doble clic! */
                    desk_open_icon(desktop,ic);
                    ic->last_click_tick = 0;
                } else {
                    ic->last_click_tick = now;
                    ic->last_click_x    = event->x;
                    ic->last_click_y    = event->y;
                }
                handled = true;
            }
        }
        return handled;
    }

    if (event->type == GUI_EVENT_MOUSE_MOVE) {
        if (!desktop->cursor_custom) {
            uint8_t edges = desktop->resize_window ? desktop->resize_edges :
                desk_resize_edges(gui_desktop_window_at(desktop, event->x, event->y),
                                  event->x, event->y);
            gui_desktop_set_cursor_style(desktop, desk_resize_cursor(edges));
        }
        if (desktop->resize_window) {
            desk_resize_window(desktop, event->x, event->y);
            return true;
        }
        if (desktop->drag_window) {
            if (desktop->drag_outline_enabled) {
        desktop->drag_icon_ghost=false;
                gui_rect_t old_outline = desktop->drag_outline_bounds;
                gui_rect_t new_outline = old_outline;
                new_outline.x = event->x - desktop->drag_off_x;
                new_outline.y = event->y - desktop->drag_off_y;
                if (new_outline.x != old_outline.x ||
                    new_outline.y != old_outline.y) {
                    desktop->drag_outline_bounds = new_outline;
                    desktop->drag_outline_visible = true;
                    gui_desktop_invalidate_overlay_rect(desktop, old_outline);
                    gui_desktop_invalidate_overlay_rect(desktop, new_outline);
                }
            } else {
                desktop->drag_window->bounds.x =
                    event->x - desktop->drag_off_x;
                desktop->drag_window->bounds.y =
                    event->y - desktop->drag_off_y;
                desktop->drag_window->mode_restore_valid = false;
                desktop->drag_window->dirty = true;
            }
            /* Window movement consumes this pointer motion.  Forwarding it
               again to every widget/client caused extra Ring-3 upcalls and
               content paints while the outline was moving. */
            return true;
        }
        if (desktop->focused_window &&
            deskmanager_dispatch_widgets(desktop->focused_window, event)) {
            handled = true;
        }
        if (desktop->focused_window &&
            gui_window_dispatch_event(desktop->focused_window, event)) {
            handled = true;
        }

        /* Actualizar hover de iconos */
        if (st && !st->setup_only) {
            for (int i = 0; i < st->icon_count; i++) {
                gui_rect_t r = {st->icons[i].x - 2, st->icons[i].y - 2,
                                DESK_ICON_W + 4, DESK_ICON_H + 4};
                bool hovered = gui_rect_contains(r, event->x, event->y);
                if (st->icons[i].hovered != hovered) {
                    st->icons[i].hovered = hovered;
                    gui_desktop_invalidate_rect(desktop,
                                                desk_icon_rect(&st->icons[i]));
                    handled = true;
                }
            }
        }
        return handled;
    }

    if (event->type == GUI_EVENT_KEY && desktop->focused_window) {
        if (deskmanager_dispatch_widgets(desktop->focused_window, event))
            return true;
        return gui_window_dispatch_event(desktop->focused_window, event);
    }

    if (event->type == GUI_EVENT_KEY_UP && desktop->focused_window) {
        if (!desktop->focused_window->extended_events)
            return false;
        return gui_window_dispatch_event(desktop->focused_window, event);
    }

    if (event->type == GUI_EVENT_MOUSE_WHEEL && desktop->focused_window)
        return gui_window_dispatch_event(desktop->focused_window, event);

    return false;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Destructor
 * ────────────────────────────────────────────────────────────────────────── */

static void deskmanager_destroy(gui_program_t *program) {
    if (!program || !program->state) return;
    deskmanager_state_t *st = (deskmanager_state_t *)program->state;
#ifdef BLES_VISION_SERVICE
    vision_deskbar_shutdown_layer();
#endif
    for (int i = 0; i < st->icon_count; i++)
        desk_clear_icon(&st->icons[i]);
    if (g_desk_state == st) g_desk_state = NULL;
    if (g_desk_desktop && g_desk_state == NULL) g_desk_desktop = NULL;
    kfree(program->state);
    program->state = NULL;
}

void deskmanager_refresh_layout(void) {
    if (g_wallpaper_path[0]) (void)deskmanager_set_wallpaper(g_wallpaper_path);
    if (g_desk_state && g_desk_desktop)
        desk_sync_drive_icons(g_desk_state, g_desk_desktop, true);
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Instalación
 * ────────────────────────────────────────────────────────────────────────── */

void deskmanager_install(gui_desktop_t *desktop) {
    bootsplash_show("@HD53E5F05", 78);
    if (compat_mode_allow_icon_images()) bk_iconpak_preload_on_gui_start();
    bootsplash_pulse();

    deskmanager_state_t *st;
    gui_program_t *prog;

    if (!desktop) return;

    st = (deskmanager_state_t *)kzalloc(sizeof(deskmanager_state_t));
    if (!st) return;

    st->selected_icon=st->drag_icon=-1;
    st->auto_arrange=false;st->align_grid=true;
    (void)bk_file_mkdir(BK_DESKTOP_FILES);
    (void)bk_file_mkdir(BK_TRASH_FILES);
    desk_layout_preferences(st,false);
    st->icon_count = 0;
    st->base_icon_count = 0;
    g_desk_desktop = desktop;
    g_desk_state = st;

    /* El escritorio normal arranca con el fondo clásico de BlesKernOS. */
    if (!g_wallpaper_path[0] && compat_mode_allow_wallpaper()) {
        g_wallpaper_mode = DESK_WALLPAPER_TILE;
        if (!deskmanager_set_wallpaper(DESK_DEFAULT_WALLPAPER))
            g_desktop_background = DESK_DEFAULT_BACKGROUND;
    }

    void *config = NULL;
    uint32_t config_size = 0;
    bootsplash_pulse();
    bootsplash_debug("@H4F9706BD");
    if (bk_user_config_read_all(BK_DESKTOP_CONFIG_PATH,
                                BK_DESKTOP_CONFIG_LEGACY_PATH,
                                &config, &config_size) && config) {
        bootsplash_debug("@H8790A8E4");
        char *line = (char *)config;
        while (*line) {
            char *end = line;
            char *eq = NULL;
            while (*end && *end != '\r' && *end != '\n') {
                if (*end == '=' && !eq) eq = end;
                end++;
            }
            char saved = *end;
            *end = '\0';
            if (eq) {
                *eq++ = '\0';
                desk_icon_open_fn open_fn = NULL;
                if (kstrcmp(line, "files") == 0) {
                    open_fn = desk_open_files_app;
                } else if (kstrcmp(line, "shell") == 0) {
                    open_fn = desk_open_shell_app;
                } else if (kstrcmp(line, "editor") == 0) {
                    open_fn = desk_open_editor_app;
                } else if (kstrcmp(line, "netsurf") == 0 ||
                           kstrcmp(line, "browser") == 0) {
                    open_fn = desk_open_netsurf_app;
                } else if (kstrcmp(line, "font") == 0) {
                    open_fn = desk_open_font_app;
                } else if (kstrcmp(line, "calculator") == 0) {
                    open_fn = desk_open_calculator_app;
                } else if (kstrcmp(line, "processes") == 0) {
                    open_fn = desk_open_processmanager_app;
                } else if (kstrcmp(line, "events") == 0) {
                    open_fn = desk_open_events_app;
                } else if (kstrcmp(line, "performance") == 0) {
                    open_fn = desk_open_performance_app;
                } else if (kstrcmp(line, "midamp") == 0) {
                    open_fn = desk_open_midamp_app;
                } else if (kstrcmp(line, "viewer") == 0) {
                    open_fn = desk_open_viewer_app;

                } else if (kstrcmp(line, "control") == 0 ||
                           kstrcmp(line, "settings") == 0) {
                    open_fn = desk_open_control_panel;
                }
                if (open_fn) {
                    char *comma1 = eq;
                    char *comma2 = NULL;
                    while (*comma1 && *comma1 != ',') comma1++;
                    if (*comma1) {
                        *comma1++ = '\0';
                        comma2 = comma1;
                        while (*comma2 && *comma2 != ',') comma2++;
                    }
                    int x = 16;
                    int y = 12 + st->icon_count * 80;
                    if (comma2 && *comma2) {
                        *comma2++ = '\0';
                        x = 0;
                        y = 0;
                        while (*comma1 >= '0' && *comma1 <= '9')
                            x = x * 10 + (*comma1++ - '0');
                        while (*comma2 >= '0' && *comma2 <= '9')
                            y = y * 10 + (*comma2++ - '0');
                    }
                    desk_add_icon(st, x, y, eq, open_fn);
                }
            }
            *end = saved;
            line = end;
            while (*line == '\r' || *line == '\n') line++;
        }
        kfree(config);
    }

    /* Los perfiles viejos guardaban sólo Archivos y Rendimiento. Se conservan
     * sus posiciones y se completan los accesos de trabajo sin duplicarlos. */
    desk_ensure_default_program_icons(st, desktop);
    desk_add_icon(st,desktop->surface.width-74,desktop->surface.height-170,"Papelera",desk_open_trash);
    st->base_icon_count = st->icon_count;
    desk_sync_drive_icons(st, desktop, true);

    prog = gui_desktop_register_program(desktop, "deskmanager", st,
                                        deskmanager_paint,
                                        deskmanager_handle_event,
                                        deskmanager_destroy);
#ifdef BLES_VISION_SERVICE
    if (prog) prog->scene_owner = true;
#endif
    if (!prog) {
        if (g_desk_state == st) g_desk_state = NULL;
        desk_remove_icons_from(st, 0);
        kfree(st);
    }
}

void deskmanager_install_setup(gui_desktop_t *desktop,
                               const char *wallpaper_path) {
    deskmanager_state_t *st;
    gui_program_t *prog;

    if (!desktop) return;
    st = (deskmanager_state_t *)kzalloc(sizeof(deskmanager_state_t));
    if (!st) return;

    st->setup_only = true;
    st->icon_count = 0;
    st->base_icon_count = 0;
    g_desk_desktop = desktop;
    g_desk_state = st;

    /* Noche queda en el escritorio, no dentro del cliente de SETUP.BEX. */
    deskmanager_set_wallpaper_mode(DESK_WALLPAPER_STRETCH);
    if (!wallpaper_path || !deskmanager_set_wallpaper(wallpaper_path))
        deskmanager_set_background(0x00101830U);

    prog = gui_desktop_register_program(desktop, "setup-shell", st,
                                        deskmanager_paint,
                                        deskmanager_handle_event,
                                        deskmanager_destroy);
#ifdef BLES_VISION_SERVICE
    if (prog) prog->scene_owner = true;
#endif
    if (!prog) {
        if (g_desk_state == st) g_desk_state = NULL;
        kfree(st);
    }
}
