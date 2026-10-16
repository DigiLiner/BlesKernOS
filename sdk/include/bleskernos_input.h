#ifndef BLESKERNOS_INPUT_PREFERENCES_H
#define BLESKERNOS_INPUT_PREFERENCES_H
/* Desktop input preferences. Values are milliseconds except boolean swap,
 * layout (0 US, 1 ES), and stock cursor scheme (0 classic, 1 dark, 2 teal). */
#define BK_INPUT_REPEAT_DELAY 0U
#define BK_INPUT_REPEAT_INTERVAL 1U
#define BK_INPUT_CARET_BLINK 2U
#define BK_INPUT_LAYOUT 3U
#define BK_INPUT_SWAP_BUTTONS 4U
#define BK_INPUT_DOUBLE_CLICK 5U
#define BK_INPUT_CURSOR_SCHEME 6U
#define BK_INPUT_PREFERENCE_COUNT 7U
uint32_t bk_input_preference_get(uint32_t preference);
bool bk_input_preference_set(uint32_t preference, uint32_t value);
#endif
