#ifndef BK_GRAPHICS_RESOURCES_H
#define BK_GRAPHICS_RESOURCES_H

#include "types.h"
#include "../../gui/image.h"

#define BK_GRAPHICS_PAK_PATH "/SYSTEM/GRAPHICS.PAK"

bool bk_graphics_icon_load(const char *name, gui_image_t *image);
/* Load a native-size variant, never resample a larger bitmap. */
bool bk_graphics_icon_load_size(const char *name, uint16_t size, gui_image_t *image);
bool bk_graphics_preload_boot_icons(void);
uint32_t bk_graphics_icon_count(void);
bool bk_graphics_icon_name(uint32_t index, char *buffer, uint32_t capacity);

#endif
