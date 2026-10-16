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
    for (uint32_t i = 0U; i < capacity; i++) {
        char value;
        if (!copy_from_user(&value, source + i, 1U)) return false;
        destination[i] = value;
        if (!value) return true;
    }
    destination[capacity - 1U] = '\0';
    return false;
}
