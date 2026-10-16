#include "common.h"

static int run(int argc, char **argv) {
    uint32_t samples = 1U, delay = 1000U;
    for (int i = 1; i < argc; i++) {
        bool valid;
        if (command_is(argv[i], "--help")) {
            kprintf("Uso: top [-n muestras] [-d intervalo_ms]\n");
            return 0;
        }
        bool count = command_is(argv[i], "-n");
        if ((!count && !command_is(argv[i], "-d")) || ++i >= argc)
            return command_error("top", "uso: top [-n muestras] [-d intervalo_ms]");
        uint32_t value = command_number(argv[i], &valid);
        if (!valid || !value || (count ? value > 1000U : value > 60000U))
            return command_error("top", "muestras: 1..1000; intervalo: 1..60000 ms");
        if (count) samples = value; else delay = value;
    }
    for (uint32_t i = 0; i < samples; i++) {
        system_memory_info_t info;
        kprintf("Muestra %u/%u | CPU: %u%%\n", i + 1U, samples, bk_proc_cpu_usage());
        if (bk_sys_memory_info(&info))
            kprintf("RAM: usada=%u KiB libre=%u KiB total=%u KiB\n",
                    (uint32_t)(info.used_bytes / 1024U), (uint32_t)(info.free_bytes / 1024U),
                    (uint32_t)(info.total_bytes / 1024U));
        int status = command_show_processes(true);
        if (status) return status;
        if (i + 1U < samples) bk_sys_sleep_ms(delay);
    }
    return 0;
}

BK_COMMAND_MAIN(run)
