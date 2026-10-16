#ifndef BK_VISION_RESOURCES_H
#define BK_VISION_RESOURCES_H
#include "types.h"
#include "../../gui/gui.h"
bool bk_vision_resource_register(void *object,bool program);
void bk_vision_resource_retire(void *object);
void bk_vision_resource_forget(void *object);
void bk_vision_resource_owner(void *object,uint32_t owner);
bool vision_resources_checkpoint(gui_desktop_t *desktop);
void vision_resources_recover(gui_desktop_t *desktop);
#endif
