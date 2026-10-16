#include "../kernel/include/vision_resources.h"
#include "gui.h"
#include "../kernel/include/memory.h"
#include "../kernel/include/task.h"
#include "../kernel/include/klock.h"
#include "../kernel/include/keyboard.h"
#include "../programs/programs.h"
#include "color_palette.h"

#ifndef BLES_VISION_SERVICE
static kmutex_t g_desktop_paint_lock = KMUTEX_INITIALIZER;
#endif
/* task_preempt_disable() only protects the current CPU. Window list and focus
   transitions also run from Ring-3 completion/cleanup paths on APs, so they
   need a real cross-CPU lock. Keep this lock strictly to pointer/flag updates;
   drawing and callbacks remain outside it. */
static kspinlock_t g_desktop_state_lock = KSPINLOCK_INITIALIZER;

static void gui_desktop_record_damage(gui_desktop_t *desktop,
                                      gui_rect_t rect,
                                      const gui_program_t *publisher);
static void gui_desktop_request_scene_owner_rect(gui_desktop_t *desktop,
                                                  gui_rect_t rect);


#ifndef BLES_VISION_SERVICE
void gui_desktop_paint_lock(void) {
    kmutex_lock(&g_desktop_paint_lock);
}

void gui_desktop_paint_unlock(void) {
    kmutex_unlock(&g_desktop_paint_lock);
}

void gui_desktop_abandon_paint_lock(uint32_t tid) {
    kmutex_abandon_owner(&g_desktop_paint_lock,tid);
}
#endif

