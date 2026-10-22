#include "gui/gui.h"
extern int printf(const char *,...);
#define O(type,member) ((unsigned)__builtin_offsetof(type,member))
int main(void){printf("%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u",O(gui_desktop_t,first_window),O(gui_desktop_t,mouse_x),O(gui_desktop_t,mouse_y),sizeof(gui_window_t),O(gui_window_t,bounds),O(gui_window_t,title),O(gui_window_t,next),O(gui_window_t,visible),O(gui_window_t,listed),O(gui_window_t,owner_pid),O(gui_window_t,controller_pid),O(gui_window_t,stream_pixels),O(gui_window_t,stream_width),GUI_BORDER_SIZE,GUI_TITLEBAR_HEIGHT,O(gui_desktop_t,cursor_busy_count),O(gui_desktop_t,focused_window));}
