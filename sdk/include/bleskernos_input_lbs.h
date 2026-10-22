#ifndef BLESKERNOS_INPUT_LBS_H
#define BLESKERNOS_INPUT_LBS_H
#include <bleskernos_api.h>
#include <bleskernos_lbs.h>
#define BK_INPUT_CAP_KEYBOARD 1U
#define BK_INPUT_CAP_MOUSE 2U
#define BK_INPUT_CAP_FOCUS 4U
#define BK_INPUT_CAP_GAMEPAD 8U
typedef struct bk_input_state bk_input_state_t;
typedef struct {
    uint32_t size, events;
    int32_t x, y;
    uint32_t buttons, modifiers, focused;
} bk_input_snapshot_t;
uint32_t bk_input_capabilities(void);
bk_input_state_t *bk_input_state_create(void);
void bk_input_state_destroy(bk_input_state_t *);
/* Feed events delivered to this window by Vision. Coordinates are local to
   that window; this API does not capture input belonging to other processes. */
int bk_input_state_event(bk_input_state_t *, const bk_gui_event_t *);
bool bk_input_state_key(const bk_input_state_t *, uint8_t native_key);
int bk_input_state_snapshot(const bk_input_state_t *, bk_input_snapshot_t *);
#endif