static void copy_text(char *dst, size_t dst_len, const char *src) {
    if (!dst || !dst_len) return;
    if (!src) src = "";
    kstrncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void gui_desktop_clear_cursor_trail(gui_desktop_t *desktop) {
    if (!desktop) return;
    desktop->cursor_trail_count = 0;
    desktop->cursor_paint_count = 0;
}

static void gui_desktop_push_cursor_trail(gui_desktop_t *desktop, int x, int y) {
    if (!desktop || !desktop->cursor_trail_enabled) return;
    if (desktop->cursor_trail_count > 0) {
        uint8_t last = (uint8_t)(desktop->cursor_trail_count - 1);
        if (desktop->cursor_trail_x[last] == x &&
            desktop->cursor_trail_y[last] == y) return;
    }

    if (desktop->cursor_trail_count < GUI_CURSOR_TRAIL_MAX) {
        desktop->cursor_trail_x[desktop->cursor_trail_count] = x;
        desktop->cursor_trail_y[desktop->cursor_trail_count] = y;
        desktop->cursor_trail_count++;
        return;
    }

    for (uint8_t i = 1; i < GUI_CURSOR_TRAIL_MAX; i++) {
        desktop->cursor_trail_x[i - 1] = desktop->cursor_trail_x[i];
        desktop->cursor_trail_y[i - 1] = desktop->cursor_trail_y[i];
    }
    desktop->cursor_trail_x[GUI_CURSOR_TRAIL_MAX - 1] = x;
    desktop->cursor_trail_y[GUI_CURSOR_TRAIL_MAX - 1] = y;
}

static bool gui_program_surface_size(gui_surface_t *surface,
                                     const gui_surface_t *source) {
    uint32_t *pixels;
    uint32_t count;

    if (!surface || !source || !source->pixels) return false;
    if (surface->pixels && surface->width == source->width &&
        surface->height == source->height && surface->pitch == source->pitch)
        return true;
    count = (uint32_t)source->pitch * source->height;
    pixels = (uint32_t *)krealloc(surface->pixels, count * sizeof(uint32_t));
    if (!pixels) return false;
    /* El compositor conserva el buffer despues de devolver CPU a Ring 3. */
    if (!mm_set_allocation_owner(pixels, 0U)) {
        kfree(pixels);
        surface->pixels = NULL;
        surface->width = surface->height = surface->pitch = 0;
        return false;
    }
    (void)mm_set_allocation_subsystem(pixels,MM_SUBSYSTEM_GUI);
    surface->pixels = pixels;
    surface->width = source->width;
    surface->height = source->height;
    surface->pitch = source->pitch;
    gui_gfx_reset_clip(surface);
    return true;
}

static bool gui_program_cache_size(gui_program_t *program, const gui_surface_t *source) {
    bool indexed = gui_display_color_depth() == 8U;
    gui_surface_t *cache = &program->paint_cache;
    if (cache->pixels && cache->width == source->width &&
        cache->height == source->height && cache->pitch == source->pitch &&
        program->paint_cache_indexed == indexed) return true;
    uint32_t count = (uint32_t)source->pitch * source->height;
    uint32_t *pixels = krealloc(cache->pixels, count * (indexed ? 1U : 4U));
    if (!pixels) return false;
    (void)mm_set_allocation_owner(pixels, 0U);
    (void)mm_set_allocation_subsystem(pixels,MM_SUBSYSTEM_GUI);
    *cache = *source;
    cache->pixels = pixels;
    program->paint_cache_indexed = indexed;
    program->paint_ready = false;
    return true;
}

bool gui_program_prepare_paint(gui_program_t *program,
                               const gui_surface_t *source,
                               gui_surface_t **staging_out) {
    gui_rect_t screen;
    gui_rect_t copy_rect;

    if (staging_out) *staging_out = NULL;
    if (!program || !source || !source->pixels || !staging_out ||
        !gui_program_cache_size(program, source) ||
        !gui_program_surface_size(&program->paint_staging, source))
        return false;

    screen = (gui_rect_t){0, 0, source->width, source->height};
    if (!program->paint_ready) {
        uint32_t count = (uint32_t)source->pitch * source->height;

        /* Initialize both buffers before the first complete paint. Later
         * callbacks start from the latest published frame. */
        kmemcpy(program->paint_staging.pixels, source->pixels,
                (size_t)count * sizeof(uint32_t));
        copy_rect = screen;
    } else {
        gui_rect_t source_clip = gui_gfx_get_clip(source);
        if (!gui_rect_intersect(screen, source_clip, &copy_rect))
            return false;
        /* Both buffers must contain the same complete shell before a
         * clipped callback. Otherwise alternating clips resurrect an older
         * popup/background from the previous staging buffer. Client pixels
         * belong to the window layer and must not enter the shell cache. */
        if (program->paint_cache_indexed) {
            uint8_t *cache = (uint8_t *)program->paint_cache.pixels;
            for (uint32_t i=0; i<(uint32_t)source->pitch*source->height; i++)
                program->paint_staging.pixels[i] = content_rgb332_to_888(cache[i]);
        } else kmemcpy(program->paint_staging.pixels, program->paint_cache.pixels,
                (size_t)source->pitch * source->height * sizeof(uint32_t));
        if (program->scene_owner) {
            uint32_t count = program->paint_overlay_count > 12U ? 12U : program->paint_overlay_count;
            /* The first rectangle is the permanent Deskbar; only old popups
             * need restoration outside the requested damage rectangle. */
            for (uint32_t i = 1; i < count; i++)
                copy_rect = gui_rect_union(copy_rect, program->paint_overlays[i]);
            if (!gui_rect_intersect(screen, copy_rect, &copy_rect)) return false;
        }
    }
    program->staging_overlay_count = 0;
    gui_gfx_set_clip(&program->paint_staging, copy_rect);
    *staging_out = &program->paint_staging;
    return true;
}

void gui_program_composite_paint(const gui_program_t *program,
                                 gui_surface_t *destination) {
    gui_rect_t visible;
    gui_rect_t screen;

    if (!program || !program->paint_ready || !program->paint_cache.pixels ||
        !destination || !destination->pixels) return;
    /* A display-mode change replaces the desktop surface immediately, while
       a Ring-3 full-screen painter may still own a cache produced for the old
       geometry. Never index that cache using the new width/height. The normal
       prepare path will resize it before the next callback. */
    if (program->paint_cache.width != destination->width ||
        program->paint_cache.height != destination->height ||
        program->paint_cache.pitch != destination->pitch) return;
    screen = (gui_rect_t){0, 0, destination->width, destination->height};
    if (!gui_rect_intersect(screen, gui_gfx_get_clip(destination), &visible))
        return;
    for (int y = visible.y; y < visible.y + visible.h; y++) {
        if (program->paint_cache_indexed) {
            const uint8_t *row = (const uint8_t *)program->paint_cache.pixels +
                (uint32_t)y * program->paint_cache.pitch;
            for (int x=visible.x; x<visible.x+visible.w; x++)
                destination->pixels[(uint32_t)y*destination->pitch+x] = content_rgb332_to_888(row[x]);
            continue;
        }
        kmemcpy(&destination->pixels[(uint32_t)y * destination->pitch +
                                     (uint32_t)visible.x],
                &program->paint_cache.pixels[(uint32_t)y *
                    program->paint_cache.pitch + (uint32_t)visible.x],
                (size_t)visible.w * sizeof(uint32_t));
    }
}

void gui_program_finish_paint(gui_program_t *program) {
    gui_surface_t swap;
    if (!program || !program->paint_staging.pixels) return;
    if (program->paint_cache_indexed) {
        uint8_t *cache = (uint8_t *)program->paint_cache.pixels;
        for (uint32_t i=0; i<(uint32_t)program->paint_staging.pitch*program->paint_staging.height; i++)
            cache[i] = content_rgb888_to_332(program->paint_staging.pixels[i]);
        program->paint_cache.clip = program->paint_staging.clip;
    } else {
        swap = program->paint_cache;
        program->paint_cache = program->paint_staging;
        program->paint_staging = swap;
    }
    program->paint_overlay_count = program->staging_overlay_count > 12U
        ? 12U : program->staging_overlay_count;
    kmemcpy(program->paint_overlays, program->staging_overlays,
            program->paint_overlay_count * sizeof(gui_rect_t));
    program->paint_pending = false;
    program->paint_ready = true;
}

void gui_program_release_paint(gui_program_t *program) {
    if (!program) return;
    if (program->paint_cache.pixels) kfree(program->paint_cache.pixels);
    if (program->paint_staging.pixels) kfree(program->paint_staging.pixels);
    kmemset(&program->paint_cache, 0, sizeof(program->paint_cache));
    kmemset(&program->paint_staging, 0, sizeof(program->paint_staging));
}

void gui_desktop_init(gui_desktop_t *desktop, gui_surface_t surface) {
    if (!desktop) return;
    kmemset(desktop, 0, sizeof(*desktop));
    desktop->surface = surface;
    desktop->mouse_x = surface.width / 2;
    desktop->mouse_y = surface.height / 2;
    desktop->next_program_id = 1;
    desktop->next_window_id = 1;
    desktop->next_widget_id = 1;
}

gui_program_t *gui_desktop_register_program(gui_desktop_t *desktop, const char *name, void *state, gui_program_paint_t paint, gui_program_event_t handle_event, gui_program_destroy_t destroy) {
    gui_program_t *program;

    if (!desktop || !paint) return NULL;

    program = (gui_program_t *)kzalloc(sizeof(gui_program_t));
    if (!program) return NULL;
    if (!mm_set_allocation_owner(program, 0U)) {
        kfree(program);
        return NULL;
    }

    program->id = desktop->next_program_id++;
    program->state = state;
    program->paint = paint;
    program->handle_event = handle_event;
    program->destroy = destroy;
    program->paint_requested = true;
    if ((uint32_t)(uintptr_t)paint >= HEAP_START && task_current_is_user()) {
        program->callback_pid = task_current_pid();
        /* Native desktop paint/event callbacks still serialize with the CPU0
         * compositor.  Only this owner thread is pinned; process workers are
         * unaffected and may run on APs. */
        (void)task_set_affinity_mask(program->callback_pid, 1U);
    }
    copy_text(program->name, sizeof(program->name), name);

    if(!bk_vision_resource_register(program,true)){kfree(program);return NULL;}
    program->prev = desktop->last_program;
    program->next = NULL;
    if (desktop->last_program) desktop->last_program->next = program;
    desktop->last_program = program;
    if (!desktop->first_program) desktop->first_program = program;
    return program;
}


void gui_desktop_unregister_program(gui_desktop_t *desktop, gui_program_t *program) {
    if (!desktop || !program) return;
    bk_vision_resource_retire(program);
    bk_vision_resource_forget(program);
    task_preempt_disable();

    if (program->prev) program->prev->next = program->next;
    if (program->next) program->next->prev = program->prev;
    if (desktop->first_program == program) desktop->first_program = program->next;
    if (desktop->last_program == program) desktop->last_program = program->prev;

    program->prev = NULL;
    program->next = NULL;
    task_preempt_enable();

    if (program->destroy && program->callback_pid) {
        uint32_t argument = (uint32_t)(uintptr_t)program;
        if (task_queue_user_upcall(program->callback_pid,
                (uint32_t)(uintptr_t)program->destroy, &argument, 1,
                NULL, 0, -4)) return;
    } else if (program->destroy) {
        program->destroy(program);
    }
    gui_program_release_paint(program);
    bk_vision_resource_forget(program);
    kfree(program);
}

void gui_desktop_shell_window_state_changed(gui_desktop_t *desktop) {
    gui_rect_t bar;
    if (!desktop) return;
    bar = (gui_rect_t){0,
        desktop->surface.height > 72 ? (int)desktop->surface.height - 72 : 0,
        (int)desktop->surface.width,
        desktop->surface.height > 72 ? 72 : (int)desktop->surface.height};
    gui_desktop_request_scene_owner_rect(desktop, bar);
    gui_request_paint();
}

gui_window_t *gui_desktop_create_window(gui_desktop_t *desktop, int x, int y, int w, int h, const char *title) {
    gui_window_t *window;
    if (!desktop) return NULL;
    /* Window construction can run in an application worker while the GUI task
       is traversing the desktop.  Serialize publication with compositor/event
       lifetime so no partially configured object becomes visible. */
    gui_desktop_paint_lock();
    window = gui_window_create(desktop, x, y, w, h, title);
    if (window) {
        gui_desktop_add_window(desktop, window);
        gui_desktop_shell_window_state_changed(desktop);
    }
    gui_desktop_paint_unlock();
    return window;
}

bool gui_window_is_focusable(const gui_window_t *window) {
    return window && window->destroy_state == 0U && window->visible &&
           !window->minimized && window->input_enabled;
}

typedef struct {
    gui_rect_t previous_bounds;
    gui_rect_t next_bounds;
    gui_window_t *previous_window;
    gui_window_t *next_window;
    bool previous_valid;
    bool next_valid;
    bool repaired;
    bool focus_changed;
} gui_focus_damage_t;

static bool gui_desktop_contains_window_locked(
    const gui_desktop_t *desktop, const gui_window_t *window) {
    const gui_window_t *scan;
    if (!desktop || !window) return false;
    scan = desktop->first_window;
    while (scan) {
        if (scan == window) return true;
        scan = scan->next;
    }
    return false;
}

static bool gui_desktop_focusable_member_locked(
    const gui_desktop_t *desktop, const gui_window_t *window) {
    const gui_window_t *scan;
    if (!desktop || !window) return false;
    scan = desktop->first_window;
    while (scan) {
        if (scan == window) return gui_window_is_focusable(scan);
        scan = scan->next;
    }
    return false;
}

static gui_window_t *gui_desktop_top_focusable_locked(
    gui_desktop_t *desktop, const gui_window_t *exclude) {
    gui_window_t *candidate;

    if (!desktop) return NULL;
    candidate = desktop->last_window;
    while (candidate) {
        if (candidate != exclude && gui_window_is_focusable(candidate))
            return candidate;
        candidate = candidate->prev;
    }
    return NULL;
}

static void gui_desktop_assign_focus_locked(gui_desktop_t *desktop,
                                             gui_window_t *window,
                                             gui_focus_damage_t *damage) {
    gui_window_t *previous;
    gui_window_t *scan;

    if (!desktop || !damage) return;
    kmemset(damage, 0, sizeof(*damage));
    previous = desktop->focused_window;

    /* The common no-focus case is completely stable and needs no list walk.
     * This matters for desktop/menu clicks because focus assignment used to
     * scan the whole Z-order even when both old and requested focus are NULL. */
    if (!previous && !window) return;

    /* Never dereference a caller-provided/stale pointer until it is found in
       the live desktop list. A cleanup on another CPU may already have
       detached the object. */
    if (!gui_desktop_focusable_member_locked(desktop, window)) window = NULL;

    /* Once membership has been proven, assigning the already-focused window
     * is a no-op.  Skip the second full Z-order pass; the periodic repair path
     * remains responsible for recovering inconsistent focus flags. */
    if (window && window == previous) return;

    scan = desktop->first_window;
    while (scan) {
        bool should_focus = scan == window;
        if (scan == previous) {
            damage->previous_bounds = scan->bounds;
            damage->previous_window = scan;
            damage->previous_valid = true;
        }
        if (scan->focused != should_focus) {
            scan->focused = should_focus;
            /* Focus changes only affect CDE title/menu chrome.  Marking the
             * complete client dirty forced Ring-3 repaint callbacks and made
             * a simple focus change as expensive as redrawing the app. */
            if (scan != previous && scan != window) damage->repaired = true;
        }
        scan = scan->next;
    }

    desktop->focused_window = window;
    damage->next_window = window;
    damage->focus_changed = previous != window;
    if (window) {
        damage->next_bounds = window->bounds;
        damage->next_valid = true;
        window->focused = true;
    }
}

static gui_rect_t gui_desktop_focus_chrome_rect(gui_rect_t bounds) {
    int height = GUI_TITLEBAR_HEIGHT + GUI_MENU_HEIGHT;
    if (height > bounds.h) height = bounds.h;
    return (gui_rect_t){bounds.x, bounds.y, bounds.w, height};
}

static void gui_desktop_dispatch_focus_event(gui_window_t *window,
                                             gui_event_type_t type) {
    gui_event_t event;
    if (!window || !window->extended_events || window->destroy_state != 0U)
        return;
    kmemset(&event, 0, sizeof(event));
    event.type = type;
    (void)gui_window_dispatch_event(window, &event);
}

static void gui_desktop_finish_focus_damage(gui_desktop_t *desktop,
                                             const gui_focus_damage_t *damage) {
    if (!desktop || !damage) return;
    /* Focusing an already-focused window is the overwhelmingly common click
       path.  Do not repaint title bars or bump the paint generation when no
       focus bit actually changed. */
    if (!damage->focus_changed && !damage->repaired) return;

    /* Never queue Ring-3 callbacks while g_desktop_state_lock is held.
       Windows opt in explicitly, so legacy applications preserve 0.8 input
       and focus semantics. */
    if (damage->focus_changed && damage->previous_valid && damage->previous_window) {
        gui_widget_t *widget=damage->previous_window->widgets;
        while(widget) {
            if(widget->pressed || widget->hovered) {
                if(widget->pressed && widget->style==GUI_WIDGET_STYLE_SLIDER)
                    bk_gui_slider_set_value(widget,widget->range_start);
                widget->pressed=false; widget->hovered=false;
                gui_desktop_invalidate_rect(desktop,
                    gui_widget_screen_bounds(damage->previous_window,widget));
            }
            widget=widget->next;
        }
    }
    if (damage->focus_changed) {
        gui_desktop_dispatch_focus_event(damage->previous_window,
                                         GUI_EVENT_FOCUS_LOST);
        gui_desktop_dispatch_focus_event(damage->next_window,
                                         GUI_EVENT_FOCUS_GAIN);
    }

    if (damage->repaired) {
        gui_desktop_invalidate_all(desktop);
    } else {
        if (damage->previous_valid)
            gui_desktop_invalidate_rect(desktop,
                gui_desktop_focus_chrome_rect(damage->previous_bounds));
        if (damage->next_valid)
            gui_desktop_invalidate_rect(desktop,
                gui_desktop_focus_chrome_rect(damage->next_bounds));
    }
    gui_request_paint();
    if (damage->focus_changed)
        gui_desktop_shell_window_state_changed(desktop);
}

static void gui_desktop_repair_focus(gui_desktop_t *desktop) {
    gui_focus_damage_t damage;
    gui_window_t *target;
    gui_window_t *scan;
    bool current_valid;
    bool invariant_ok = true;
    uint32_t flags;

    if (!desktop) return;
    flags = kspin_lock_irqsave(&g_desktop_state_lock);
    target = desktop->focused_window;
    /* Structural window operations repair focus while holding the same state
       lock.  On the normal input path, a live focused window with its bit set
       is therefore already a valid invariant; scanning every window for every
       PS/2 packet was pure O(window_count) overhead. */
    if (target && target->focused && gui_window_is_focusable(target)) {
        kspin_unlock_irqrestore(&g_desktop_state_lock, flags);
        return;
    }
    if (!target && !desktop->first_window) {
        kspin_unlock_irqrestore(&g_desktop_state_lock, flags);
        return;
    }
    current_valid = !target ||
        gui_desktop_focusable_member_locked(desktop, target);
    scan = desktop->first_window;
    while (scan) {
        if (scan->focused != (target && current_valid && scan == target)) {
            invariant_ok = false;
            break;
        }
        scan = scan->next;
    }
    if (current_valid && invariant_ok) {
        kspin_unlock_irqrestore(&g_desktop_state_lock, flags);
        return;
    }
    if (!current_valid)
        target = gui_desktop_top_focusable_locked(desktop, NULL);
    gui_desktop_assign_focus_locked(desktop, target, &damage);
    kspin_unlock_irqrestore(&g_desktop_state_lock, flags);
    gui_desktop_finish_focus_damage(desktop, &damage);
}

void gui_desktop_focus_window(gui_desktop_t *desktop, gui_window_t *window) {
    gui_focus_damage_t damage;
    uint32_t flags;

    if (!desktop) return;
    flags = kspin_lock_irqsave(&g_desktop_state_lock);
    gui_desktop_assign_focus_locked(desktop, window, &damage);
    kspin_unlock_irqrestore(&g_desktop_state_lock, flags);
    gui_desktop_finish_focus_damage(desktop, &damage);
}

void gui_desktop_focus_top_window(gui_desktop_t *desktop,
                                  const gui_window_t *exclude) {
    gui_focus_damage_t damage;
    gui_window_t *fallback;
    uint32_t flags;

    if (!desktop) return;
    flags = kspin_lock_irqsave(&g_desktop_state_lock);
    fallback = gui_desktop_top_focusable_locked(desktop, exclude);
    gui_desktop_assign_focus_locked(desktop, fallback, &damage);
    kspin_unlock_irqrestore(&g_desktop_state_lock, flags);
    gui_desktop_finish_focus_damage(desktop, &damage);
}

void gui_desktop_window_hidden(gui_desktop_t *desktop, gui_window_t *window) {
    gui_focus_damage_t damage;
    gui_window_t *target;
    gui_rect_t exposed;
    uint32_t flags;

    if (!desktop || !window) return;
    exposed = window->paint_bounds_valid ? window->paint_bounds : window->bounds;

    flags = kspin_lock_irqsave(&g_desktop_state_lock);
    if (!gui_desktop_contains_window_locked(desktop, window)) {
        kspin_unlock_irqrestore(&g_desktop_state_lock, flags);
        return;
    }
    if (desktop->drag_window == window) {
        desktop->drag_window = NULL;
        desktop->drag_outline_visible = false;
    }
    if (desktop->resize_window == window) {
        desktop->resize_window = NULL;
        desktop->resize_edges = 0U;
        desktop->drag_outline_visible = false;
    }
    target = desktop->focused_window;
    if (target == window ||
        (target && !gui_desktop_focusable_member_locked(desktop, target)))
        target = gui_desktop_top_focusable_locked(desktop, window);
    gui_desktop_assign_focus_locked(desktop, target, &damage);
    kspin_unlock_irqrestore(&g_desktop_state_lock, flags);

    gui_desktop_invalidate_rect(desktop, exposed);
    gui_desktop_finish_focus_damage(desktop, &damage);
    gui_desktop_shell_window_state_changed(desktop);
}

void gui_desktop_add_window(gui_desktop_t *desktop, gui_window_t *window) {
    gui_focus_damage_t damage;
    uint32_t flags;

    if (!desktop || !window) return;
    flags = kspin_lock_irqsave(&g_desktop_state_lock);
    window->desktop = desktop;
    window->prev = desktop->last_window;
    window->next = NULL;
    if (desktop->last_window) desktop->last_window->next = window;
    desktop->last_window = window;
    if (!desktop->first_window) desktop->first_window = window;
    window->dirty = true;
    gui_desktop_assign_focus_locked(desktop, window, &damage);
    kspin_unlock_irqrestore(&g_desktop_state_lock, flags);
    gui_desktop_finish_focus_damage(desktop, &damage);
}

void gui_desktop_raise_window(gui_desktop_t *desktop, gui_window_t *window) {
    gui_rect_t bounds;
    bool reordered = false;
    uint32_t flags;

    if (!desktop || !window) return;
    gui_desktop_paint_lock();
    flags = kspin_lock_irqsave(&g_desktop_state_lock);
    if (!gui_desktop_contains_window_locked(desktop, window)) {
        kspin_unlock_irqrestore(&g_desktop_state_lock, flags);
        gui_desktop_paint_unlock();
        return;
    }
    bounds = window->bounds;
    if (desktop->last_window != window) {
        if (window->prev) window->prev->next = window->next;
        if (window->next) window->next->prev = window->prev;
        if (desktop->first_window == window)
            desktop->first_window = window->next;
        if (desktop->last_window == window)
            desktop->last_window = window->prev;

        window->prev = desktop->last_window;
        window->next = NULL;
        if (desktop->last_window) desktop->last_window->next = window;
        desktop->last_window = window;
        if (!desktop->first_window) desktop->first_window = window;
        window->dirty = true;
        reordered = true;
    }
    kspin_unlock_irqrestore(&g_desktop_state_lock, flags);

    /* Reordering changes every overlapping pixel inside this rectangle even
       when neither window changed its own contents. */
    if (reordered) {
        gui_desktop_invalidate_rect(desktop, bounds);
        gui_request_paint();
    }
    gui_desktop_paint_unlock();
}

void gui_desktop_remove_window(gui_desktop_t *desktop, gui_window_t *window) {
    gui_focus_damage_t damage;
    gui_window_t *target;
    gui_rect_t exposed;
    uint32_t flags;

    if (!desktop || !window) return;
    gui_desktop_paint_lock();
    exposed = window->paint_bounds_valid ? window->paint_bounds : window->bounds;

    flags = kspin_lock_irqsave(&g_desktop_state_lock);
    if (!gui_desktop_contains_window_locked(desktop, window)) {
        kspin_unlock_irqrestore(&g_desktop_state_lock, flags);
        gui_desktop_paint_unlock();
        return;
    }

    bk_vision_resource_retire(window);
    if (window->prev) window->prev->next = window->next;
    if (window->next) window->next->prev = window->prev;
    if (desktop->first_window == window) desktop->first_window = window->next;
    if (desktop->last_window == window) desktop->last_window = window->prev;

    if (desktop->drag_window == window) {
        desktop->drag_window = NULL;
        desktop->drag_outline_visible = false;
    }
    if (desktop->resize_window == window) {
        desktop->resize_window = NULL;
        desktop->resize_edges = 0U;
        desktop->drag_outline_visible = false;
    }

    target = desktop->focused_window;
    if (target == window ||
        (target && !gui_desktop_focusable_member_locked(desktop, target)))
        target = gui_desktop_top_focusable_locked(desktop, NULL);
    gui_desktop_assign_focus_locked(desktop, target, &damage);

    window->focused = false;
    window->prev = NULL;
    window->next = NULL;
    window->desktop = NULL;
    kspin_unlock_irqrestore(&g_desktop_state_lock, flags);

    gui_desktop_invalidate_rect(desktop, exposed);
    gui_desktop_finish_focus_damage(desktop, &damage);
    gui_desktop_shell_window_state_changed(desktop);
    gui_desktop_paint_unlock();
}

static void gui_desktop_unlink_dead_program(gui_desktop_t *desktop,
                                            gui_program_t *program) {
    if (!desktop || !program) return;
    task_preempt_disable();
    if (program->prev) program->prev->next = program->next;
    if (program->next) program->next->prev = program->prev;
    if (desktop->first_program == program)
        desktop->first_program = program->next;
    if (desktop->last_program == program)
        desktop->last_program = program->prev;
    task_preempt_enable();
    /* El callback destroy pertenece al hilo que ya terminó. No se lo puede
     * volver a invocar desde el reaper. */
    gui_program_release_paint(program);
    bk_vision_resource_forget(program);
    kfree(program);
}

void gui_desktop_cleanup_thread(gui_desktop_t *desktop, uint32_t tid) {
    gui_window_t *window;
    gui_program_t *program;

    if (!desktop || !tid) return;
    if (desktop->cursor_custom && desktop->cursor_owner_pid == tid)
        gui_desktop_reset_cursor(desktop, tid);

    /* owner_pid es deliberadamente un ID de proceso: las aplicaciones usan
     * bk_sys_getpid() al crear la ventana. No debe compararse con el TID del
     * hilo principal, porque ese TID coincide con el PID y suele terminar
     * apenas crea el worker real de la aplicación. Sólo los callbacks son
     * propiedad de un hilo concreto. */
    window = desktop->first_window;
    while (window) {
        gui_window_t *next = window->next;
        if (window->content_pid == tid || window->event_pid == tid) {
            gui_desktop_remove_window(desktop, window);
            gui_window_destroy(window);
        }
        window = next;
    }

    program = desktop->first_program;
    while (program) {
        gui_program_t *next = program->next;
        if (program->callback_pid == tid)
            gui_desktop_unlink_dead_program(desktop, program);
        program = next;
    }
    gui_request_paint();
}

void gui_desktop_cleanup_process(gui_desktop_t *desktop,
                                 uint32_t process_id) {
    gui_window_t *window;

    if (!desktop || !process_id) return;
    window = desktop->first_window;
    while (window) {
        gui_window_t *next = window->next;
        if (window->owner_pid == process_id) {
            gui_desktop_remove_window(desktop, window);
            gui_window_destroy(window);
        }
        window = next;
    }
    gui_request_paint();
}

void gui_desktop_cleanup_owner(gui_desktop_t *desktop, uint32_t owner_pid) {
    /* Compatibilidad con el nombre histórico: un owner sin contexto puede
     * representar tanto un TID de callback como un PID de ventana. El kernel
     * nuevo usa las dos funciones explícitas para no mezclar ambos conceptos. */
    gui_desktop_cleanup_thread(desktop, owner_pid);
    gui_desktop_cleanup_process(desktop, owner_pid);
}

gui_window_t *gui_desktop_window_at(gui_desktop_t *desktop, int x, int y) {
    gui_window_t *window;
    if (!desktop) return NULL;
    task_preempt_disable();
    window = desktop->last_window;
    while (window) {
        /* A Ring-3 close can leave a tombstoned object linked until its last
         * callback returns.  It must not form an invisible input shield over
         * the desktop or over another application during that interval. */
        if (window->destroy_state == 0U &&
            gui_window_contains(window, x, y)) {
            task_preempt_enable();
            return window;
        }
        window = window->prev;
    }
    task_preempt_enable();
    return NULL;
}

void gui_desktop_handle_event(gui_desktop_t *desktop, const gui_event_t *event) {
    gui_program_t *program;
    int old_x;
    int old_y;

    if (!desktop || !event) return;
    gui_desktop_repair_focus(desktop);

    old_x = desktop->mouse_x;
    old_y = desktop->mouse_y;
    if (event->type == GUI_EVENT_MOUSE_MOVE || event->type == GUI_EVENT_MOUSE_DOWN ||
        event->type == GUI_EVENT_MOUSE_UP || event->type == GUI_EVENT_MOUSE_WHEEL) {
        desktop->mouse_x = event->x;
        desktop->mouse_y = event->y;
        desktop->mouse_buttons = event->buttons;
        if ((old_x != desktop->mouse_x || old_y != desktop->mouse_y) &&
            desktop->cursor_trail_enabled)
            gui_desktop_push_cursor_trail(desktop, old_x, old_y);
    }

    if (desktop->error_visible) {
        int width = desktop->surface.width > GUI_ALERT_WIDTH
            ? GUI_ALERT_WIDTH : desktop->surface.width - 24;
        int height = GUI_ALERT_HEIGHT;
        int x;
        int y;
        gui_rect_t close_button;
        gui_rect_t ok_button;

        if (width < 220) width = 220;
        x = (desktop->surface.width - width) / 2;
        y = (desktop->surface.height - height) / 2;
        close_button = (gui_rect_t){x + width - 28, y + 7, 19, 18};
        ok_button = (gui_rect_t){x + width - 105, y + height - 43, 82, 27};
        if ((event->type == GUI_EVENT_MOUSE_DOWN
             && (gui_rect_contains(close_button, event->x, event->y)
                 || gui_rect_contains(ok_button, event->x, event->y)))
            || (event->type == GUI_EVENT_KEY
                && (event->key == KEY_ESCAPE || event->key == KEY_ENTER))) {
            desktop->error_visible = false;
            gui_desktop_invalidate_all(desktop);
            gui_request_paint();
        }
        /* El aviso es modal: nunca deja que el clic o la tecla alcance a la
           ventana que queda debajo. */
        return;
    }

    /* Screensavers must close before normal windows/apps consume the input.
       Without this, SSLOGO can remain active behind the desktop and leave
       black regions while other apps keep receiving events. */
    program = desktop->last_program;
    while (program) {
        if (program->handle_event) {
#ifdef BLES_VISION_SERVICE
            if (program->callback_pid == task_current_pid()) {
                if (program->handle_event(program, desktop, event)) return;
                program = program->prev;
                continue;
            }
#endif
            if (program->callback_pid) {
                uint32_t arguments[3] = {
                    (uint32_t)(uintptr_t)program,
                    (uint32_t)(uintptr_t)desktop, 0U};
                if (task_queue_user_upcall(program->callback_pid,
                        (uint32_t)(uintptr_t)program->handle_event,
                        arguments, 3, event, sizeof(*event), 2)) return;
            } else if (program->handle_event(program, desktop, event)) {
                return;
            }
        }
        program = program->prev;
    }
}

bool gui_desktop_request_owner_paint(gui_desktop_t *desktop,
                                     uint32_t process_id,
                                     uint32_t thread_id) {
    gui_window_t *window;
    gui_program_t *program;
    bool found = false;
    bool full_desktop = false;

    uint32_t flags;

    if (!desktop || (!process_id && !thread_id)) return false;
    flags = kspin_lock_irqsave(&g_desktop_state_lock);
    window = desktop->first_window;
    while (window) {
        if ((process_id && window->owner_pid == process_id) ||
            (thread_id && (window->content_pid == thread_id ||
                           window->event_pid == thread_id))) {
            window->dirty = true;
            found = true;
        }
        window = window->next;
    }
    program = desktop->first_program;
    while (program) {
        if (thread_id && program->callback_pid == thread_id) {
            program->paint_requested = true;
            found = true;
            full_desktop = true;
        }
        program = program->next;
    }
    kspin_unlock_irqrestore(&g_desktop_state_lock, flags);

    /* Los gui_program históricos (principalmente screensavers) no poseen una
     * ventana que delimite su dibujo. Sólo ellos requieren invalidación total.
     * Las aplicaciones modernas quedan limitadas a sus propias ventanas. */
    if (full_desktop) gui_desktop_invalidate_all(desktop);
    return found;
}

static void gui_desktop_paint_windows_layer(gui_desktop_t *desktop,
                                            gui_surface_t *surface) {
    gui_window_t *window;
    gui_rect_t screen;
    gui_rect_t win_clip;
    gui_rect_t content_clip;
    gui_rect_t saved_clip;

    if (!desktop || !surface) return;
    screen = gui_gfx_get_clip(surface);
    window = desktop->first_window;
    while (window) {
        bool intersects = window->visible &&
            gui_rect_intersect(window->bounds, screen, &win_clip);
        if (intersects)
            gui_window_paint(surface, window, screen);
        if (intersects && window->streaming_content && window->stream_ready) {
            gui_window_paint_stream_content(surface, window, screen);
        } else if (intersects && window->content_paint) {
            if (window->content_pid) {
#ifdef BLES_VISION_SERVICE
                if(window->content_pid==task_current_pid()) {
                    gui_surface_t *staging=NULL;
                    if(window->dirty && gui_window_begin_content_paint(window,surface,&staging)) {
                        window->content_paint(window,staging,window->content_context);
                        (void)gui_window_capture_content(window,staging);
                        gui_window_end_content_paint(window);
                    }
                    gui_window_paint_cached_content(surface,window,screen);
                } else
#endif
                {
                bool dragging = desktop->drag_window == window;
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
                            &staging))
                        arguments[1] = (uint32_t)(uintptr_t)staging;
                    if (staging) {
                        /* Publish before waking the producer: an AP may
                         * finish the callback before this syscall returns. */
                        window->content_pending = true;
                        window->content_repaint = false;
                    }
                    if (!staging || !task_queue_window_upcall(window,
                            window->content_pid,
                            (uint32_t)(uintptr_t)window->content_paint,
                            arguments, 3, NULL, 0, -2)) {
                        window->content_pending = false;
                        if (staging) gui_window_end_content_paint(window);
                        window->content_repaint = true;
                    }
                }
                }
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
        if (intersects) gui_window_paint_widgets(surface, window, screen);
        if (intersects) gui_gpu_compositor_capture_window(window, surface);
        if (intersects) gui_window_paint_menus(surface, window);
        window = window->next;
    }
}

