#include <bleskernos_input_lbs.h>
#include "resources.h"
#include <string.h>
struct bk_input_state {
    bk_input_snapshot_t snapshot;
    uint8_t keys[256];
};
uint32_t bk_input_capabilities(void) {
    return BK_INPUT_CAP_KEYBOARD | BK_INPUT_CAP_MOUSE | BK_INPUT_CAP_FOCUS;
}
bk_input_state_t *bk_input_state_create(void) {
    bk_input_state_t *s = bk_sys_alloc(sizeof(*s));
    if (!s)
        return 0;
    memset(s, 0, sizeof(*s));
    s->snapshot.size = sizeof(s->snapshot);
    bk_lbs_resource_acquire();
    return s;
}
void bk_input_state_destroy(bk_input_state_t *s) {
    if (s) {
        bk_sys_free(s);
        bk_lbs_resource_release();
    }
}
int bk_input_state_event(bk_input_state_t *s, const bk_gui_event_t *e) {
    if (!s || !e)
        return BK_LBS_INVALID;
    s->snapshot.events++;
    s->snapshot.modifiers = (e->shift ? 1U : 0U) | (e->ctrl ? 2U : 0U) | (e->alt ? 4U : 0U);
    switch (e->type) {
    case BK_GUI_EVENT_KEY:
        s->keys[e->key] = 1;
        break;
    case BK_GUI_EVENT_KEY_UP:
        s->keys[e->key] = 0;
        break;
    case BK_GUI_EVENT_MOUSE_MOVE:
    case BK_GUI_EVENT_MOUSE_DOWN:
    case BK_GUI_EVENT_MOUSE_UP:
    case BK_GUI_EVENT_MOUSE_WHEEL:
        s->snapshot.x = e->x;
        s->snapshot.y = e->y;
        s->snapshot.buttons = e->buttons;
        break;
    case BK_GUI_EVENT_FOCUS_GAIN:
        s->snapshot.focused = 1;
        break;
    case BK_GUI_EVENT_FOCUS_LOST:
        s->snapshot.focused = 0;
        s->snapshot.buttons = 0;
        s->snapshot.modifiers = 0;
        memset(s->keys, 0, sizeof(s->keys));
        break;
    default:
        break;
    }
    return 0;
}
bool bk_input_state_key(const bk_input_state_t *s, uint8_t key) { return s && s->keys[key]; }
int bk_input_state_snapshot(const bk_input_state_t *s, bk_input_snapshot_t *out) {
    if (!s || !out || out->size < sizeof(*out))
        return BK_LBS_INVALID;
    *out = s->snapshot;
    return 0;
}
