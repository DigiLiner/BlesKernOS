#include "../include/pager.h"
#include "../include/task.h"
#include "../include/usercopy.h"
#include "../include/paging.h"
#include "../include/memory.h"

bool user_access_ok(const void *pointer, uint32_t length, bool write) {
    return paging_user_range_ok(pointer, length, write);
}

bool copy_from_user(void *destination, const void *source, uint32_t length) {
    if (!destination || (!source && length)) return false;
    if (!length) return true;
    uint32_t pid=task_current_process_id();
    if(!paging_process_pin(pid))return false;
    if(!pager_pin(pid,(uint32_t)(uintptr_t)source,length,false)){
        paging_process_unpin(pid);return false;
    }
    bool ok=user_access_ok(source,length,false);
    if(ok)kmemcpy(destination,source,length);
    pager_unpin(pid,(uint32_t)(uintptr_t)source,length);paging_process_unpin(pid);return ok;
}

bool copy_to_user(void *destination, const void *source, uint32_t length) {
    if ((!destination && length) || !source) return false;
    if (!length) return true;
    uint32_t pid=task_current_process_id();
    if(!paging_process_pin(pid))return false;
    if(!pager_pin(pid,(uint32_t)(uintptr_t)destination,length,true)){
        paging_process_unpin(pid);return false;
    }
    bool ok=user_access_ok(destination,length,true);
    if(ok)kmemcpy(destination,source,length);
    pager_unpin(pid,(uint32_t)(uintptr_t)destination,length);paging_process_unpin(pid);return ok;
}

bool copy_string_from_user(char *destination, uint32_t capacity,
                           const char *source) {
    if (!destination || !source || capacity < 2U) return false;
    uint32_t pid = task_current_process_id();
    if (!paging_process_pin(pid)) return false;
    uint32_t used = 0U;
    bool terminated = false;
    while (used < capacity) {
        uintptr_t address = (uintptr_t)source + used;
        if (address < (uintptr_t)source) break;
        /* Validate/pin once per page rather than once per character. Do not
         * touch the next page before finding out whether this one has NUL. */
        uint32_t chunk = 4096U - (address & 4095U);
        if (chunk > capacity - used) chunk = capacity - used;
        if (!pager_pin(pid, (uint32_t)address, chunk, false)) break;
        bool readable = user_access_ok((const void *)address, chunk, false);
        if (readable) {
            for (uint32_t i = 0U; i < chunk; i++) {
                char value = ((const volatile char *)address)[i];
                destination[used++] = value;
                if (!value) { terminated = true; break; }
            }
        }
        pager_unpin(pid, (uint32_t)address, chunk);
        if (!readable || terminated) break;
    }
    paging_process_unpin(pid);
    if (used == capacity && !terminated) destination[capacity - 1U] = '\0';
    return terminated;
}
