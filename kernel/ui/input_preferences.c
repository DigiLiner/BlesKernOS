#include "../include/api.h"

static uint32_t g_input_preferences[BK_INPUT_PREFERENCE_COUNT] = {
    500U, 60U, 500U, 0U, 0U, 500U, 0U
};

uint32_t bk_input_preference_get(uint32_t preference) {
    return preference < BK_INPUT_PREFERENCE_COUNT ? g_input_preferences[preference] : 0U;
}

bool bk_input_preference_set(uint32_t preference, uint32_t value) {
    switch (preference) {
        case BK_INPUT_REPEAT_DELAY: if (value < 250U || value > 1000U) return false; break;
        case BK_INPUT_REPEAT_INTERVAL: if (value < 30U || value > 250U) return false; break;
        case BK_INPUT_CARET_BLINK: if (value && (value < 200U || value > 1000U)) return false; break;
        case BK_INPUT_LAYOUT: case BK_INPUT_SWAP_BUTTONS: if (value > 1U) return false; break;
        case BK_INPUT_DOUBLE_CLICK: if (value < 200U || value > 900U) return false; break;
        case BK_INPUT_CURSOR_SCHEME: if (value > 2U) return false; break;
        default: return false;
    }
    if (g_input_preferences[preference] == value) return true;
    g_input_preferences[preference] = value;
    if (preference == BK_INPUT_CURSOR_SCHEME && gui_get_desktop()) {
        gui_desktop_invalidate_all(gui_get_desktop());
        gui_request_paint();
    }
    return true;
}
