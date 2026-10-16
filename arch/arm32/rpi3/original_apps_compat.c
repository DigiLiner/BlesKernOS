#include "../../../kernel/include/api.h"
#include "../../../kernel/include/block.h"
#include "../../../kernel/include/fat.h"
#include "../../../kernel/include/memory.h"
#include "../../../kernel/include/network.h"
#include "../../../kernel/include/task.h"
#include "task_arm.h"

#ifdef bk_device_block_count
#undef bk_device_block_count
#endif
extern uint32_t block_count(void);

typedef struct {
    bool mounted; char mount_name[16]; char device_name[16]; char filesystem[9];
    char volume_label[12]; uint8_t fat_bits; uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster; uint32_t total_sectors;
    uint32_t total_clusters; uint64_t total_bytes; uint64_t free_bytes;
    bool read_only;
} arm_original_volume_info_t;

/* Compatibility needed by the original desktop applications while their
 * public API calls are moved across the AArch32 privilege boundary.  These
 * adapters preserve the original program sources and DTO layouts. */

size_t bk_string_length(const char *text) { return kstrlen(text ? text : ""); }

uint32_t pci_device_count(void) { return 0U; }
uint32_t driver_count(void) { return 0U; }
bool bk_device_pci_info(uint32_t index, bk_pci_info_t *info) {
    (void)index; if (info) kmemset(info, 0, sizeof(*info)); return false;
}
bool bk_device_driver_info(uint32_t index, bk_driver_info_t *info) {
    (void)index; if (info) kmemset(info, 0, sizeof(*info)); return false;
}
uint32_t bk_device_block_count(void) { return block_count(); }

bool bk_device_volume_info(arm_original_volume_info_t *info) {
    fat_fs_t fs; uint64_t total = 0U, free_bytes = 0U;
    if (!info || !fat_get_current(&fs)) return false;
    kmemset(info, 0, sizeof(*info)); info->mounted = true;
    kstrncpy(info->mount_name, fat_get_active_name(), sizeof(info->mount_name) - 1U);
    if (fs.device) kstrncpy(info->device_name, fs.device->name,
                            sizeof(info->device_name) - 1U);
    kstrncpy(info->filesystem, fs.fs_name, sizeof(info->filesystem) - 1U);
    kstrncpy(info->volume_label, fs.volume_label, sizeof(info->volume_label) - 1U);
    info->fat_bits = (uint8_t)fs.type; info->bytes_per_sector = fs.bytes_per_sector;
    info->sectors_per_cluster = fs.sectors_per_cluster;
    info->total_sectors = fs.total_sectors; info->total_clusters = fs.total_clusters;
    info->read_only = fs.device ? fs.device->read_only : true;
    (void)fat_get_space(&fs, &total, &free_bytes);
    info->total_bytes = total; info->free_bytes = free_bytes; return true;
}
bool bk_device_check_volume(void *report) {
    return report && fat_check_current((fat_check_report_t *)report);
}
bool bk_device_repair_volume(void *repair, void *after) {
    return repair && after && fat_repair_current((fat_repair_report_t *)repair,
                                                  (fat_check_report_t *)after);
}
bool bk_device_mount_volume(const char *name) { return name && fat_set_active(name); }

bool bk_net_get_info(bk_net_info_t *info) {
    net_info_t value;
    if (!info) return false;
    network_get_info(&value); kmemset(info, 0, sizeof(*info));
    info->configured = value.configured; info->link_up = value.link_up;
    info->stack_ready = value.stack_ready; info->tls_ready = value.tls_ready;
    kstrncpy(info->device, value.device, sizeof(info->device) - 1U);
    info->mtu = value.mtu; kmemcpy(info->mac, value.mac, 6U);
    kmemcpy(info->address, value.address, 4U); kmemcpy(info->netmask, value.netmask, 4U);
    kmemcpy(info->gateway, value.gateway, 4U); kmemcpy(info->dns, value.dns, 4U);
    info->rx_packets = value.rx_packets; info->tx_packets = value.tx_packets;
    info->rx_dropped = value.rx_dropped; return true;
}
bool bk_net_dhcp(uint32_t timeout_ms) { return network_dhcp(timeout_ms); }
bool bk_net_ping(const uint8_t address[4], uint32_t timeout_ms,
                 uint32_t *round_trip_ms) {
    return network_ping(address, timeout_ms, round_trip_ms);
}
bool bk_net_resolve(const char *host, uint8_t address[4], uint32_t timeout_ms) {
    return network_resolve(host, address, timeout_ms);
}

