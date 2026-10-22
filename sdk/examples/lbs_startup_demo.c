#include <bleskernos_lbs.h>
#include <bleskernos_api.h>
#include <dlfcn.h>
static void *system_library;
const bk_lbs_requirement_t bk_lbs_requirements[] = {
    {"SYSTEM.LBS", BK_LBS_ABI_MAJOR, &system_library}, {0, 0, 0}};
void bk_lbs_program_main(void *unused) {
    uint32_t (*pid)(void) = dlsym(system_library, "bk_sys_getpid");
    (void)unused;
    if (pid && pid() == bk_sys_getpid())
        bk_sys_log("[LBSSTART] ALL PASS\n");
}
