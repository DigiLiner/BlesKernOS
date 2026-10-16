/* Disposable benchmark only: same counters as the general perfmon command. */
static void svga_validation_perf(void *arg UNUSED) {
    uint32_t early = 0U;
    /* Avoid interleaving this marker with the boot pixel-test diagnostics. */
    task_sleep(pit_get_frequency_hz() * 20U);
    for (uint32_t i = 0U; i < 20U; i++) {
        uint32_t start = pit_get_ticks();
        task_sleep(4U);
        if (pit_get_ticks() - start < 4U) early++;
    }
    kprintf("[VALIDATION] kernel sleep deadlines early=%u tests=20\n", early);
    for (;;) {
        task_sleep(pit_get_frequency_hz() * 10U);
        perfmon_force_report();
    }
}