static void gui_desktop_paint_program_entry(gui_desktop_t *desktop,
                                             gui_program_t *program) {
    if (!desktop || !program || !program->paint) return;
#ifdef BLES_VISION_SERVICE
    /* The server paints its own shell synchronously in CPL3. Sending this
     * callback back to itself delayed client composition behind another
     * scheduler round trip and could continually request the same scene. */
    if (program->callback_pid == task_current_pid()) {
        gui_surface_t *staging = NULL;
        if (program->paint_requested && !program->paint_pending &&
            gui_program_prepare_paint(program, &desktop->surface, &staging)) {
            program->paint_pending = true;
            program->paint(program, desktop, staging);
            program->paint_requested = false;
            gui_program_finish_paint(program);
        }
        if (program->paint_ready)
            gui_program_composite_paint(program, &desktop->surface);
        return;
    }
#endif
    if (program->callback_pid) {
        gui_surface_t *staging = NULL;
        /* A pending asynchronous repaint does not invalidate the completed
         * cache. Keep it visible until the producer publishes its next frame,
         * just as window content does: slow savers otherwise vanish/flicker. */
        if (program->paint_ready)
            gui_program_composite_paint(program, &desktop->surface);
        if (program->paint_requested && !program->paint_pending &&
            gui_program_prepare_paint(program, &desktop->surface, &staging)) {
            uint32_t arguments[3] = {
                (uint32_t)(uintptr_t)program,
                (uint32_t)(uintptr_t)desktop,
                (uint32_t)(uintptr_t)staging};
            program->paint_requested = false;
            program->paint_pending = true;
            if (task_queue_user_upcall(program->callback_pid,
                    (uint32_t)(uintptr_t)program->paint, arguments, 3,
                    NULL, 0, -3))
                ; /* Completion may already have cleared paint_pending. */
            else {
                program->paint_pending = false;
                program->paint_requested = true;
            }
        }
    } else {
        program->paint(program, desktop, &desktop->surface);
    }
}

