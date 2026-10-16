#include "../../include/mouse.h"
#include "../../include/memory.h"
#include "../../include/task.h"
#include "../../include/klock.h"
#include "../../../sdk/include/bleskernos_input.h"

static const mouse_driver_ops_t *g_mouse_driver;
static uint8_t g_fallback_sensitivity = 3;
static mouse_state_t g_injected_mouse;
static bool g_injected_active;
static kspinlock_t injected_lock=KSPINLOCK_INITIALIZER;
static int32_t g_bound_width = 800;
static int32_t g_bound_height = 600;

#define MOUSE_EVENTS 64U
static mouse_state_t mouse_events[MOUSE_EVENTS];
static uint32_t mouse_head,mouse_tail;
static bool mouse_edges[MOUSE_EVENTS];
static uint8_t producer_buttons;
static int32_t producer_wheel;
static kspinlock_t mouse_events_lock=KSPINLOCK_INITIALIZER;
void mouse_publish_state(const mouse_state_t *state) {
 if(!state)return;
 if(g_injected_active && state->device_id!=0xfeU && state->device_id!=0xffU)return;
 uint32_t flags=kspin_lock_irqsave(&mouse_events_lock);
 bool edge=state->buttons!=producer_buttons || state->wheel!=producer_wheel;
 producer_buttons=state->buttons;producer_wheel=state->wheel;
 if(mouse_head!=mouse_tail) {
  uint32_t previous=(mouse_head+MOUSE_EVENTS-1U)%MOUSE_EVENTS;
  /* Movement may be coalesced; button and wheel transitions remain distinct. */
  if(!edge && !mouse_edges[previous]) {
   mouse_events[previous]=*state;kspin_unlock_irqrestore(&mouse_events_lock,flags);return;
  }
 }
 uint32_t next=(mouse_head+1U)%MOUSE_EVENTS;
 if(next==mouse_tail)mouse_tail=(mouse_tail+1U)%MOUSE_EVENTS;
 mouse_events[mouse_head]=*state;mouse_edges[mouse_head]=edge;mouse_head=next;
 kspin_unlock_irqrestore(&mouse_events_lock,flags);
}
bool mouse_pop_event(mouse_state_t *state) {
 if(!state)return false;
 uint32_t flags=kspin_lock_irqsave(&mouse_events_lock);
 if(mouse_head==mouse_tail){kspin_unlock_irqrestore(&mouse_events_lock,flags);return false;}
 *state=mouse_events[mouse_tail];mouse_tail=(mouse_tail+1U)%MOUSE_EVENTS;
 kspin_unlock_irqrestore(&mouse_events_lock,flags);
 if(bk_input_preference_get(BK_INPUT_SWAP_BUTTONS)) {
  uint8_t b=state->buttons;state->buttons=(b&~3U)|((b&1U)<<1)|((b&2U)>>1);
 }
 return true;
}

static int32_t mouse_clamp(int32_t value, int32_t limit) {
    if (value < 0) return 0;
    if (limit > 0 && value >= limit) return limit - 1;
    return value;
}

bool mouse_register_driver(const mouse_driver_ops_t *ops) {
    if (!ops || !ops->is_present || !ops->get_state || !ops->set_bounds ||
        !ops->set_position || !ops->set_sensitivity ||
        !ops->get_sensitivity) return false;
    g_mouse_driver = ops;
    /* Device changes preserve the desktop-wide preference. */
    ops->set_sensitivity(g_fallback_sensitivity);
    return true;
}

void mouse_init(void) {}

bool mouse_is_present(void) {
    return g_injected_active || (g_mouse_driver && g_mouse_driver->is_present());
}

void mouse_get_state(mouse_state_t *state) {
    if (!state) return;
    uint32_t injected_flags=kspin_lock_irqsave(&injected_lock);
    if (g_injected_active) *state = g_injected_mouse;
    else kmemset(state, 0, sizeof(*state));
    bool injected=g_injected_active;
    kspin_unlock_irqrestore(&injected_lock,injected_flags);
    if (!injected && g_mouse_driver) g_mouse_driver->get_state(state);
    if (bk_input_preference_get(BK_INPUT_SWAP_BUTTONS)) {
        uint8_t buttons = state->buttons;
        state->buttons = (uint8_t)((buttons & ~3U) | ((buttons & 1U) << 1) | ((buttons & 2U) >> 1));
    }
}

void mouse_set_bounds(int32_t width, int32_t height) {
    uint32_t injected_flags=kspin_lock_irqsave(&injected_lock);
    if (width > 0) g_bound_width = width;
    if (height > 0) g_bound_height = height;
    if (g_injected_active) {
        g_injected_mouse.x = mouse_clamp(g_injected_mouse.x, g_bound_width);
        g_injected_mouse.y = mouse_clamp(g_injected_mouse.y, g_bound_height);
    }
    kspin_unlock_irqrestore(&injected_lock,injected_flags);
    if (g_mouse_driver) g_mouse_driver->set_bounds(width, height);
}

