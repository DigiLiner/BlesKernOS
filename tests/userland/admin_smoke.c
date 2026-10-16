#include "../../kernel/include/api.h"
#include "../../kernel/include/syscall.h"

static void check(bool ok, const char *name) {
    kprintf("[ADMINTEST] %s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        (void)syscall1(SYS_EXIT, 1);
        for (;;)
            bk_sys_sleep_ms(100);
    }
}
static bool wait_state(uint32_t id, uint32_t target) {
    for (uint32_t i = 0; i < 400; i++) {
        bk_startup_info_t e;
        if (bk_startup_get(id - 1, &e) && e.state == target &&
            (target != BK_SERVICE_STOPPED || !e.pid))
            return true;
        bk_sys_sleep_ms(50);
    }
    bk_startup_info_t last;
    if (bk_startup_get(id-1,&last)) kprintf("[ADMINTEST] wait id=%u state=%u pid=%u error=%s\n",id,last.state,last.pid,last.last_error);
    return false;
}
static void service_worker(void *unused UNUSED) {
    while (!bk_proc_exit_requested())
        bk_sys_sleep_ms(20);
}
void bleskernos_program_main(gui_desktop_t *desktop UNUSED) {
    const char *arg = bk_proc_launch_arg();
    if (arg && !bk_runtime_strcmp(arg, "--service")) {
        check(bk_proc_spawn_thread("admin-worker", service_worker, NULL) >= 0,
              "service worker created");
        while (!bk_service_ready(5))
            bk_sys_sleep_ms(20);
        while (!bk_proc_exit_requested())
            bk_sys_sleep_ms(20);
        return;
    }
    while (!bk_service_ready(4))
        bk_sys_sleep_ms(20);
    check(true, "service readiness ownership");
    bk_startup_info_t e;
    check(bk_startup_get(0, &e) && e.state == BK_SERVICE_RUNNING, "Vision ready");
    check(!bk_service_control(1, BK_SERVICE_STOP), "critical service protected");
    check(!bk_proc_end_process(e.pid), "critical process protected");
    check(!bk_service_ready(1), "foreign readiness rejected");
    bk_event_info_t event;
    check(bk_event_count() > 0 && bk_event_get(0, &event), "Ring 3 event snapshot");
    check(!bk_event_get(0, (bk_event_info_t *)0x00100000), "kernel output alias rejected");
    bk_memory_detail_t ram;
    check(bk_memory_detail(&ram) && ram.total == ram.used + ram.free &&
              ram.heap_used <= ram.heap_total,
          "RAM accounting");
    kmemset(&e, 0, sizeof(e));
    e.phase = BK_PHASE_DESKTOP;
    e.timeout_ms = 15000;
    e.service = 1;
    bk_runtime_strcpy(e.name, "Admin fixture");
    bk_runtime_strcpy(e.path, "/SYSTEM/TESTS/NATIVE/ADMINTEST.BEX");
    bk_runtime_strcpy(e.arguments, "--service");
    check(bk_startup_save(&e), "persistent service definition");
    check(bk_service_control(5, BK_SERVICE_START), "start accepted");
    check(wait_state(5, BK_SERVICE_RUNNING), "start waits for ready");
    check(bk_startup_get(4, &e), "snapshot running service");
    uint32_t original_pid = e.pid;
    check(bk_service_control(5, BK_SERVICE_RESTART), "cooperative restart requested");
    bool restarted = false;
    for (uint32_t n = 0; n < 400; n++) {
        bk_startup_get(4, &e);
        if (e.pid && e.pid != original_pid && e.state == BK_SERVICE_RUNNING) {
            restarted = true;
            break;
        }
        bk_sys_sleep_ms(50);
    }
    check(restarted, "restart creates new process");
    check(wait_state(5, BK_SERVICE_RUNNING), "restart ready");
    check(bk_service_control(5, BK_SERVICE_STOP) && wait_state(5, BK_SERVICE_STOPPED),
          "cooperative stop");
    check(bk_startup_remove(5), "remove stopped service");
    check(!bk_network_profile_apply(0, false, "10.0.2.15", "255.0.255.0", "10.0.2.2", "10.0.2.3"),
          "invalid netmask rejected");
    check(bk_network_profile_apply(1, false, "10.0.2.15", "255.255.255.0", "10.0.2.2", "10.0.2.3"),
          "static network applied and saved");
    bk_network_profile_t profile;
    check(bk_network_profile_get(1, &profile) && !profile.dhcp &&
              !bk_runtime_strcmp(profile.ip, "10.0.2.15"),
          "saved network profile read");
    bk_event_get(0,&event);
    uint32_t export_after=event.sequence;
    check(bk_event_export(), "event export queued");
    bool exported=false;
    for(uint32_t tries=0;tries<200 && !exported;tries++) {
        uint32_t count=bk_event_count();
        for(uint32_t i=0;i<count;i++) {
            bk_event_info_t result;
            if(!bk_event_get(i,&result)) continue;
            if((int32_t)(result.sequence-export_after)<=0) break;
            if(!bk_runtime_strcmp(result.code,"EVENT_EXPORT_READY")) {exported=true;break;}
        }
        if(!exported) bk_sys_sleep_ms(50);
    }
    vfs_dir_entry_t file;
    check(exported && bk_file_stat("/SYSTEM/LOGS/EVENTS-EXPORT.TXT", &file) && file.size > 0,
          "event export saved");
    kprintf("[ADMINTEST] ALL PASS\n");
    const char *apps[] = {"/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX",
                          "/SYSTEM/PROGRAMS/EVENT_VIEWER.BEX"};
    for (uint32_t i = 0; i < 2; i++) {
        bk_proc_info_t before[128];
        uint32_t n = bk_proc_snapshot(before, 128), cutoff = 0;
        for (uint32_t j = 0; j < n; j++)
            if (before[j].pid > cutoff)
                cutoff = before[j].pid;
        check(bk_app_launch(apps[i], NULL), "launch admin UI");
        kprintf("[ADMINTEST] UI %u visible\n", i);
        bk_sys_sleep_ms(10000);
        if (ram.total < 96U * 1024U * 1024U) {
            bk_proc_info_t after[128];
            n = bk_proc_snapshot(after, 128);
            for (uint32_t j = 0; j < n; j++)
                if (after[j].user && after[j].process_id > cutoff &&
                    after[j].state != BK_PROC_ZOMBIE)
                    bk_proc_end_process(after[j].process_id);
            bk_sys_sleep_ms(1500);
        }
    }
    kprintf("[ADMINTEST] UI ALL PASS\n");
}