static void gui_desktop_paint_shell_overlays(gui_desktop_t *desktop) {
    gui_surface_t *surface = &desktop->surface;
    gui_rect_t saved = gui_gfx_get_clip(surface);
    for (gui_program_t *p = desktop->first_program; p; p = p->next) {
        if (!p->scene_owner || !p->paint_ready) continue;
        uint32_t count = p->paint_overlay_count > 12U ? 12U : p->paint_overlay_count;
        for (uint32_t i = 0; i < count; i++) {
            gui_rect_t clipped;
            if (!gui_rect_intersect(saved, p->paint_overlays[i], &clipped)) continue;
            gui_gfx_set_clip(surface, clipped);
            gui_program_composite_paint(p, surface);
        }
    }
    gui_gfx_set_clip(surface, saved);
}

void gui_desktop_paint_programs(gui_desktop_t *desktop) {
    gui_program_t *program;

    if (!desktop) return;

    /* Layer 0: Vision shell.  It is asynchronous and changes only when the
     * shell itself changes. */
    for (program = desktop->first_program; program; program = program->next) {
        if (program->scene_owner)
            gui_desktop_paint_program_entry(desktop, program);
    }
    for (program = desktop->first_program; program; program = program->next) {
        if (program->scene_owner &&
            (program->paint_requested || program->paint_pending ||
             !program->paint_ready))
            return;
    }

    /* Layer 1: application windows.  Keep this hot raster/composition path in
     * the compositor for now instead of bouncing every client update through
     * Vision.  Vision still owns desktop policy and the shell; v4 can move
     * this layer to shared user surfaces without per-primitive syscalls. */
    gui_desktop_paint_windows_layer(desktop, &desktop->surface);

    /* Shell menus must cover every application, including later repaints. */
    gui_desktop_paint_shell_overlays(desktop);

    /* Layer 2: fullscreen/overlay gui_programs such as screensavers. */
    for (program = desktop->first_program; program; program = program->next) {
        if (!program->scene_owner)
            gui_desktop_paint_program_entry(desktop, program);
    }
}