static bool g_perf_enabled;
bool bk_perf_enabled(void) { return g_perf_enabled; }
void bk_perf_set_enabled(bool enabled) { g_perf_enabled = enabled; }
void bk_perf_reset(void) { }
bool bk_perf_snapshot(bk_perf_snapshot_t *snapshot, bool reset_interval) {
    (void)reset_interval; if (!snapshot) return false;
    kmemset(snapshot, 0, sizeof(*snapshot)); return true;
}
int bk_perf_benchmark(bk_perf_benchmark_t *result) {
    if (result) kmemset(result, 0, sizeof(*result)); return -1;
}

bool bk_proc_launch_arg_copy(char *buffer, uint32_t capacity) {
    if (!buffer || !capacity) return false; buffer[0] = '\0'; return false;
}

uint32_t task_count(void) { return rpi3_task_live_count(); }
const task_t *task_get(uint32_t index) {
    static task_t view;
    rpi3_task_public_info_t items[32];
    uint32_t count = rpi3_task_snapshot(items, 32U);
    if (index >= count) return NULL;
    kmemset(&view, 0, sizeof(view)); view.pid = items[index].pid;
    view.process_id = items[index].process_id;
    kstrncpy(view.name, items[index].name, sizeof(view.name) - 1U);
    view.state = (task_state_t)items[index].state;
    view.cpu_ticks = items[index].cpu_ticks; view.idle = items[index].idle;
    view.user = items[index].user; view.system = !items[index].user;
    view.memory_bytes = (uint32_t)mm_get_process_usage(
        view.process_id ? view.process_id : view.pid);
    return &view;
}
uint32_t task_process_memory_bytes(uint32_t process_id) {
    return (uint32_t)mm_get_process_usage(process_id);
}
const char *task_user_export_string(const char *source) { return source; }
bool shell_take_exit_request(void) { return false; }

static bool g_startup_sound_enabled = true;
bool startup_sound_enabled(void) { return g_startup_sound_enabled; }
bool startup_sound_set_enabled(bool enabled) {
    g_startup_sound_enabled = enabled; return true;
}

static bool g_screensaver_enabled;
static uint32_t g_screensaver_timeout = 300U;
static char g_screensaver_path[VFS_MAX_PATH];
bool screensaver_is_enabled(void) { return g_screensaver_enabled; }
uint32_t screensaver_get_timeout_seconds(void) { return g_screensaver_timeout; }
void screensaver_set_enabled(bool enabled) { g_screensaver_enabled = enabled; }
void screensaver_set_timeout_seconds(uint32_t seconds) {
    g_screensaver_timeout = seconds ? seconds : 60U;
}
const char *screensaver_get_path(void) { return g_screensaver_path; }
void screensaver_set_path(const char *path) {
    kstrncpy(g_screensaver_path, path ? path : "",
             sizeof(g_screensaver_path) - 1U);
}
bool screensaver_preview(gui_desktop_t *desktop) { (void)desktop; return false; }
uint8_t gui_display_color_depth(void) { return 32U; }
bool gui_change_resolution(gui_desktop_t *desktop, uint16_t width,
                           uint16_t height, uint8_t bpp) {
    (void)desktop; (void)width; (void)height; (void)bpp;
    /* El framebuffer del firmware Pi conserva su modo hasta implementar el
       cambio atómico por mailbox; el panel original sigue mostrando el modo. */
    return false;
}
void ZB_close(void *buffer) { (void)buffer; }
void tglClose(void) { }
