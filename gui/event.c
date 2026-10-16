#include "gui.h"
#include "../sdk/include/bleskernos_input.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/keyboard.h"
#include "../kernel/include/mouse.h"
#include "../kernel/include/task.h"

/* Estado reconstruido en el orden de la cola. Consultar sólo el estado global
 * perdía Shift cuando press+tecla+release llegaban antes del siguiente frame. */
static kbd_modifiers_t gui_key_modifiers;

static uint8_t g_repeat_key;
static uint32_t g_repeat_due;
static gui_window_t *g_repeat_window;
static uint32_t input_now_ms(void) {
    uint32_t hz=pit_get_frequency_hz();
    return hz ? (uint32_t)((uint64_t)pit_get_ticks()*1000U/hz) : 0U;
}
static uint8_t repeat_identity(uint8_t key) {
    if(key==0xD1U) return 0xF1U;
    return key>='A' && key<='Z' ? (uint8_t)(key+32) : key;
}
static bool repeat_same_key(uint8_t held,uint8_t released) {
    if(repeat_identity(held)==repeat_identity(released)) return true;
    const unsigned char *plain=(const unsigned char *)"1234567890-=[]\\;'`,./";
    const unsigned char *shift=(const unsigned char *)"!@#$%^&*()_+{}|:\"~<>?";
    if(!bk_input_preference_get(BK_INPUT_LAYOUT)) {
        for(uint32_t i=0;plain[i];i++) if(shift[i]==held && plain[i]==released) return true;
    } else {
        static const uint8_t es_plain[]={'1','2','3','4','5','6','7','8','9','0','\'',0xA1,'`','+',0xF1,'\'',0xBA,0xE7,',','.','-','<'};
        static const uint8_t es_shift[]={'!','"',0xB7,'$','%','&','/','(',')','=','?',0xBF,'^','*',0xD1,'"',0xAA,0xC7,';',':','_','>'};
        for(uint32_t i=0;i<sizeof(es_plain);i++) if(es_shift[i]==held && es_plain[i]==released) return true;
    }
    return false;
}
static bool repeatable_key(uint8_t key) {
    return (key>=32U && key<127U) || key==KEY_BACKSPACE ||
           key==KEY_DELETE || (key>=KEY_UP && key<=KEY_PGDN) || key>=0xA0U;
}

static bool queue_full(gui_event_queue_t *queue) {
    return ((queue->head + 1) % GUI_MAX_EVENTS) == queue->tail;
}

static bool queue_empty(gui_event_queue_t *queue) {
    return queue->head == queue->tail;
}

static void push_event(gui_event_queue_t *queue, gui_event_t event) {
    if (!queue || queue_full(queue)) return;
    queue->queue[queue->head] = event;
    queue->head = (queue->head + 1) % GUI_MAX_EVENTS;
}

void gui_event_queue_reset(gui_event_queue_t *queue) {
    if (!queue) return;
    task_preempt_disable();
    queue->head = 0;
    queue->tail = 0;
    task_preempt_enable();
}

bool gui_event_queue_push(gui_event_queue_t *queue, const gui_event_t *event) {
    bool pushed = false;
    if (!queue || !event) return false;
    task_preempt_disable();
    if (!queue_full(queue)) {
        queue->queue[queue->head] = *event;
        queue->head = (queue->head + 1) % GUI_MAX_EVENTS;
        pushed = true;
    }
    task_preempt_enable();
    return pushed;
}

bool gui_event_queue_pop(gui_event_queue_t *queue, gui_event_t *event) {
    bool ok = false;
    if (!queue || !event) return false;
    task_preempt_disable();
    if (!queue_empty(queue)) {
        *event = queue->queue[queue->tail];
        queue->tail = (queue->tail + 1) % GUI_MAX_EVENTS;
        ok = true;
    }
    task_preempt_enable();
    return ok;
}

void gui_event_init(gui_event_queue_t *queue) {
    mouse_state_t mouse;
    if (!queue) return;
    queue->head = 0;
    queue->tail = 0;
    mouse_get_state(&mouse);
    queue->last_mouse_buttons = mouse.buttons;
    queue->last_mouse_x = mouse.x;
    queue->last_mouse_y = mouse.y;
    queue->last_mouse_wheel = mouse.wheel;
}