bool gui_desktop_scene_waiting(const gui_desktop_t *desktop) {
    const gui_program_t *program;
    if (!desktop) return false;
    for (program = desktop->first_program; program; program = program->next) {
        if (!program->scene_owner) continue;
        return program->paint_requested || program->paint_pending ||
               !program->paint_ready;
    }
    return false;
}

void gui_desktop_paint(gui_desktop_t *desktop) {
    gui_desktop_paint_lock();
    gui_compositor_paint(desktop);
    gui_desktop_paint_unlock();
}

bool gui_desktop_has_dirty(const gui_desktop_t *desktop) {
    const gui_window_t *window;

    if (!desktop) return false;
    if (desktop->dirty_valid) return true;
    window = desktop->first_window;
    while (window) {
        /* A GPU viewport can publish a new frame without changing any CPU
         * window pixels.  Treat that as pending work so gui_request_paint()
         * does not turn it into an unnecessary full-desktop invalidation. */
        if (window->dirty || window->gpu_view_frame_dirty) return true;
        window = window->next;
    }
    return false;
}

static void gui_desktop_record_damage(gui_desktop_t *desktop,
                                      gui_rect_t rect,
                                      const gui_program_t *publisher) {
    gui_desktop_t *active = gui_get_desktop();
    gui_program_t *program;
    gui_rect_t screen;
    gui_rect_t clipped;

    /* Ring 3 applications receive a geometry-only desktop proxy.  Desktop
       invalidation is global by design, so never record damage in that copy. */
    if (active) desktop = active;
    if (!desktop || rect.w <= 0 || rect.h <= 0) return;
    screen = (gui_rect_t){0, 0, desktop->surface.width,
                          desktop->surface.height};
    if (!gui_rect_intersect(screen, rect, &clipped)) return;

    desktop->dirty_generation++;
    if (desktop->dirty_valid)
        desktop->dirty_rect = gui_rect_union(desktop->dirty_rect, clipped);
    else {
        desktop->dirty_rect = clipped;
        desktop->dirty_valid = true;
    }

    /* Vision v3.6 owns only the shell layer (wallpaper, desktop icons and
     * Deskbar), not application pixels.  Application/window damage must not
     * force another Ring-3 Vision pass: doing that serialized every client
     * frame behind a full scene round-trip. Damage from each program requests
     * its own cache, including animated overlays; other clients leave the
     * shell cache alone. Kernel window lifecycle code uses
     * gui_desktop_request_scene_owner_rect() explicitly for Deskbar changes. */
    for (program = desktop->first_program; program; program = program->next) {
        if (program != publisher &&
            task_current_pid() == program->callback_pid) {
            program->paint_requested = true;
            break;
        }
    }
}

