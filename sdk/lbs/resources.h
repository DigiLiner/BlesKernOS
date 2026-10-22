#ifndef BK_LBS_RESOURCES_INTERNAL_H
#define BK_LBS_RESOURCES_INTERNAL_H
/* Module-local accounting: these helpers are hidden from the dynamic ABI. */
void bk_lbs_resource_acquire(void) __attribute__((visibility("hidden")));
void bk_lbs_resource_release(void) __attribute__((visibility("hidden")));
#endif
