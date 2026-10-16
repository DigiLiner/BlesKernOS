#include "../include/linux_exec.h"

void syscall_swap_process_ids(uint32_t first,uint32_t second) {
    syscall_resources_t *a=resource_for(first,false),*b=resource_for(second,false);
    if(a)a->process_id=second;
    if(b)b->process_id=first;
}

static int32_t linux_exec_strings(linux_exec_args_t *args,uint32_t vector,bool environment) {
    uint32_t total=args->argc;
    if(vector) for(uint32_t i=0;;i++) {
        uint32_t string;
        if(i>=128 || vector+i*4<vector)return -7;
        if(!copy_from_user(&string,(void *)(uintptr_t)(vector+i*4),4))return -BK_EFAULT;
        if(!string)break;
        if(total>=128)return -7;
        args->offsets[total++]=args->used;
        for(uint32_t j=0;;) {
            if(args->used>=sizeof(args->data))return -7;
            uint32_t address=string+j;
            if(address<string)return -BK_EFAULT;
            uint32_t chunk=4096U-(address&4095U);
            if(chunk>sizeof(args->data)-args->used)chunk=sizeof(args->data)-args->used;
            /* Pin/validate once per page, including environment strings.
             * Never read the next page before looking for this page's NUL. */
            if(!copy_from_user(args->data+args->used,(void *)(uintptr_t)address,chunk))return -BK_EFAULT;
            bool terminated=false;
            for(uint32_t n=0;n<chunk;n++) {
                if(!args->data[args->used++]){terminated=true;break;}
            }
            if(terminated)break;
            j+=chunk;
        }
    }
    if(environment)args->envc=total-args->argc;
    else {
        args->argc=total;
        if(!total){args->argc=1;args->offsets[0]=args->used;args->data[args->used++]=0;}
    }
    return 0;
}

static int32_t sys_linux_execve(registers_t *r) {
    char path[VFS_MAX_PATH];
    if(!copy_string_from_user(path,sizeof(path),(void *)r->ebx))return -BK_EFAULT;
    if(task_thread_count(task_current_process_id()))return -BK_ENOSYS;
    void *allocation;uint32_t size;
    linux_exec_args_t *args=mm_alloc_guarded_stack(sizeof(*args),false,&allocation,&size);
    if(!args)return -BK_ENOMEM;
    kmemset(args,0,sizeof(*args));
    int32_t result=linux_exec_strings(args,r->ecx,false),pid=-1;
    if(!result)result=linux_exec_strings(args,r->edx,true);
    if(result)goto done;
    vfs_dir_entry_t entry;
    if(!vfs_stat(path,&entry)){result=-BK_ENOENT;goto done;}
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] stage=spawn-begin old=%u path=%s\n",
            task_current_process_id(), path);
#endif
    pid=elf_spawn_linux_suspended(path);
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] stage=spawn-end old=%u fresh=%d\n",
            task_current_process_id(), pid);
#endif
    if(pid<0){result=-8;goto done;}
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] stage=stack-begin fresh=%d argc=%u envc=%u used=%u\n",
            pid,args->argc,args->envc,args->used);
#endif
    if(!task_linux_exec_stack(pid,args)){
        kprintf("[WINE:EXEC] stage=stack-fail fresh=%d\n",pid);
        result=-7;goto done;
    }
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] stage=stack-ok fresh=%d\n",pid);
#endif
    uint32_t old=task_current_process_id();
    syscall_resources_t *parent=resource_for(old,false),*child=resource_for(pid,true);
    if(!child){result=-BK_ENOMEM;goto done;}
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] stage=fd-inherit-begin old=%u fresh=%d\n",old,pid);
#endif
    syscall_inherit_files(old,pid);
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] stage=fd-inherit-end old=%u fresh=%d\n",old,pid);
#endif
    if(parent)for(uint32_t fd=0;fd<SYSCALL_FD_SLOTS+3;fd++) {
        int *raw=fd<3?&child->stdio_fds[fd]:&child->fds[fd-3];
        if(resource_fd(parent,fd)>=0 && *raw<0){result=-BK_EMFILE;goto done;}
        if(child->linux_fd_flags[fd]&1) {
            if(*raw>=0)sys_raw_close(*raw);
            *raw=-1;child->linux_fd_flags[fd]=0;
        }
    }
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] stage=commit-begin old=%u fresh=%d\n",old,pid);
#endif
    if(!task_linux_exec_commit(pid)){
        kprintf("[WINE:EXEC] stage=commit-fail old=%u fresh=%d\n",old,pid);
        result=-BK_EAGAIN;goto done;
    }
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[WINE:EXEC] stage=commit-ok process=%u\n",old);
#endif
    pid=-1;
done:
    if(pid>=0)task_request_exit(pid);
    mm_free_guarded_stack(allocation,args,size);return result;
}

/* Aplicar el cierre POSIX solamente cuando el nuevo exec ya puede publicarse. */
void syscall_linux_exec_locks(void) {
    uint32_t owner=task_current_process_id();
    syscall_resources_t *r=resource_for(owner,false);
    if(r)for(unsigned fd=0;fd<SYSCALL_FD_SLOTS+3;fd++)
        if((r->linux_fd_flags[fd]&1) && resource_fd(r,fd)>=0)
            linux_unlock_file(owner,resource_fd(r,fd));
}
