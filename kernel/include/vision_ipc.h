#ifndef BK_VISION_IPC_H
#define BK_VISION_IPC_H
#include "types.h"
#include "../../gui/gui.h"
/* Fixed messages carry values, never client stack or string pointers. */
enum {VISION_IPC_CREATE=1,VISION_IPC_CLOSE,VISION_IPC_FOCUS,
      VISION_IPC_BOUNDS,VISION_IPC_MIN_SIZE,VISION_IPC_BORDERLESS,VISION_IPC_DESTROY,VISION_IPC_CONTENT,VISION_IPC_EVENT,
      VISION_IPC_EXTENDED,VISION_IPC_COMPACT,VISION_IPC_STREAMING,VISION_IPC_OWNER,VISION_IPC_FILE_DIALOG,VISION_IPC_ABOUT_ATTACH,VISION_IPC_ABOUT_SHOW,VISION_IPC_PROGRAM_ADD,VISION_IPC_PROGRAM_REMOVE,VISION_IPC_MENU_ADD,VISION_IPC_MENU_ITEM};
typedef struct {
    uint32_t cookie,operation,process_id,thread_id,window;
    gui_rect_t bounds;
    uint32_t values[6];
    char title[64];
    char path[260],extension[16];
    struct {char name[48],version[24],description[128],copyright[64],icon_path[64];} about;
} bk_vision_request_t;
bool vision_ipc_routes_user(void);
bool vision_ipc_call(bk_vision_request_t *request,uint32_t *result);
bool bk_vision_ipc_take(bk_vision_request_t *request);
bool bk_vision_ipc_reply(uint32_t cookie,uint32_t result);
void vision_ipc_server_online(uint32_t pid);
void vision_ipc_server_offline(uint32_t pid);
void vision_ipc_process_cleanup(uint32_t pid);
#endif
