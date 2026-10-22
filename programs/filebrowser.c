#include "../sdk/include/bleskernos_theme.h"
/*
 * filebrowser.c — Navegador de archivos para BlesKernOS GUI
 *
 * Integración:
 *   1. Añadir al Makefile (ya está en KERNEL_SOURCES como programs/filebrowser.c)
 *   2. Declarar en programs.h:
 *        void filebrowser_install(gui_desktop_t *desktop);
 *        void filebrowser_open_from_desktop(gui_desktop_t *desktop);
 *   3. Llamar filebrowser_install(desktop) desde el arranque GUI.
 *
 *  deskmanager.c llama filebrowser_open_from_desktop() al doble clic en el icono.
 */

#include "../kernel/include/api.h"
#include <stdio.h>
#include "system/desktop/file_progress.h"

/* ── Ventana ── */
#define FB_WIN_X    60
#define FB_WIN_Y    40
#define FB_WIN_W   640
#define FB_WIN_H   380

/* ── Lista interna ── */
#define FB_LIST_X       6
#define FB_LIST_Y       2
#define FB_TILE_W      80
#define FB_TILE_H      64
#define FB_ICON_SIZE   32

/* ── Colores ── */
#define FB_COLOR_FOLDER  0x00E8B840
#define FB_COLOR_SEL     BK_THEME_SELECTION
#define FB_COLOR_TEXT    BK_THEME_TEXT
#define FB_COLOR_TEXTSEL BK_THEME_SELECTION_TEXT
#define FB_COLOR_BG      BK_THEME_PANEL

/* ── Doble clic dentro de la lista ── */
#define FB_DBLCLICK_TICKS ((bk_sys_tick_frequency() + 1U) / 2U)
#define FB_ICON_BYTES (FB_ICON_SIZE * FB_ICON_SIZE * sizeof(uint32_t))
#define FB_LIST_ROW_H 38
#define FB_TREE_W      150
#define FB_TREE_ROW_H   24
#define FB_TREE_PAD      4
#define FB_TREE_MAX_NODES 128
#define FB_TREE_MAX_OPEN   32
#define FB_DIR_INITIAL_CAPACITY 64U
#define FB_DIR_HARD_LIMIT 8192U

enum { FB_DIALOG_NONE = 0, FB_DIALOG_NEW_FOLDER, FB_DIALOG_RENAME };

enum {
    FB_DRIVE_NONE = 0,
    FB_DRIVE_HDD,
    FB_DRIVE_CD,
    FB_DRIVE_USB,
    FB_DRIVE_FLOPPY,
    FB_DRIVE_SD
};

enum {
    FB_MENU_OPEN = 100, FB_MENU_NEW_FOLDER, FB_MENU_RENAME, FB_MENU_DELETE,
    FB_MENU_PROPERTIES, FB_MENU_COPY, FB_MENU_CUT, FB_MENU_PASTE,
    FB_MENU_VIEW_ICONS, FB_MENU_VIEW_LIST, FB_MENU_REFRESH, FB_MENU_ABOUT
};

/* ──────────────────────────────────────────────── */

typedef struct {
    char path[VFS_MAX_PATH];
    char label[VFS_MAX_NAME];
    uint8_t depth;
    bool expandable;
    bool expanded;
} fb_tree_node_t;

typedef struct {
    gui_desktop_t   *desktop;
    gui_window_t   *window;
    char            cwd[VFS_MAX_PATH];
    vfs_dir_entry_t *entries;
    uint8_t         *entry_drive;
    char            (*entry_mount)[16];
    uint32_t        entry_capacity;
    uint32_t        entry_count;
    bool            drive_view;
    int             selected;
    int             scroll;
    gui_scrollbar_drag_t scrollbar_drag;
    uint32_t        btn_up_id;
    uint32_t        btn_copy_id;
    uint32_t        btn_paste_id;
    uint32_t        btn_new_id;
    uint32_t        btn_refresh_id;
    void           *associations;
    uint32_t        associations_size;
    bool            associations_owned;
    char            address[VFS_MAX_PATH];
    char            status[48];
    bool            editing_address;
    bool            list_view;
    bool            tree_root_expanded;
    bool            tree_cwd_expanded;
    int             tree_scroll;
    gui_scrollbar_drag_t tree_scrollbar_drag;
    char            tree_open[FB_TREE_MAX_OPEN][VFS_MAX_PATH];
    uint8_t         tree_open_count;
    fb_tree_node_t  tree_nodes[FB_TREE_MAX_NODES];
    uint16_t        tree_node_count;
    uint8_t         dialog;
    char            dialog_input[VFS_MAX_NAME];
    uint32_t        file_change_seen;
    bool            drag_pending;
    int             drag_x, drag_y, drag_entry;
    bool            delete_dialog_open;
    uint32_t        delete_dialog_token;
    uint32_t       *folder_icon;
    uint32_t       *file_icon;
    uint32_t       *text_icon;
    uint32_t       *config_icon;
    uint32_t       *image_icon;
    uint32_t       *object_icon;
    uint32_t       *midi_icon;
    uint32_t       *hdd_icon;
    uint32_t       *cd_icon;
    uint32_t       *usb_icon;
    uint32_t       *floppy_icon;
    uint32_t       *shell_icon;
    uint32_t       *editor_icon;
    uint32_t       *calc_icon;
    uint32_t       *files_icon;
    uint32_t       *midamp_icon;
    uint32_t       *netscape_icon;
    /* doble clic en lista */
    uint32_t        last_list_click_tick;
    int             last_list_click_idx;
} fb_state_t;

typedef struct {
    vfs_dir_entry_t entry;
    char path[VFS_MAX_PATH];
    char mount[16];
    uint8_t drive;
    int active_tab;
} fb_properties_t;

typedef struct {
    fb_state_t *owner;
    gui_desktop_t *desktop;
    gui_window_t *window;
    char path[VFS_MAX_PATH];
    char name[VFS_MAX_NAME];
    bool directory;
    bool permanent;
    uint8_t action;
    uint32_t delete_button_id;
    uint32_t cancel_button_id;
    uint32_t owner_token;
    uint32_t *warning_icon;
} fb_delete_dialog_t;

enum {
    FB_CONTEXT_OPEN = 1,
    FB_CONTEXT_COPY,
    FB_CONTEXT_CUT,
    FB_CONTEXT_PASTE,
    FB_CONTEXT_RENAME,
    FB_CONTEXT_DELETE,
    FB_CONTEXT_PROPERTIES,
    FB_CONTEXT_NEW_FOLDER,
    FB_CONTEXT_REFRESH,
    FB_CONTEXT_EDIT,
};

static fb_state_t *g_fb = NULL;
static char g_fb_clipboard[VFS_MAX_PATH];
static bool g_fb_clipboard_cut;
static bool g_fb_clipboard_directory;
static uint32_t g_fb_delete_token;
static void fb_main(void *argument);
static void fb_tree_set_open(fb_state_t *st, const char *path, bool open);

static gui_rect_t fb_address_rect(const fb_state_t *st) {
    gui_rect_t content = bk_gui_window_content_rect_raw(st->window);
    return (gui_rect_t){content.x + 38, content.y + 4, content.w - 44, 22};
}

static gui_rect_t fb_list_rect(const fb_state_t *st) {
    gui_rect_t content;
    int list_top;
    int list_h;

    if (!st || !st->window) return (gui_rect_t){0, 0, 0, 0};
    content = bk_gui_window_content_rect_raw(st->window);
    list_top = content.y + 60;
    list_h = content.h - 80;
    return (gui_rect_t){content.x + FB_LIST_X, list_top,
                        content.w - FB_LIST_X * 2, list_h};
}

static gui_rect_t fb_tree_rect(const fb_state_t *st) {
    gui_rect_t list = fb_list_rect(st);
    if (list.w < FB_TREE_W + FB_TILE_W + GUI_SCROLLBAR_SIZE + 12)
        return (gui_rect_t){0, 0, 0, 0};
    return (gui_rect_t){list.x, list.y, FB_TREE_W, list.h};
}

static gui_rect_t fb_items_rect(const fb_state_t *st) {
    gui_rect_t list = fb_list_rect(st);
    gui_rect_t tree = fb_tree_rect(st);
    if (tree.w > 0) {
        list.x += tree.w + FB_TREE_PAD;
        list.w -= tree.w + FB_TREE_PAD;
    }
    return list;
}

static int fb_cols_for_rect(gui_rect_t list) {
    int cols = (list.w - GUI_SCROLLBAR_SIZE) / FB_TILE_W;
    return cols < 1 ? 1 : cols;
}

static int fb_item_row_height(const fb_state_t *st) {
    return st && st->list_view ? FB_LIST_ROW_H : FB_TILE_H;
}

static int fb_visible_slots(const fb_state_t *st) {
    gui_rect_t list = fb_items_rect(st);
    int cols = st && st->list_view ? 1 : fb_cols_for_rect(list);
    int rows = list.h / fb_item_row_height(st);
    if (list.w <= 0 || list.h <= 0) return 1;
    if (rows < 1) rows = 1;
    return rows * cols;
}

/* scroll se guarda como indice de entrada, pero en vista de iconos siempre
 * debe apuntar al comienzo de una fila. Antes el thumb podia dejarlo en 1,
 * 2, 3... y toda la grilla se corria un archivo. */
static void fb_clamp_scroll(fb_state_t *st) {
    gui_rect_t items;
    int cols;
    int rows;
    int total_rows;
    int max_scroll;

    if (!st) return;
    items = fb_items_rect(st);
    cols = st->list_view ? 1 : fb_cols_for_rect(items);
    rows = items.h / fb_item_row_height(st);
    if (rows < 1) rows = 1;
    total_rows = ((int)st->entry_count + cols - 1) / cols;
    max_scroll = total_rows > rows ? (total_rows - rows) * cols : 0;

    if (st->scroll < 0) st->scroll = 0;
    if (st->scroll > max_scroll) st->scroll = max_scroll;
    if (cols > 1) st->scroll = (st->scroll / cols) * cols;
}

/* ICONS.PAK cache.
 *
 * Formato:
 *   magic    "BKIP"
 *   u32      version = 1
 *   u32      count
 *   entries[count]:
 *      char name[16]   // sin .BMP, ej: FOLDER
 *      u32  width
 *      u32  height
 *      u32  offset
 *      u32  size
 *   payload:
 *      uint32_t pixels[width * height] en RGB32/ARGB32
 */
typedef struct {
    bool loaded;
    void *pak_data;
    uint32_t pak_size;

    uint32_t *folder_icon;
    uint32_t *file_icon;
    uint32_t *text_icon;
    uint32_t *config_icon;
    uint32_t *image_icon;
    uint32_t *object_icon;
    uint32_t *midi_icon;
    uint32_t *hdd_icon;
    uint32_t *cd_icon;
    uint32_t *usb_icon;
    uint32_t *floppy_icon;
    uint32_t *shell_icon;
    uint32_t *editor_icon;
    uint32_t *calc_icon;
    uint32_t *files_icon;
    uint32_t *midamp_icon;
    uint32_t *netscape_icon;

    void *associations;
    uint32_t associations_size;
} fb_icon_cache_t;

static fb_icon_cache_t g_fb_icon_cache;

static uint32_t fb_rd32(const uint8_t *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool fb_name16_eq(const uint8_t *name16, const char *name) {
    uint32_t i = 0;
    if (!name) return false;
    while (i < 16 && name[i]) {
        if ((char)name16[i] != name[i]) return false;
        i++;
    }
    return i < 16 && name16[i] == '\0';
}

static uint32_t *fb_pak_find_icon(const char *name) {
    uint8_t *data = (uint8_t *)g_fb_icon_cache.pak_data;
    uint32_t count;

    if (!data || g_fb_icon_cache.pak_size < 12) return NULL;
    if (data[0] != 'B' || data[1] != 'K' ||
        data[2] != 'I' || data[3] != 'P') return NULL;
    if (fb_rd32(data + 4) != 1) return NULL;

    count = fb_rd32(data + 8);
    if (count > (g_fb_icon_cache.pak_size - 12U) / 32U) return NULL;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t *e = data + 12U + i * 32U;
        uint32_t w = fb_rd32(e + 16);
        uint32_t h = fb_rd32(e + 20);
        uint32_t off = fb_rd32(e + 24);
        uint32_t size = fb_rd32(e + 28);
        uint32_t need;
        uint32_t *src;
        uint32_t *out;

        if (!fb_name16_eq(e, name)) continue;
        if (!w || !h || w > 4096U || h > 4096U ||
            w > 0xFFFFFFFFU / h ||
            w * h > 0xFFFFFFFFU / sizeof(uint32_t)) return NULL;
        need = w * h * sizeof(uint32_t);
        if (size < need) return NULL;
        if (off > g_fb_icon_cache.pak_size ||
            need > g_fb_icon_cache.pak_size - off) return NULL;

        src = (uint32_t *)(data + off);
        out = (uint32_t *)bk_sys_alloc(FB_ICON_BYTES);
        if (!out) return NULL;

        for (uint32_t y = 0; y < FB_ICON_SIZE; y++) {
            uint32_t sy = (y * h) / FB_ICON_SIZE;
            for (uint32_t x = 0; x < FB_ICON_SIZE; x++) {
                uint32_t sx = (x * w) / FB_ICON_SIZE;
                out[y * FB_ICON_SIZE + x] = src[sy * w + sx];
            }
        }

        return out;
    }

    return NULL;
}

static bool fb_load_icon_pak(void) {
    /*
     * El set viejo /ICONS/ICONS.PAK queda deliberadamente desactivado.
     * program_load_bmp_icon_scaled() traduce las rutas historicas a
     * GRAPHICS.PAK, por lo que FileBrowser usa la misma familia React95 que
     * el resto del escritorio sin una segunda cache paralela.
     */
    return false;
}