static void poll_mouse_snapshot(gui_event_queue_t *queue,mouse_state_t mouse) {
    uint8_t changed = (uint8_t)(mouse.buttons ^ queue->last_mouse_buttons);

    if (mouse.x != queue->last_mouse_x || mouse.y != queue->last_mouse_y) {
        push_event(queue, (gui_event_t){
            .type = GUI_EVENT_MOUSE_MOVE, .x = mouse.x, .y = mouse.y,
            .dx = mouse.x - queue->last_mouse_x,
            .dy = mouse.y - queue->last_mouse_y, .buttons = mouse.buttons
        });
    }

    if (mouse.wheel != queue->last_mouse_wheel) {
        push_event(queue, (gui_event_t){
            .type = GUI_EVENT_MOUSE_WHEEL, .x = mouse.x, .y = mouse.y,
            .dy = mouse.wheel - queue->last_mouse_wheel,
            .buttons = mouse.buttons
        });
    }

    if (changed & MOUSE_LEFT_BUTTON) {
        push_event(queue, (gui_event_t){
            .type = (mouse.buttons & MOUSE_LEFT_BUTTON)
                ? GUI_EVENT_MOUSE_DOWN : GUI_EVENT_MOUSE_UP,
            .x = mouse.x, .y = mouse.y, .buttons = mouse.buttons,
            .button = MOUSE_LEFT_BUTTON
        });
    }
    if (changed & MOUSE_RIGHT_BUTTON) {
        push_event(queue, (gui_event_t){
            .type = (mouse.buttons & MOUSE_RIGHT_BUTTON)
                ? GUI_EVENT_MOUSE_DOWN : GUI_EVENT_MOUSE_UP,
            .x = mouse.x, .y = mouse.y, .buttons = mouse.buttons,
            .button = MOUSE_RIGHT_BUTTON
        });
    }
    if (changed & MOUSE_MIDDLE_BUTTON) {
        push_event(queue, (gui_event_t){
            .type = (mouse.buttons & MOUSE_MIDDLE_BUTTON)
                ? GUI_EVENT_MOUSE_DOWN : GUI_EVENT_MOUSE_UP,
            .x = mouse.x, .y = mouse.y, .buttons = mouse.buttons,
            .button = MOUSE_MIDDLE_BUTTON
        });
    }

    queue->last_mouse_buttons=mouse.buttons;queue->last_mouse_x=mouse.x;
    queue->last_mouse_y=mouse.y;queue->last_mouse_wheel=mouse.wheel;
}

void gui_event_poll(gui_event_queue_t *queue) {
    mouse_state_t mouse;
    kbd_modifiers_t mods;
    kbd_key_event_t key_event;

    if (!queue) return;
    mouse_get_state(&mouse);
    uint32_t mouse_count=0;
    while(((queue->tail+GUI_MAX_EVENTS-queue->head-1U)%GUI_MAX_EVENTS)>=5U && mouse_pop_event(&mouse)) {
        poll_mouse_snapshot(queue,mouse);mouse_count++;
    }
    if(!mouse_count && ((queue->tail+GUI_MAX_EVENTS-queue->head-1U)%GUI_MAX_EVENTS)>=5U)
        poll_mouse_snapshot(queue,mouse);

    gui_desktop_t *repeat_desktop=gui_get_desktop();
    gui_window_t *focused=repeat_desktop ? repeat_desktop->focused_window : NULL;
    uint32_t now=input_now_ms();
    if (focused!=g_repeat_window) {g_repeat_key=0U;g_repeat_window=focused;}
    while (!queue_full(queue) && kbd_next_event(&key_event)) {
        if (repeatable_key(key_event.key)) {
            if (key_event.pressed) {
                if (repeat_same_key(g_repeat_key,key_event.key)) continue;
                g_repeat_key=key_event.key;
                g_repeat_due=now+bk_input_preference_get(BK_INPUT_REPEAT_DELAY);
            } else if (repeat_same_key(g_repeat_key,key_event.key)) g_repeat_key=0U;
        }
        /* Actualizar primero: las ventanas normales no reciben key-up, pero
         * el estado interno sí debe observar la liberación de Shift/Ctrl/Alt. */
        switch (key_event.key) {
            case KEY_SHIFT: gui_key_modifiers.shift = key_event.pressed; break;
            case KEY_CTRL: gui_key_modifiers.ctrl = key_event.pressed; break;
            case KEY_ALT: gui_key_modifiers.alt = key_event.pressed; break;
            default: break;
        }
        mods = gui_key_modifiers;
        if (!key_event.pressed) {
            gui_desktop_t *desktop = gui_get_desktop();
            if (!desktop || !desktop->focused_window ||
                !desktop->focused_window->extended_events)
                continue;
        }
        push_event(queue, (gui_event_t){
            key_event.pressed ? GUI_EVENT_KEY : GUI_EVENT_KEY_UP,
            mouse.x, mouse.y, 0, 0, mouse.buttons, 0, key_event.key,
            mods.shift, mods.ctrl, mods.alt
        });
    }

    if (g_repeat_key && focused && (int32_t)(now-g_repeat_due)>=0) {
        push_event(queue,(gui_event_t){GUI_EVENT_KEY,mouse.x,mouse.y,0,0,mouse.buttons,0,
                   (char)g_repeat_key,gui_key_modifiers.shift,gui_key_modifiers.ctrl,gui_key_modifiers.alt});
        g_repeat_due=now+bk_input_preference_get(BK_INPUT_REPEAT_INTERVAL);
    }
    gui_widget_tick_caret(now);

}

bool gui_event_next(gui_event_queue_t *queue, gui_event_t *event) {
    return gui_event_queue_pop(queue, event);
}
