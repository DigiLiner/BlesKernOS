#ifndef GUI_COLOR_PALETTE_H
#define GUI_COLOR_PALETTE_H
#include "../sdk/include/bleskernos_theme.h"
/* One byte per cached pixel, with exact theme colors and neutral greys.
 * RGB332 expanded C0C0C0 to DBDBFF, giving low-RAM windows a violet tint. */
static const uint32_t content_cache_theme[16] = {
    BK_THEME_FACE, BK_THEME_FACE_HOVER, BK_THEME_FACE_DOWN,
    BK_THEME_TAB_INACTIVE, BK_THEME_LIGHT, BK_THEME_MIDLIGHT,
    BK_THEME_SHADOW, BK_THEME_DARK, BK_THEME_TEXT, BK_THEME_MUTED,
    BK_THEME_SELECTION, BK_THEME_ACTIVE_TITLE, BK_THEME_ACTIVE_GRAIN,
    BK_THEME_INACTIVE_TITLE, BK_THEME_INACTIVE_GRAIN, BK_THEME_FACE_GRAIN
};
static inline uint8_t content_rgb888_to_332(uint32_t color) {
    color &= 0xFFFFFFU;
    for (uint32_t i = 0; i < 16U; ++i)
        if (content_cache_theme[i] == color) return (uint8_t)i;
    uint32_t r = (color >> 16) & 255U, g = (color >> 8) & 255U, b = color & 255U;
    if (r == g && g == b) return (uint8_t)(232U + (r * 23U + 127U) / 255U);
    return (uint8_t)(16U + ((r + 25U) / 51U) * 36U +
                     ((g + 25U) / 51U) * 6U + (b + 25U) / 51U);
}
static inline uint32_t content_rgb332_to_888(uint8_t color) {
    if (color < 16U) return content_cache_theme[color];
    if (color >= 232U) {
        uint32_t grey = ((color - 232U) * 255U + 11U) / 23U;
        return grey * 0x010101U;
    }
    uint32_t index = color - 16U;
    return ((index / 36U) * 51U << 16) |
           (((index / 6U) % 6U) * 51U << 8) | ((index % 6U) * 51U);
}

#endif