static void gui_desktop_request_scene_owner_rect(gui_desktop_t *desktop,
                                                  gui_rect_t rect) {
    gui_program_t *program;
    if (!desktop) return;
    for (program = desktop->first_program; program; program = program->next) {
        if (!program->scene_owner) continue;
        program->paint_requested = true;
        gui_desktop_record_damage(desktop, rect, program);
        return;
    }
    gui_desktop_record_damage(desktop, rect, NULL);
}

void gui_desktop_invalidate_rect(gui_desktop_t *desktop, gui_rect_t rect) {
    gui_desktop_record_damage(desktop, rect, NULL);
}

void gui_desktop_invalidate_overlay_rect(gui_desktop_t *desktop, gui_rect_t rect) {
    gui_desktop_t *active = gui_get_desktop();
    gui_program_t *program;
    const gui_program_t *scene_owner = NULL;

    /* The outline/animation is rendered by the kernel compositor above the
     * cached Vision shell.  Marking the shell dirty here would bounce every
     * mouse packet through Ring 3 and serialize the whole desktop while a
     * window is being dragged. */
    if (active) desktop = active;
    if (!desktop) return;
    for (program = desktop->first_program; program; program = program->next) {
        if (program->scene_owner) {
            scene_owner = program;
            break;
        }
    }
    gui_desktop_record_damage(desktop, rect, scene_owner);
}

