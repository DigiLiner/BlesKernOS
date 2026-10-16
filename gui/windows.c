#include "../kernel/include/vision_resources.h"
#include "../sdk/include/bleskernos_theme.h"
#include "gui.h"
#include "../kernel/include/memory.h"
#include "../kernel/include/about_dialog.h"
#include "../kernel/include/task.h"
#include "../kernel/include/klock.h"
#include "../kernel/include/compat_mode.h"
#include "../kernel/include/system_sounds.h"


#define GUI_CONTENT_STAGING_POOL_SIZE 4

/* La geometria sigue siendo CDE/Motif, pero la superficie de ventanas y
 * controles usa el gris neutro clasico que Wine muestra en sus aplicaciones.
 * El acento turquesa de BlesKernOS se conserva en las barras de titulo activas. */
#define GUI_CDE_FACE BK_THEME_FACE
#define GUI_CDE_FACE_GRAIN BK_THEME_FACE_GRAIN
#define GUI_CDE_ACTIVE_TITLE BK_THEME_ACTIVE_TITLE
#define GUI_CDE_ACTIVE_GRAIN BK_THEME_ACTIVE_GRAIN
#define GUI_CDE_INACTIVE_TITLE BK_THEME_INACTIVE_TITLE
#define GUI_CDE_INACTIVE_GRAIN BK_THEME_INACTIVE_GRAIN
#define GUI_CDE_MENU BK_THEME_FACE
#define GUI_CDE_MENU_PRESSED BK_THEME_FACE_DOWN
#define GUI_CDE_HIGHLIGHT BK_THEME_LIGHT
#define GUI_CDE_MIDLIGHT BK_THEME_MIDLIGHT
#define GUI_CDE_SHADOW BK_THEME_SHADOW
#define GUI_CDE_DARK BK_THEME_DARK
#define GUI_CDE_TEXT BK_THEME_TEXT

/* Existing applications chose their outer dimensions for the former 2 px
 * frame and 20 px title bar. Inflate new decorated windows just enough to
 * preserve the client area instead of squeezing fixed-layout applets. */
#define GUI_LEGACY_BORDER_SIZE      2
#define GUI_LEGACY_TITLEBAR_HEIGHT 20
#define GUI_CHROME_COMPAT_W     ((GUI_BORDER_SIZE - GUI_LEGACY_BORDER_SIZE) * 2)
#define GUI_CHROME_COMPAT_H \
    ((GUI_TITLEBAR_HEIGHT - GUI_LEGACY_TITLEBAR_HEIGHT) + \
     (GUI_BORDER_SIZE - GUI_LEGACY_BORDER_SIZE))

#ifndef BLES_VISION_SERVICE
typedef struct {
    gui_surface_t surface;
    bool in_use;
    uint32_t capacity;
} gui_content_staging_slot_t;

static gui_content_staging_slot_t
    g_content_staging_pool[GUI_CONTENT_STAGING_POOL_SIZE];
static kspinlock_t g_content_staging_lock = KSPINLOCK_INITIALIZER;
#endif

/* Motif/CDE construia profundidad con dos pares de luces y sombras, no con
 * bordes planos.  Estas primitivas se usan sólo sobre marcos y controles
 * pequeños: agregan detalle sin volver a pintar el cliente ni encarecer el
 * movimiento de ventanas. */
static void cde_draw_bevel(gui_surface_t *surface, gui_rect_t bounds,
                           bool sunken, uint32_t fill, int depth) {
    uint32_t light_outer = BK_THEME_BEVEL_OUTER_TL(sunken);
    uint32_t light_inner = BK_THEME_BEVEL_INNER_TL(sunken);
    uint32_t dark_outer = BK_THEME_BEVEL_OUTER_BR(sunken);
    uint32_t dark_inner = BK_THEME_BEVEL_INNER_BR(sunken);

    if (!surface || bounds.w <= 0 || bounds.h <= 0) return;
    gui_gfx_fill_rect(surface, bounds, fill);
    if (depth < 1 || bounds.w < 3 || bounds.h < 3) return;

    gui_gfx_fill_rect(surface,
        (gui_rect_t){bounds.x, bounds.y, bounds.w, 1}, light_outer);
    gui_gfx_fill_rect(surface,
        (gui_rect_t){bounds.x, bounds.y, 1, bounds.h}, light_outer);
    gui_gfx_fill_rect(surface,
        (gui_rect_t){bounds.x, bounds.y + bounds.h - 1, bounds.w, 1}, dark_outer);
    gui_gfx_fill_rect(surface,
        (gui_rect_t){bounds.x + bounds.w - 1, bounds.y, 1, bounds.h}, dark_outer);

    if (depth > 1 && bounds.w >= 5 && bounds.h >= 5) {
        gui_gfx_fill_rect(surface,
            (gui_rect_t){bounds.x + 1, bounds.y + 1, bounds.w - 2, 1},
            light_inner);
        gui_gfx_fill_rect(surface,
            (gui_rect_t){bounds.x + 1, bounds.y + 1, 1, bounds.h - 2},
            light_inner);
        gui_gfx_fill_rect(surface,
            (gui_rect_t){bounds.x + 1, bounds.y + bounds.h - 2,
                         bounds.w - 2, 1}, dark_inner);
        gui_gfx_fill_rect(surface,
            (gui_rect_t){bounds.x + bounds.w - 2, bounds.y + 1,
                         1, bounds.h - 2}, dark_inner);
    }
}

static void cde_stipple_rect(gui_surface_t *surface, gui_rect_t bounds,
                             uint32_t color) {
    int y;
    if (!surface || bounds.w < 3 || bounds.h < 3) return;
    for (y = bounds.y + 1; y < bounds.y + bounds.h - 1; y += 3) {
        int x = bounds.x + 1 + ((y / 3) & 1);
        for (; x < bounds.x + bounds.w - 1; x += 4)
            gui_gfx_putpixel(surface, x, y, color);
    }
}

#ifndef BLES_VISION_SERVICE
static void content_staging_release_slot(int slot) {
    uint32_t flags;
    if (slot < 0 || slot >= GUI_CONTENT_STAGING_POOL_SIZE) return;
    flags = kspin_lock_irqsave(&g_content_staging_lock);
    g_content_staging_pool[slot].in_use = false;
    kspin_unlock_irqrestore(&g_content_staging_lock, flags);
}
#endif


#ifndef BLES_VISION_SERVICE
static uint32_t content_staging_slot_limit(void) {
    /* Cuatro superficies de 640x480x32 son 4.7 MiB retenidos. Con 16 MiB
       compiten con el escritorio, iconos y la memoria de la aplicación y
       dejan el heap fragmentado: una ventana de 512 KiB ya no puede abrir.
       Un staging reutilizable mantiene el primer callback funcional; los
       demás se reintentan cuando se libera, sin reservar pantallas enteras. */
    uint32_t top = mm_physical_top();
    if (top <= 16U * 1024U * 1024U) return 1U;
    /* 64 MiB leaves a ~20 MiB heap. Four 800x600 staging buffers plus
       client caches exhaust it when administration tools open together. */
    if (top <= 96U * 1024U * 1024U) return 1U;
    return GUI_CONTENT_STAGING_POOL_SIZE;
}
#endif


/* Machines with a small heap cannot safely retain full client surfaces while
 * the desktop, application heap and decoded assets are resident.  The direct
 * surface path is ABI-compatible with old applications (absolute pixels and
 * pitch) and needs no extra contiguous allocation. */
#ifndef BLES_VISION_SERVICE
static bool content_cache_uses_rgb332(void) {
    return compat_mode_is_low_memory() ||
           mm_physical_top() <= 96U * 1024U * 1024U;
}

#endif
#include "color_palette.h"

