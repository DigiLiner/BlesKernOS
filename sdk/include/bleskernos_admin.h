#ifndef BLESKERNOS_ADMIN_H
#define BLESKERNOS_ADMIN_H
#define BK_ADMIN_MAX 16U
#define BK_EVENT_CAPACITY 128U
#define BK_SERVICE_STOPPED 0U
#define BK_SERVICE_STARTING 1U
#define BK_SERVICE_RUNNING 2U
#define BK_SERVICE_STOPPING 3U
#define BK_SERVICE_FAILED 4U
#define BK_PHASE_BOOT 0U
#define BK_PHASE_DESKTOP 1U
#define BK_PHASE_DELAYED 2U
#define BK_SERVICE_START 1U
#define BK_SERVICE_STOP 2U
#define BK_SERVICE_RESTART 3U
#define BK_EVENT_KERNEL 0U
#define BK_EVENT_MEMORY 1U
#define BK_EVENT_DRIVER 2U
#define BK_EVENT_DISK 3U
#define BK_EVENT_NETWORK 4U
#define BK_EVENT_SERVICE 5U
#define BK_EVENT_APPLICATION 6U
#define BK_EVENT_INFO 0U
#define BK_EVENT_WARNING 1U
#define BK_EVENT_ERROR 2U
/* Fixed-width public snapshots, never pointers to kernel objects. */
typedef struct {
    uint32_t sequence, uptime_ms, pid, source, severity;
    char code[32], message[192];
} bk_event_info_t;
typedef struct {
    uint32_t id, pid, state, phase, order, enabled, critical, service;
    uint32_t dependencies, timeout_ms, last_result;
    char name[32], description[80], path[160], arguments[160], last_error[96];
} bk_startup_info_t;
typedef struct {
    uint32_t total, used, free, reserved, heap_total, heap_used, heap_free;
    uint32_t vm_total, vm_free;
} bk_memory_detail_t;
bool bk_proc_end_process(uint32_t process_id);
uint32_t bk_proc_priority(uint32_t process_id, uint32_t level);
uint32_t bk_event_count(void);
bool bk_event_get(uint32_t newest_index, bk_event_info_t *out);
bool bk_event_export(void);
uint32_t bk_startup_count(void);
bool bk_startup_get(uint32_t index, bk_startup_info_t *out);
bool bk_startup_save(const bk_startup_info_t *entry);
bool bk_startup_remove(uint32_t id);
bool bk_service_control(uint32_t id, uint32_t action);
bool bk_service_ready(uint32_t id);
bool bk_memory_detail(bk_memory_detail_t *out);
typedef struct {
    uint32_t dhcp;
    char ip[32], mask[32], gateway[32], dns[32];
} bk_network_profile_t;
bool bk_network_profile_get(uint32_t index, bk_network_profile_t *out);
bool bk_network_profile_apply(uint32_t profile, bool dhcp, const char *ip, const char *mask,
                              const char *gateway, const char *dns);
#endif
