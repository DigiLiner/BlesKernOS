#include <bleskernos_lbs.h>
/* A shared ABI anchor makes algorithm modules explicitly depend on SYSTEM. */
uint32_t bk_lbs_system_abi(void) {
    return BK_LBS_ABI_MAJOR;
}