void gui_desktop_publish_program_rect(gui_desktop_t *desktop,
                                      const gui_program_t *program,
                                      gui_rect_t rect) {
    gui_desktop_record_damage(desktop, rect, program);
}

void gui_desktop_invalidate_all(gui_desktop_t *desktop) {
    if (!desktop) return;
    gui_desktop_invalidate_rect(desktop, (gui_rect_t){0, 0,
        desktop->surface.width, desktop->surface.height});
}

void gui_desktop_reflow(gui_desktop_t *desktop) {
    gui_window_t *window;
    const int reserved_bottom = 30;
    int work_h;
    uint32_t flags;

    if (!desktop) return;

    if (desktop->mouse_x >= desktop->surface.width)
        desktop->mouse_x = desktop->surface.width > 0
                         ? desktop->surface.width - 1 : 0;
    if (desktop->mouse_y >= desktop->surface.height)
        desktop->mouse_y = desktop->surface.height > 0
                         ? desktop->surface.height - 1 : 0;

    work_h = (int)desktop->surface.height - reserved_bottom;
    if (work_h < GUI_TITLEBAR_HEIGHT) work_h = desktop->surface.height;

    flags = kspin_lock_irqsave(&g_desktop_state_lock);
    window = desktop->first_window;
    while (window) {
        gui_rect_t original = window->bounds;
        gui_rect_t adjusted = original;
        bool forced = false;

        /* Never resize a fixed-layout application merely because the display
           got smaller. That used to squeeze widgets and content into a new
           outer rectangle without asking the application to relayout, which
           produced the apparent "distorted" windows. Keep the client geometry
           intact and only move enough of the title bar on-screen. */
        if (window->mode_restore_valid) {
            gui_rect_t saved = window->mode_restore_bounds;
            if (saved.x >= 0 && saved.y >= 0 &&
                saved.x + saved.w <= (int)desktop->surface.width &&
                saved.y + saved.h <= work_h) {
                adjusted = saved;
                window->mode_restore_valid = false;
            }
        }

        if (adjusted.w <= (int)desktop->surface.width) {
            if (adjusted.x + adjusted.w > (int)desktop->surface.width) {
                adjusted.x = (int)desktop->surface.width - adjusted.w;
                forced = true;
            }
            if (adjusted.x < 0) {
                adjusted.x = 0;
                forced = true;
            }
        } else if (adjusted.x != 0) {
            adjusted.x = 0;
            forced = true;
        }

        if (adjusted.h <= work_h) {
            if (adjusted.y + adjusted.h > work_h) {
                adjusted.y = work_h - adjusted.h;
                forced = true;
            }
            if (adjusted.y < 0) {
                adjusted.y = 0;
                forced = true;
            }
        } else if (adjusted.y != 0) {
            adjusted.y = 0;
            forced = true;
        }

        if (forced && !window->mode_restore_valid) {
            window->mode_restore_bounds = original;
            window->mode_restore_valid = true;
        }
        if (adjusted.x != window->bounds.x ||
            adjusted.y != window->bounds.y) {
            window->bounds.x = adjusted.x;
            window->bounds.y = adjusted.y;
        }
        window->paint_bounds_valid = false;
        window->dirty = true;
        window = window->next;
    }
    kspin_unlock_irqrestore(&g_desktop_state_lock, flags);
}

void gui_desktop_reset_after_mode_change(gui_desktop_t *desktop) {
    gui_window_t *window;
    gui_program_t *program;
    uint32_t flags;

    if (!desktop) return;
    /* The old cursor backing belongs to the previous surface dimensions. */
    desktop->cursor_valid = false;
    desktop->cursor_trail_count = 0U;
    desktop->cursor_paint_count = 0U;
    desktop->paint_valid = false;
    desktop->dirty_valid = false;
    desktop->drag_window = NULL;
    desktop->resize_window = NULL;
    desktop->drag_outline_visible = false;

    flags = kspin_lock_irqsave(&g_desktop_state_lock);
    window = desktop->first_window;
    while (window) {
        /* A callback may already be queued with a pointer to its borrowed
           staging view. Keep that buffer alive until the callback completes;
           finish_content_paint() rejects it if reflow changed the client rect.
           Mark a follow-up repaint so the new geometry is produced afterward. */
        window->content_repaint = true;
        window->paint_bounds_valid = false;
        window->dirty = true;
        window = window->next;
    }
    program = desktop->first_program;
    while (program) {
        program->paint_requested = true;
        /* Do not clear paint_pending: an already queued callback still owns
           its staging pointer. Existing caches are retained until the normal
           prepare path can resize them, but never composited across modes. */
        if (program->paint_cache.width != desktop->surface.width ||
            program->paint_cache.height != desktop->surface.height ||
            program->paint_cache.pitch != desktop->surface.pitch)
            program->paint_ready = false;
        program = program->next;
    }
    kspin_unlock_irqrestore(&g_desktop_state_lock, flags);

    gui_desktop_invalidate_all(desktop);
    gui_request_paint();
}

void gui_desktop_set_cursor_trail(gui_desktop_t *desktop, bool enabled) {
    /* Preferences belong to the real desktop, never its Ring-3 proxy. */
    if (gui_get_desktop()) desktop = gui_get_desktop();
    if (!desktop) return;
    if (desktop->cursor_trail_enabled == enabled) return;
    desktop->cursor_trail_enabled = enabled;
    gui_desktop_clear_cursor_trail(desktop);
    gui_desktop_invalidate_all(desktop);
    gui_request_paint();
}

bool gui_desktop_cursor_trail_enabled(const gui_desktop_t *desktop) {
    /* Preferences belong to the real desktop, never its Ring-3 proxy. */
    if (gui_get_desktop()) desktop = gui_get_desktop();
    return desktop && desktop->cursor_trail_enabled;
}


bool gui_desktop_set_cursor_image(gui_desktop_t *desktop,
                                  const uint32_t *pixels,
                                  uint16_t width, uint16_t height,
                                  int hotspot_x, int hotspot_y,
                                  uint32_t owner_pid) {
    uint32_t row;
    uint32_t col;
    if (!desktop || !pixels || !width || !height ||
        width > GUI_CURSOR_WIDTH || height > GUI_CURSOR_HEIGHT ||
        hotspot_x < 0 || hotspot_y < 0 || hotspot_x >= (int)width ||
        hotspot_y >= (int)height || !owner_pid) return false;
    for (row = 0U; row < GUI_CURSOR_HEIGHT; row++)
        for (col = 0U; col < GUI_CURSOR_WIDTH; col++)
            desktop->cursor_pixels[row * GUI_CURSOR_WIDTH + col] =
                row < height && col < width ? pixels[row * width + col] : 0U;
    desktop->cursor_width = width;
    desktop->cursor_height = height;
    desktop->cursor_hotspot_x = (int16_t)hotspot_x;
    desktop->cursor_hotspot_y = (int16_t)hotspot_y;
    desktop->cursor_owner_pid = owner_pid;
    desktop->cursor_custom = true;
    desktop->cursor_valid = false;
    gui_desktop_clear_cursor_trail(desktop);
    gui_desktop_invalidate_all(desktop);
    gui_request_paint();
    return true;
}