void mouse_set_position(int32_t x, int32_t y) {
    uint32_t injected_flags=kspin_lock_irqsave(&injected_lock);
    if (g_injected_active) {
        g_injected_mouse.x = mouse_clamp(x, g_bound_width);
        g_injected_mouse.y = mouse_clamp(y, g_bound_height);
    }
    kspin_unlock_irqrestore(&injected_lock,injected_flags);
    if (g_mouse_driver) g_mouse_driver->set_position(x, y);
}

void mouse_set_sensitivity(uint8_t sensitivity) {
    if (sensitivity < 1) sensitivity = 1;
    if (sensitivity > 5) sensitivity = 5;
    g_fallback_sensitivity = sensitivity;
    if (g_mouse_driver) g_mouse_driver->set_sensitivity(sensitivity);
}

uint8_t mouse_get_sensitivity(void) {
    return g_fallback_sensitivity;
}

void mouse_inject_relative(int32_t dx, int32_t dy, int32_t wheel,
                           uint8_t buttons) {
    uint8_t sensitivity = mouse_get_sensitivity();
    mouse_state_t base;
    bool need_base = !g_injected_active;
    kmemset(&base, 0, sizeof(base));
    if (need_base) {
        if (g_mouse_driver && g_mouse_driver->is_present())
            g_mouse_driver->get_state(&base);
        else {
            base.x = g_bound_width / 2;
            base.y = g_bound_height / 2;
        }
    }
    if (sensitivity > 3U) {
        dx = dx * (int32_t)(sensitivity - 1U) / 2;
        dy = dy * (int32_t)(sensitivity - 1U) / 2;
    } else if (sensitivity < 3U) {
        dx /= (int32_t)(4U - sensitivity);
        dy /= (int32_t)(4U - sensitivity);
    }
    uint32_t injected_flags=kspin_lock_irqsave(&injected_lock);
    if (!g_injected_active) {
        g_injected_mouse = base;
        g_injected_mouse.device_id = 0xFFU;
        g_injected_mouse.packet_size = 4U;
        g_injected_mouse.present = true;
        g_injected_active = true;
    }
    g_injected_mouse.dx = dx;
    g_injected_mouse.dy = dy;
    g_injected_mouse.x = mouse_clamp(g_injected_mouse.x + dx, g_bound_width);
    g_injected_mouse.y = mouse_clamp(g_injected_mouse.y + dy, g_bound_height);
    g_injected_mouse.wheel += wheel;
    g_injected_mouse.buttons = buttons;
    g_injected_mouse.packets++;
    mouse_publish_state(&g_injected_mouse);
    kspin_unlock_irqrestore(&injected_lock,injected_flags);
}

void mouse_inject_absolute(int32_t x, int32_t y, int32_t max_x, int32_t max_y,
                           int32_t wheel, uint8_t buttons) {
    int32_t scaled_x;
    int32_t scaled_y;
    int32_t old_x;
    int32_t old_y;
    if (max_x <= 0) max_x = 1;
    if (max_y <= 0) max_y = 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > max_x) x = max_x;
    if (y > max_y) y = max_y;

    /* 0x7fff * a normal desktop dimension fits comfortably in int32_t, so
       this deliberately avoids pulling a 64-bit division helper into the
       freestanding ARM kernel. */
    scaled_x = g_bound_width > 1
        ? (x * (g_bound_width - 1)) / max_x : 0;
    scaled_y = g_bound_height > 1
        ? (y * (g_bound_height - 1)) / max_y : 0;

    uint32_t injected_flags=kspin_lock_irqsave(&injected_lock);
    if (!g_injected_active) {
        kmemset(&g_injected_mouse, 0, sizeof(g_injected_mouse));
        g_injected_mouse.x = g_bound_width / 2;
        g_injected_mouse.y = g_bound_height / 2;
        g_injected_mouse.device_id = 0xFEU;
        g_injected_mouse.packet_size = 6U;
        g_injected_mouse.present = true;
        g_injected_active = true;
    }
    old_x = g_injected_mouse.x;
    old_y = g_injected_mouse.y;
    g_injected_mouse.x = mouse_clamp(scaled_x, g_bound_width);
    g_injected_mouse.y = mouse_clamp(scaled_y, g_bound_height);
    g_injected_mouse.dx = g_injected_mouse.x - old_x;
    g_injected_mouse.dy = g_injected_mouse.y - old_y;
    g_injected_mouse.wheel += wheel;
    g_injected_mouse.buttons = buttons;
    g_injected_mouse.packets++;
    mouse_publish_state(&g_injected_mouse);
    kspin_unlock_irqrestore(&injected_lock,injected_flags);
}

void mouse_inject_disconnect(void) {
    uint32_t injected_flags=kspin_lock_irqsave(&injected_lock);
    g_injected_active = false;
    kmemset(&g_injected_mouse, 0, sizeof(g_injected_mouse));
    kspin_unlock_irqrestore(&injected_lock,injected_flags);
}
