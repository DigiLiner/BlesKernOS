#ifndef KERNEL_ADMIN_H
#define KERNEL_ADMIN_H
#include "types.h"
#include "../../sdk/include/bleskernos_admin.h"
void admin_init(void);
void admin_storage_ready(void);
void admin_desktop_ready(void);
bool admin_network_restore(void);
void admin_capture_char(char c);
void admin_emit(uint32_t source, uint32_t severity, const char *code, const char *message,
                uint32_t pid);
bool admin_enabled(uint32_t id);
void admin_adopt(uint32_t id, uint32_t pid);
void admin_ready(uint32_t id);
void admin_fail(uint32_t id, const char *reason);
#endif