static void fb_cache_load_resources(void) {
    if (g_fb_icon_cache.loaded) return;

    if (fb_load_icon_pak()) {
        g_fb_icon_cache.folder_icon = fb_pak_find_icon("FOLDER");
        g_fb_icon_cache.file_icon = fb_pak_find_icon("FILE");
        g_fb_icon_cache.text_icon = fb_pak_find_icon("TEXT");
        g_fb_icon_cache.config_icon = fb_pak_find_icon("CONFIG");
        g_fb_icon_cache.image_icon = fb_pak_find_icon("IMAGE");
        g_fb_icon_cache.object_icon = fb_pak_find_icon("OBJECT");
        g_fb_icon_cache.midi_icon = fb_pak_find_icon("MIDI");
        g_fb_icon_cache.hdd_icon = fb_pak_find_icon("HDD");
        g_fb_icon_cache.cd_icon = fb_pak_find_icon("CD");
        g_fb_icon_cache.usb_icon = fb_pak_find_icon("USB");
        g_fb_icon_cache.floppy_icon = fb_pak_find_icon("FLOPPY");
    }

#define FB_CACHE_LOAD_ICON(field, path) do { \
    if (!g_fb_icon_cache.field) { \
        g_fb_icon_cache.field = bk_app_load_icon(path, FB_ICON_SIZE, FB_ICON_SIZE); \
        bk_sys_yield(); \
    } \
} while (0)

    /*
     * Fallback compatible: si ICONS.PAK no existe o falta algún icono,
     * cargamos el BMP viejo. Si BK_ICON_PAK_ONLY=1 y no hay BMPs, simplemente
     * quedarán NULL; fb_draw_bmp_icon() lo tolera.
     */
    FB_CACHE_LOAD_ICON(folder_icon, "/ICONS/FOLDER.BMP");
    FB_CACHE_LOAD_ICON(file_icon, "/ICONS/FILE.BMP");
    FB_CACHE_LOAD_ICON(text_icon, "/ICONS/TEXT.BMP");
    FB_CACHE_LOAD_ICON(config_icon, "/ICONS/CONFIG.BMP");
    FB_CACHE_LOAD_ICON(image_icon, "/ICONS/IMAGE.BMP");
    FB_CACHE_LOAD_ICON(object_icon, "/ICONS/OBJECT.BMP");
    FB_CACHE_LOAD_ICON(midi_icon, "/ICONS/MIDI.BMP");
    FB_CACHE_LOAD_ICON(hdd_icon, "/ICONS/HDD.BMP");
    FB_CACHE_LOAD_ICON(cd_icon, "/ICONS/CD.BMP");
    FB_CACHE_LOAD_ICON(usb_icon, "/ICONS/USB.BMP");
    FB_CACHE_LOAD_ICON(floppy_icon, "/ICONS/FLOPPY.BMP");
    /* GRAPHICS.PAK ya esta cacheado globalmente: cargar tambien los iconos
     * de aplicaciones permite que Associations.INI conserve iconos propios
     * sin volver al arte BMP antiguo. */
    FB_CACHE_LOAD_ICON(shell_icon, "/ICONS/SHELL.BMP");
    FB_CACHE_LOAD_ICON(editor_icon, "/ICONS/EDITOR.BMP");
    FB_CACHE_LOAD_ICON(calc_icon, "/ICONS/CALC.BMP");
    FB_CACHE_LOAD_ICON(files_icon, "/ICONS/FILES.BMP");
    FB_CACHE_LOAD_ICON(midamp_icon, "/ICONS/MIDAMP.BMP");
    FB_CACHE_LOAD_ICON(netscape_icon, "/ICONS/NETSCAPE.BMP");

#undef FB_CACHE_LOAD_ICON

    if (!bk_file_read_all("/ASSOC.INI",
                      &g_fb_icon_cache.associations,
                      &g_fb_icon_cache.associations_size)) {
        (void)bk_file_read_all("/Associations.INI",
                           &g_fb_icon_cache.associations,
                           &g_fb_icon_cache.associations_size);
    }

    g_fb_icon_cache.loaded = true;
}

static void fb_load_resources(fb_state_t *st) {
    if (!st) return;

    fb_cache_load_resources();

    st->folder_icon = g_fb_icon_cache.folder_icon;
    st->file_icon = g_fb_icon_cache.file_icon;
    st->text_icon = g_fb_icon_cache.text_icon;
    st->config_icon = g_fb_icon_cache.config_icon;
    st->image_icon = g_fb_icon_cache.image_icon;
    st->object_icon = g_fb_icon_cache.object_icon;
    st->midi_icon = g_fb_icon_cache.midi_icon;
    st->hdd_icon = g_fb_icon_cache.hdd_icon;
    st->cd_icon = g_fb_icon_cache.cd_icon;
    st->usb_icon = g_fb_icon_cache.usb_icon;
    st->floppy_icon = g_fb_icon_cache.floppy_icon;
    st->shell_icon = g_fb_icon_cache.shell_icon;
    st->editor_icon = g_fb_icon_cache.editor_icon;
    st->calc_icon = g_fb_icon_cache.calc_icon;
    st->files_icon = g_fb_icon_cache.files_icon;
    st->midamp_icon = g_fb_icon_cache.midamp_icon;
    st->netscape_icon = g_fb_icon_cache.netscape_icon;

    st->associations = g_fb_icon_cache.associations;
    st->associations_size = g_fb_icon_cache.associations_size;
    void *fresh=NULL;uint32_t fresh_size=0;
    if(bk_file_read_all("/ASSOC.INI",&fresh,&fresh_size)&&fresh){st->associations=fresh;st->associations_size=fresh_size;st->associations_owned=true;}
}

static bool fb_cdrom_available(void) {
    /* El kernel monta ISO9660 una vez después de cargar sus drivers. Nunca
       iniciar I/O ATAPI desde la creación o pintura de esta ventana. */
    return bk_device_has_cdrom();
}

static void fb_join_path(char *out, uint32_t capacity,
                         const char *dir, const char *name) {
    size_t len;
    if (!out || !capacity) return;
    bk_runtime_strncpy(out, dir ? dir : "/", capacity - 1);
    out[capacity - 1] = '\0';
    len = bk_runtime_strlen(out);
    if (len > 1 && out[len - 1] != '/' && len + 1 < capacity) {
        out[len++] = '/';
        out[len] = '\0';
    }
    bk_runtime_strncpy(out + len, name ? name : "", capacity - len - 1);
}

static void fb_draw_bmp_icon(gui_surface_t *s, int x, int y,
                             const uint32_t *pixels) {
    if (!pixels) return;
    bk_app_draw_icon(s, x, y, pixels, FB_ICON_SIZE, FB_ICON_SIZE);
}

static void fb_draw_small_icon(gui_surface_t *surface, int x, int y,
                               const uint32_t *pixels) {
    if (!surface || !pixels) return;
    for (int dy = 0; dy < 20; dy++) {
        for (int dx = 0; dx < 20; dx++) {
            uint32_t color = pixels[(dy * FB_ICON_SIZE / 20) * FB_ICON_SIZE +
                                    (dx * FB_ICON_SIZE / 20)];
            if ((color >> 24) && x + dx >= 0 && y + dy >= 0 &&
                x + dx < surface->width && y + dy < surface->height &&
                bk_gui_rect_contains(surface->clip, x + dx, y + dy))
                bk_gui_gfx_putpixel(surface, x + dx, y + dy,
                                 color & 0x00FFFFFF);
        }
    }
}

static const char *fb_extension(const char *name) {
    const char *dot = NULL;
    while (name && *name) {
        if (*name == '.') dot = name;
        name++;
    }
    return dot ? dot : "";
}

static bool fb_ext_is(const char *ext, const char *upper, const char *lower) {
    return bk_runtime_strcmp(ext, upper) == 0 || bk_runtime_strcmp(ext, lower) == 0;
}