void gui_desktop_reset_cursor(gui_desktop_t *desktop, uint32_t owner_pid) {
    if (!desktop || !desktop->cursor_custom) return;
    if (owner_pid && desktop->cursor_owner_pid != owner_pid) return;
    desktop->cursor_custom = false;
    desktop->cursor_width = 0U;
    desktop->cursor_height = 0U;
    desktop->cursor_hotspot_x = 0;
    desktop->cursor_hotspot_y = 0;
    desktop->cursor_owner_pid = 0U;
    desktop->cursor_style = GUI_CURSOR_ARROW;
    desktop->cursor_valid = false;
    gui_desktop_clear_cursor_trail(desktop);
    gui_desktop_invalidate_all(desktop);
    gui_request_paint();
}

void gui_desktop_set_cursor_style(gui_desktop_t *desktop,
                                  gui_cursor_style_t style) {
    if (!desktop || style >= GUI_CURSOR_COUNT) return;
    if (desktop->cursor_style == style) return;
    desktop->cursor_style = style;
    desktop->cursor_valid = false;
    gui_desktop_clear_cursor_trail(desktop);
    gui_desktop_invalidate_rect(desktop, (gui_rect_t){
        desktop->mouse_x - GUI_CURSOR_WIDTH, desktop->mouse_y - GUI_CURSOR_HEIGHT,
        GUI_CURSOR_WIDTH * 2, GUI_CURSOR_HEIGHT * 2});
    gui_request_paint();
}

void gui_desktop_show_error(gui_desktop_t *desktop, const char *title,
                            const char *text) {
    gui_desktop_show_alert(desktop, GUI_ALERT_ERROR, title, text, 0);
}

void gui_desktop_show_alert(gui_desktop_t *desktop, gui_alert_kind_t kind,
                            const char *title, const char *text,
                            int32_t code) {
    if (!desktop) return;
    if (kind > GUI_ALERT_NETWORK) kind = GUI_ALERT_ERROR;
    copy_text(desktop->error_title, sizeof(desktop->error_title),
              title ? title : "Error");
    copy_text(desktop->error_text, sizeof(desktop->error_text),
              text ? text : "Operacion no completada.");
    desktop->error_kind = kind;
    desktop->error_code = code;
    desktop->error_visible = true;
    gui_desktop_invalidate_all(desktop);
    gui_request_paint();
}

void gui_desktop_set_drag_outline(gui_desktop_t *desktop, bool enabled) {
    /* Preferences belong to the real desktop, never its Ring-3 proxy. */
    if (gui_get_desktop()) desktop = gui_get_desktop();
    if (!desktop) return;
    desktop->drag_outline_enabled = enabled;
    if (!enabled) desktop->drag_outline_visible = false;
}

bool gui_desktop_drag_outline_enabled(const gui_desktop_t *desktop) {
    /* Preferences belong to the real desktop, never its Ring-3 proxy. */
    if (gui_get_desktop()) desktop = gui_get_desktop();
    return desktop && desktop->drag_outline_enabled;
}

static int gui_lerp_int(int from, int to, uint32_t t256) {
    int64_t delta = (int64_t)to - (int64_t)from;
    return from + (int)((delta * (int64_t)t256) / 256LL);
}

static void gui_desktop_start_window_outline_animation(
    gui_desktop_t *desktop, gui_rect_t from, gui_rect_t to,
    uint32_t start_tick, uint32_t duration_ticks, uint32_t restore_window_id) {
    if (!desktop) return;
    if (!duration_ticks) duration_ticks = 1U;
    if (desktop->minimize_animation_active)
        gui_desktop_invalidate_overlay_rect(desktop, desktop->minimize_animation_bounds);
    desktop->minimize_animation_active = true;
    desktop->minimize_animation_from = from;
    desktop->minimize_animation_to = to;
    desktop->minimize_animation_bounds = from;
    desktop->minimize_animation_start_tick = start_tick;
    desktop->minimize_animation_duration_ticks = duration_ticks;
    desktop->minimize_animation_restore_window_id = restore_window_id;
    gui_desktop_invalidate_overlay_rect(desktop, from);
    gui_desktop_invalidate_overlay_rect(desktop, to);
    gui_request_paint();
}

void gui_desktop_start_minimize_animation(gui_desktop_t *desktop,
                                          gui_rect_t from, gui_rect_t to,
                                          uint32_t start_tick,
                                          uint32_t duration_ticks) {
    gui_desktop_start_window_outline_animation(desktop, from, to, start_tick,
                                               duration_ticks, 0U);
}

void gui_desktop_start_restore_animation(gui_desktop_t *desktop,
                                         uint32_t window_id,
                                         gui_rect_t from, gui_rect_t to,
                                         uint32_t start_tick,
                                         uint32_t duration_ticks) {
    if (!window_id) return;
    gui_desktop_start_window_outline_animation(desktop, from, to, start_tick,
                                               duration_ticks, window_id);
}

static gui_window_t *gui_desktop_window_by_id(gui_desktop_t *desktop,
                                               uint32_t window_id) {
    gui_window_t *window;
    if (!desktop || !window_id) return NULL;
    window = desktop->first_window;
    while (window) {
        if (window->listed && window->id == window_id) return window;
        window = window->next;
    }
    return NULL;
}

bool gui_desktop_tick_minimize_animation(gui_desktop_t *desktop,
                                         uint32_t now_tick) {
    gui_rect_t old_bounds;
    gui_rect_t next;
    uint32_t elapsed;
    uint32_t t256;
    uint32_t eased;

    if (!desktop || !desktop->minimize_animation_active) return false;
    old_bounds = desktop->minimize_animation_bounds;
    elapsed = now_tick - desktop->minimize_animation_start_tick;
    if (elapsed >= desktop->minimize_animation_duration_ticks) {
        uint32_t restore_window_id =
            desktop->minimize_animation_restore_window_id;
        desktop->minimize_animation_bounds = desktop->minimize_animation_to;
        desktop->minimize_animation_active = false;
        desktop->minimize_animation_restore_window_id = 0U;
        gui_desktop_invalidate_overlay_rect(desktop, old_bounds);
        gui_desktop_invalidate_overlay_rect(desktop, desktop->minimize_animation_to);

        if (restore_window_id) {
            gui_window_t *window =
                gui_desktop_window_by_id(desktop, restore_window_id);
            if (window && window->minimized) {
                gui_window_restore(window);
                gui_desktop_raise_window(desktop, window);
                gui_desktop_focus_window(desktop, window);
            }
        }
        gui_request_paint();
        return true;
    }

    t256 = (elapsed * 256U) / desktop->minimize_animation_duration_ticks;
    /* smoothstep(0..1): mantiene el arranque suave y acelera hacia Deskbar. */
    eased = (t256 * t256 * (768U - 2U * t256)) >> 16;
    if (eased > 256U) eased = 256U;

    next.x = gui_lerp_int(desktop->minimize_animation_from.x,
                          desktop->minimize_animation_to.x, eased);
    next.y = gui_lerp_int(desktop->minimize_animation_from.y,
                          desktop->minimize_animation_to.y, eased);
    next.w = gui_lerp_int(desktop->minimize_animation_from.w,
                          desktop->minimize_animation_to.w, eased);
    next.h = gui_lerp_int(desktop->minimize_animation_from.h,
                          desktop->minimize_animation_to.h, eased);
    if (next.w < 2) next.w = 2;
    if (next.h < 2) next.h = 2;

    if (next.x == old_bounds.x && next.y == old_bounds.y &&
        next.w == old_bounds.w && next.h == old_bounds.h)
        return false;

    desktop->minimize_animation_bounds = next;
    gui_desktop_invalidate_overlay_rect(desktop, old_bounds);
    gui_desktop_invalidate_overlay_rect(desktop, next);
    gui_request_paint();
    return true;
}