#ifndef BLES_VISION_SERVICE
static int content_staging_acquire(const gui_surface_t *source,
                                   gui_surface_t *view_out, const gui_rect_t *region) {
    gui_content_staging_slot_t *pool_slot;
    uint32_t pixel_count;
    uint32_t *pixels;
    uint32_t flags;
    int slot = -1;

    if (!source || !source->pixels || !view_out) return -1;
    flags = kspin_lock_irqsave(&g_content_staging_lock);
    for (uint32_t i = 0; i < content_staging_slot_limit(); i++) {
        if (g_content_staging_pool[i].in_use) continue;
        g_content_staging_pool[i].in_use = true;
        slot = (int)i;
        break;
    }
    kspin_unlock_irqrestore(&g_content_staging_lock, flags);
    if (slot < 0) return -1;

    pool_slot = &g_content_staging_pool[slot];
    uint32_t pitch=region?(uint32_t)region->w:source->pitch;
    pixel_count=pitch*(region?(uint32_t)region->h:source->height);
    if (!pool_slot->surface.pixels || pixel_count>pool_slot->capacity) {
        /* The slot is exclusive and its previous paint has finished. Release
           old storage before growing, avoiding a two-buffer allocation peak. */
        if(pool_slot->surface.pixels)kfree(pool_slot->surface.pixels);
        pool_slot->surface.pixels=NULL;pool_slot->capacity=0;
        pixels = (uint32_t *)kmalloc(pixel_count * sizeof(uint32_t));
        if (!pixels || !mm_set_allocation_owner(pixels, 0U)) {
            if (pixels) kfree(pixels);
            content_staging_release_slot(slot);
            return -1;
        }
        if (pool_slot->surface.pixels) kfree(pool_slot->surface.pixels);
        (void)mm_set_allocation_subsystem(pixels,MM_SUBSYSTEM_GUI);
        pool_slot->surface.pixels = pixels;
        pool_slot->capacity=pixel_count;
    }
    pool_slot->surface.width=source->width;pool_slot->surface.height=source->height;
    pool_slot->surface.pitch=pitch;pool_slot->surface.origin_x=region?region->x:0;
    pool_slot->surface.origin_y=region?region->y:0;
    pool_slot->surface.storage_width=region?region->w:source->width;
    pool_slot->surface.storage_height=region?region->h:source->height;
    pool_slot->surface.clip = (gui_rect_t){0, 0,
        source->width, source->height};
    *view_out = pool_slot->surface;
    return slot;
}
#endif


static uint32_t callback_owner(const void *callback) {
    uint32_t pid;

    if (!callback || (uint32_t)(uintptr_t)callback < HEAP_START ||
        !task_current_is_user()) return 0U;
    pid = task_current_pid();
    /*
     * Transitional SMP rule: native GUI callbacks still share mutable window,
     * widget and staging objects with the CPU0 compositor.  Keep only the
     * callback-owning thread on CPU0; worker threads from the same process
     * remain free to use APs for rendering, physics, networking, etc.
     */
    (void)task_set_affinity_mask(pid, 1U);
    return pid;
}

static bool queue_menu_callback(const gui_menu_item_t *item,
                                gui_window_t *window) {
    uint32_t arguments[3];
    if (!item || !item->callback) return false;
    if (!item->callback_pid) {
#ifdef BLES_VISION_SERVICE
        return false; /* No CPL0 callback may be jumped to from CPL3. */
#endif
        item->callback(window, item->id, item->context);
        return true;
    }
#ifdef BLES_VISION_SERVICE
    if(item->callback_pid==task_current_pid()) {
        item->callback(window,item->id,item->context);return true;
    }
#endif
    arguments[0] = (uint32_t)(uintptr_t)window;
    arguments[1] = item->id;
    arguments[2] = (uint32_t)(uintptr_t)item->context;
    return task_queue_window_upcall(window, item->callback_pid,
        (uint32_t)(uintptr_t)item->callback, arguments, 3, NULL, 0, -1);
}