static bool fb_text_equal_nocase(const char *left, const char *right,
                                 size_t length) {
    for (size_t i = 0; i < length; i++) {
        char a = left[i], b = right[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return false;
    }
    return true;
}

static bool fb_assoc_value(const fb_state_t *st, const char *name,
                           const char **value_out, size_t *length_out) {
    const char *ext = fb_extension(name);
    const char *line;
    if (!st || !st->associations || !name || !value_out || !length_out)
        return false;
    line = (const char *)st->associations;
    while (*line) {
        const char *end = line, *eq = NULL;
        while (*end && *end != '\r' && *end != '\n') {
            if (*end == '=' && !eq) eq = end;
            end++;
        }
        if (eq) {
            size_t key_len = (size_t)(eq - line);
            const char *extension = *ext == '.' ? ext + 1 : ext;
            if ((key_len == bk_runtime_strlen(name) &&
                 fb_text_equal_nocase(line, name, key_len)) ||
                (key_len == bk_runtime_strlen(extension) &&
                 fb_text_equal_nocase(line, extension, key_len))) {
                *value_out = eq + 1;
                *length_out = (size_t)(end - (eq + 1));
                return true;
            }
        }
        line = end;
        while (*line == '\r' || *line == '\n') line++;
    }
    return false;
}

static const uint32_t *fb_icon_for_entry(const fb_state_t *st,
                                         const vfs_dir_entry_t *entry,
                                         uint32_t index) {
    const char *ext;
    if (st->drive_view && index < st->entry_count) {
        switch (st->entry_drive[index]) {
            case FB_DRIVE_HDD: return st->hdd_icon;
            case FB_DRIVE_CD: return st->cd_icon;
            case FB_DRIVE_USB: return st->usb_icon;
            case FB_DRIVE_FLOPPY: return st->floppy_icon;
            case FB_DRIVE_SD: return st->hdd_icon;
            default: break;
        }
    }
    if (entry->type == VFS_NODE_DIR) return st->folder_icon;

    ext = fb_extension(entry->name);
    if (st->associations) {
        const char *value;
        size_t value_len;
        if (fb_assoc_value(st, entry->name, &value, &value_len)) {
            const char *separator = value;
            while (separator < value + value_len && *separator != '|') separator++;
            value_len = (size_t)(separator - value);
#define FB_ASSOC(path, icon) \
                        if (value_len == sizeof(path) - 1 && \
                            bk_runtime_strncmp(value, path, value_len) == 0 && \
                            (icon)) return (icon)
                        FB_ASSOC("/ICONS/SHELL.BMP", st->shell_icon);
                        FB_ASSOC("/ICONS/FILES.BMP", st->files_icon);
                        FB_ASSOC("/ICONS/EDITOR.BMP", st->editor_icon);
                        FB_ASSOC("/ICONS/CALC.BMP", st->calc_icon);
                        FB_ASSOC("/ICONS/MIDAMP.BMP", st->midamp_icon);
                        FB_ASSOC("/ICONS/NETSCAPE.BMP", st->netscape_icon);
                        FB_ASSOC("/ICONS/IMAGE.BMP", st->image_icon);
                        FB_ASSOC("/ICONS/OBJECT.BMP", st->object_icon);
                        FB_ASSOC("/ICONS/CONFIG.BMP", st->config_icon);
                        FB_ASSOC("/ICONS/TEXT.BMP", st->text_icon);
                        FB_ASSOC("/ICONS/MIDI.BMP", st->midi_icon);
#undef FB_ASSOC
        }
    }
    if (fb_ext_is(ext, ".TXT", ".txt")) return st->text_icon;
    if (fb_ext_is(ext, ".INI", ".ini")) return st->config_icon;
    if (fb_ext_is(ext, ".BMP", ".bmp") ||
        fb_ext_is(ext, ".GIF", ".gif")) return st->image_icon;
    if (fb_ext_is(ext, ".DVR", ".dvr")) return st->config_icon;
    if (fb_ext_is(ext, ".CPL", ".cpl")) return st->config_icon;
    if (fb_ext_is(ext, ".WAD", ".wad")) return st->midi_icon;
    if (fb_ext_is(ext, ".BEX", ".o") ||
        fb_ext_is(ext, ".BEX", ".bex") ||
        fb_ext_is(ext, ".ELF", ".elf") ||
        fb_ext_is(ext, ".EXE", ".exe")) return st->object_icon;
    if (fb_ext_is(ext, ".MID", ".mid") ||
        fb_ext_is(ext, ".KAR", ".kar")) return st->midi_icon;
    return st->file_icon;
}

static void fb_draw_tile_name(gui_surface_t *surface, int x, int y,
                              const char *name, uint32_t color,
                              gui_rect_t clip) {
    char first[24];
    char second[24];
    size_t length = bk_runtime_strlen(name);
    size_t first_count = length > 16 ? 16 : length;
    size_t second_count = length > first_count ? length - first_count : 0;
    const char *paren = NULL;
    int text_x;
    (void)x;
    for (const char *scan = name; *scan; scan++)
        if (*scan == '(' && scan > name) { paren = scan; break; }
    if (paren && (size_t)(paren - name) <= 18U) {
        first_count = (size_t)(paren - name);
        while (first_count && name[first_count - 1U] == ' ') first_count--;
        second_count = length - (size_t)(paren - name);
    }
    if (second_count > 20) second_count = 20;
    bk_runtime_strncpy(first, name, first_count);
    first[first_count] = '\0';
    text_x = clip.x + (clip.w - (int)bk_gui_font_text_width(first)) / 2;
    if (text_x < clip.x) text_x = clip.x;
    bk_gui_font_draw_string_clipped(surface, text_x, y, first, color, clip);
    if (second_count) {
        const char *second_start = paren ? paren : name + first_count;
        bk_runtime_strncpy(second, second_start, second_count);
        second[second_count] = '\0';
        if (!paren && length > first_count + second_count && second_count >= 3) {
            second[second_count - 3] = '.';
            second[second_count - 2] = '.';
            second[second_count - 1] = '.';
        }
        text_x = clip.x + (clip.w - (int)bk_gui_font_text_width(second)) / 2;
        if (text_x < clip.x) text_x = clip.x;
        bk_gui_font_draw_string_clipped(surface, text_x, y + 12, second, color, clip);
    }
}


static bool fb_reserve_entries(fb_state_t *st, uint32_t required) {
    uint32_t capacity;
    vfs_dir_entry_t *entries;
    uint8_t *drives;
    char (*mounts)[16];

    if (!st) return false;
    if (required <= st->entry_capacity) return true;
    capacity = st->entry_capacity ? st->entry_capacity
                                  : FB_DIR_INITIAL_CAPACITY;
    while (capacity < required && capacity < FB_DIR_HARD_LIMIT) {
        uint32_t next = capacity * 2U;
        capacity = next > FB_DIR_HARD_LIMIT ? FB_DIR_HARD_LIMIT : next;
    }
    if (capacity < required) return false;

    entries = (vfs_dir_entry_t *)bk_sys_alloc_zero(
        sizeof(*entries) * capacity);
    drives = (uint8_t *)bk_sys_alloc_zero(sizeof(*drives) * capacity);
    mounts = (char (*)[16])bk_sys_alloc_zero(sizeof(*mounts) * capacity);
    if (!entries || !drives || !mounts) {
        if (entries) bk_sys_free(entries);
        if (drives) bk_sys_free(drives);
        if (mounts) bk_sys_free(mounts);
        return false;
    }

    for (uint32_t i = 0;
         i < st->entry_count && i < st->entry_capacity; i++) {
        entries[i] = st->entries[i];
        drives[i] = st->entry_drive[i];
        bk_runtime_strncpy(mounts[i], st->entry_mount[i],
                           sizeof(mounts[i]) - 1U);
    }
    if(st->associations_owned)bk_sys_free(st->associations);
    if (st->entries) bk_sys_free(st->entries);
    if (st->entry_drive) bk_sys_free(st->entry_drive);
    if (st->entry_mount) bk_sys_free(st->entry_mount);
    st->entries = entries;
    st->entry_drive = drives;
    st->entry_mount = mounts;
    st->entry_capacity = capacity;
    return true;
}

static bool fb_read_directory(const char *path,
                              vfs_dir_entry_t **entries_out,
                              uint32_t *count_out) {
    uint32_t capacity = FB_DIR_INITIAL_CAPACITY;
    if (!entries_out || !count_out) return false;
    *entries_out = NULL;
    *count_out = 0U;

    while (capacity <= FB_DIR_HARD_LIMIT) {
        vfs_dir_entry_t *entries = (vfs_dir_entry_t *)bk_sys_alloc_zero(
            sizeof(*entries) * capacity);
        uint32_t count = 0U;
        bool ok;
        if (!entries) return false;
        ok = bk_file_list_dir(path, entries, capacity, &count);
        if (!ok) {
            bk_sys_free(entries);
            return false;
        }
        if (count < capacity || capacity == FB_DIR_HARD_LIMIT) {
            *entries_out = entries;
            *count_out = count;
            return true;
        }
        bk_sys_free(entries);
        capacity *= 2U;
        if (capacity > FB_DIR_HARD_LIMIT) capacity = FB_DIR_HARD_LIMIT;
    }
    return false;
}

static void fb_add_drive(fb_state_t *st, const char *name, uint8_t drive,
                         const char *mount) {
    vfs_dir_entry_t *entry;
    if (!fb_reserve_entries(st, st->entry_count + 1U)) return;
    entry = &st->entries[st->entry_count];
    bk_runtime_strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->type = VFS_NODE_DIR;
    entry->size = 0;
    entry->attributes = 0;
    st->entry_drive[st->entry_count] = drive;
    bk_runtime_strncpy(st->entry_mount[st->entry_count], mount ? mount : "",
                       sizeof(st->entry_mount[st->entry_count]) - 1);
    st->entry_mount[st->entry_count][
        sizeof(st->entry_mount[st->entry_count]) - 1] = '\0';
    st->entry_count++;
}

static void fb_load_drives(fb_state_t *st) {
    uint32_t device_count;
    const char *active_mount = bk_device_mount_name();
    st->entry_count = 0;
    st->selected = 0;
    st->scroll = 0;

    device_count = bk_device_block_count();
    for (uint32_t i = 0; i < device_count; i++) {
        bk_block_info_t device;
        char label[VFS_MAX_NAME];
        char path[16];
        uint8_t drive;

        if (!bk_device_block_info(i, &device) ||
            device.type == BK_BLOCK_ATAPI) continue;
        if (device.type == BK_BLOCK_ATA) drive = FB_DRIVE_HDD;
        else if (device.type == BK_BLOCK_SD) drive = FB_DRIVE_SD;
        else if (device.type == BK_BLOCK_USB) drive = FB_DRIVE_USB;
        else if (device.type == BK_BLOCK_FLOPPY) {
            /* El FDC se registra incluso sin disquete: no mostrar una unidad
             * fantasma que bloquearia al usuario al intentar abrirla. */
            if (!active_mount ||
                bk_runtime_strcmp(active_mount, device.name) != 0) continue;
            drive = FB_DRIVE_FLOPPY;
        }
        else continue;

        if (drive == FB_DRIVE_HDD)
            snprintf(label, sizeof(label), "Disco local (%s)", device.name);
        else if (drive == FB_DRIVE_SD)
            snprintf(label, sizeof(label), "Tarjeta SD (%s)", device.name);
        else if (drive == FB_DRIVE_USB)
            snprintf(label, sizeof(label), "Unidad USB (%s)", device.name);
        else
            snprintf(label, sizeof(label), "Disquete (%s)", device.name);
        snprintf(path, sizeof(path), "/%s", device.name);
        fb_add_drive(st, label, drive, path);
    }
    /* C: de Wine no es otro disco: es una carpeta normal del BFS principal.
     * Mostrar un acceso rapido aqui evita obligar al usuario a atravesar
     * SYSTEM/WINE, pero al abrirlo navegamos la ruta real y el boton Subir
     * vuelve a /SYSTEM como con cualquier otra carpeta. */
    {
        vfs_dir_entry_t wine_root;
        if (bk_file_stat("/SYSTEM/WINE", &wine_root) &&
            wine_root.type == VFS_NODE_DIR)
            fb_add_drive(st, "Programas de Windows", FB_DRIVE_NONE,
                         "/SYSTEM/WINE");
    }
    if (fb_cdrom_available())
        fb_add_drive(st, "CD-ROM", FB_DRIVE_CD, "/CDROM");
}

static bool fb_at_volume_root(const char *path) {
    const char *name;
    if (!path || path[0] != '/' || !path[1]) return false;
    name = path + 1;
    if (bk_runtime_strcmp(name, "CDROM") == 0) return true;
    for (const char *cursor = name; *cursor; cursor++)
        if (*cursor == '/') return false;
    return bk_device_block_get(name) != NULL;
}

/* ══════════════════════════════════════════════════
 *  Cargar directorio
 * ══════════════════════════════════════════════════ */

static bool fb_load_dir(fb_state_t *st) {
    uint32_t found = 0U;
    uint32_t parent = 0U;
    uint32_t usable;

    if (!st) return false;
    if (st->drive_view) {
        fb_load_drives(st);
        return true;
    }

    st->entry_count = 0U;
    st->selected = 0;
    st->scroll = 0;
    parent = (st->cwd[0] == '/' && st->cwd[1] != '\0') ? 1U : 0U;

    /* Reutilizar el buffer persistente del FileBrowser. Antes se creaba un
     * segundo buffer temporal para cada carpeta y luego se copiaba entero. */
    if (!fb_reserve_entries(st, FB_DIR_INITIAL_CAPACITY + parent)) {
        bk_runtime_strcpy(st->status, "Sin memoria para mostrar archivos");
        return false;
    }

    for (;;) {
        usable = st->entry_capacity - parent;
        found = 0U;
        if (!bk_file_list_dir(st->cwd, st->entries + parent,
                              usable, &found)) {
            bk_runtime_strcpy(st->status, "No se pudo leer el directorio");
            return false;
        }
        /* No confiar un índice al pintor si un filesystem defectuoso informa
         * más entradas que las que realmente caben en el buffer entregado. */
        if (found > usable) found = usable;
        if (found < usable || st->entry_capacity >= FB_DIR_HARD_LIMIT)
            break;
        if (!fb_reserve_entries(st, st->entry_capacity * 2U)) {
            bk_runtime_strcpy(st->status, "Sin memoria para mostrar archivos");
            return false;
        }
    }

    if (parent) {
        bk_runtime_strncpy(st->entries[0].name, "..",
                           sizeof(st->entries[0].name) - 1U);
        st->entries[0].name[sizeof(st->entries[0].name) - 1U] = '\0';
        st->entries[0].type = VFS_NODE_DIR;
        st->entries[0].size = 0U;
        st->entries[0].attributes = 0U;
    }

    /* Compactar en el mismo buffer para quitar las entradas . y ... */
    {
        uint32_t write = parent;
        for (uint32_t read = parent; read < parent + found; read++) {
            vfs_dir_entry_t *entry = &st->entries[read];
            if (!bk_runtime_strncmp(entry->name,".bk-",4)) continue;
            if (entry->name[0] == '.' &&
                (entry->name[1] == '\0' ||
                 (entry->name[1] == '.' && entry->name[2] == '\0')))
                continue;
            if (write != read) st->entries[write] = *entry;
            write++;
        }
        st->entry_count = write;
    }

    snprintf(st->status, sizeof(st->status), "%u elementos",
             st->entry_count - parent);
    return true;
}

/* ══════════════════════════════════════════════════
 *  Actualizar título de ventana
 * ══════════════════════════════════════════════════ */

static void fb_update_title(fb_state_t *st) {
    if (!st->window) return;
    char title[48];
    if (st->drive_view) {
        bk_runtime_strncpy(st->window->title, "@H06585867",
                 sizeof(st->window->title) - 1);
        st->window->title[sizeof(st->window->title) - 1] = '\0';
        st->window->dirty = true;
        return;
    }
    bk_runtime_strncpy(title, "Archivos: ", sizeof(title) - 1);
    size_t tlen = bk_runtime_strlen(title);
    bk_runtime_strncpy(title + tlen, st->cwd, sizeof(title) - tlen - 1);
    title[sizeof(title) - 1] = '\0';
    bk_runtime_strncpy(st->window->title, title, sizeof(st->window->title) - 1);
    st->window->title[sizeof(st->window->title) - 1] = '\0';
    st->window->dirty = true;
    bk_runtime_strncpy(st->address, st->cwd, sizeof(st->address) - 1);
    st->address[sizeof(st->address) - 1] = '\0';
}

/* ══════════════════════════════════════════════════
 *  Navegar a subdir / padre
 * ══════════════════════════════════════════════════ */

static void fb_navigate(fb_state_t *st, const char *name) {
    char new_path[VFS_MAX_PATH];
    char old_path[VFS_MAX_PATH];

    if (!st || !name) return;
    bk_runtime_strncpy(old_path, st->cwd, sizeof(old_path) - 1U);
    old_path[sizeof(old_path) - 1U] = '\0';

    if (bk_runtime_strcmp(name, "..") == 0) {
        if (fb_at_volume_root(st->cwd)) {
            st->drive_view = true;
            bk_runtime_strcpy(st->cwd, "/");
            fb_load_dir(st);
            fb_update_title(st);
            return;
        }
        bk_runtime_strncpy(new_path, st->cwd, sizeof(new_path) - 1U);
        new_path[sizeof(new_path) - 1U] = '\0';
        size_t len = bk_runtime_strlen(new_path);
        if (len > 1 && new_path[len - 1] == '/') new_path[--len] = '\0';
        while (len > 1 && new_path[len - 1] != '/') new_path[--len] = '\0';
        if (len > 1) new_path[--len] = '\0';
        if (len == 0) { new_path[0] = '/'; new_path[1] = '\0'; }
    } else {
        /* Mantener visible la rama desde la que se entra. Las demas ramas
         * abiertas conservan su estado independiente. */
        fb_tree_set_open(st, old_path, true);
        bk_runtime_strncpy(new_path, st->cwd, sizeof(new_path) - 1U);
        new_path[sizeof(new_path) - 1U] = '\0';
        size_t len = bk_runtime_strlen(new_path);
        if (len > 0 && new_path[len - 1] != '/' && len + 1 < sizeof(new_path)) {
            new_path[len++] = '/';
            new_path[len] = '\0';
        }
        bk_runtime_strncpy(new_path + len, name,
                           sizeof(new_path) - len - 1U);
    }

    /* FileBrowser mantiene una ruta propia. vfs_chdir() resolvia la ruta una
     * vez y fb_load_dir() la resolvia otra vez inmediatamente despues. Ademas
     * cambiaba el CWD global de todas las aplicaciones. */
    st->drive_view = false;
    bk_runtime_strncpy(st->cwd, new_path, sizeof(st->cwd) - 1U);
    st->cwd[sizeof(st->cwd) - 1U] = '\0';
    if (!fb_load_dir(st)) {
        bk_runtime_strncpy(st->cwd, old_path, sizeof(st->cwd) - 1U);
        st->cwd[sizeof(st->cwd) - 1U] = '\0';
        (void)fb_load_dir(st);
        bk_runtime_strcpy(st->status, "No se pudo abrir la carpeta");
    }
    fb_update_title(st);
}

static bool fb_is_text_file(const char *name) {
    const char *ext = fb_extension(name);
    return fb_ext_is(ext, ".TXT", ".txt");
}

static void fb_open_text(gui_desktop_t *desktop, fb_state_t *st,
                         const char *name) {
    char path[VFS_MAX_PATH];
    bk_runtime_strncpy(path, st->cwd, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    size_t len = bk_runtime_strlen(path);
    if (len > 1 && path[len - 1] != '/') {
        path[len++] = '/';
        path[len] = '\0';
    }
    bk_runtime_strncpy(path + len, name, sizeof(path) - len - 1);
    (void)bk_app_execute_path_arg(desktop, "/SYSTEM/PROGRAMS/TEXTEDITOR.BEX", path);
}

static void fb_execute_object(gui_desktop_t *desktop, fb_state_t *st,
                              const char *name) {
    char path[VFS_MAX_PATH];
    size_t len;

    bk_runtime_strncpy(path, st->cwd, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    len = bk_runtime_strlen(path);
    if (len > 1 && path[len - 1] != '/' && len + 1 < sizeof(path)) {
        path[len++] = '/';
        path[len] = '\0';
    }
    bk_runtime_strncpy(path + len, name, sizeof(path) - len - 1);
    if (!bk_app_execute_path(desktop, path) && st->window) {
        bool is_pe = bk_app_is_win32(path);
        const char *error = is_pe ? "No se pudo iniciar Wine" : bk_app_elf_last_error();

        bk_runtime_strncpy(st->window->title, is_pe ? "Error Wine: " : "Error ELF: ",
                 sizeof(st->window->title) - 1);
        st->window->title[sizeof(st->window->title) - 1] = '\0';
        len = bk_runtime_strlen(st->window->title);
        bk_runtime_strncpy(st->window->title + len, error,
                 sizeof(st->window->title) - len - 1);
        st->window->title[sizeof(st->window->title) - 1] = '\0';
        st->window->dirty = true;
    }
}

static bool fb_open_association_action(fb_state_t *st, const char *name,bool editing) {
    const char *value;
    const char *separator;
    size_t value_len;
    char application[VFS_MAX_PATH];
    char path[VFS_MAX_PATH];
    if (!fb_assoc_value(st, name, &value, &value_len)) return false;
    separator = value;
    while (separator < value + value_len && *separator != '|') separator++;
    if (separator >= value + value_len || separator + 1 >= value + value_len)
        return false;
    const char *association_end = value + value_len;
    value = separator + 1;
    if(editing){while(value<association_end&&*value&&*value!='|'&&*value!='\r'&&*value!='\n')value++;if(value>=association_end||*value!='|')return false;value++;}
    {
        const char *end = value;
        while (*end && *end != '|' && *end != '\r' && *end != '\n') end++;
        value_len = (size_t)(end - value);
    }
    if (!value_len || value_len >= sizeof(application)) return false;
    bk_runtime_strncpy(application, value, value_len);
    application[value_len] = '\0';
    fb_join_path(path, sizeof(path), st->cwd, name);
    return bk_app_execute_path_arg(st->desktop, application, path);
}

static bool fb_open_selected(fb_state_t *st) {
    vfs_dir_entry_t *e;
    gui_desktop_t *desktop;
    if (!st || st->selected < 0 ||
        (uint32_t)st->selected >= st->entry_count) return false;
    e = &st->entries[st->selected];
    desktop = st->desktop;
    if (st->drive_view) {
        const char *path = st->entry_mount[st->selected];
        char target[16];
        bk_runtime_strncpy(target, path, sizeof(target) - 1U);
        target[sizeof(target) - 1U] = '\0';
        if (!target[0]) return true;

        /* Las rutas /ataN seleccionan su propio volumen en VFS. No exigir
         * que la operacion tambien cambie el CWD global para poder abrirlo. */
        st->drive_view = false;
        bk_runtime_strncpy(st->cwd, target, sizeof(st->cwd) - 1U);
        st->cwd[sizeof(st->cwd) - 1U] = '\0';
        if (!fb_load_dir(st)) {
            st->drive_view = true;
            bk_runtime_strcpy(st->cwd, "/");
            fb_load_drives(st);
            snprintf(st->status, sizeof(st->status),
                     "No se pudo abrir %s", target + (target[0] == '/'));
            if (st->window) st->window->dirty = true;
            return true;
        }
        fb_update_title(st);
        (void)bk_system_sound_event_play(BK_SYSTEM_SOUND_BUTTON);
    } else if (e->type == VFS_NODE_DIR) {
        fb_navigate(st, e->name);
        (void)bk_system_sound_event_play(BK_SYSTEM_SOUND_BUTTON);
    } else if (fb_ext_is(fb_extension(e->name), ".DVR", ".dvr")) {
        char path[VFS_MAX_PATH];
        fb_join_path(path, sizeof(path), st->cwd, e->name);
        bk_runtime_strcpy(st->status, bk_device_driver_load(path) ? "@H62E1C365" :
                                               "@HCB0782AD");
    } else if (fb_open_association_action(st,e->name,false)) {
        /* Association.INI decides the program before built-in fallbacks. */
    } else if (fb_is_text_file(e->name)) {
        fb_open_text(desktop, st, e->name);
    } else if (fb_ext_is(fb_extension(e->name), ".BMP", ".bmp") ||
               fb_ext_is(fb_extension(e->name), ".GIF", ".gif")) {
        char path[VFS_MAX_PATH];
        fb_join_path(path, sizeof(path), st->cwd, e->name);
        (void)bk_app_execute_path_arg(desktop, "/SYSTEM/PROGRAMS/IMAGEVIEWER.BEX", path);
    } else if (fb_ext_is(fb_extension(e->name), ".BEX", ".bex") ||
               bk_app_is_object(e->name) ||
               bk_app_is_win32(e->name)) {
        fb_execute_object(desktop, st, e->name);
    } else {
        bk_runtime_strcpy(st->status, "@H2BE7E5D8");
    }
    if (st->window) st->window->dirty = true;
    return true;
}

static void fb_properties_paint(gui_window_t *window, gui_surface_t *surface,
                                void *context) {
    fb_properties_t *p = (fb_properties_t *)context;
    gui_rect_t c;
    gui_rect_t page;
    char value[64];
    int x;
    int y;
    if (!p || !window || !surface) return;
    c = bk_gui_window_content_rect_raw(window);
    bk_gui_gfx_fill_rect(surface, c, BK_THEME_FACE);
    bk_gui_gfx_fill_rect(surface, (gui_rect_t){c.x + 10, c.y + 6, 82, 22},
                      p->active_tab == 0 ? 0x00FFFFFF : 0x00C0C0C0);
    bk_gui_gfx_draw_rect(surface, (gui_rect_t){c.x + 10, c.y + 6, 82, 22},
                      0x00606060);
    bk_gui_font_draw_string(surface, c.x + 22, c.y + 13, "@H5561FD0B",
                         0x00101010, 0, false);
    bk_gui_gfx_fill_rect(surface, (gui_rect_t){c.x + 92, c.y + 6, 82, 22},
                      p->active_tab == 1 ? 0x00FFFFFF : 0x00C0C0C0);
    bk_gui_gfx_draw_rect(surface, (gui_rect_t){c.x + 92, c.y + 6, 82, 22},
                      0x00606060);
    bk_gui_font_draw_string(surface, c.x + 102, c.y + 13, "@H322BA665",
                         0x00101010, 0, false);
    page = (gui_rect_t){c.x + 10, c.y + 27, c.w - 20, c.h - 37};
    bk_gui_gfx_fill_rect(surface, page, 0x00FFFFFF);
    bk_gui_gfx_draw_rect(surface, page, 0x00606060);
    x = page.x + 14;
    y = page.y + 18;
    if (p->active_tab == 0) {
        bk_gui_font_draw_string(surface, x, y, "@H5830640E", 0x00101010, 0, false);
        bk_gui_font_draw_string_clipped(surface, x + 82, y, p->entry.name,
                                     0x00101010, page);
        y += 22;
        bk_gui_font_draw_string(surface, x, y, "@H941F0DBF", 0x00101010, 0, false);
        bk_gui_font_draw_string(surface, x + 82, y,
            p->drive ? "@HACA9B4D5" :
            (p->entry.type == VFS_NODE_DIR ? "@H0BC9F2E9" : "@H796914D7"),
            0x00101010, 0, false);
        y += 22;
        bk_gui_font_draw_string(surface, x, y, "@HA7A6D186", 0x00101010, 0, false);
        bk_gui_font_draw_string_clipped(surface, x + 82, y, p->path,
                                     0x00101010, page);
        y += 22;
        snprintf(value, sizeof(value), "@H516F097C", p->entry.size);
        bk_gui_font_draw_string(surface, x, y, "@H1174346F", 0x00101010, 0, false);
        bk_gui_font_draw_string(surface, x + 82, y, value, 0x00101010, 0, false);
    } else {
        snprintf(value, sizeof(value), "0x%02x", p->entry.attributes);
        bk_gui_font_draw_string(surface, x, y, "@HA71400A1", 0x00101010, 0, false);
        bk_gui_font_draw_string(surface, x + 105, y, value, 0x00101010, 0, false);
        y += 22;
        bk_gui_font_draw_string(surface, x, y, "@H9CC7D1CB", 0x00101010, 0, false);
        bk_gui_font_draw_string(surface, x + 105, y, p->mount, 0x00101010, 0, false);
        y += 22;
        bk_gui_font_draw_string(surface, x, y, "@H108C74F6", 0x00101010, 0, false);
        bk_gui_font_draw_string(surface, x + 105, y,
            (p->entry.attributes & FAT_ATTR_READ_ONLY) ? "@H40025F11" : "@H61E6BE1A",
            0x00101010, 0, false);
        y += 22;
        bk_gui_font_draw_string(surface, x, y, "@HC80F936B", 0x00101010, 0, false);
        bk_gui_font_draw_string(surface, x + 105, y,
            (p->entry.attributes & FAT_ATTR_HIDDEN) ? "@H40025F11" : "@H61E6BE1A",
            0x00101010, 0, false);
    }
}

static bool fb_properties_event(gui_window_t *window,
                                const gui_event_t *event, void *context) {
    fb_properties_t *p = (fb_properties_t *)context;
    gui_rect_t c;
    if (!p || !window || !event || event->type != GUI_EVENT_MOUSE_DOWN ||
        event->button != MOUSE_LEFT_BUTTON) return false;
    c = bk_gui_window_content_rect_raw(window);
    if (bk_gui_rect_contains((gui_rect_t){c.x + 10, c.y + 6, 82, 22},
                          event->x, event->y)) p->active_tab = 0;
    else if (bk_gui_rect_contains((gui_rect_t){c.x + 92, c.y + 6, 82, 22},
                               event->x, event->y)) p->active_tab = 1;
    else return false;
    window->dirty = true;
    return true;
}

static void fb_open_properties(fb_state_t *st) {
    fb_properties_t *p;
    gui_window_t *window;
    char title[48];
    const char *mount;
    if (!st || st->selected < 0 ||
        (uint32_t)st->selected >= st->entry_count) return;
    p = (fb_properties_t *)bk_sys_alloc_zero(sizeof(*p));
    if (!p) return;
    p->entry = st->entries[st->selected];
    p->drive = st->drive_view ? st->entry_drive[st->selected] : 0;
    if (st->drive_view)
        bk_runtime_strncpy(p->path, st->entry_mount[st->selected],
                           sizeof(p->path) - 1);
    else fb_join_path(p->path, sizeof(p->path), st->cwd, p->entry.name);
    mount = st->drive_view ? st->entry_mount[st->selected]
                           : (fb_at_volume_root(st->cwd) ? st->cwd + 1
                                                        : bk_device_mount_name());
    bk_runtime_strncpy(p->mount, mount ? mount : "@H79CECCFE", sizeof(p->mount) - 1);
    bk_runtime_strcpy(title, "Propiedades: ");
    bk_runtime_strncpy(title + bk_runtime_strlen(title), p->entry.name,
             sizeof(title) - bk_runtime_strlen(title) - 1);
    window = bk_gui_create_window(st->desktop, 118, 72, 390, 250, title);
    if (!window) { bk_sys_free(p); return; }
    bk_gui_set_window_content(window, fb_properties_paint, p);
    bk_gui_set_window_event_handler(window, fb_properties_event, p);
    window->owner_pid = bk_sys_getpid();
    bk_gui_request_paint();
}

/* ══════════════════════════════════════════════════
 *  Callback botón "Subir"
 * ══════════════════════════════════════════════════ */

static void fb_btn_up_cb(gui_window_t *window UNUSED, uint32_t id UNUSED) {
    fb_state_t *st = window ? (fb_state_t *)window->content_context : NULL;
    if (!st) return;
    if (!st->drive_view &&
        ((st->cwd[0] == '/' && st->cwd[1] == '\0') ||
         fb_at_volume_root(st->cwd))) {
        st->drive_view = true;
        fb_load_dir(st);
        fb_update_title(st);
    } else if (!st->drive_view) {
        fb_navigate(st, "..");
    }
}

static void fb_delete_dialog_paint(gui_window_t *window,
                                   gui_surface_t *surface, void *context) {
    fb_delete_dialog_t *dialog = (fb_delete_dialog_t *)context;
    gui_rect_t c;
    gui_rect_t message;
    if (!window || !surface || !dialog) return;
    c = bk_gui_window_content_rect_raw(window);
    bk_gui_gfx_fill_rect(surface, c, 0x00D4D0C8);

    if (dialog->warning_icon)
        bk_app_draw_icon(surface, c.x + 18, c.y + 20,
                                 dialog->warning_icon, 44, 44);
    else {
        /* Fallback por si la imagen no está presente en una instalación vieja. */
        bk_gui_gfx_fill_rect(surface, (gui_rect_t){c.x + 18, c.y + 20, 44, 44},
                          0x00E5B52A);
        bk_gui_gfx_draw_rect(surface, (gui_rect_t){c.x + 18, c.y + 20, 44, 44},
                          0x00605010);
        bk_gui_font_draw_string_scaled(surface, c.x + 34, c.y + 29, "!",
                                    0x00202020, 2);
    }

    message = (gui_rect_t){c.x + 76, c.y + 15, c.w - 92, 76};
    bk_gui_font_draw_string_clipped(surface, message.x, message.y + 2,
        dialog->permanent ? "Eliminar definitivamente?" : "Enviar a la papelera?", 0x00101010, message);
    bk_gui_font_draw_string_clipped(surface, message.x, message.y + 24,
        dialog->name, 0x00004080, message);
    bk_gui_font_draw_string_clipped(surface, message.x, message.y + 46,
        dialog->permanent ? "No se puede deshacer." : "Podras restaurarlo desde la papelera.",
        0x00505050, message);

    bk_gui_gfx_draw_line(surface, c.x + 12, c.y + 99,
                      c.x + c.w - 12, c.y + 99, 0x00808080);
}

static void fb_delete_dialog_button(gui_window_t *window, uint32_t id) {
    fb_delete_dialog_t *dialog = window
        ? (fb_delete_dialog_t *)window->content_context : NULL;
    if (!dialog) return;
    dialog->action = id == dialog->delete_button_id ? 1U : 2U;
    bk_gui_close_window(window);
}

static void fb_delete_dialog_task(void *argument) {
    fb_delete_dialog_t *dialog = (fb_delete_dialog_t *)argument;
    if (!dialog || !dialog->desktop) {
        if (dialog) bk_sys_free(dialog);
        bk_proc_exit();
    }
    dialog->warning_icon = bk_app_load_icon(
        "/ICONS/WARNING.BMP", 44, 44);
    dialog->window = bk_gui_create_window(dialog->desktop, 142, 88,
        410, 180, "@HCC4E9C02");
    if (dialog->window) {
        gui_widget_t *button;
        bk_gui_set_window_content(dialog->window, fb_delete_dialog_paint, dialog);
        bk_gui_set_window_min_size(dialog->window, 350, 170);
        dialog->window->owner_pid = bk_sys_getpid();
        bk_proc_bind_window(dialog->window);
        button = bk_gui_widget_create(dialog->desktop, dialog->window,
            GUI_WIDGET_BUTTON, (gui_rect_t){214, 116, 82, 24},
            "@H79252B86", fb_delete_dialog_button);
        if (button) {
            dialog->delete_button_id = button->id;
            (void)bk_gui_widget_set_icon(button, "Delete");
        }
        button = bk_gui_widget_create(dialog->desktop, dialog->window,
            GUI_WIDGET_BUTTON, (gui_rect_t){304, 116, 82, 24},
            "@HEA3D2B54", fb_delete_dialog_button);
        if (button) {
            dialog->cancel_button_id = button->id;
            (void)bk_gui_widget_set_icon(button, "Close");
        }
        bk_gui_focus_window(dialog->desktop,dialog->window);
        bk_gui_request_paint();
    }

    while (!bk_proc_exit_requested() && dialog->window &&
           dialog->window->listed && !dialog->action)
        bk_sys_sleep_ticks(2);

    if (dialog->action == 1U && g_fb == dialog->owner &&
        dialog->owner->delete_dialog_open &&
        dialog->owner->delete_dialog_token == dialog->owner_token) {
        bool started=bk_shell_job_one(dialog->desktop,
            dialog->permanent?BK_SHELL_JOB_DELETE:BK_SHELL_JOB_TRASH,dialog->path,NULL);
        bk_runtime_strcpy(dialog->owner->status,started?"Procesando archivos...":"No se pudo iniciar la operacion");
        fb_load_dir(dialog->owner);
        if (dialog->owner->window) dialog->owner->window->dirty = true;
    }
    if (g_fb == dialog->owner &&
        dialog->owner->delete_dialog_token == dialog->owner_token)
        dialog->owner->delete_dialog_open = false;
    if (dialog->window) {
        bk_gui_destroy_window(dialog->desktop,dialog->window);
        bk_proc_bind_window(NULL);
    }
    if (dialog->warning_icon) bk_sys_free(dialog->warning_icon);
    bk_sys_free(dialog);
    bk_proc_exit();
}

static void fb_open_delete_dialog(fb_state_t *st) {
    fb_delete_dialog_t *dialog;
    vfs_dir_entry_t *entry;
    if (!st || st->drive_view || st->delete_dialog_open || st->selected < 0 ||
        (uint32_t)st->selected >= st->entry_count) return;
    entry = &st->entries[st->selected];
    if (bk_runtime_strcmp(entry->name, "..") == 0) return;
    dialog = (fb_delete_dialog_t *)bk_sys_alloc_zero(sizeof(*dialog));
    if (!dialog) {
        bk_runtime_strcpy(st->status, "@H4D614B10");
        return;
    }
    dialog->owner = st;
    dialog->desktop = st->desktop;
    dialog->directory = entry->type == VFS_NODE_DIR;
    dialog->permanent = bk_shell_inside(st->cwd,BK_TRASH_FILES);
    bk_runtime_strncpy(dialog->name, entry->name, sizeof(dialog->name) - 1);
    fb_join_path(dialog->path, sizeof(dialog->path), st->cwd, entry->name);
    st->delete_dialog_open = true;
    st->delete_dialog_token = ++g_fb_delete_token;
    if (!st->delete_dialog_token) st->delete_dialog_token = ++g_fb_delete_token;
    dialog->owner_token = st->delete_dialog_token;
    if (bk_proc_spawn_thread("delete-confirm", fb_delete_dialog_task, dialog) < 0) {
        st->delete_dialog_open = false;
        bk_runtime_strcpy(st->status, "@H2DC23EFB");
        bk_sys_free(dialog);
    }
}

static void fb_btn_delete_cb(gui_window_t *window,uint32_t id UNUSED) {
    fb_state_t *st=window ? (fb_state_t *)window->content_context : NULL;
    if(st)fb_open_delete_dialog(st);
}

static void fb_begin_dialog(fb_state_t *st, uint8_t dialog) {
    if (!st || st->drive_view) return;
    st->dialog = dialog;
    st->dialog_input[0] = '\0';
    if (dialog == FB_DIALOG_RENAME && st->selected >= 0 &&
        (uint32_t)st->selected < st->entry_count &&
        bk_runtime_strcmp(st->entries[st->selected].name, "..") != 0)
        bk_runtime_strncpy(st->dialog_input, st->entries[st->selected].name,
                 sizeof(st->dialog_input) - 1);
    if (st->window) st->window->dirty = true;
}

static void fb_finish_dialog(fb_state_t *st) {
    char old_path[VFS_MAX_PATH], new_path[VFS_MAX_PATH];
    if (!st || !st->dialog) return;
    if (st->dialog == FB_DIALOG_NEW_FOLDER) {
        fb_join_path(new_path, sizeof(new_path), st->cwd, st->dialog_input);
        bk_runtime_strcpy(st->status, st->dialog_input[0] && bk_file_mkdir(new_path)
            ? "@H4DD50AF7" : "@H45493FF4");
    } else if (st->selected >= 0 &&
               (uint32_t)st->selected < st->entry_count) {
        vfs_dir_entry_t *entry = &st->entries[st->selected];
        fb_join_path(old_path, sizeof(old_path), st->cwd, entry->name);
        if (st->dialog == FB_DIALOG_RENAME) {
            fb_join_path(new_path, sizeof(new_path), st->cwd,
                         st->dialog_input);
            bk_runtime_strcpy(st->status, st->dialog_input[0] &&
                    bk_file_rename(old_path, new_path)
                    ? "@HBB4962A9" : "@H5634A526");
        }
    }
    st->dialog = FB_DIALOG_NONE;
    fb_load_dir(st);
    if (st->window) st->window->dirty = true;
}

static void fb_btn_copy_cb(gui_window_t *window, uint32_t id UNUSED) {
    fb_state_t *st = window ? (fb_state_t *)window->content_context : NULL;
    if (!st || st->drive_view || st->selected < 0 ||
        (uint32_t)st->selected >= st->entry_count) return;
    if (bk_runtime_strcmp(st->entries[st->selected].name, "..") == 0) return;
    fb_join_path(g_fb_clipboard, sizeof(g_fb_clipboard), st->cwd,
                 st->entries[st->selected].name);
    g_fb_clipboard_cut = false;
    g_fb_clipboard_directory =
        st->entries[st->selected].type == VFS_NODE_DIR;
    bk_runtime_strcpy(st->status, "@H853D008B");
    window->dirty = true;
}

static void fb_btn_cut_cb(gui_window_t *window, uint32_t id UNUSED) {
    fb_state_t *st = window ? (fb_state_t *)window->content_context : NULL;
    if (!st || st->drive_view || st->selected < 0 ||
        (uint32_t)st->selected >= st->entry_count ||
        bk_runtime_strcmp(st->entries[st->selected].name, "..") == 0) return;
    fb_join_path(g_fb_clipboard, sizeof(g_fb_clipboard), st->cwd,
                 st->entries[st->selected].name);
    g_fb_clipboard_cut = true;
    g_fb_clipboard_directory =
        st->entries[st->selected].type == VFS_NODE_DIR;
    bk_runtime_strcpy(st->status, "@HC6567740");
    window->dirty = true;
}

static void fb_btn_paste_cb(gui_window_t *window, uint32_t id UNUSED) {
    fb_state_t *st = window ? (fb_state_t *)window->content_context : NULL;
    char destination[VFS_MAX_PATH];
    const char *name;
    if (!st || st->drive_view || !g_fb_clipboard[0]) return;
    name = g_fb_clipboard;
    for (const char *p = g_fb_clipboard; *p; p++) if (*p == '/') name = p + 1;
    fb_join_path(destination, sizeof(destination), st->cwd, name);
    if (bk_runtime_strcmp(destination, g_fb_clipboard) == 0) {
        bk_runtime_strcpy(st->status, "@HDD307CA6");
    } else if (g_fb_clipboard_directory &&
               bk_runtime_strncmp(destination, g_fb_clipboard,
                        bk_runtime_strlen(g_fb_clipboard)) == 0 &&
               destination[bk_runtime_strlen(g_fb_clipboard)] == '/') {
        bk_runtime_strcpy(st->status, "@HCFE1FAC7");
    } else {
        bool started=bk_shell_job_one(st->desktop,g_fb_clipboard_cut?BK_SHELL_JOB_MOVE:BK_SHELL_JOB_COPY,
            g_fb_clipboard,st->cwd);
        bk_runtime_strcpy(st->status,started?"Procesando archivos...":"No se pudo iniciar la transferencia");
        if(started && g_fb_clipboard_cut)g_fb_clipboard[0]='\0';
    }
    window->dirty = true;
}

static void fb_btn_new_cb(gui_window_t *window, uint32_t id UNUSED) {
    fb_state_t *st = window ? (fb_state_t *)window->content_context : NULL;
    if (!st || st->drive_view) return;
    fb_begin_dialog(st, FB_DIALOG_NEW_FOLDER);
}

static void fb_btn_refresh_cb(gui_window_t *window, uint32_t id UNUSED) {
    fb_state_t *st = window ? (fb_state_t *)window->content_context : NULL;
    if (!st) return;
    fb_load_dir(st);
    bk_runtime_strcpy(st->status, "@H416E3A4E");
    window->dirty = true;
}

static void fb_context_cb(gui_window_t *window, uint32_t item_id,
                          void *context) {
    fb_state_t *st = (fb_state_t *)context;
    if (!st || !window) return;
    switch (item_id) {
        case FB_CONTEXT_OPEN: (void)fb_open_selected(st); break;
        case FB_CONTEXT_EDIT:
            if(st->selected>=0&&(uint32_t)st->selected<st->entry_count){const char *name=st->entries[st->selected].name;if(!fb_open_association_action(st,name,true))fb_open_text(st->desktop,st,name);}
            break;
        case FB_CONTEXT_COPY: fb_btn_copy_cb(window, 0); break;
        case FB_CONTEXT_CUT: fb_btn_cut_cb(window, 0); break;
        case FB_CONTEXT_PASTE: fb_btn_paste_cb(window, 0); break;
        case FB_CONTEXT_RENAME: fb_begin_dialog(st, FB_DIALOG_RENAME); break;
        case FB_CONTEXT_DELETE: fb_open_delete_dialog(st); break;
        case 590: {
            char path[VFS_MAX_PATH];
            if (st->selected >= 0 && (uint32_t)st->selected < st->entry_count) {
                fb_join_path(path,sizeof(path),st->cwd,st->entries[st->selected].name);
                bool restored = bk_shell_restore(path);
                if(restored) bk_gui_files_changed();
                fb_load_dir(st);
                bk_runtime_strcpy(st->status,restored ? "Restaurado" : "No se pudo restaurar");
                st->window->dirty = true;
            }
            break;
        }
        case FB_CONTEXT_PROPERTIES: fb_open_properties(st); break;
        case FB_CONTEXT_NEW_FOLDER: fb_btn_new_cb(window, 0); break;
        case FB_CONTEXT_REFRESH: fb_btn_refresh_cb(window, 0); break;
        default: break;
    }
}

static void fb_open_context_menu(fb_state_t *st, int x, int y, int hit) {
    bool has_entry;
    bool can_copy;
    if (!st || !st->window) return;
    has_entry = hit >= 0 && (uint32_t)hit < st->entry_count;
    if (has_entry) st->selected = hit;
    can_copy = has_entry && !st->drive_view &&
               bk_runtime_strcmp(st->entries[hit].name, "..") != 0;
    bk_gui_window_context_clear(st->window);
    if (!bk_runtime_strcmp(st->cwd,BK_TRASH_FILES) && can_copy)
        (void)bk_gui_window_context_add_item(st->window,590,"Restaurar",true,fb_context_cb,st);
    if (has_entry) {
        (void)bk_gui_window_context_add_item(st->window, FB_CONTEXT_OPEN,
            "@H5A216C13", true, fb_context_cb, st);
        (void)bk_gui_window_context_add_item(st->window, FB_CONTEXT_EDIT,"Editar",can_copy,fb_context_cb,st);
        (void)bk_gui_window_context_add_item(st->window, FB_CONTEXT_COPY,
            "@HAFCCD5E7", can_copy, fb_context_cb, st);
        (void)bk_gui_window_context_add_item(st->window, FB_CONTEXT_CUT,
            "@H79C25276", can_copy, fb_context_cb, st);
        (void)bk_gui_window_context_add_item(st->window, FB_CONTEXT_RENAME,
            "@H33AFD949", can_copy, fb_context_cb, st);
        (void)bk_gui_window_context_add_item(st->window, FB_CONTEXT_DELETE,
            "@H79252B86", can_copy, fb_context_cb, st);
        (void)bk_gui_window_context_add_item(st->window, FB_CONTEXT_PROPERTIES,
            "@H1933FA0F", true, fb_context_cb, st);
    }
    (void)bk_gui_window_context_add_item(st->window, FB_CONTEXT_PASTE,
        "@H0362F7D4", !st->drive_view && g_fb_clipboard[0], fb_context_cb, st);
    (void)bk_gui_window_context_add_item(st->window, FB_CONTEXT_NEW_FOLDER,
        "@HAAAADA68", !st->drive_view, fb_context_cb, st);
    (void)bk_gui_window_context_add_item(st->window, FB_CONTEXT_REFRESH,
        "@H7CC1E2B3", true, fb_context_cb, st);
    bk_gui_window_context_open(st->window, x, y);
}

/* ══════════════════════════════════════════════════
 *  Pintar lista dentro de la ventana
 * ══════════════════════════════════════════════════ */

static int fb_tree_max_rows(gui_rect_t tree) {
    int rows = (tree.h - FB_TREE_PAD * 2) / FB_TREE_ROW_H;
    return rows < 1 ? 1 : rows;
}

static int fb_tree_open_index(const fb_state_t *st, const char *path) {
    for (uint8_t i = 0; st && path && i < st->tree_open_count; i++)
        if (bk_runtime_strcmp(st->tree_open[i], path) == 0) return i;
    return -1;
}

static void fb_tree_set_open(fb_state_t *st, const char *path, bool open) {
    int index = fb_tree_open_index(st, path);
    if (!st || !path) return;
    if (open && index < 0 && st->tree_open_count < FB_TREE_MAX_OPEN) {
        bk_runtime_strncpy(st->tree_open[st->tree_open_count], path,
                           VFS_MAX_PATH - 1U);
        st->tree_open_count++;
    } else if (!open && index >= 0) {
        for (uint8_t i = (uint8_t)index; i + 1U < st->tree_open_count; i++)
            bk_runtime_strcpy(st->tree_open[i], st->tree_open[i + 1U]);
        st->tree_open_count--;
    }
}

static void fb_tree_add_node(fb_state_t *st, const char *path,
                             const char *label, int depth, bool expandable) {
    fb_tree_node_t *node;
    if (!st || st->tree_node_count >= FB_TREE_MAX_NODES) return;
    node = &st->tree_nodes[st->tree_node_count++];
    bk_runtime_strncpy(node->path, path, sizeof(node->path) - 1U);
    bk_runtime_strncpy(node->label, label, sizeof(node->label) - 1U);
    node->depth = (uint8_t)depth;
    node->expandable = expandable;
    node->expanded = expandable && fb_tree_open_index(st, path) >= 0;
}

static void fb_tree_add_children(fb_state_t *st, const char *parent,
                                 int depth) {
    vfs_dir_entry_t *entries = NULL;
    uint32_t count = 0U;
    if (!st || depth > 12 || fb_tree_open_index(st, parent) < 0 ||
        !fb_read_directory(parent, &entries, &count)) return;
    for (uint32_t i = 0; i < count && st->tree_node_count < FB_TREE_MAX_NODES; i++) {
        char path[VFS_MAX_PATH];
        if (entries[i].type != VFS_NODE_DIR ||
            bk_runtime_strcmp(entries[i].name, ".") == 0 ||
            bk_runtime_strcmp(entries[i].name, "..") == 0) continue;
        fb_join_path(path, sizeof(path), parent, entries[i].name);
        fb_tree_add_node(st, path, entries[i].name, depth, true);
        fb_tree_add_children(st, path, depth + 1);
    }
    bk_sys_free(entries);
}

static void fb_tree_rebuild(fb_state_t *st) {
    uint32_t device_count;
    if (!st) return;
    st->tree_node_count = 0U;
    fb_tree_add_node(st, "", "@H06585867", 0, true);
    if (fb_tree_open_index(st, "") < 0) return;
    device_count = bk_device_block_count();
    for (uint32_t i = 0; i < device_count; i++) {
        bk_block_info_t device;
        char path[16];
        if (!bk_device_block_info(i, &device) || device.type == BK_BLOCK_ATAPI)
            continue;
        snprintf(path, sizeof(path), "/%s", device.name);
        fb_tree_add_node(st, path, device.name, 1, true);
        fb_tree_add_children(st, path, 2);
    }
    if (fb_cdrom_available()) {
        fb_tree_add_node(st, "/CDROM", "CD-ROM", 1, true);
        fb_tree_add_children(st, "/CDROM", 2);
    }
    if (st->tree_scroll < 0) st->tree_scroll = 0;
    if (st->tree_scroll >= (int)st->tree_node_count)
        st->tree_scroll = st->tree_node_count ? st->tree_node_count - 1 : 0;
}

static void fb_tree_draw_row(gui_surface_t *surface, gui_rect_t tree,
                             int row, int indent, const char *label,
                             const uint32_t *icon, bool selected,
                             int expansion) {
    gui_rect_t line;
    int y = tree.y + FB_TREE_PAD + row * FB_TREE_ROW_H;
    int control_x = tree.x + 5 + indent * 14;
    int ix = control_x + (expansion >= 0 ? 14 : 0);
    if (y + FB_TREE_ROW_H > tree.y + tree.h) return;
    line = (gui_rect_t){tree.x + 2, y, tree.w - 4, FB_TREE_ROW_H - 1};
    if (selected)
        bk_gui_gfx_fill_rect(surface, line, 0x00C8C8C8);
    if (indent > 0) {
        int stem_x = tree.x + 10 + (indent - 1) * 14;
        bk_gui_gfx_draw_line(surface, stem_x, y,
                             stem_x, y + FB_TREE_ROW_H / 2, 0x00808080);
        bk_gui_gfx_draw_line(surface, stem_x, y + FB_TREE_ROW_H / 2,
                             ix - 2, y + FB_TREE_ROW_H / 2, 0x00808080);
    }
    if (expansion >= 0) {
        gui_rect_t button = {control_x, y + 6, 11, 11};
        bk_gui_gfx_fill_rect(surface, button, 0x00F4F4F0);
        bk_gui_gfx_draw_rect(surface, button, 0x00606060);
        bk_gui_gfx_draw_line(surface, control_x + 2, y + 11,
                             control_x + 8, y + 11, 0x00202020);
        if (!expansion)
            bk_gui_gfx_draw_line(surface, control_x + 5, y + 8,
                                 control_x + 5, y + 14, 0x00202020);
    }
    fb_draw_small_icon(surface, ix, y + 2, icon);
    bk_gui_font_draw_string_clipped(surface, ix + 24, y + 8,
        label ? label : "", FB_COLOR_TEXT,
        (gui_rect_t){ix + 22, y, tree.x + tree.w - ix - 25,
                     FB_TREE_ROW_H});
}

/* Panel izquierdo tipo Explorer. La rama principal siempre muestra
 * "Este equipo" y, debajo, las unidades o las carpetas hijas del cwd. */
static void fb_paint_tree(gui_surface_t *surface, fb_state_t *st) {
    gui_rect_t tree = fb_tree_rect(st);
    int max_rows;
    gui_scrollbar_t scrollbar;
    if (!surface || !st || tree.w <= 0 || tree.h <= 0) return;

    fb_tree_rebuild(st);

    bk_gui_gfx_fill_rect(surface, tree, 0x00E8E8E8);
    bk_gui_gfx_draw_rect(surface, tree, 0x00606060);
    bk_gui_gfx_draw_line(surface, tree.x + tree.w - 2, tree.y + 1,
                         tree.x + tree.w - 2, tree.y + tree.h - 2,
                         0x00FFFFFF);
    max_rows = fb_tree_max_rows(tree);
    for (int row = 0; row < max_rows; row++) {
        int index = st->tree_scroll + row;
        fb_tree_node_t *node;
        const uint32_t *icon;
        if (index >= (int)st->tree_node_count) break;
        node = &st->tree_nodes[index];
        icon = index == 0 ? (st->files_icon ? st->files_icon : st->hdd_icon)
                          : (node->depth == 1 ? st->hdd_icon : st->folder_icon);
        fb_tree_draw_row(surface, tree, row, node->depth, node->label, icon,
            node->path[0] && bk_runtime_strcmp(node->path, st->cwd) == 0,
            node->expandable ? node->expanded : -1);
    }
    bk_gui_scrollbar_init_vertical(&scrollbar,
        (gui_rect_t){tree.x + tree.w - GUI_SCROLLBAR_SIZE, tree.y,
                     GUI_SCROLLBAR_SIZE, tree.h},
        (uint32_t)st->tree_scroll, (uint32_t)max_rows, st->tree_node_count);
    bk_gui_scrollbar_paint_vertical(surface, &scrollbar);
}

/* Devuelve true cuando el click pertenecia al tree, incluso si cae sobre
 * espacio vacio: asi nunca selecciona accidentalmente un icono del panel
 * derecho. */
static bool fb_tree_click(fb_state_t *st, int x, int y) {
    gui_rect_t tree = fb_tree_rect(st);
    int row;
    if (!st || tree.w <= 0 || !bk_gui_rect_contains(tree, x, y)) return false;
    if (y < tree.y + FB_TREE_PAD) return true;
    row = st->tree_scroll + (y - tree.y - FB_TREE_PAD) / FB_TREE_ROW_H;
    if (row < 0 || row >= (int)st->tree_node_count) return true;
    {
        fb_tree_node_t *node = &st->tree_nodes[row];
        int control_x = tree.x + 5 + node->depth * 14;
        bool control = node->expandable && x >= control_x && x < control_x + 12;
        if (control) {
            fb_tree_set_open(st, node->path, !node->expanded);
        } else if (!node->path[0]) {
            st->drive_view = true;
            bk_runtime_strcpy(st->cwd, "/");
            (void)fb_load_dir(st);
            fb_update_title(st);
        } else {
            st->drive_view = false;
            bk_runtime_strncpy(st->cwd, node->path, sizeof(st->cwd) - 1U);
            (void)fb_load_dir(st);
            fb_update_title(st);
        }
        fb_tree_rebuild(st);
        st->window->dirty = true;
    }
    (void)bk_system_sound_event_play(BK_SYSTEM_SOUND_BUTTON);
    return true;
}

static void fb_paint_list(gui_surface_t *surface, fb_state_t *st) {
    gui_rect_t list;
    gui_scrollbar_t scrollbar;
    int cols;
    int visible;

    if (!st->window) return;
    fb_clamp_scroll(st);
    list = fb_items_rect(st);
    if (list.w <= GUI_SCROLLBAR_SIZE || list.h <= 0) return;

    fb_paint_tree(surface, st);
    bk_gui_gfx_fill_rect(surface, list, FB_COLOR_BG);
    bk_gui_gfx_draw_rect(surface, list, 0x00808080);
    bk_gui_scrollbar_init_vertical(&scrollbar,
        (gui_rect_t){list.x + list.w - GUI_SCROLLBAR_SIZE, list.y,
                     GUI_SCROLLBAR_SIZE, list.h},
        (uint32_t)st->scroll, (uint32_t)fb_visible_slots(st), st->entry_count);

    cols = st->list_view ? 1 : fb_cols_for_rect(list);
    visible = fb_visible_slots(st);
    for (int i = 0; i < visible; i++) {
        int idx = st->scroll + i;
        if ((uint32_t)idx >= st->entry_count) break;
        vfs_dir_entry_t *e = &st->entries[idx];
        int ex = list.x + (i % cols) * FB_TILE_W;
        int ey = list.y + (i / cols) * fb_item_row_height(st);
        bool sel = (idx == st->selected);
        int item_w = st->list_view ? list.w - GUI_SCROLLBAR_SIZE : FB_TILE_W - 4;
        int item_h = st->list_view ? FB_LIST_ROW_H - 1 : FB_TILE_H - 3;
        if (sel)
            bk_gui_gfx_fill_rect(surface, (gui_rect_t){ex, ey, item_w, item_h},
                              FB_COLOR_SEL);
        if (sel) {
            bk_gui_gfx_draw_rect(surface,
                (gui_rect_t){ex, ey, item_w, item_h}, 0x00002060);
            bk_gui_gfx_draw_rect(surface,
                (gui_rect_t){ex + 2, ey + 2, item_w - 4, item_h - 4},
                0x00FFFFFF);
        }
        if (st->list_view) {
            fb_draw_bmp_icon(surface, ex + 3, ey + 3,
                             fb_icon_for_entry(st, e, (uint32_t)idx));
            bk_gui_font_draw_string_clipped(surface, ex + FB_ICON_SIZE + 10,
                ey + 14, e->name, sel ? FB_COLOR_TEXTSEL : FB_COLOR_TEXT,
                (gui_rect_t){ex + FB_ICON_SIZE + 8, ey + 2,
                             item_w - FB_ICON_SIZE - 12, item_h - 4});
        } else {
            fb_draw_bmp_icon(surface, ex + (FB_TILE_W - FB_ICON_SIZE) / 2,
                             ey + 3, fb_icon_for_entry(st, e, (uint32_t)idx));
            fb_draw_tile_name(surface, ex + 4, ey + 39, e->name,
                              sel ? FB_COLOR_TEXTSEL : FB_COLOR_TEXT,
                              (gui_rect_t){ex + 3, ey + 37, FB_TILE_W - 10, 24});
        }
    }
    bk_gui_scrollbar_paint_vertical(surface, &scrollbar);
}

/* ══════════════════════════════════════════════════
 *  Hit-test en la lista
 * ══════════════════════════════════════════════════ */

static int fb_hit_entry(fb_state_t *st, int ex, int ey) {
    gui_rect_t list;
    int cols;
    int col;
    int row;
    int idx;

    if (!st->window) return -1;
    list = fb_items_rect(st);
    if (list.w <= GUI_SCROLLBAR_SIZE || list.h <= 0) return -1;

    if (ex < list.x || ex >= list.x + list.w - GUI_SCROLLBAR_SIZE) return -1;
    if (ey < list.y || ey >= list.y + list.h) return -1;

    cols = st->list_view ? 1 : fb_cols_for_rect(list);
    col = st->list_view ? 0 : (ex - list.x) / FB_TILE_W;
    row = (ey - list.y) / fb_item_row_height(st);
    if (col < 0 || col >= cols) return -1;
    idx = st->scroll + row * cols + col;
    if ((uint32_t)idx >= st->entry_count) return -1;
    return idx;
}

/* ══════════════════════════════════════════════════
 *  Paint del programa
 * ══════════════════════════════════════════════════ */

static void fb_content(gui_window_t *window UNUSED,
                       gui_surface_t *surface, void *context) {
    fb_state_t *st = (fb_state_t *)context;
    if (st) {
        gui_rect_t content = bk_gui_window_content_rect_raw(st->window);
        gui_rect_t address = fb_address_rect(st);
        int x = address.x;
        int y = address.y;
        bk_gui_gfx_fill_rect(surface,
            (gui_rect_t){content.x + 2, content.y + 28, content.w - 4, 28},
            0x00C0C0C0);
        bk_gui_gfx_draw_line(surface, content.x + 2, content.y + 56,
                             content.x + content.w - 3, content.y + 56,
                             0x00808080);
        bk_gui_gfx_fill_rect(surface, address,
                          st->editing_address ? 0x00FFF4C8 : 0x00FFFFFF);
        bk_gui_gfx_draw_rect(surface, address,
                          0x00606060);
        bk_gui_font_draw_string_clipped(surface, x + 4, y + 6,
            st->editing_address ? st->address : st->cwd, 0x00101010,
            (gui_rect_t){x + 3, y + 2, content.w - 52, 18});
        fb_paint_list(surface, st);
        if (st->status[0]) {
            gui_rect_t list = fb_list_rect(st);
            gui_rect_t status = {list.x, list.y + list.h + 2, list.w, 16};
            bk_gui_gfx_fill_rect(surface, status, 0x00E8E8E8);
            bk_gui_gfx_draw_rect(surface, status, 0x00808080);
            bk_gui_font_draw_string_clipped(surface, status.x + 4,
                                         status.y + 4, st->status,
                                         0x00101010, status);
        }
        if (st->dialog) {
            gui_rect_t c = bk_gui_window_content_rect_raw(st->window);
            gui_rect_t box = {c.x + (c.w - 300) / 2,
                              c.y + (c.h - 105) / 2, 300, 105};
            const char *caption = st->dialog == FB_DIALOG_NEW_FOLDER
                ? "@HDB2502C6" : "@H33AFD949";
            bk_gui_gfx_fill_rect(surface, box, BK_THEME_FACE);
            bk_gui_gfx_draw_rect(surface, box, 0x00303030);
            bk_gui_font_draw_string(surface, box.x + 10, box.y + 12, caption,
                                 0x00101010, 0, false);
            bk_gui_gfx_fill_rect(surface,
                (gui_rect_t){box.x + 10, box.y + 35, box.w - 20, 22},
                0x00FFFFFF);
            bk_gui_gfx_draw_rect(surface,
                (gui_rect_t){box.x + 10, box.y + 35, box.w - 20, 22},
                0x00606060);
            bk_gui_font_draw_string_clipped(surface, box.x + 14, box.y + 42,
                st->dialog_input, 0x00101010,
                (gui_rect_t){box.x + 12, box.y + 37, box.w - 24, 18});
            bk_gui_font_draw_string(surface, box.x + 10, box.y + 78,
                                 "@H46184691",
                                 0x00303030, 0, false);
        }
    }
}

static void fb_menu_cb(gui_window_t *window, uint32_t item_id,
                       void *context) {
    fb_state_t *st = (fb_state_t *)context;
    if (!st || !window) return;
    switch (item_id) {
        case FB_MENU_OPEN: (void)fb_open_selected(st); break;
        case FB_MENU_NEW_FOLDER: fb_begin_dialog(st, FB_DIALOG_NEW_FOLDER); break;
        case FB_MENU_RENAME: fb_begin_dialog(st, FB_DIALOG_RENAME); break;
        case FB_MENU_DELETE: fb_open_delete_dialog(st); break;
        case FB_MENU_PROPERTIES: fb_open_properties(st); break;
        case FB_MENU_COPY: fb_btn_copy_cb(window, 0); break;
        case FB_MENU_CUT: fb_btn_cut_cb(window, 0); break;
        case FB_MENU_PASTE: fb_btn_paste_cb(window, 0); break;
        case FB_MENU_VIEW_ICONS: st->list_view = false; st->scroll = 0; break;
        case FB_MENU_VIEW_LIST: st->list_view = true; st->scroll = 0; break;
        case FB_MENU_REFRESH: fb_btn_refresh_cb(window, 0); break;
        case FB_MENU_ABOUT:
            bk_about_show(st->desktop, &(bk_about_info_t){
                "@HC46B7E2C", "@H1C1E4EC2",
                "@H298CADA7",
                "@H7A28E1E5", "/ICONS/FILES.BMP"});
            break;
        default: return;
    }
    window->dirty = true;
}

/* ══════════════════════════════════════════════════
 *  Handle events del programa
 * ══════════════════════════════════════════════════ */

static bool fb_window_event(gui_window_t *window UNUSED,
                            const gui_event_t *event, void *context) {
    fb_state_t *st = (fb_state_t *)context;
    if (!st || !event) return false;

    /* Solo nos interesan eventos dentro de la ventana abierta */
    if (!st->window) return false;
    if (event->type == GUI_EVENT_FILES_CHANGED) {
        (void)fb_load_dir(st); st->window->dirty = true; return true;
    }
    if (event->type == GUI_EVENT_FILE_DROP) {
        char source[VFS_MAX_PATH], directory[VFS_MAX_PATH];
        if (!bk_gui_file_drop_take(st->window,event,source)) return true;
        bk_runtime_strcpy(directory,st->drive_view ? "" : st->cwd);
        gui_rect_t tree=fb_tree_rect(st);
        if(bk_gui_rect_contains(tree,event->x,event->y)) {
            int row=st->tree_scroll+(event->y-tree.y-FB_TREE_PAD)/FB_TREE_ROW_H;
            if(event->y<tree.y+FB_TREE_PAD || row<0 || row>=(int)st->tree_node_count) {
                while(bk_gui_file_drop_take(st->window,event,source)){}return true;
            }
            bk_runtime_strcpy(directory,st->tree_nodes[row].path);
        } else {
            int hit=fb_hit_entry(st,event->x,event->y);
            if(hit>=0 && st->drive_view)bk_runtime_strcpy(directory,st->entry_mount[hit]);
            else if(hit>=0 && st->entries[hit].type==VFS_NODE_DIR)
                fb_join_path(directory,sizeof(directory),st->cwd,st->entries[hit].name);
        }
        if(!directory[0]) {while(bk_gui_file_drop_take(st->window,event,source)){}return true;}
        bk_shell_job_t *job=bk_shell_job_create(st->desktop,
            !bk_runtime_strcmp(directory,BK_TRASH_FILES)?BK_SHELL_JOB_TRASH:BK_SHELL_JOB_MOVE,directory);
        bool queued=job!=NULL;
        do {if(!bk_shell_job_add(job,source))queued=false;}
        while(bk_gui_file_drop_take(st->window,event,source));
        if(queued)queued=bk_shell_job_start(job);else bk_sys_free(job);
        bk_runtime_strcpy(st->status,queued?"Procesando archivos...":"No se pudo iniciar la transferencia");
        st->window->dirty = true; return true;
    }
    if (event->type == GUI_EVENT_MOUSE_MOVE && st->drag_pending &&
        (event->buttons & MOUSE_LEFT_BUTTON)) {
        int dx=event->x-st->drag_x,dy=event->y-st->drag_y;
        if (dx*dx+dy*dy >= 36 && st->drag_entry >= 0 &&
            (uint32_t)st->drag_entry < st->entry_count && !st->drive_view &&
            bk_runtime_strcmp(st->entries[st->drag_entry].name,"..")) {
            char source[VFS_MAX_PATH];
            fb_join_path(source,sizeof(source),st->cwd,st->entries[st->drag_entry].name);
            if(!bk_gui_file_drag_start_icon(st->window,source,
                fb_icon_for_entry(st,&st->entries[st->drag_entry],st->drag_entry),
                FB_ICON_SIZE,FB_ICON_SIZE))return true;
            st->drag_pending = false; st->last_list_click_tick = 0;
            return true;
        }
    }
    if (event->type == GUI_EVENT_MOUSE_UP || event->type == GUI_EVENT_FOCUS_LOST)
        st->drag_pending = false;


    if (st->dialog && event->type == GUI_EVENT_KEY) {
        size_t len = bk_runtime_strlen(st->dialog_input);
        if (event->key == KEY_ESCAPE) {
            st->dialog = FB_DIALOG_NONE;
        } else if (event->key == KEY_ENTER || event->key == '\n') {
            fb_finish_dialog(st);
        } else if (event->key == KEY_BACKSPACE) {
            if (len) st->dialog_input[len - 1] = '\0';
        } else if (event->key >= 32 &&
                   event->key < 127 && len + 1 < sizeof(st->dialog_input) &&
                   event->key != '/' && event->key != '\\') {
            st->dialog_input[len] = (char)event->key;
            st->dialog_input[len + 1] = '\0';
        }
        st->window->dirty = true;
        return true;
    }

    {
        gui_rect_t tree = fb_tree_rect(st);
        gui_scrollbar_t scrollbar;
        uint32_t new_scroll;
        int visible = fb_tree_max_rows(tree);
        bk_gui_scrollbar_init_vertical(&scrollbar,
            (gui_rect_t){tree.x + tree.w - GUI_SCROLLBAR_SIZE, tree.y,
                         GUI_SCROLLBAR_SIZE, tree.h},
            (uint32_t)st->tree_scroll, (uint32_t)visible,
            st->tree_node_count);
        if ((st->tree_scrollbar_drag.active ||
             bk_gui_rect_contains(tree, event->x, event->y)) &&
            bk_gui_scrollbar_handle_event_vertical(&scrollbar,
                &st->tree_scrollbar_drag, event, 1U, &new_scroll)) {
            st->tree_scroll = (int)new_scroll;
            st->window->dirty = true;
            return true;
        }
    }

    {
        gui_rect_t list = fb_items_rect(st);
        gui_scrollbar_t scrollbar;
        uint32_t new_scroll;
        int cols = st->list_view ? 1 : fb_cols_for_rect(list);
        bk_gui_scrollbar_init_vertical(&scrollbar,
            (gui_rect_t){list.x + list.w - GUI_SCROLLBAR_SIZE, list.y,
                         GUI_SCROLLBAR_SIZE, list.h},
            (uint32_t)st->scroll, (uint32_t)fb_visible_slots(st),
            st->entry_count);
        if ((st->scrollbar_drag.active ||
             bk_gui_rect_contains(list, event->x, event->y)) &&
            bk_gui_scrollbar_handle_event_vertical(&scrollbar,
                &st->scrollbar_drag, event, (uint32_t)cols, &new_scroll)) {
            /* El thumb devuelve indices absolutos. Cuantizar a fila completa
             * evita que al arrastrar la barra la grilla corra un solo icono. */
            if (cols > 1) new_scroll = (new_scroll / (uint32_t)cols) *
                                      (uint32_t)cols;
            st->scroll = (int)new_scroll;
            fb_clamp_scroll(st);
            st->window->dirty = true;
            return true;
        }
    }

    if (event->type == GUI_EVENT_KEY) {
        if (st->editing_address) {
            size_t len = bk_runtime_strlen(st->address);
            if (event->key == '\n') {
                char previous[VFS_MAX_PATH];
                bk_runtime_strncpy(previous, st->cwd, sizeof(previous) - 1U);
                previous[sizeof(previous) - 1U] = '\0';
                st->drive_view = false;
                bk_runtime_strncpy(st->cwd, st->address,
                                   sizeof(st->cwd) - 1U);
                st->cwd[sizeof(st->cwd) - 1U] = '\0';
                if (fb_load_dir(st)) {
                    fb_update_title(st);
                    st->status[0] = '\0';
                } else {
                    bk_runtime_strncpy(st->cwd, previous,
                                       sizeof(st->cwd) - 1U);
                    st->cwd[sizeof(st->cwd) - 1U] = '\0';
                    (void)fb_load_dir(st);
                    bk_runtime_strcpy(st->status, "@HC0DF19BB");
                }
                st->editing_address = false;
            } else if (event->key == 27) {
                st->editing_address = false;
            } else if (event->key == '\b') {
                if (len) st->address[len - 1] = '\0';
            } else if (event->key >= 32 && event->key < 127 &&
                       len + 1 < sizeof(st->address)) {
                st->address[len] = event->key;
                st->address[len + 1] = '\0';
            }
            st->window->dirty = true;
            return true;
        }
        if (event->key == '\n' && st->selected >= 0 &&
            (uint32_t)st->selected < st->entry_count) {
            (void)fb_open_selected(st);
            return true;
        }
        if (event->key == 'c' || event->key == 'C') {
            fb_btn_copy_cb(st->window, 0); return true;
        }
        if (event->key == 'v' || event->key == 'V') {
            fb_btn_paste_cb(st->window, 0); return true;
        }
        if ((uint8_t)event->key == KEY_F2) {
            fb_begin_dialog(st, FB_DIALOG_RENAME); return true;
        }
        if ((uint8_t)event->key == KEY_DELETE) {
            fb_open_delete_dialog(st); return true;
        }
        if ((uint8_t)event->key == KEY_PGUP) {
            st->scroll -= fb_visible_slots(st);
            fb_clamp_scroll(st);
            st->window->dirty = true;
            return true;
        }
        if ((uint8_t)event->key == KEY_PGDN) {
            st->scroll += fb_visible_slots(st);
            fb_clamp_scroll(st);
            st->window->dirty = true;
            return true;
        }
    }

    if (event->type == GUI_EVENT_MOUSE_DOWN) {
        gui_rect_t address_rect = fb_address_rect(st);
        if (event->button == MOUSE_LEFT_BUTTON &&
            fb_tree_click(st, event->x, event->y))
            return true;
        int hit = fb_hit_entry(st, event->x, event->y);
        if (event->button == MOUSE_RIGHT_BUTTON) {
            fb_open_context_menu(st, event->x, event->y, hit);
            st->window->dirty = true;
            return true;
        }
        if (!st->drive_view &&
            bk_gui_rect_contains(address_rect, event->x, event->y)) {
            st->editing_address = true;
            bk_runtime_strncpy(st->address, st->cwd, sizeof(st->address) - 1);
            bk_runtime_strcpy(st->status, "@HF1D1BB1A");
            st->window->dirty = true;
            return true;
        }
        if (hit >= 0) {
            if (event->button == MOUSE_LEFT_BUTTON) {
                st->drag_pending = true; st->drag_entry = hit;
                st->drag_x = event->x; st->drag_y = event->y;
            }
            if (st->selected != hit) {
                st->selected = hit;
                st->window->dirty = true;
            }
            return true;
        }
        return false;
    }

    if (event->type == GUI_EVENT_MOUSE_UP) {
        int hit = fb_hit_entry(st, event->x, event->y);
        if (hit >= 0 && hit == st->selected) {
            uint32_t now   = bk_sys_ticks();
            uint32_t delta = now - st->last_list_click_tick;

            if (delta < FB_DBLCLICK_TICKS &&
                hit == st->last_list_click_idx &&
                st->last_list_click_tick != 0) {
                /* Doble clic en entrada → navegar si es directorio */
                (void)fb_open_selected(st);
                st->last_list_click_tick = 0;
            } else {
                st->last_list_click_tick = now;
                st->last_list_click_idx  = hit;
            }
            /* Un mouse-up simple no cambia ningun pixel. Repintar otra vez
             * toda la ventana duplicaba el costo de cada clic. La apertura
             * por doble clic ya marca dirty desde fb_open_selected(). */
            return true;
        }
        return false;
    }

    return false;
}

/* ══════════════════════════════════════════════════
 *  Destructor
 * ══════════════════════════════════════════════════ */

static void fb_cleanup(fb_state_t *st) {
    if (!st) return;
    bk_gui_file_drag_cancel(st->window);
    if (st->window) {
        bk_gui_destroy_window(st->desktop,st->window);
        bk_proc_bind_window(NULL);
    }

    /*
     * Los iconos y Associations.INI viven en g_fb_icon_cache.
     * No se liberan por ventana: así el filebrowser no vuelve a leer
     * ICONS.PAK/BMPs cada vez que se abre.
     */
    if (st->entries) bk_sys_free(st->entries);
    if (st->entry_drive) bk_sys_free(st->entry_drive);
    if (st->entry_mount) bk_sys_free(st->entry_mount);
    if (g_fb == st) g_fb = NULL;
    bk_sys_free(st);
}

void filebrowser_open_path(gui_desktop_t *desktop, const char *path) {
    fb_state_t *st;
    if (!desktop) return;

    st = (fb_state_t *)bk_sys_alloc_zero(sizeof(fb_state_t));
    if (!st) return;
    st->desktop = desktop;
    st->selected = -1;
    st->list_view = false;
    fb_tree_set_open(st, "", true);
    st->tree_root_expanded = true;
    st->tree_cwd_expanded = true;
    st->drive_view = !path || !path[0] || bk_runtime_strcmp(path, "/") == 0;
    bk_runtime_strncpy(st->cwd, path && path[0] ? path : bk_file_getcwd(),
             sizeof(st->cwd) - 1);
    st->cwd[sizeof(st->cwd) - 1] = '\0';
    g_fb = st;
    if (bk_proc_spawn_thread("filebrowser", fb_main, st) < 0) {
        fb_cleanup(st);
    }
}

void filebrowser_open_from_desktop(gui_desktop_t *desktop) {
    filebrowser_open_path(desktop, "/");
}

/* ══════════════════════════════════════════════════
 *  Instalación (llamar una vez al arrancar la GUI)
 * ══════════════════════════════════════════════════ */

bool filebrowser_get_runtime_info(program_runtime_info_t *info) {
    if (!info || !g_fb) return false;
    info->window = g_fb->window;
    info->memory_bytes = (uint32_t)sizeof(*g_fb);
    info->memory_bytes += g_fb->entry_capacity *
        ((uint32_t)sizeof(vfs_dir_entry_t) + (uint32_t)sizeof(uint8_t) + 16U);
    if (g_fb->folder_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->file_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->text_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->config_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->image_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->object_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->midi_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->hdd_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->cd_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->usb_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->floppy_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->shell_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->editor_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->calc_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->files_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->midamp_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->netscape_icon) info->memory_bytes += FB_ICON_BYTES;
    if (g_fb->window) {
        info->memory_bytes += (uint32_t)sizeof(gui_window_t);
        info->memory_bytes += (uint32_t)sizeof(gui_widget_t);
    }
    return true;
}

static void fb_main(void *argument) {
    fb_state_t *st = (fb_state_t *)argument;
    if (!st || !st->desktop) {
        fb_cleanup(st);
        bk_proc_exit();
    }

    bk_proc_set_memory_hint(sizeof(*st) + FB_ICON_BYTES * 17);
    /*
     * Como app externa puede ceder CPU durante la carga de iconos y el
     * escritorio llegar a pintar una primera vez con la lista vacia.
     * Dejamos el estado listo antes de crear la ventana.
     */
    fb_load_resources(st);
    fb_load_dir(st);
    st->window = bk_gui_create_window(st->desktop,
        FB_WIN_X, FB_WIN_Y, FB_WIN_W, FB_WIN_H, "@HC46B7E2C");
    if (st->window) {
        int file_menu = bk_gui_add_menu(st->window, "@H2B183663");
        int edit_menu = bk_gui_add_menu(st->window, "@HC2C76CB1");
        int view_menu = bk_gui_add_menu(st->window, "@H5E4BAA18");
        int help_menu = bk_gui_add_menu(st->window, "@HB89DF01A");
        bk_gui_add_menu_item(st->window, file_menu, FB_MENU_OPEN,
                                 "@H5A216C13", fb_menu_cb, st);
        bk_gui_add_menu_item(st->window, file_menu, FB_MENU_NEW_FOLDER,
                                 "@HAAAADA68", fb_menu_cb, st);
        bk_gui_add_menu_item(st->window, file_menu, FB_MENU_RENAME,
                                 "@H33AFD949", fb_menu_cb, st);
        bk_gui_add_menu_item(st->window, file_menu, FB_MENU_DELETE,
                                 "@H79252B86", fb_menu_cb, st);
        bk_gui_add_menu_item(st->window, file_menu, FB_MENU_PROPERTIES,
                                 "@H1933FA0F", fb_menu_cb, st);
        bk_gui_add_menu_item(st->window, edit_menu, FB_MENU_COPY,
                                 "@HAFCCD5E7", fb_menu_cb, st);
        bk_gui_add_menu_item(st->window, edit_menu, FB_MENU_CUT,
                                 "@H79C25276", fb_menu_cb, st);
        bk_gui_add_menu_item(st->window, edit_menu, FB_MENU_PASTE,
                                 "@H0362F7D4", fb_menu_cb, st);
        bk_gui_add_menu_item(st->window, view_menu, FB_MENU_VIEW_ICONS,
                                 "@H25A180BA", fb_menu_cb, st);
        bk_gui_add_menu_item(st->window, view_menu, FB_MENU_VIEW_LIST,
                                 "@HF7E2C340", fb_menu_cb, st);
        bk_gui_add_menu_item(st->window, view_menu, FB_MENU_REFRESH,
                                 "@H7CC1E2B3", fb_menu_cb, st);
        bk_gui_add_menu_item(st->window, help_menu, FB_MENU_ABOUT,
                                 "@H90A01076", fb_menu_cb, st);
        bk_gui_set_window_content(st->window, fb_content, st);
        bk_gui_set_window_event_handler(st->window, fb_window_event, st);
        bk_gui_set_window_min_size(st->window, 220, 150);
        st->window->owner_pid = bk_sys_getpid();
        bk_proc_bind_window(st->window);

        gui_widget_t *btn = bk_gui_widget_create(st->desktop, st->window,
            GUI_WIDGET_BUTTON,
            (gui_rect_t){FB_LIST_X, FB_LIST_Y, 24, 22},
            "", fb_btn_up_cb);
        if (btn) {
            st->btn_up_id = btn->id;
            (void)bk_gui_widget_set_icon(btn, "Back");
        }

        /* Toolbar compacto: una sola medida e iconos de 16 px. Los textos
         * siguen disponibles en los menus; aca priorizamos espacio util. */
        btn = bk_gui_widget_create(st->desktop, st->window, GUI_WIDGET_BUTTON,
            (gui_rect_t){6, 30, 26, 24}, "", fb_btn_copy_cb);
        if (btn) {
            st->btn_copy_id = btn->id;
            (void)bk_gui_widget_set_icon(btn, "Copy");
        }
        btn = bk_gui_widget_create(st->desktop, st->window, GUI_WIDGET_BUTTON,
            (gui_rect_t){38, 30, 26, 24}, "", fb_btn_paste_cb);
        if (btn) {
            st->btn_paste_id = btn->id;
            (void)bk_gui_widget_set_icon(btn, "Paste");
        }
        btn = bk_gui_widget_create(st->desktop, st->window, GUI_WIDGET_BUTTON,
            (gui_rect_t){70, 30, 26, 24}, "", fb_btn_new_cb);
        if (btn) {
            st->btn_new_id = btn->id;
            (void)bk_gui_widget_set_icon(btn, "Folder");
        }
        btn = bk_gui_widget_create(st->desktop, st->window, GUI_WIDGET_BUTTON,
            (gui_rect_t){102, 30, 26, 24}, "", fb_btn_refresh_cb);
        if (btn) {
            st->btn_refresh_id = btn->id;
            (void)bk_gui_widget_set_icon(btn, "Refresh");
        }
        btn=bk_gui_widget_create(st->desktop,st->window,GUI_WIDGET_BUTTON,
            (gui_rect_t){134,30,26,24},"",fb_btn_delete_cb);
        if(btn)(void)bk_gui_widget_set_icon(btn,"Delete");
        fb_update_title(st);
        st->window->dirty = true;
        bk_gui_request_paint();
    }

    while (!bk_proc_exit_requested()) {
        if (!st->window || !st->window->listed) break;
        if(st->file_change_seen!=bk_gui_files_sequence() && !st->dialog && !st->delete_dialog_open) {
            st->file_change_seen=bk_gui_files_sequence();
            (void)fb_load_dir(st);st->window->dirty=true;
        }
        bk_sys_sleep_ticks(4);
    }

    fb_cleanup(st);
    bk_proc_exit();
}

void filebrowser_install(gui_desktop_t *desktop UNUSED) {}

void bleskernos_program_main(gui_desktop_t *desktop) {
    fb_state_t *st;
    const char *path = bk_app_launch_argument();

    if (!desktop) {
        bk_proc_exit();
    }

    st = (fb_state_t *)bk_sys_alloc_zero(sizeof(fb_state_t));
    if (!st) {
        bk_proc_exit();
    }

    st->desktop = desktop;
    st->selected = -1;
    st->list_view = false;
    fb_tree_set_open(st, "", true);
    st->tree_root_expanded = true;
    st->tree_cwd_expanded = true;
    st->drive_view = !path || !path[0] || bk_runtime_strcmp(path, "/") == 0;
    bk_runtime_strncpy(st->cwd, path && path[0] ? path : bk_file_getcwd(),
             sizeof(st->cwd) - 1);
    st->cwd[sizeof(st->cwd) - 1] = '\0';
    g_fb = st;

    /*
     * Como ELF externo, el navegador debe vivir en esta misma tarea.
     * Si abre una segunda tarea y la principal termina, la ventana no llega
     * a mantenerse estable.
     */
    fb_main(st);
}

bool filebrowser_global_copy(void) {
    if (!g_fb || !g_fb->window || !g_fb->window->focused) return false;
    fb_btn_copy_cb(g_fb->window, 0);
    return true;
}

bool filebrowser_global_paste(void) {
    if (!g_fb || !g_fb->window || !g_fb->window->focused) return false;
    fb_btn_paste_cb(g_fb->window, 0);
    return true;
}
