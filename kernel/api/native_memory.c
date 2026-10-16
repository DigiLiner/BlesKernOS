#include "../include/native_memory.h"
#include "../include/memory.h"
#include "../include/paging.h"
#include "../include/pager.h"
#include "../include/task.h"
#include "../include/usercopy.h"
#include "../include/api.h"
#include "../include/syscall.h"
#include "../libc/include/string.h"
#include "../libc/include/stdlib.h"
#include "../libc/include/errno.h"

extern size_t bk_string_length(const char *text);
extern int bk_string_compare(const char *left,const char *right);
extern char *bk_string_copy_n(char *destination,const char *source,size_t capacity);
extern char *bk_string_concat(char *destination,const char *source);
extern int bk_memory_compare(const void *left,const void *right,size_t size);

#define TARGET(fn) (target==(uint32_t)(uintptr_t)&fn)

static bool native_buffer_pin(uint32_t pid,uint32_t address,uint32_t length,bool write,void **token) {
    *token=NULL;
    if(!length)return true;
    if(!address || length>0xffffffffU-address)return false;
    if(!pager_pin(pid,address,length,write))return false;
    bool ok=user_access_ok((void *)(uintptr_t)address,length,write);
    /* Private mappings may intentionally shadow legacy heap virtual addresses.
     * Inspect each identity fragment without mistaking private backing for a
     * legacy allocation. Page tables and effective permissions were checked. */
    for(uint32_t off=0;ok && off<length;) {
        uint32_t n=4096U-((address+off)&4095U);
        if(n>length-off)n=length-off;
        if(!(paging_pager_entry(pid,address+off)&0x400U)) {
            void *fragment=NULL;
            ok=mm_user_heap_pin(address+off,n,pid,&fragment);
            if(fragment) {
                if(*token){
                    if(*token!=fragment)ok=false;
                    mm_unpin_address_allocation(fragment);
                } else *token=fragment;
            }
        }
        off+=n;
    }
    if(!ok){
        pager_unpin(pid,address,length);
        if(*token)mm_unpin_address_allocation(*token);
        *token=NULL;
    }
    return ok;
}

static void native_buffer_unpin(uint32_t pid,uint32_t address,uint32_t length,void *token) {
    if(length)pager_unpin(pid,address,length);
    if(token)mm_unpin_address_allocation(token);
}
/* Bound scanning to mapped payloads and a finite amount of kernel work. The
 * caller pins the measured range again before a bounded operation, so racing
 * edits to a terminator cannot extend a write beyond the verified destination. */
static bool native_string_extent(uint32_t pid,uint32_t address,uint32_t limit,
                                 bool bounded,uint32_t *size) {
    *size=0;
    if(!limit)return true;
    if(!address)return false;
    uint32_t maximum=limit<1048576U?limit:1048576U;
    for(uint32_t off=0;off<maximum;) {
        if(off>0xffffffffU-address)return false;
        uint32_t at=address+off,chunk=4096U-(at&4095U);
        if(chunk>maximum-off)chunk=maximum-off;
        if(!(paging_pager_entry(pid,at)&0x400U)) {
            uint32_t available=mm_user_heap_available(at,pid);
            if(chunk>available)chunk=available;
        }
        void *token=NULL;
        if(!chunk || !native_buffer_pin(pid,at,chunk,false,&token))return false;
        const uint8_t *text=(const uint8_t *)(uintptr_t)at;
        uint32_t n=0;while(n<chunk && text[n])n++;
        native_buffer_unpin(pid,at,chunk,token);
        if(n<chunk){*size=off+n;return true;}
        off+=chunk;
    }
    *size=maximum;return bounded && maximum==limit;
}
static bool native_string_dispatch(uint32_t target,const uint32_t *a,uint64_t *result) {
    bool length=TARGET(kstrlen)||TARGET(strlen)||TARGET(bk_string_length);
    bool compare=TARGET(kstrcmp)||TARGET(strcmp)||TARGET(bk_string_compare);
    bool limited_compare=TARGET(kstrncmp)||TARGET(strncmp);
    bool copy=TARGET(kstrcpy)||TARGET(strcpy);
    bool limited_copy=TARGET(kstrncpy)||TARGET(strncpy)||TARGET(bk_string_copy_n);
    bool append=TARGET(kstrcat)||TARGET(strcat)||TARGET(bk_string_concat);
    if(!length && !compare && !limited_compare && !copy && !limited_copy && !append)return false;
    *result=0;
    uint32_t pid=task_current_process_id(),left=0,right=0,limit=1048576U;
    uint32_t first_bytes=0,second_bytes=0;
    bool first=false,second=false,ok=false;
    void *first_token=NULL,*second_token=NULL;
    if(!paging_process_pin(pid)){errno=BK_EFAULT;return true;}
    if(limited_copy || limited_compare)limit=a[2];
    if(length) {
        if(!a[0] && TARGET(bk_string_length))ok=true;
        else ok=native_string_extent(pid,a[0],limit,false,&left);
        if(ok)*result=left;
        goto done;
    }
    bool public_string=TARGET(bk_string_compare)||TARGET(bk_string_copy_n)||TARGET(bk_string_concat);
    if(a[1] || !public_string)
        if(!native_string_extent(pid,a[1],limit,limited_copy||limited_compare,&right))goto done;
    second_bytes=right;
    /* Only the measured bytes are used: the terminator is never trusted by
     * an unbounded strcpy/strcmp after a concurrent user modification. */
    if(second_bytes){second=native_buffer_pin(pid,a[1],second_bytes,false,&second_token);if(!second)goto done;}
    if(compare || limited_compare || append) {
        if(a[0] || !TARGET(bk_string_compare))
            if(!native_string_extent(pid,a[0],limit,limited_compare,&left))goto done;
    }
    if(compare || limited_compare) {
        first_bytes=left;
        if(first_bytes){first=native_buffer_pin(pid,a[0],first_bytes,false,&first_token);if(!first)goto done;}
        uint32_t common=left<right?left:right;
        int order=common?kmemcmp((void *)(uintptr_t)a[0],(void *)(uintptr_t)a[1],common):0;
        if(!order && left!=right) {
            if(left>right)order=((const uint8_t *)(uintptr_t)a[0])[right];
            else order=-((const uint8_t *)(uintptr_t)a[1])[left];
        }
        *result=(uint32_t)order;ok=true;goto done;
    }
    if(limited_copy)first_bytes=limit;
    else {
        if(right==0xffffffffU || left>0xffffffffU-right-1U)goto done;
        first_bytes=left+right+1U;
    }
    if(first_bytes){first=native_buffer_pin(pid,a[0],first_bytes,true,&first_token);if(!first)goto done;}
    uint8_t *dst=(uint8_t *)(uintptr_t)a[0];
    uint32_t count=right;
    if(limited_copy && count>limit)count=limit;
    if(count)memmove(dst+left,(const void *)(uintptr_t)a[1],count);
    if(limited_copy){if(limit>count)kmemset(dst+count,0,limit-count);}
    else dst[left+right]=0;
    *result=a[0];ok=true;
done:
    if(second)native_buffer_unpin(pid,a[1],second_bytes,second_token);
    if(first)native_buffer_unpin(pid,a[0],first_bytes,first_token);
    if(!ok)errno=BK_EFAULT;
    paging_process_unpin(pid);return true;
}

