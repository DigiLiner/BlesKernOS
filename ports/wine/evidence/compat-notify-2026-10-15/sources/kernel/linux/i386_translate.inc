/* Primera frontera de compatibilidad con la ABI Linux i386.  Una tarea que
 * activa esta personalidad sigue usando int 0x80, pero desde ese momento EAX
 * se interpreta con la numeracion de Linux 2.6/i386 y los argumentos conservan
 * el orden EBX, ECX, EDX, ESI, EDI, EBP.  Traducimos solamente llamadas cuya
 * semantica ya puede cumplir el kernel; las restantes devuelven -ENOSYS en vez
 * de caer accidentalmente en una syscall nativa con el mismo numero. */
static bool linux_i386_translate(registers_t *regs) {
    uint32_t linux_number = regs->eax;

    switch (linux_number) {
        case 291U: case 332U: case 292U: case 293U: {
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result;
            if(linux_number==292U)result=linux_inotify_add(regs);
            else if(linux_number==293U)result=linux_inotify_remove(regs->ebx,regs->ecx);
            else result=linux_inotify_init(linux_number==332U?regs->ebx:0U);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 123U:
            regs->eax=(uint32_t)sys_linux_modify_ldt(regs,regs->ebx,regs->ecx,regs->edx);
            return false;
        case 85U: case 305U: {
            uint32_t size=linux_number==85?regs->edx:regs->esi;
            int32_t result=-BK_EINVAL;
            if((int32_t)size>0) {
                char path[VFS_MAX_PATH];
                kernel_domains_enter(KDOMAIN_VFS);
                result=linux_at_path(regs,linux_number==85?-100:(int32_t)regs->ebx,
                    (void *)(linux_number==85?regs->ebx:regs->ecx),path);
                if(!result) {
                    char target[VFS_MAX_PATH],original[VFS_MAX_PATH];bool direct=false;
                    const char *user_path=(void *)(linux_number==85?regs->ebx:regs->ecx);
                    if(!copy_user_string(regs,user_path,original,sizeof(original)))result=-BK_EFAULT;
                    else {
                        result=linux_resolve_proc_fd(original,target,&direct);
                        if(!result)result=direct?(int32_t)kstrlen(target):vfs_readlink(path,target,sizeof(target));
                    }
                    if(result>=0) {
                        if((uint32_t)result>size)result=size;
                        if(!copy_to_user((void *)(linux_number==85?regs->ecx:regs->edx),target,result))result=-BK_EFAULT;
                    }
                }
                kernel_domains_exit(KDOMAIN_VFS);
            }
            regs->eax=result;return false;
        }
        case 83U: case 304U: {
            char target[VFS_MAX_PATH],path[VFS_MAX_PATH];
            if(!copy_string_from_user(target,sizeof(target),(void *)regs->ebx)) {regs->eax=-BK_EFAULT;return false;}
            kernel_domains_enter(KDOMAIN_VFS);
            int result=linux_at_path(regs,linux_number==83?-100:(int32_t)regs->ecx,
                (void *)(linux_number==83?regs->ecx:regs->edx),path);
            if(!result)result=vfs_symlink(target,path);
            kernel_domains_exit(KDOMAIN_VFS);regs->eax=result;return false;
        }
        case 196U: {
            char path[VFS_MAX_PATH];
            if(!copy_string_from_user(path,sizeof(path),(void *)regs->ebx)){regs->eax=-BK_EFAULT;return false;}
            kernel_domains_enter(KDOMAIN_VFS);regs->eax=linux_lstat_path(path,(void *)regs->ecx);
            kernel_domains_exit(KDOMAIN_VFS);return false;
        }
        case 55U: case 221U:
            if((regs->ecx>=5 && regs->ecx<=7) || (linux_number==221 && regs->ecx>=12 && regs->ecx<=14)) {
                regs->eax=linux_fcntl_lock(regs);return false;
            }
            regs->eax=SYS_LINUX_FCNTL64;return true;
        case 93U: case 194U: {
            kernel_domains_enter(KDOMAIN_VFS);
            int raw=resource_fd(resource_for(task_current_process_id(),false),regs->ebx);
            int32_t result;
            if(raw<0)result=-BK_EBADF;
            else if(raw>=LINUX_IO_BASE)result=-BK_EINVAL;
            else if((regs->ecx&0x80000000U) || (linux_number==194 && regs->edx))result=-27;
            else if(!(vfs_status_flags(raw)&3))result=-BK_EINVAL;
            else result=vfs_truncate(raw,regs->ecx)?0:-BK_EIO;
            kernel_domains_exit(KDOMAIN_VFS);regs->eax=result;return false;
        }
        case 144U: {
            kernel_domains_enter(KDOMAIN_TASK|KDOMAIN_VFS);
            uint32_t address=regs->ebx,length=regs->ecx,flags=regs->edx;
            int32_t result=0;
            syscall_resources_t *r=resource_for(task_current_process_id(),false);
            if((address&4095U) || (flags&~7U) || (flags&5U)==5U)result=-BK_EINVAL;
            else if(length>0xfffff000U || address+length+4095U<address)result=-BK_ENOMEM;
            else {
                uint32_t end=(address+length+4095U)&~4095U;
                while(address<end) {
                    syscall_vm_map_t *found=NULL;
                    if(r)for(unsigned i=0;i<r->vm.map_capacity;i++) {
                        syscall_vm_map_t *m=&r->vm.maps[i];
                        if(m->backing && address>=m->address && address<m->address+m->length){found=m;break;}
                    }
                    if(!found){result=-BK_ENOMEM;break;}
                    if(found->shared && !vfs_shared_sync(found->shared)){result=-BK_EIO;break;}
                    address=found->address+found->length;
                }
            }
            kernel_domains_exit(KDOMAIN_TASK|KDOMAIN_VFS);regs->eax=result;return false;
        }
        case 42U: case 331U:
            kernel_domains_enter(KDOMAIN_VFS);
            regs->eax=(uint32_t)linux_make_pair(regs->ebx,linux_number==331?regs->ecx:0,true);
            kernel_domains_exit(KDOMAIN_VFS);return false;
        case 102U:
            regs->eax=(uint32_t)linux_socketcall(regs->ebx,regs->ecx);return false;
        case 359U: case 360U: case 361U: case 362U: case 363U: case 364U:
        case 365U: case 366U: case 367U: case 368U: case 369U: case 370U:
        case 371U: case 372U: case 373U: {
            static const uint8_t ops[]={1,8,2,3,4,18,15,14,6,7,11,16,12,17,13};
            uint32_t args[6]={regs->ebx,regs->ecx,regs->edx,regs->esi,regs->edi,regs->ebp};
            regs->eax=(uint32_t)linux_socket_operation(ops[linux_number-359],args);return false;
        }
        case 168U:
            regs->eax=(uint32_t)linux_poll(regs->ebx,regs->ecx,(int32_t)regs->edx);return false;
        case 142U:
            regs->eax=(uint32_t)linux_select(regs);return false;
        case 308U:
            regs->eax=(uint32_t)linux_pselect(regs);return false;
        case 174U: case 175U: case 176U: case 186U:
        case 37U: case 238U: case 270U:
            regs->eax=(uint32_t)task_linux_signal_syscall(regs);return false;
        case 220U:
            kernel_domains_enter(KDOMAIN_VFS);
            regs->eax=(uint32_t)linux_getdents64(regs);
            kernel_domains_exit(KDOMAIN_VFS);return false;
        case 140U: {
            int raw=resource_fd(resource_for(task_current_process_id(),false),regs->ebx);
            int64_t offset=((int64_t)(int32_t)regs->ecx<<32)|regs->edx;
            int32_t result;
            if(raw<0)result=-BK_EBADF;
            else if(raw>=LINUX_IO_BASE)result=-29;
            else if(!user_mapping_write_ok(regs,(void *)regs->esi,8))result=-BK_EFAULT;
            else if(offset!=(int32_t)offset)result=-75;
            else {
                kernel_domains_enter(KDOMAIN_VFS);
                result=vfs_seek(raw,(int32_t)offset,regs->edi);
                kernel_domains_exit(KDOMAIN_VFS);
                if(result>=0) {uint64_t value=(uint32_t)result;result=copy_to_user((void *)regs->esi,&value,8)?0:-BK_EFAULT;}
                else result=-BK_EINVAL;
            }
            regs->eax=(uint32_t)result;return false;
        }
        case 5U: case 295U: {           /* open / openat */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_openat(regs,linux_number==5?-100:(int32_t)regs->ebx,
                (const char *)(uintptr_t)(linux_number==5?regs->ebx:regs->ecx),
                linux_number==5?regs->ecx:regs->edx,
                linux_number==5?regs->edx:regs->esi);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 40U: {                    /* rmdir */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_rmdir(regs,(const char *)(uintptr_t)regs->ebx);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 39U: case 296U: {          /* mkdir / mkdirat */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_mkdirat(regs,
                linux_number==39?-100:(int32_t)regs->ebx,
                (const char *)(uintptr_t)(linux_number==39?regs->ebx:regs->ecx),
                linux_number==39?regs->ecx:regs->edx);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 15U: case 306U: {          /* chmod / fchmodat */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_chmodat(regs,
                linux_number==15?-100:(int32_t)regs->ebx,
                (const char *)(uintptr_t)(linux_number==15?regs->ebx:regs->ecx),
                linux_number==15?regs->ecx:regs->edx);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 94U: {                     /* fchmod */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_fchmod(regs->ebx,regs->ecx);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 320U: case 412U: {         /* utimensat / utimensat_time64 */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_utimensat(regs,linux_number==412U);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 307U: case 439U: {          /* faccessat / faccessat2 */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_faccessat(regs,(int32_t)regs->ebx,
                (const char *)(uintptr_t)regs->ecx,regs->edx,
                linux_number==439U?regs->esi:0U);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 268U: case 269U: {          /* statfs64 / fstatfs64 */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_statfs64(regs,linux_number==269U);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 383U: {                    /* statx */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_statx(regs);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 300U: {                    /* fstatat64 */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_fstatat64(regs);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 180U: {                    /* pread64 */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_pread64(regs);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 181U: {                    /* pwrite64 */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_pwrite64(regs);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 133U: {                    /* fchdir */
            kernel_domains_enter(KDOMAIN_VFS);
            int32_t result=sys_linux_fchdir(regs->ebx);
            kernel_domains_exit(KDOMAIN_VFS);
            regs->eax=(uint32_t)result;return false;
        }
        case 146U: {                    /* writev */
            kernel_domains_enter(KDOMAIN_VFS|KDOMAIN_DRIVER);
            int32_t result=sys_linux_writev(regs);
            kernel_domains_exit(KDOMAIN_VFS|KDOMAIN_DRIVER);
            regs->eax=(uint32_t)result;return false;
        }
        case 1U:                         /* exit */
        case 252U:                       /* exit_group */
            regs->eax = SYS_EXIT;
            return true;
        case 3U: regs->eax = SYS_READ; return true;
        case 4U: regs->eax = SYS_WRITE; return true;
        case 6U: regs->eax = SYS_CLOSE; return true;
        case 7U: regs->eax = SYS_WAITPID; return true;
        case 10U: regs->eax = SYS_UNLINK; return true;
        case 11U: regs->eax = SYS_LINUX_EXECVE; return true;
        case 12U: regs->eax = SYS_CHDIR; return true;
        case 13U: regs->eax = SYS_LINUX_TIME; return true;
        case 19U: regs->eax = SYS_FILE_SEEK; return true;
        case 20U: regs->eax = SYS_GETPID; return true;
        case 24U:                         /* getuid */
        case 47U:                         /* getgid */
        case 49U:                         /* geteuid */
        case 50U:                         /* getegid */
        case 199U:                        /* getuid32 */
        case 200U:                        /* getgid32 */
        case 201U:                        /* geteuid32 */
        case 202U:                        /* getegid32 */
            regs->eax = 0U;
            return false;
        case 60U:                         /* umask */
            regs->eax = linux_set_umask(regs->ebx);
            return false;
        case 65U:                         /* getpgrp */
            regs->eax = task_current_process_id();
            return false;
        case 132U:                        /* getpgid */
            regs->eax = regs->ebx ? regs->ebx : task_current_process_id();
            return false;
        case 33U: regs->eax = SYS_LINUX_ACCESS; return true;
        case 38U: regs->eax = SYS_RENAME; return true;
        case 41U:                        /* dup */
            regs->ecx = (uint32_t)-1;
            regs->eax = SYS_FILE_DUP;
            return true;
        case 63U: regs->eax = SYS_FILE_DUP; return true; /* dup2 */
        case 64U: regs->eax = SYS_GETPPID; return true;
        case 54U: regs->eax = SYS_LINUX_IOCTL; return true;
        case 78U: regs->eax = SYS_LINUX_GETTIMEOFDAY; return true;
        case 45U: regs->eax = SYS_VM_BRK; return true;
        case 90U: {                      /* old_mmap(struct mmap_arg_struct*) */
            uint32_t arguments[6];
            if (!copy_from_user(arguments,
                    (const void *)(uintptr_t)regs->ebx, sizeof(arguments))) {
                regs->eax = (uint32_t)-BK_EFAULT;
                return false;
            }
            regs->ebx=arguments[0]; regs->ecx=arguments[1];
            if (arguments[5] & (PAGING_PAGE_SIZE-1U)) {
                regs->eax=(uint32_t)-BK_EINVAL;return false;
            }
            regs->edx=arguments[2]; regs->esi=arguments[3];
            regs->edi=arguments[4]; regs->ebp=arguments[5] / PAGING_PAGE_SIZE;
            regs->eax=SYS_VM_MMAP;
            return true;
        }
        case 91U: regs->eax = SYS_VM_MUNMAP; return true;
        case 118U: regs->eax = SYS_FILE_SYNC; return true;
        case 120U: regs->eax = SYS_LINUX_CLONE; return true;
        case 116U: regs->eax = SYS_LINUX_SYSINFO; return true;
        case 122U: regs->eax = SYS_LINUX_UNAME; return true;
        case 125U: regs->eax = SYS_VM_MPROTECT; return true;
        case 158U: regs->eax = SYS_YIELD; return true;
        case 162U: regs->eax = SYS_LINUX_NANOSLEEP; return true;
        case 192U: regs->eax = SYS_VM_MMAP; return true; /* mmap2 */
        case 195U:                        /* stat64 */
            regs->eax = SYS_LINUX_STAT64;
            return true;
        case 197U: regs->eax = SYS_LINUX_FSTAT64; return true;
        case 191U:                        /* ugetrlimit */
        case 76U:                         /* old getrlimit */
            regs->eax = SYS_LINUX_GETRLIMIT;
            return true;
        case 219U: regs->eax = SYS_LINUX_MADVISE; return true;
        case 224U:                        /* gettid */
            regs->eax = task_current_tid();
            return false;
        case 258U: regs->eax = SYS_LINUX_SET_TID_ADDRESS; return true;
        case 265U: regs->eax = SYS_LINUX_CLOCK_GETTIME; return true;
        case 266U: regs->eax = SYS_LINUX_CLOCK_GETRES; return true;
        case 403U:                       /* clock_gettime64 */
            regs->eax = (uint32_t)sys_linux_clock64(regs, regs->ebx,
                (void *)(uintptr_t)regs->ecx, false);
            return false;
        case 406U:                       /* clock_getres_time64 */
            regs->eax = (uint32_t)sys_linux_clock64(regs, regs->ebx,
                (void *)(uintptr_t)regs->ecx, true);
            return false;
        case 311U: regs->eax = SYS_LINUX_SET_ROBUST_LIST; return true;
        case 312U: regs->eax=(uint32_t)task_linux_robust(regs,true);return false;
        case 183U: regs->eax = SYS_GETCWD; return true;
        case 240U: regs->eax = SYS_LINUX_FUTEX; return true;
        case 241U: regs->eax = SYS_LINUX_SCHED_SETAFFINITY; return true;
        case 242U: regs->eax = SYS_LINUX_SCHED_GETAFFINITY; return true;
        case 243U: regs->eax = SYS_LINUX_SET_THREAD_AREA; return true;
        case 244U: regs->eax = SYS_LINUX_GET_THREAD_AREA; return true;
        case 267U:                       /* clock_nanosleep */
            if(regs->ecx!=0U) {
                regs->eax=(uint32_t)-BK_EINVAL;
                return false;
            }
            regs->ebx=regs->edx;
            regs->ecx=regs->esi;
            regs->eax=SYS_LINUX_NANOSLEEP;
            return true;
        case 340U: regs->eax=SYS_LINUX_PRLIMIT64; return true;
        case 355U: regs->eax=SYS_LINUX_GETRANDOM; return true;
        default:
            regs->eax = (uint32_t)-BK_ENOSYS;
            return false;
    }
}
