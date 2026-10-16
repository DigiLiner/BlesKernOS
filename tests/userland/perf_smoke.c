#include "../../kernel/include/api.h"
static volatile uint32_t heartbeats;
static volatile bool stop;
static void heartbeat(void *arg) {
    (void)arg;
    while (!stop) { heartbeats++; bk_sys_sleep_ticks(1U); }
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;
    bk_sys_sleep_ms(3000U);
    int thread = bk_proc_spawn_thread("bench-peer", heartbeat, NULL);
    bk_perf_benchmark_t result;
    uint32_t before = heartbeats;
    int status = bk_perf_benchmark(&result);
    bool ok = status == 0 && thread >= 0 && heartbeats > before &&
        (result.available_mask & PERFMON_BENCH_TIMER) &&
        (result.available_mask & PERFMON_BENCH_SCHEDULER);
    stop = true;
    bk_sys_sleep_ms(100U);
    kprintf("[BENCHTEST] yields=%u/s sched=%u timer=%u late_avg=%u late_max=%u disk=%u peers=%u\n",
        result.scheduler_yields_s,result.scheduler_score,result.timer_score,
        result.timer_average_late_us,result.timer_max_late_us,result.disk_score,
        heartbeats-before);
    kprintf("[BENCHTEST] DONE result=%s\n",ok ? "PASS" : "FAIL");
}
