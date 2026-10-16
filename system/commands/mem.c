#include "common.h"

static int run(int argc, char **argv) {
    uint32_t divisor = 1024U;
    const char *unit = "KiB";
    for (int i = 1; i < argc; i++) {
        if (command_is(argv[i], "-b")) { divisor = 1U; unit = "bytes"; }
        else if (command_is(argv[i], "-k")) { divisor = 1024U; unit = "KiB"; }
        else if (command_is(argv[i], "-m")) { divisor = 1024U * 1024U; unit = "MiB"; }
        else {
            kprintf("Uso: mem [-b|-k|-m]\nMuestra RAM usada, libre y reservada; incluye frames VM.\n");
            return command_is(argv[i], "--help") ? 0 : 1;
        }
    }
    system_memory_info_t info;
    if (!bk_sys_memory_info(&info)) return command_error("mem", "sin estadisticas de memoria");
    uint32_t managed = info.total_bytes >= info.reserved_bytes
        ? (uint32_t)(info.total_bytes - info.reserved_bytes) : 0U;
    uint32_t percent = managed
        ? (uint32_t)((uint64_t)info.used_bytes * 100U / managed) : 0U;
    kprintf("RAM (%s): instalada=%u gestionable=%u usada=%u libre=%u (%u%% de gestionable)\n",
            unit, (uint32_t)(info.total_bytes / divisor), managed / divisor,
            (uint32_t)(info.used_bytes / divisor),
            (uint32_t)(info.free_bytes / divisor), percent);
    kprintf("Fuera del pool gestionado: %u %s (NO es RAM consumida)\n",
            (uint32_t)(info.reserved_bytes / divisor), unit);
    return 0;
}

BK_COMMAND_MAIN(run)
