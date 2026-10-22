#include "../include/vision_ipc.h"
#include "../include/task.h"
#include "../include/kernel_domains.h"
#include "../include/klock.h"
#include "../include/memory.h"
#include "../include/pit.h"

#define VISION_IPC_SLOTS 32U
/* No allocations while waiting for or recovering the graphical server. */
static struct {
    bk_vision_request_t request;
    uint32_t state,result;
} slots[VISION_IPC_SLOTS];
static kspinlock_t lock=KSPINLOCK_INITIALIZER;
static uint32_t server_pid,sequence;
static bool activated;
/* Server and client reply slots use different event keys. */
static uint32_t server_event;
void vision_ipc_notify_native_gpu(void) {
    (void)task_event_signal((uint32_t)(uintptr_t)&server_event);
}

bool vision_ipc_routes_user(void) {
    return activated && task_current_is_user() &&
           task_current_pid()!=gui_vision_owner_pid();
}
void vision_ipc_server_online(uint32_t pid) {
    uint32_t flags=kspin_lock_irqsave(&lock);
    server_pid=pid;activated=true;
    kspin_unlock_irqrestore(&lock,flags);
}
void vision_ipc_server_offline(uint32_t pid) {
    uint32_t flags=kspin_lock_irqsave(&lock);
    if(server_pid==pid) {
        server_pid=0;
        for(uint32_t i=0;i<VISION_IPC_SLOTS;i++)
            if(slots[i].state==1U || slots[i].state==2U) {
                slots[i].state=3U;slots[i].result=0;
            }
    }
    kspin_unlock_irqrestore(&lock,flags);
}
void vision_ipc_process_cleanup(uint32_t pid) {
    uint32_t flags=kspin_lock_irqsave(&lock);
    for(uint32_t i=0;i<VISION_IPC_SLOTS;i++)
        if(slots[i].state && slots[i].request.process_id==pid)
            slots[i].state=0;
    kspin_unlock_irqrestore(&lock,flags);
}
bool vision_ipc_call(bk_vision_request_t *request,uint32_t *result) {
    kernel_domain_snapshot_t saved;
    uint32_t flags,index,cookie,start=pit_get_ticks(),hz=pit_get_frequency_hz();
    if(result)*result=0;
    if(!request || !result)return false;
    request->process_id=task_current_process_id();request->thread_id=task_current_pid();
    flags=kspin_lock_irqsave(&lock);
    for(index=0;index<VISION_IPC_SLOTS;index++)if(!slots[index].state)break;
    if(!server_pid || index==VISION_IPC_SLOTS) {
        kspin_unlock_irqrestore(&lock,flags);return false;
    }
    cookie=++sequence;if(!cookie)cookie=++sequence;
    request->cookie=cookie;slots[index].request=*request;slots[index].state=1U;
    kspin_unlock_irqrestore(&lock,flags);
    (void)task_event_signal((uint32_t)(uintptr_t)&server_event);
    /* Never sleep retaining the application API's GUI/TASK/VFS domains. */
    kernel_domains_drop_current(&saved);
    bool completed=false;
    uint32_t wait_key=(uint32_t)(uintptr_t)&slots[index];
    for(;;) {
        /* Arm before checking the result so a reply between the check and
         * sleep is latched. Timer polling used to add a tick to every frame. */
        bool armed=task_event_wait_prepare(wait_key);
        flags=kspin_lock_irqsave(&lock);
        if(slots[index].request.cookie!=cookie || !slots[index].state) {
            kspin_unlock_irqrestore(&lock,flags);break;
        }
        if(slots[index].state==3U) {
            *result=slots[index].result;slots[index].state=0;
            completed=true;kspin_unlock_irqrestore(&lock,flags);break;
        }
        if(pit_get_ticks()-start>(hz?hz:100U)*10U || task_exit_requested()) {
            slots[index].state=0;kspin_unlock_irqrestore(&lock,flags);break;
        }
        kspin_unlock_irqrestore(&lock,flags);
        if(armed)(void)task_event_wait_commit(wait_key,1U);else task_sleep(1U);
    }
    task_event_wait_cancel(wait_key);
    kernel_domains_restore(&saved);return completed;
}
void bk_vision_ipc_wait(uint32_t timeout_ticks) {
    if(!server_pid || task_current_pid()!=server_pid)return;
    /* Native captures have no synchronous IPC request to keep the producer
     * waiting. Poll their actual fences at a bounded cadence, and retain the
     * pending bit so an early notification cannot be lost before arming. */
    if(linux_native_gpu_pending())timeout_ticks=1U;
    uint32_t key=(uint32_t)(uintptr_t)&server_event;
    bool armed=task_event_wait_prepare(key),pending=false;
    uint32_t flags=kspin_lock_irqsave(&lock);
    for(uint32_t i=0;i<VISION_IPC_SLOTS;i++)if(slots[i].state==1U) {
        pending=true;break;
    }
    kspin_unlock_irqrestore(&lock,flags);
    /* Arm before scanning: a request arriving between the scan and sleep
     * is latched. This is called after frame_end, without GUI/GFX domains. */
    if(!pending) {
        if(armed)(void)task_event_wait_commit(key,timeout_ticks);
        else task_sleep(timeout_ticks?timeout_ticks:1U);
    }
    task_event_wait_cancel(key);
}
bool bk_vision_ipc_take(bk_vision_request_t *request) {
    if(!request || task_current_process_id()!=server_pid)return false;
    uint32_t flags=kspin_lock_irqsave(&lock);
    for(uint32_t i=0;i<VISION_IPC_SLOTS;i++)if(slots[i].state==1U) {
        *request=slots[i].request;slots[i].state=2U;
        kspin_unlock_irqrestore(&lock,flags);return true;
    }
    kspin_unlock_irqrestore(&lock,flags);return false;
}
bool bk_vision_ipc_reply(uint32_t cookie,uint32_t result) {
    if(!cookie || task_current_process_id()!=server_pid)return false;
    uint32_t flags=kspin_lock_irqsave(&lock);
    for(uint32_t i=0;i<VISION_IPC_SLOTS;i++)
        if(slots[i].state==2U && slots[i].request.cookie==cookie) {
            slots[i].result=result;slots[i].state=3U;
            kspin_unlock_irqrestore(&lock,flags);
            (void)task_event_signal((uint32_t)(uintptr_t)&slots[i]);return true;
        }
    kspin_unlock_irqrestore(&lock,flags);return false;
}