bool native_memory_dispatch(uint32_t target,const uint32_t *a,uint64_t *result) {
    bool copy=TARGET(kmemcpy)||TARGET(memcpy)||TARGET(memmove);
    bool clear=TARGET(kmemset)||TARGET(memset);
    bool compare=TARGET(kmemcmp)||TARGET(memcmp)||TARGET(bk_memory_compare);
    bool release=TARGET(kfree)||TARGET(free)||TARGET(bk_sys_free);
    bool resize=TARGET(krealloc)||TARGET(realloc)||TARGET(bk_sys_realloc);
    bool info=TARGET(mm_get_system_info)||TARGET(bk_sys_memory_info);
    bool metadata=TARGET(mm_set_allocation_owner)||TARGET(mm_set_allocation_subsystem);
    if(!copy && !clear && !compare && !release && !resize && !metadata && !info)
        return native_string_dispatch(target,a,result);
    *result=0;
    uint32_t pid=task_current_process_id();
    if(metadata){
        if(pid==gui_vision_owner_pid() && pid) {
            if(TARGET(mm_set_allocation_owner) && a[1]==0U)
                *result=mm_user_publish((void *)(uintptr_t)a[0],pid);
            else if(TARGET(mm_set_allocation_subsystem) && a[1]<MM_SUBSYSTEM_COUNT)
                *result=mm_user_subsystem((void *)(uintptr_t)a[0],(uint8_t)a[1],pid);
        }
        if(!*result)errno=EACCES;
        return true;
    }
    if(release){if(!mm_user_free((void *)(uintptr_t)a[0],pid))errno=EACCES;return true;}
    if(resize){*result=(uint32_t)(uintptr_t)mm_user_realloc((void *)(uintptr_t)a[0],a[1],pid);return true;}
    if(info){
        void *token=NULL;
        if(!paging_process_pin(pid)){errno=BK_EFAULT;return true;}
        if(native_buffer_pin(pid,a[0],sizeof(system_memory_info_t),true,&token)){
            system_memory_info_t snapshot;mm_get_system_info(&snapshot);
            kmemcpy((void *)(uintptr_t)a[0],&snapshot,sizeof(snapshot));
            native_buffer_unpin(pid,a[0],sizeof(snapshot),token);
            *result=TARGET(bk_sys_memory_info)?1U:0U;
        } else errno=BK_EFAULT;
        paging_process_unpin(pid);return true;
    }
    uint32_t length=a[2];bool first=false,second=false;
    void *first_token=NULL,*second_token=NULL;
    if(!paging_process_pin(pid)){errno=BK_EFAULT;return true;}
    first=native_buffer_pin(pid,a[0],length,!compare,&first_token);
    if(first && !clear)second=native_buffer_pin(pid,a[1],length,false,&second_token);
    if(first && (clear || second)) {
        void *dst=(void *)(uintptr_t)a[0];const void *src=(void *)(uintptr_t)a[1];
        if(clear)*result=(uint32_t)(uintptr_t)kmemset(dst,(int)a[1],length);
        else if(compare)*result=(uint32_t)kmemcmp(dst,src,length);
        else *result=(uint32_t)(uintptr_t)memmove(dst,src,length);
    } else errno=BK_EFAULT;
    if(second)native_buffer_unpin(pid,a[1],length,second_token);
    if(first)native_buffer_unpin(pid,a[0],length,first_token);
    paging_process_unpin(pid);return true;
}
