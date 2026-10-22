#include <bleskernos.h>
#include <bleskernos_api.h>
static void message(const char *prefix, const char *name) {
    char out[200];
    uint32_t n = 0, i = 0;
    while (prefix[i] && n + 1 < sizeof(out))
        out[n++] = prefix[i++];
    i = 0;
    while (name[i] && n + 2 < sizeof(out))
        out[n++] = name[i++];
    out[n++] = '\n';
    out[n] = 0;
    bk_sys_log(out);
}
void bleskernos_program_main(void *unused) {
    static const char *apps[] = {"FILE.BEX",        "TEXTEDITOR.BEX", "PAINT.BEX",
                                 "IMAGEVIEWER.BEX", "MIDAMP.BEX",     "VIEWER.BEX"};
    uint32_t i, j;
    (void)unused;
    for (i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
        char path[96];
        uint32_t pos = 0;
        const char *prefix = "/SYSTEM/PROGRAMS/";
        while (*prefix)
            path[pos++] = *prefix++;
        for (j = 0; apps[i][j]; j++)
            path[pos++] = apps[i][j];
        path[pos] = 0;
        int32_t pid = bk_spawn(path, 0);
        if (pid < 0) {
            message("[LBSAPPS] FAIL spawn: ", apps[i]);
            bk_exit(1);
        }
        bk_sys_sleep_ms(800);
        bk_process_info_t tasks[128];
        uint32_t count = bk_proc_snapshot(tasks, 128);
        bool running = false;
        for (j = 0; j < count; j++)
            if (tasks[j].process_id == (uint32_t)pid && tasks[j].state != BK_PROCESS_ZOMBIE &&
                tasks[j].state != BK_PROCESS_UNUSED)
                running = true;
        if (!running) {
            message("[LBSAPPS] FAIL lifetime: ", apps[i]);
            bk_exit(2);
        }
        message("[LBSAPPS] PASS startup: ", apps[i]);
        if (bk_kill(pid) < 0) {
            message("[LBSAPPS] FAIL cleanup: ", apps[i]);
            bk_exit(3);
        }
        int32_t status;
        (void)bk_waitpid(pid, &status);
    }
    bk_sys_log("[LBSAPPS] ALL PASS\n");
    bk_exit(0);
}