static void copy_text(char *dst, size_t dst_len, const char *src) {
    if (!dst || !dst_len) return;
    if (!src) src = "";
    kstrncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

gui_window_t *gui_window_create(gui_desktop_t *desktop, int x, int y, int w, int h, const char *title) {
    gui_window_t *window;
    int available_w;
    int available_h;
    if (!desktop) return NULL;
    available_w = (int)desktop->surface.width - (x > 0 ? x : 0);
    available_h = (int)desktop->surface.height - 24 - (y > 0 ? y : 0);
    if (w > 0 && w + GUI_CHROME_COMPAT_W <= available_w)
        w += GUI_CHROME_COMPAT_W;
    if (h > 0 && h + GUI_CHROME_COMPAT_H <= available_h)
        h += GUI_CHROME_COMPAT_H;
    window = (gui_window_t *)kzalloc(sizeof(gui_window_t));
    if (!window) return NULL;
    if (!mm_set_allocation_owner(window, 0U)) {
        kfree(window);
        return NULL;
    }

    window->id = desktop->next_window_id++;
    window->desktop = desktop;
    window->bounds = (gui_rect_t){x, y, w, h};
    /* Fixed-layout native programs historically received a generic 160x90
       minimum even when their controls were authored for a much larger
       client.  Shrinking such a window made gui_window_clamp_local_rect()
       pull several controls onto the same edge.  Treat the authored outer
       size as the safe floor; modern/extended clients may opt out below. */
    window->layout_authored_w = w > 0 ? w : 160;
    window->layout_authored_h = h > 0 ? h : 90;
    window->layout_floor_w = window->layout_authored_w;
    window->layout_floor_h = window->layout_authored_h;
    window->layout_guard_enabled = true;
    window->layout_min_explicit = false;
    window->min_w = window->layout_floor_w;
    window->min_h = window->layout_floor_h;
    window->bg_color = GUI_CDE_FACE;
    window->title_color = GUI_CDE_ACTIVE_TITLE;
    window->border_color = GUI_CDE_DARK;
    window->visible = true;
    window->listed = true;
    window->minimized = false;
    window->input_enabled = true;
    window->borderless = false;
    window->resizable = true;
    window->drag_height = 0;
    window->dirty = true;
    window->open_menu = -1;
    window->pressed_menu_item = -1;
    window->context_menu.pressed_item = -1;
    window->content_staging_slot = -1;
    copy_text(window->title, sizeof(window->title), title);
    if(!bk_vision_resource_register(window,false)){kfree(window);return NULL;}
    (void)system_sound_event_play(SYSTEM_SOUND_WINDOW_OPEN);
    return window;
}



int gui_window_content_top(const gui_window_t *window) {
    return ((window && window->borderless) ? 0 : GUI_TITLEBAR_HEIGHT) +
           ((window && window->menu_count) ? GUI_MENU_HEIGHT : 0);
}

gui_rect_t gui_window_content_rect(const gui_window_t *window) {
    gui_rect_t rect;
    int top;

    if (!window) return (gui_rect_t){0, 0, 0, 0};
    top = gui_window_content_top(window);
    int border = window->borderless ? 0 : GUI_BORDER_SIZE;
    rect = (gui_rect_t){
        window->bounds.x + border,
        window->bounds.y + top,
        window->bounds.w - border * 2,
        window->bounds.h - top - border
    };
    if (rect.w < 0) rect.w = 0;
    if (rect.h < 0) rect.h = 0;
    return rect;
}

gui_rect_t gui_window_content_rect_inset(const gui_window_t *window, int inset) {
    gui_rect_t rect = gui_window_content_rect(window);
    if (inset < 0) inset = 0;
    rect.x += inset;
    rect.y += inset;
    rect.w -= inset * 2;
    rect.h -= inset * 2;
    if (rect.w < 0) rect.w = 0;
    if (rect.h < 0) rect.h = 0;
    return rect;
}

gui_rect_t gui_window_clamp_local_rect(const gui_window_t *window,
                                       gui_rect_t rect) {
    gui_rect_t content = gui_window_content_rect(window);
    int local_w = content.w;
    int local_h = content.h;

    if (rect.w < 0) rect.w = 0;
    if (rect.h < 0) rect.h = 0;
    if (rect.w > local_w) rect.w = local_w;
    if (rect.h > local_h) rect.h = local_h;
    if (rect.x < 0) rect.x = 0;
    if (rect.y < 0) rect.y = 0;
    if (rect.x + rect.w > local_w) rect.x = local_w - rect.w;
    if (rect.y + rect.h > local_h) rect.y = local_h - rect.h;
    if (rect.x < 0) rect.x = 0;
    if (rect.y < 0) rect.y = 0;
    return rect;
}

int gui_window_add_menu(gui_window_t *window, const char *label) {
    if (!window || window->menu_count >= GUI_MAX_MENUS) return -1;
    int index = window->menu_count++;
    kstrncpy(window->menus[index].label, label ? label : "",
             sizeof(window->menus[index].label) - 1);
    /* A menu consumes client height.  Re-evaluate widget-derived limits so a
       late-added Help menu can never push the bottom row out of the window. */
    gui_window_refresh_layout_limits(window);
    return index;
}

bool gui_window_add_menu_item(gui_window_t *window, int menu,
                              uint32_t id, const char *label,
                              gui_menu_callback_t callback, void *context) {
    if (!window || menu < 0 || menu >= window->menu_count) return false;
    gui_menu_t *target = &window->menus[menu];
    if (target->item_count >= GUI_MAX_MENU_ITEMS) return false;
    gui_menu_item_t *item = &target->items[target->item_count++];
    item->id = id;
    item->enabled = true;
    item->callback = callback;
    item->context = context;
    item->callback_pid = callback_owner((const void *)(uintptr_t)callback);
    kstrncpy(item->label, label ? label : "", sizeof(item->label) - 1);
    return true;
}

static gui_rect_t menu_title_rect(gui_window_t *window, int index) {
    int x = window->bounds.x + GUI_BORDER_SIZE;
    for (int i = 0; i < index; i++)
        x += (int)gui_font_text_width(window->menus[i].label) + 12;
    return (gui_rect_t){x, window->bounds.y + GUI_TITLEBAR_HEIGHT,
                        (int)gui_font_text_width(window->menus[index].label) + 12,
                        GUI_MENU_HEIGHT};
}

static gui_rect_t menu_popup_rect(gui_window_t *window, int index) {
    gui_rect_t title = menu_title_rect(window, index);
    int width = 80;
    gui_menu_t *menu = &window->menus[index];
    for (int i = 0; i < menu->item_count; i++) {
        int candidate = (int)gui_font_text_width(menu->items[i].label) + 20;
        if (candidate > width) width = candidate;
    }
    return (gui_rect_t){title.x, title.y + title.h, width,
                        menu->item_count * 18 + 4};
}

static gui_rect_t context_popup_rect(const gui_window_t *window) {
    const gui_context_menu_t *menu = &window->context_menu;
    return (gui_rect_t){menu->x, menu->y, menu->width,
                        menu->item_count * 22 + 4};
}

static gui_rect_t window_menu_bar_rect(const gui_window_t *window) {
    if (!window) return (gui_rect_t){0, 0, 0, 0};
    return (gui_rect_t){window->bounds.x + GUI_BORDER_SIZE,
                        window->bounds.y + GUI_TITLEBAR_HEIGHT,
                        window->bounds.w - GUI_BORDER_SIZE * 2,
                        GUI_MENU_HEIGHT};
}

static void window_invalidate_menu_rect(gui_window_t *window,
                                        gui_rect_t rect) {
    if (!window || !window->desktop || rect.w <= 0 || rect.h <= 0) return;
    gui_desktop_invalidate_rect(window->desktop, rect);
    gui_request_paint();
}

static gui_rect_t generic_context_rect(const gui_context_menu_t *menu) {
    if (!menu) return (gui_rect_t){0, 0, 0, 0};
    return (gui_rect_t){menu->x, menu->y, menu->width,
                        menu->item_count * 22 + 4};
}

void gui_context_menu_clear(gui_context_menu_t *menu) {
    if (!menu) return;
    menu->item_count = 0;
    menu->open = false;
    menu->width = 100;
    menu->pressed_item = -1;
}

bool gui_context_menu_add_item(gui_context_menu_t *menu, uint32_t id,
                               const char *label, bool enabled,
                               gui_menu_callback_t callback, void *context) {
    gui_menu_item_t *item;
    int width;
    if (!menu || menu->item_count >= GUI_MAX_CONTEXT_ITEMS) return false;
    item = &menu->items[menu->item_count++];
    item->id = id;
    item->enabled = enabled;
    item->callback = callback;
    item->context = context;
    item->callback_pid = callback_owner((const void *)(uintptr_t)callback);
    copy_text(item->label, sizeof(item->label), label);
    width = (int)gui_font_text_width(item->label) + 28;
    if (width > menu->width) menu->width = width;
    return true;
}

void gui_context_menu_open(gui_context_menu_t *menu, int x, int y,
                           gui_rect_t limits) {
    int height;
    if (!menu || !menu->item_count) return;
    height = menu->item_count * 22 + 4;
    if (x + menu->width > limits.x + limits.w)
        x = limits.x + limits.w - menu->width;
    if (y + height > limits.y + limits.h)
        y = limits.y + limits.h - height;
    if (x < limits.x) x = limits.x;
    if (y < limits.y) y = limits.y;
    menu->x = x;
    menu->y = y;
    menu->pressed_item = -1;
    menu->open = true;
}

void gui_context_menu_close(gui_context_menu_t *menu) {
    if (!menu) return;
    menu->open = false;
    menu->pressed_item = -1;
}

void gui_context_menu_paint(gui_surface_t *surface,
                            const gui_context_menu_t *menu) {
    gui_rect_t popup;
    int i;
    if (!surface || !menu || !menu->open) return;
    popup = generic_context_rect(menu);
    cde_draw_bevel(surface, popup, false, GUI_CDE_FACE, 2);
    for (i = 0; i < menu->item_count; i++) {
        gui_rect_t item_rect = {popup.x + 3, popup.y + 3 + i * 22,
                                popup.w - 6, 21};
        const gui_menu_item_t *item = &menu->items[i];
        if (menu->pressed_item == i && item->enabled)
            cde_draw_bevel(surface, item_rect, true,
                           GUI_CDE_MENU_PRESSED, 1);
        gui_font_draw_string_clipped(surface, item_rect.x + 8,
                                     item_rect.y + 6, item->label,
                                     item->enabled
                                         ? (menu->pressed_item == i
                                                ? 0x00FFFFFFU
                                                : GUI_CDE_TEXT)
                                         : 0x00776F6BU,
                                     item_rect);
    }
}

bool gui_context_menu_handle_event(gui_context_menu_t *menu,
                                   gui_window_t *callback_window,
                                   const gui_event_t *event) {
    gui_rect_t popup;
    if (!menu || !event || !menu->open) return false;
    popup = generic_context_rect(menu);
    if (event->type == GUI_EVENT_MOUSE_DOWN) {
        if (event->button == 1 && gui_rect_contains(popup, event->x, event->y)) {
            int item = (event->y - popup.y - 2) / 22;
            if (item >= 0 && item < menu->item_count && menu->items[item].enabled)
                menu->pressed_item = item;
            return true;
        }
        gui_context_menu_close(menu);
        return false;
    }
    if (event->type == GUI_EVENT_MOUSE_UP && menu->pressed_item >= 0) {
        gui_menu_item_t item = menu->items[menu->pressed_item];
        gui_context_menu_close(menu);
        if (item.enabled && item.callback)
            (void)queue_menu_callback(&item, callback_window);
        return true;
    }
    return true;
}

void gui_window_context_clear(gui_window_t *window) {
    if (!window) return;
    gui_context_menu_clear(&window->context_menu);
}

bool gui_window_context_add_item(gui_window_t *window, uint32_t id,
                                 const char *label, bool enabled,
                                 gui_menu_callback_t callback, void *context) {
    if (!window) return false;
    return gui_context_menu_add_item(&window->context_menu, id, label,
                                     enabled, callback, context);
}

void gui_window_context_open(gui_window_t *window, int x, int y) {
    gui_context_menu_t *menu;
    int height;
    if (!window || !window->context_menu.item_count) return;
    menu = &window->context_menu;
    height = menu->item_count * 22 + 4;
    if (x + menu->width > window->bounds.x + window->bounds.w)
        x = window->bounds.x + window->bounds.w - menu->width;
    if (y + height > window->bounds.y + window->bounds.h)
        y = window->bounds.y + window->bounds.h - height;
    if (x < window->bounds.x) x = window->bounds.x;
    if (y < window->bounds.y) y = window->bounds.y;
    menu->x = x;
    menu->y = y;
    menu->pressed_item = -1;
    menu->open = true;
    window->open_menu = -1;
    window_invalidate_menu_rect(window, context_popup_rect(window));
}

void gui_window_context_close(gui_window_t *window) {
    gui_rect_t old_popup;
    if (!window) return;
    old_popup = context_popup_rect(window);
    gui_context_menu_close(&window->context_menu);
    window_invalidate_menu_rect(window, old_popup);
}

void gui_window_set_text_context(gui_window_t *window, gui_rect_t bounds,
                                 bool has_selection, bool editable,
                                 gui_menu_callback_t callback, void *context) {
    if (!window) return;
    window->text_context.bounds = gui_window_clamp_local_rect(window, bounds);
    window->text_context.has_selection = has_selection;
    window->text_context.editable = editable;
    window->text_context.callback = callback;
    window->text_context.context = context;
    window->text_context.callback_pid =
        callback_owner((const void *)(uintptr_t)callback);
}

void gui_window_clear_text_context(gui_window_t *window) {
    if (!window) return;
    window->text_context.bounds = (gui_rect_t){0, 0, 0, 0};
    window->text_context.has_selection = false;
    window->text_context.editable = false;
    window->text_context.callback = NULL;
    window->text_context.context = NULL;
    window->text_context.callback_pid = 0U;
}

static bool gui_window_open_custom_text_context(gui_window_t *window,
                                                int x, int y) {
    gui_text_context_t *text;
    gui_rect_t content;
    gui_rect_t screen_bounds;
    gui_context_menu_t *menu;

    if (!window) return false;
    text = &window->text_context;
    if (!text->has_selection || !text->callback ||
        text->bounds.w <= 0 || text->bounds.h <= 0) return false;
    content = gui_window_content_rect(window);
    screen_bounds = (gui_rect_t){content.x + text->bounds.x,
                                 content.y + text->bounds.y,
                                 text->bounds.w, text->bounds.h};
    if (!gui_rect_contains(screen_bounds, x, y)) return false;

    gui_window_context_clear(window);
    menu = &window->context_menu;
    if (gui_context_menu_add_item(menu, GUI_TEXT_ACTION_COPY, "Copiar", true,
                                  text->callback, text->context))
        menu->items[menu->item_count - 1U].callback_pid = text->callback_pid;
    if (text->editable &&
        gui_context_menu_add_item(menu, GUI_TEXT_ACTION_PASTE, "Pegar", true,
                                  text->callback, text->context))
        menu->items[menu->item_count - 1U].callback_pid = text->callback_pid;
    if (!menu->item_count) return false;
    gui_window_context_open(window, x, y);
    return true;
}

bool gui_window_open_text_context_at(gui_window_t *window, int x, int y) {
    if (!window) return false;
    if (gui_window_open_custom_text_context(window, x, y)) return true;
    return gui_widget_open_text_context_at(window, x, y);
}

void gui_window_paint_menus(gui_surface_t *surface, gui_window_t *window) {
    gui_rect_t bar;
    uint32_t bar_bg;
    uint32_t bar_grain;
    uint32_t bar_text;
    int i;
    if (!surface || !window || !window->visible) return;
    bar = (gui_rect_t){window->bounds.x + GUI_BORDER_SIZE,
                      window->bounds.y + GUI_TITLEBAR_HEIGHT,
                      window->bounds.w - GUI_BORDER_SIZE * 2,
                      GUI_MENU_HEIGHT};
    bar_bg = window->focused ? GUI_CDE_MENU : GUI_CDE_INACTIVE_TITLE;
    bar_grain = window->focused ? GUI_CDE_FACE_GRAIN
                                : GUI_CDE_INACTIVE_GRAIN;
    bar_text = (((bar_bg >> 16) & 0xFFU) +
                ((bar_bg >> 8) & 0xFFU) +
                (bar_bg & 0xFFU)) < 400U ? 0x00FFFFFFU : GUI_CDE_TEXT;
    if (window->menu_count) {
        cde_draw_bevel(surface, bar, false, bar_bg, 1);
        cde_stipple_rect(surface, bar, bar_grain);
        for (i = 0; i < window->menu_count; i++) {
            gui_rect_t title = menu_title_rect(window, i);
            if (window->open_menu == i)
                cde_draw_bevel(surface, title, true,
                               GUI_CDE_MENU_PRESSED, 1);
            gui_font_draw_string_clipped(surface, title.x + 6, title.y + 6,
                                         window->menus[i].label,
                                         window->open_menu == i
                                             ? 0x00FFFFFFU : bar_text,
                                         title);
        }
    }
    if (window->open_menu >= 0) {
        int index = window->open_menu;
        gui_menu_t *menu = &window->menus[index];
        gui_rect_t popup = menu_popup_rect(window, index);
        cde_draw_bevel(surface, popup, false, GUI_CDE_FACE, 2);
        for (i = 0; i < menu->item_count; i++) {
            gui_rect_t item = {popup.x + 3, popup.y + 3 + i * 18,
                               popup.w - 6, 17};
            if (window->pressed_menu_item == i)
                cde_draw_bevel(surface, item, true,
                               GUI_CDE_MENU_PRESSED, 1);
            gui_font_draw_string_clipped(surface, item.x + 6, item.y + 4,
                                         menu->items[i].label,
                                         window->pressed_menu_item == i
                                             ? 0x00FFFFFFU : GUI_CDE_TEXT,
                                         item);
        }
    }
    if (window->context_menu.open) {
        gui_context_menu_t *menu = &window->context_menu;
        gui_rect_t popup = context_popup_rect(window);
        cde_draw_bevel(surface, popup, false, GUI_CDE_FACE, 2);
        for (i = 0; i < menu->item_count; i++) {
            gui_rect_t item_rect = {popup.x + 3, popup.y + 3 + i * 22,
                                    popup.w - 6, 21};
            gui_menu_item_t *item = &menu->items[i];
            if (menu->pressed_item == i && item->enabled)
                cde_draw_bevel(surface, item_rect, true,
                               GUI_CDE_MENU_PRESSED, 1);
            gui_font_draw_string_clipped(surface, item_rect.x + 8,
                                         item_rect.y + 6, item->label,
                                         item->enabled
                                             ? (menu->pressed_item == i
                                                    ? 0x00FFFFFFU
                                                    : GUI_CDE_TEXT)
                                             : 0x00776F6BU,
                                         item_rect);
        }
    }
}

bool gui_window_handle_menu_event(gui_window_t *window,
                                  const gui_event_t *event) {
    if (!window || !event) return false;
    if (window->context_menu.open) {
        gui_context_menu_t *menu = &window->context_menu;
        gui_rect_t popup = context_popup_rect(window);
        if (event->type == GUI_EVENT_MOUSE_DOWN) {
            if (event->button == 1 &&
                gui_rect_contains(popup, event->x, event->y)) {
                int item = (event->y - popup.y - 2) / 22;
                if (item >= 0 && item < menu->item_count &&
                    menu->items[item].enabled) {
                    menu->pressed_item = item;
                    window_invalidate_menu_rect(window, popup);
                }
                return true;
            }
            gui_window_context_close(window);
        }
        if (event->type == GUI_EVENT_MOUSE_UP && menu->pressed_item >= 0) {
            int index = menu->pressed_item;
            gui_menu_item_t item = menu->items[index];
            gui_window_context_close(window);
            if (item.enabled && item.callback)
                (void)queue_menu_callback(&item, window);
            return true;
        }
        if (menu->open) return true;
    }
    if (!window->menu_count) return false;
    if (event->type == GUI_EVENT_MOUSE_DOWN) {
        for (int i = 0; i < window->menu_count; i++) {
            if (gui_rect_contains(menu_title_rect(window, i),
                                  event->x, event->y)) {
                int old_menu = window->open_menu;
                window->open_menu = window->open_menu == i ? -1 : i;
                window->pressed_menu_item = -1;
                window_invalidate_menu_rect(window,
                                            window_menu_bar_rect(window));
                if (old_menu >= 0)
                    window_invalidate_menu_rect(window,
                                                menu_popup_rect(window,
                                                                old_menu));
                if (window->open_menu >= 0)
                    window_invalidate_menu_rect(window,
                                                menu_popup_rect(window,
                                                    window->open_menu));
                return true;
            }
        }
        if (window->open_menu >= 0) {
            gui_rect_t popup = menu_popup_rect(window, window->open_menu);
            if (gui_rect_contains(popup, event->x, event->y)) {
                int item = (event->y - popup.y - 2) / 18;
                if (item >= 0 &&
                    item < window->menus[window->open_menu].item_count) {
                    window->pressed_menu_item = item;
                    window_invalidate_menu_rect(window, popup);
                    return true;
                }
            }
            window_invalidate_menu_rect(window, popup);
            window_invalidate_menu_rect(window, window_menu_bar_rect(window));
            window->open_menu = -1;
        }
    }
    if (event->type == GUI_EVENT_MOUSE_UP && window->open_menu >= 0 &&
        window->pressed_menu_item >= 0) {
        int menu_index = window->open_menu;
        int item_index = window->pressed_menu_item;
        gui_menu_item_t item = window->menus[menu_index].items[item_index];
        gui_rect_t popup = menu_popup_rect(window, menu_index);
        window->open_menu = -1;
        window->pressed_menu_item = -1;
        window_invalidate_menu_rect(window, popup);
        window_invalidate_menu_rect(window, window_menu_bar_rect(window));
        if (item.callback) (void)queue_menu_callback(&item, window);
        return true;
    }
    return window->open_menu >= 0;
}

#ifndef BLES_VISION_SERVICE
void gui_window_destroy(gui_window_t *window) {
    gui_widget_t *widget;
    uint32_t state;
    if (!window) return;

    /* A worker may retire a window while the GUI task is painting or dispatching
       an event against it.  The desktop lock is recursive, so GUI callbacks can
       safely destroy their own windows while an event transaction owns it. */
    gui_desktop_paint_lock();

    /* Several callbacks for the same Ring-3 window may finish on different
       CPUs. The old boolean allowed both CPUs to observe "ready to free" and
       destroy the same widget chain twice. State 1 is a tombstone; exactly one
       CPU may atomically promote it to state 2 and own finalization. */
    state = __sync_val_compare_and_swap(&window->destroy_state, 0U, 1U);
    if (state >= 2U) {
        gui_desktop_paint_unlock();
        return;
    }
    window->visible = false;
    window->listed = false;
    window->input_enabled = false;

    /* Remove queued callbacks first. An active callback keeps state 1 and
       invokes this function again after its return gate has restored the user
       context. New callbacks are rejected while state is non-zero. */
    if (!task_cancel_window_upcalls(window)) {
        gui_desktop_paint_unlock();
        return;
    }
    if (!__sync_bool_compare_and_swap(&window->destroy_state, 1U, 2U)) {
        gui_desktop_paint_unlock();
        return;
    }

    gui_window_end_content_paint(window);
    bk_about_detach(window);
    gui_gpu_compositor_forget_window(window);
    widget = window->widgets;
    window->widgets = NULL;
    window->focused_widget = NULL;
    while (widget) {
        gui_widget_t *next = widget->next;
        gui_widget_destroy(widget);
        widget = next;
    }
    if (window->content_cache) {
        kfree(window->content_cache);
        window->content_cache = NULL;
    }
    if (window->stream_pixels) {
        kfree(window->stream_pixels);
        window->stream_pixels = NULL;
    }
    /* content_staging borrows storage from the global pool and is released
       by gui_window_end_content_paint(). */
    bk_vision_resource_forget(window);
    kfree(window);
    gui_desktop_paint_unlock();
}
#endif


bool gui_window_contains(gui_window_t *window, int x, int y) {
    return window && window->visible && gui_rect_contains(window->bounds, x, y);
}

bool gui_window_titlebar_contains(gui_window_t *window, int x, int y) {
    gui_rect_t titlebar;
    if (!gui_window_contains(window, x, y)) return false;
    titlebar = (gui_rect_t){window->bounds.x, window->bounds.y,
                            window->bounds.w,
                            window->borderless ? window->drag_height
                                               : GUI_TITLEBAR_HEIGHT};
    return gui_rect_contains(titlebar, x, y);
}

gui_rect_t gui_window_minimize_button_rect(gui_window_t *window) {
    if (!window) return (gui_rect_t){0, 0, 0, 0};
    return (gui_rect_t){window->bounds.x + window->bounds.w -
                            GUI_BORDER_SIZE - 34,
                        window->bounds.y + 4, 16, 16};
}

gui_rect_t gui_window_close_button_rect(gui_window_t *window) {
    if (!window) return (gui_rect_t){0, 0, 0, 0};
    return (gui_rect_t){window->bounds.x + window->bounds.w -
                            GUI_BORDER_SIZE - 16,
                        window->bounds.y + 4, 16, 16};
}

gui_window_button_t gui_window_titlebar_button_at(gui_window_t *window,
                                                   int x, int y) {
    if (!window || !window->visible) return GUI_WINDOW_BUTTON_NONE;
    if (window->borderless || gui_setup_mode()) return GUI_WINDOW_BUTTON_NONE;
    if (gui_rect_contains(gui_window_close_button_rect(window), x, y))
        return GUI_WINDOW_BUTTON_CLOSE;
    if (gui_rect_contains(gui_window_minimize_button_rect(window), x, y))
        return GUI_WINDOW_BUTTON_MINIMIZE;
    return GUI_WINDOW_BUTTON_NONE;
}

void gui_window_minimize(gui_window_t *window) {
    if (window) {
        (void)system_sound_event_play(SYSTEM_SOUND_WINDOW_MINIMIZE);
        window->dirty = true;
        window->visible = false;
        window->listed = true;
        window->minimized = true;
        gui_desktop_window_hidden(window->desktop, window);
    }
}

void gui_window_close(gui_window_t *window) {
    if (window) {
        (void)system_sound_event_play(SYSTEM_SOUND_WINDOW_CLOSE);
        window->dirty = true;
        window->visible = false;
        window->listed = false;
        window->minimized = false;
        gui_desktop_window_hidden(window->desktop, window);
    }
}

void gui_window_restore(gui_window_t *window) {
    if (window) {
        window->visible = true;
        window->listed = true;
        window->minimized = false;
        window->dirty = true;
        if (window->desktop)
            gui_desktop_shell_window_state_changed(window->desktop);
    }
}

/* Keep a native fixed-layout window large enough for every widget without
 * inventing per-application resize code.  The important distinction from the
 * old clamp-only behaviour is that the WINDOW grows (or refuses to shrink)
 * instead of moving several controls onto the same right/bottom edge. */
static void gui_window_fit_layout_to_workarea(gui_window_t *window) {
    int work_w;
    int work_h;
    if (!window || !window->desktop) return;
    work_w = (int)window->desktop->surface.width;
    work_h = (int)window->desktop->surface.height - 72;
    if (work_w < 1) work_w = 1;
    if (work_h < 1) work_h = 1;

    if (window->bounds.w > work_w) window->bounds.w = work_w;
    if (window->bounds.h > work_h) window->bounds.h = work_h;
    if (window->bounds.x + window->bounds.w > work_w)
        window->bounds.x = work_w - window->bounds.w;
    if (window->bounds.y + window->bounds.h > work_h)
        window->bounds.y = work_h - window->bounds.h;
    if (window->bounds.x < 0) window->bounds.x = 0;
    if (window->bounds.y < 0) window->bounds.y = 0;
}

void gui_window_refresh_layout_limits(gui_window_t *window) {
    gui_widget_t *widget;
    int min_w;
    int min_h;
    int border;
    int top;
    int max_right = 0;
    int max_bottom = 0;

    if (!window) return;

    /* Native applications author their control sizes and spacing. Growing a
       button here depends on creation order and makes later neighbours overlap.
       Derive window limits from those bounds; text painting already clips. */

    if (window->layout_min_explicit) {
        min_w = window->layout_floor_w;
        min_h = window->layout_floor_h;
    } else if (window->layout_guard_enabled) {
        min_w = window->layout_authored_w;
        min_h = window->layout_authored_h;
    } else {
        min_w = 160;
        min_h = 90;
    }

    /* Extended-event clients (SDL/X11/Linux compatibility) own their resize
       layout.  Do not derive a larger minimum from their widget coordinates. */
    if (window->layout_guard_enabled) {
        for (widget = window->widgets; widget; widget = widget->next) {
            int right = widget->bounds.x + widget->bounds.w;
            int bottom = widget->bounds.y + widget->bounds.h;
            if (right > max_right) max_right = right;
            if (bottom > max_bottom) max_bottom = bottom;
        }
        border = window->borderless ? 0 : GUI_BORDER_SIZE;
        top = gui_window_content_top(window);
        if (max_right > 0) {
            int widget_min_w = max_right + 8 + border * 2;
            if (widget_min_w > min_w) min_w = widget_min_w;
        }
        if (max_bottom > 0) {
            int widget_min_h = max_bottom + 8 + top + border;
            if (widget_min_h > min_h) min_h = widget_min_h;
        }
    }

    if (min_w < 80) min_w = 80;
    if (!window->borderless && min_h < GUI_TITLEBAR_HEIGHT + 30)
        min_h = GUI_TITLEBAR_HEIGHT + 30;

    /* A minimum bigger than the physical work area is impossible to honour.
       Clamp the constraint itself, not individual controls, so controls keep
       their authored order and merely clip on genuinely tiny displays. */
    if (window->desktop) {
        int work_w = (int)window->desktop->surface.width;
        int work_h = (int)window->desktop->surface.height - 72;
        if (work_w > 0 && min_w > work_w) min_w = work_w;
        if (work_h > 0 && min_h > work_h) min_h = work_h;
    }

    window->min_w = min_w;
    window->min_h = min_h;
    if (window->bounds.w < window->min_w) window->bounds.w = window->min_w;
    if (window->bounds.h < window->min_h) window->bounds.h = window->min_h;
    gui_window_fit_layout_to_workarea(window);
    window->dirty = true;
}

void gui_window_set_min_size(gui_window_t *window, int min_w, int min_h) {
    if (!window) return;
    gui_desktop_paint_lock();
    if (!window->borderless) {
        min_w += GUI_CHROME_COMPAT_W;
        min_h += GUI_CHROME_COMPAT_H;
    }
    if (min_w < 80) min_w = 80;
    if (!window->borderless && min_h < GUI_TITLEBAR_HEIGHT + 30)
        min_h = GUI_TITLEBAR_HEIGHT + 30;
    /* An explicit application minimum replaces the conservative authored
       floor, while the global guard may still raise it to fit real widgets. */
    window->layout_floor_w = min_w;
    window->layout_floor_h = min_h;
    window->layout_min_explicit = true;
    gui_window_refresh_layout_limits(window);
    gui_desktop_paint_unlock();
}

void gui_window_set_borderless(gui_window_t *window, bool borderless,
                               uint8_t drag_height) {
    if (!window) return;
    if (borderless != window->borderless) {
        /* Undo/apply the compatibility inflation when a program deliberately
           opts out of normal CDE decorations after creating the window. */
        if (borderless) {
            if (window->bounds.w > GUI_CHROME_COMPAT_W)
                window->bounds.w -= GUI_CHROME_COMPAT_W;
            if (window->bounds.h > GUI_CHROME_COMPAT_H)
                window->bounds.h -= GUI_CHROME_COMPAT_H;
            if (window->layout_authored_w > GUI_CHROME_COMPAT_W)
                window->layout_authored_w -= GUI_CHROME_COMPAT_W;
            if (window->layout_authored_h > GUI_CHROME_COMPAT_H)
                window->layout_authored_h -= GUI_CHROME_COMPAT_H;
            if (window->layout_floor_w > GUI_CHROME_COMPAT_W)
                window->layout_floor_w -= GUI_CHROME_COMPAT_W;
            if (window->layout_floor_h > GUI_CHROME_COMPAT_H)
                window->layout_floor_h -= GUI_CHROME_COMPAT_H;
        } else {
            window->bounds.w += GUI_CHROME_COMPAT_W;
            window->bounds.h += GUI_CHROME_COMPAT_H;
            window->layout_authored_w += GUI_CHROME_COMPAT_W;
            window->layout_authored_h += GUI_CHROME_COMPAT_H;
            window->layout_floor_w += GUI_CHROME_COMPAT_W;
            window->layout_floor_h += GUI_CHROME_COMPAT_H;
        }
    }
    window->borderless = borderless;
    window->drag_height = borderless ? drag_height : 0;
    window->resizable = !borderless;
    gui_window_refresh_layout_limits(window);
    window->dirty = true;
}

void gui_window_set_content(gui_window_t *window,
                            gui_window_content_paint_t paint,
                            void *context) {
    if (!window) return;
    gui_desktop_paint_lock();
    window->content_paint = paint;
    window->content_context = context;
    window->content_pid = callback_owner((const void *)(uintptr_t)paint);
    window->content_ready = false;
    window->dirty = true;
    gui_desktop_paint_unlock();
}

#ifndef BLES_VISION_SERVICE
bool gui_window_capture_content(gui_window_t *window,
                                const gui_surface_t *surface) {
    gui_rect_t content;
    gui_rect_t screen;
    gui_rect_t source;
    uint32_t pixels;
    uint32_t *cache;
    bool rgb332;

    if (!window || !surface || !surface->pixels) return false;
    content = gui_window_content_rect(window);
    if (window->content_staging_active &&
        (content.x != window->content_staging_rect.x ||
         content.y != window->content_staging_rect.y ||
         content.w != window->content_staging_rect.w ||
         content.h != window->content_staging_rect.h))
        return false;
    /* Slot -3 painted the desktop surface itself. Continue below and retain
       a compact RGB332 copy; returning here used to make the next compositor
       pass erase the client permanently. */
    if (compat_mode_is_low_memory() &&
        window->content_staging_slot == -2 &&
        surface->pixels == window->content_cache &&
        surface->origin_x == content.x && surface->origin_y == content.y &&
        surface->storage_width == window->content_cache_width &&
        surface->storage_height == window->content_cache_height) {
        window->content_ready = true;
        return true;
    }
    screen = (gui_rect_t){0, 0, surface->width, surface->height};
    if (!gui_rect_intersect(content, screen, &source) ||
        source.w <= 0 || source.h <= 0) return false;

    pixels = (uint32_t)source.w * (uint32_t)source.h;
    rgb332 = content_cache_uses_rgb332();
    if (!window->content_cache ||
        window->content_cache_width != (uint16_t)source.w ||
        window->content_cache_height != (uint16_t)source.h ||
        window->content_cache_rgb332 != rgb332) {
        cache = (uint32_t *)kmalloc(pixels *
            (rgb332 ? sizeof(uint8_t) : sizeof(uint32_t)));
        if (!cache) return false;
        if (!mm_set_allocation_owner(cache, 0U)) {
            kfree(cache);
            return false;
        }
        if (window->content_cache) kfree(window->content_cache);
        window->content_cache = cache;
        window->content_cache_width = (uint16_t)source.w;
        window->content_cache_height = (uint16_t)source.h;
        window->content_cache_rgb332 = rgb332;
    }

    for (int y = 0; y < source.h; y++) {
        const uint32_t *src = &surface->pixels[
            (uint32_t)(source.y + y - surface->origin_y) * surface->pitch + (uint32_t)(source.x - surface->origin_x)];
        if (rgb332) {
            uint8_t *dst = (uint8_t *)(void *)window->content_cache +
                           (uint32_t)y * source.w;
            for (int x = 0; x < source.w; x++)
                dst[x] = content_rgb888_to_332(src[x]);
        } else {
            kmemcpy(&window->content_cache[(uint32_t)y * source.w], src,
                    (size_t)source.w * sizeof(uint32_t));
        }
    }
    window->content_ready = true;
    return true;
}
#endif


#ifndef BLES_VISION_SERVICE
bool gui_window_begin_content_paint(gui_window_t *window,
                                    const gui_surface_t *source,
                                    gui_surface_t **staging_out) {
    gui_rect_t content;
    gui_rect_t screen;
    gui_rect_t visible;
    int slot;

    if (staging_out) *staging_out = NULL;
    if (!window || !source || !source->pixels || !staging_out ||
        window->content_staging_active) return false;

    content = gui_window_content_rect(window);
    screen = (gui_rect_t){0, 0, source->width, source->height};
    if (!gui_rect_intersect(content, screen, &visible) ||
        visible.w <= 0 || visible.h <= 0) return false;

    /* In <=16 MiB, a separate 640x480 ARGB staging surface costs 1.2 MiB.
       Paint into the full desktop view so legacy applications keep absolute
       pixels/pitch, then capture the result into the compact cache on callback
       return. CPU0 affinity keeps this asynchronous callback serialized with
       the software compositor. */
    if (content_cache_uses_rgb332() && mm_physical_top()<=16U*1024U*1024U && !window->compact_content) {
        window->content_staging = *source;
        window->content_staging.clip = visible;
        window->content_staging_slot = -3;
        window->content_staging_rect = content;
        if (!window->streaming_content)
            gui_gfx_fill_rect(&window->content_staging, visible,
                              window->bg_color);
        window->content_staging_active = true;
        *staging_out = &window->content_staging;
        return true;
    }

    /* A full-screen staging buffer per window consumed roughly 3 MiB at
     * 1024x768 and remained allocated for the entire lifetime of the window.
     * A burst of native applications therefore exhausted the heap and left
     * only kernel-painted frames visible.  Borrow one of a small number of
     * reusable surfaces instead; windows that miss a slot remain dirty and
     * retry on the next compositor pass. */
    slot = content_staging_acquire(source, &window->content_staging,window->compact_content?&visible:NULL);
    if (slot < 0) return false;
    window->content_staging_slot = (int8_t)slot;

    window->content_staging_rect = content;
    gui_gfx_set_clip(&window->content_staging, content);
    if (!window->streaming_content || !window->visible ||
        !window->desktop || window->desktop->last_window != window)
        gui_gfx_fill_rect(&window->content_staging, content, window->bg_color);

    window->content_staging_active = true;
    *staging_out = &window->content_staging;
    return true;
}
#endif


#ifndef BLES_VISION_SERVICE
void gui_window_end_content_paint(gui_window_t *window) {
    int slot;
    if (!window || !window->content_staging_active) return;
    slot = window->content_staging_slot;
    window->content_staging_active = false;
    window->content_staging_slot = -1;
    window->content_staging_rect = (gui_rect_t){0, 0, 0, 0};
    kmemset(&window->content_staging, 0, sizeof(window->content_staging));
    if (slot >= 0) content_staging_release_slot(slot);
}
#endif


bool gui_window_update_stream(gui_window_t *window, const uint32_t *pixels,
                              uint16_t width, uint16_t height) {
    uint32_t count;
    uint32_t *copy;
    if (!window || !pixels || !width || !height) return false;
    count = (uint32_t)width * (uint32_t)height;
    if (count > (16U * 1024U * 1024U) / sizeof(uint32_t)) return false;
    if (!window->stream_pixels || window->stream_width != width ||
        window->stream_height != height) {
        copy = (uint32_t *)kmalloc((size_t)count * sizeof(uint32_t));
        if (!copy || !mm_set_allocation_owner(copy, 0U)) {
            if (copy) kfree(copy);
            return false;
        }
        if (window->stream_pixels) kfree(window->stream_pixels);
        window->stream_pixels = copy;
        window->stream_width = width;
        window->stream_height = height;
    }
    kmemcpy(window->stream_pixels, pixels, (size_t)count * sizeof(uint32_t));
    window->stream_ready = true;
    return true;
}

void gui_window_paint_stream_content(gui_surface_t *surface,
                                     const gui_window_t *window,
                                     gui_rect_t clip) {
    gui_rect_t content;
    if (!surface || !window || !window->streaming_content ||
        !window->stream_ready || !window->stream_pixels ||
        !window->stream_width || !window->stream_height) return;
    content = gui_window_content_rect(window);
    gui_gfx_draw_xrgb(surface, content, clip, window->stream_pixels,
                      window->stream_width, window->stream_height);
}

void gui_window_paint_cached_content(gui_surface_t *surface,
                                     const gui_window_t *window,
                                     gui_rect_t clip) {
    gui_rect_t content;
    gui_rect_t visible;
    gui_rect_t surface_clip;
    int copy_w;
    int copy_h;

    if (!surface || !surface->pixels || !window || !window->content_ready ||
        !window->content_cache) return;
    content = gui_window_content_rect(window);
    copy_w = content.w < window->content_cache_width
           ? content.w : window->content_cache_width;
    copy_h = content.h < window->content_cache_height
           ? content.h : window->content_cache_height;
    content.w = copy_w;
    content.h = copy_h;
    surface_clip = gui_gfx_get_clip(surface);
    if (!gui_rect_intersect(content, clip, &visible) ||
        !gui_rect_intersect(visible, surface_clip, &visible)) return;

    for (int y = visible.y; y < visible.y + visible.h; y++) {
        uint32_t source_y = (uint32_t)(y - content.y);
        uint32_t source_x = (uint32_t)(visible.x - content.x);
        uint32_t *dst = &surface->pixels[(uint32_t)y * surface->pitch +
                                         (uint32_t)visible.x];
        if (window->content_cache_rgb332) {
            const uint8_t *src =
                (const uint8_t *)(const void *)window->content_cache +
                source_y * window->content_cache_width + source_x;
            for (int x = 0; x < visible.w; x++)
                dst[x] = content_rgb332_to_888(src[x]);
        } else {
            kmemcpy(dst, &window->content_cache[source_y *
                        window->content_cache_width + source_x],
                    (size_t)visible.w * sizeof(uint32_t));
        }
    }
}

void gui_window_set_event_handler(gui_window_t *window,
                                  gui_window_event_t handler,
                                  void *context) {
    if (!window) return;
    gui_desktop_paint_lock();
    window->event_handler = handler;
    window->event_context = context;
    window->event_pid = callback_owner((const void *)(uintptr_t)handler);
    gui_desktop_paint_unlock();
}

void gui_window_set_extended_events(gui_window_t *window, bool enabled) {
    if (!window) return;
    window->extended_events = enabled;
    /* Extended clients receive GUI_EVENT_RESIZE and are expected to perform
       their own layout.  Native fixed-layout windows keep the automatic guard. */
    window->layout_guard_enabled = !enabled;
    gui_window_refresh_layout_limits(window);
}

bool gui_window_dispatch_event(gui_window_t *window,
                               const gui_event_t *event) {
    uint32_t arguments[3];
    if (!window || !event || !window->event_handler) return false;
#ifdef BLES_VISION_SERVICE
    if(window->event_pid==task_current_pid())
        return window->event_handler(window,event,window->event_context);
#endif
    if (window->event_pid) {
        arguments[0] = (uint32_t)(uintptr_t)window;
        arguments[1] = 0U;
        arguments[2] = (uint32_t)(uintptr_t)window->event_context;
        return task_queue_window_upcall(window, window->event_pid,
            (uint32_t)(uintptr_t)window->event_handler, arguments, 3,
            event, sizeof(*event), 1);
    }
#ifdef BLES_VISION_SERVICE
    /* TinyX's input bridge is a privileged device callback, not executable
     * user code. Crossing through a checked syscall preserves CPL3. */
    return bk_vision_driver_window_event(window,event);
#else
    return window->event_handler(window, event, window->event_context);
#endif
}

void gui_window_paint(gui_surface_t *surface, gui_window_t *window, gui_rect_t clip) {
    gui_rect_t frame;
    gui_rect_t titlebar;
    gui_rect_t body;
    gui_rect_t left_button;
    gui_rect_t min_button;
    gui_rect_t close_button;
    uint32_t title_bg;
    uint32_t title_grain;
    uint32_t title_text;
    int title_left;
    int title_right;
    int title_w;
    int title_x;

    if (!surface || !window || !window->visible) return;
    if (!gui_rect_intersect(window->bounds, clip, &frame)) return;

    if (window->borderless) {
        gui_gfx_fill_rect(surface, window->bounds, window->bg_color);
        return;
    }

    frame = window->bounds;
    titlebar = (gui_rect_t){frame.x + GUI_BORDER_SIZE - 1,
                            frame.y + 4,
                            frame.w - (GUI_BORDER_SIZE - 1) * 2,
                            GUI_TITLEBAR_HEIGHT - 7};
    body = (gui_rect_t){frame.x + GUI_BORDER_SIZE,
                        frame.y + GUI_TITLEBAR_HEIGHT,
                        frame.w - GUI_BORDER_SIZE * 2,
                        frame.h - GUI_TITLEBAR_HEIGHT - GUI_BORDER_SIZE};
    title_bg = window->focused ? window->title_color : GUI_CDE_INACTIVE_TITLE;
    title_grain = window->focused ? GUI_CDE_ACTIVE_GRAIN
                                  : GUI_CDE_INACTIVE_GRAIN;
    title_text = (((title_bg >> 16) & 0xFFU) +
                  ((title_bg >> 8) & 0xFFU) +
                  (title_bg & 0xFFU)) < 400U ? 0x00FFFFFFU : GUI_CDE_TEXT;

    /* Marco ancho de workstation: dos relieves exteriores, canal interior y
     * grano discreto.  El centro no se rasteriza píxel a píxel. */
    cde_draw_bevel(surface, frame, false, GUI_CDE_FACE, 2);
    if (frame.w > 8 && frame.h > 8) {
        gui_gfx_draw_rect(surface,
            (gui_rect_t){frame.x + 3, frame.y + 3,
                         frame.w - 6, frame.h - 6}, GUI_CDE_SHADOW);
        gui_gfx_draw_rect(surface,
            (gui_rect_t){frame.x + 4, frame.y + 4,
                         frame.w - 8, frame.h - 8}, GUI_CDE_HIGHLIGHT);
    }
    cde_stipple_rect(surface,
        (gui_rect_t){frame.x + 3, frame.y + 3, frame.w - 6, 3},
        GUI_CDE_FACE_GRAIN);
    cde_stipple_rect(surface,
        (gui_rect_t){frame.x + 3, frame.y + frame.h - 6, frame.w - 6, 3},
        GUI_CDE_FACE_GRAIN);
    cde_stipple_rect(surface,
        (gui_rect_t){frame.x + 3, frame.y + 6, 3, frame.h - 12},
        GUI_CDE_FACE_GRAIN);
    cde_stipple_rect(surface,
        (gui_rect_t){frame.x + frame.w - 6, frame.y + 6,
                     3, frame.h - 12}, GUI_CDE_FACE_GRAIN);

    cde_draw_bevel(surface, titlebar, true, title_bg, 1);
    cde_stipple_rect(surface, titlebar, title_grain);
    gui_gfx_fill_rect(surface, body, window->bg_color);

    left_button = (gui_rect_t){frame.x + GUI_BORDER_SIZE,
                               frame.y + 4, 16, 16};
    cde_draw_bevel(surface, left_button, false, GUI_CDE_FACE, 2);

    min_button = gui_window_minimize_button_rect(window);
    close_button = gui_window_close_button_rect(window);
    if (!gui_setup_mode()) {
        cde_draw_bevel(surface, min_button, false, GUI_CDE_FACE, 2);
        cde_draw_bevel(surface, close_button, false, GUI_CDE_FACE, 2);
        gui_gfx_fill_rect(surface,
            (gui_rect_t){min_button.x + 4, min_button.y + 10, 8, 2},
            GUI_CDE_DARK);
        gui_gfx_draw_line(surface, close_button.x + 4, close_button.y + 4,
                          close_button.x + 11, close_button.y + 11,
                          GUI_CDE_DARK);
        gui_gfx_draw_line(surface, close_button.x + 11, close_button.y + 4,
                          close_button.x + 4, close_button.y + 11,
                          GUI_CDE_DARK);
    }

    /* El botón izquierdo de Motif aloja el icono de la aplicación y mantiene
     * la silueta CDE incluso cuando el programa no suministra uno. */
    if (window->icon_pixels && window->icon_width && window->icon_height) {
        int iy;
        for (iy = 0; iy < 12; iy++) {
            int ix;
            for (ix = 0; ix < 12; ix++) {
                uint32_t pixel = window->icon_pixels[
                    (iy * window->icon_height / 12) * window->icon_width +
                    (ix * window->icon_width / 12)];
                if (pixel >> 24)
                    gui_gfx_putpixel(surface, left_button.x + 2 + ix,
                                     left_button.y + 2 + iy,
                                     pixel & 0x00FFFFFFU);
            }
        }
    } else {
        gui_gfx_fill_rect(surface,
            (gui_rect_t){left_button.x + 4, left_button.y + 5, 8, 2},
            GUI_CDE_DARK);
        gui_gfx_fill_rect(surface,
            (gui_rect_t){left_button.x + 4, left_button.y + 9, 8, 2},
            GUI_CDE_SHADOW);
    }

    title_left = left_button.x + left_button.w + 5;
    title_right = gui_setup_mode() ? frame.x + frame.w - GUI_BORDER_SIZE - 4
                                   : min_button.x - 5;
    if (title_right < title_left) title_right = title_left;
    title_w = (int)gui_font_text_width(window->title);
    title_x = frame.x + (frame.w - title_w) / 2;
    if (title_x < title_left) title_x = title_left;
    if (title_x + title_w > title_right) title_x = title_right - title_w;
    if (title_x < title_left) title_x = title_left;
    gui_font_draw_string_clipped(surface, title_x, frame.y + 8,
                                 window->title, title_text,
                                 (gui_rect_t){title_left, frame.y + 5,
                                              title_right - title_left,
                                              GUI_TITLEBAR_HEIGHT - 8});
}

void gui_window_paint_widgets(gui_surface_t *surface, gui_window_t *window,
                              gui_rect_t clip) {
    gui_rect_t client;
    gui_rect_t widget_clip;
    gui_widget_t *widget;

    if (!surface || !window || !window->visible) return;
    client = gui_window_content_rect(window);
    if (!gui_rect_intersect(clip, client, &widget_clip)) return;

    widget = window->widgets;
    while (widget) {
        if (widget->style != GUI_WIDGET_STYLE_DROPDOWN)
            gui_widget_paint(surface, window, widget, widget_clip);
        widget = widget->next;
    }

    widget = window->widgets;
    while (widget) {
        if (widget->style == GUI_WIDGET_STYLE_DROPDOWN && !gui_widget_is_dropdown_expanded(widget))
            gui_widget_paint(surface, window, widget, widget_clip);
        widget = widget->next;
    }
    /* Expanded menus are overlays above every ordinary control. */
    for(widget=window->widgets;widget;widget=widget->next)
        if(gui_widget_is_dropdown_expanded(widget)) gui_widget_paint(surface,window,widget,widget_clip);
}

#ifndef BLES_VISION_SERVICE
uint32_t gui_content_staging_reclaim(void) {
    uint32_t flags,bytes=0;
    uint32_t *pixels[GUI_CONTENT_STAGING_POOL_SIZE]={0};
    if(!kspin_try_lock_irqsave(&g_content_staging_lock,&flags))return 0;
    for(uint32_t i=0;i<GUI_CONTENT_STAGING_POOL_SIZE;i++) {
        gui_content_staging_slot_t *slot=&g_content_staging_pool[i];
        if(slot->in_use || !slot->surface.pixels)continue;
        pixels[i]=slot->surface.pixels;bytes+=slot->capacity*4U;
        slot->surface.pixels=NULL;slot->capacity=0;
    }
    kspin_unlock_irqrestore(&g_content_staging_lock,flags);
    for(uint32_t i=0;i<GUI_CONTENT_STAGING_POOL_SIZE;i++)if(pixels[i])kfree(pixels[i]);
    return bytes;
}
#endif


#ifndef BLES_VISION_SERVICE
bool bk_vision_driver_window_event(gui_window_t *window,const gui_event_t *event) {
    extern uint8_t __kernel_live_start,__kernel_text_end;
    if(task_current_process_id()!=gui_vision_owner_pid() || !window || !event ||
       window->event_pid || !window->event_handler)return false;
    bool handled;
    if(syscall_gfx_window_event(window,event,&handled))return handled;
    uintptr_t entry=(uintptr_t)window->event_handler;
    if(entry<(uintptr_t)&__kernel_live_start || entry>=(uintptr_t)&__kernel_text_end)return false;
    return window->event_handler(window,event,window->event_context);
}
#endif
