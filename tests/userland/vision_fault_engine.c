/* Test-only server: fault after acquiring the real frame transaction. */
static void vision_fault_hook(void);
#define VISION_ENGINE_FRAME_HOOK() vision_fault_hook()
#include "../../system/services/vision_engine.c"
static void vision_fault_hook(void) {
    vfs_dir_entry_t entry;
    const char *path="/SYSTEM/USER/CONFIG/VISION.FAULT";
    if(!bk_file_stat(path,&entry))return;
    if(!bk_file_remove(path))return;
    kprintf("[VISION:FAULTTEST] injecting UD2 while holding frame locks pid=%u\n",bk_sys_getpid());
    gui_desktop_t *desktop=bk_vision_desktop();
    desktop->first_window=(gui_window_t *)(uintptr_t)0xdeadbeefU;
    desktop->last_program=(gui_program_t *)(uintptr_t)0xcafebabeU;
    __asm__ volatile("ud2");
}
