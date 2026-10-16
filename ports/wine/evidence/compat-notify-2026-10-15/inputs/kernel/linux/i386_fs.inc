/* Adaptadores Linux sobre la tabla FD existente. No hay FD paralelos. */
static int32_t linux_at_path(registers_t *regs,int32_t dirfd,const char *user_path,char *full) {
    char path[VFS_MAX_PATH], joined[VFS_MAX_PATH];
    if (!copy_user_string(regs,user_path,path,sizeof(path))) return -BK_EFAULT;
    if (!path[0]) return -BK_ENOENT;
    if (path[0]!='/' && dirfd!=-100) {
        int raw=resource_fd(resource_for(task_current_process_id(),false),dirfd);
        const char *base=vfs_fd_path(raw);
        if (!base) return raw<0 ? -BK_EBADF : -20;
        if (!vfs_is_directory(raw)) return -20;
        if (kstrlen(base)+kstrlen(path)+2>sizeof(joined)) return -36;
        kstrcpy(joined,base);kstrcat(joined,"/");kstrcat(joined,path);
    } else kstrcpy(joined,path);
    return vfs_normalize_nofollow(joined,full)?0:-40;
}

static int32_t sys_linux_rmdir(registers_t *regs, const char *user_path) {
    char path[VFS_MAX_PATH], target[VFS_MAX_PATH];
    if (!copy_user_string(regs, user_path, target, sizeof(target))) return -BK_EFAULT;
    uint32_t length = kstrlen(target);
    while (length && target[length - 1U] == '/') target[--length] = 0;
    const char *last = target + length;
    while (last > target && last[-1] != '/') --last;
    if (!kstrcmp(last, ".")) return -BK_EINVAL;
    if (!kstrcmp(last, "..")) return -39;
    int32_t result = linux_at_path(regs, -100, user_path, path);
    if (result < 0) return result;
    if (!kstrcmp(path, "/")) return -16; /* EBUSY */
    if (vfs_readlink(path, target, sizeof(target)) >= 0) return -20;
    vfs_dir_entry_t entry, children[3];
    if (!vfs_stat(path, &entry)) return -BK_ENOENT;
    if (entry.type != VFS_NODE_DIR) return -20;
    uint32_t count = 0;
    if (!vfs_listdir(path, children, 3, &count)) return -BK_EIO;
    for (uint32_t i = 0; i < count; ++i)
        if (kstrcmp(children[i].name, ".") && kstrcmp(children[i].name, ".."))
            return -39; /* ENOTEMPTY */
    return vfs_remove(path) ? 0 : -BK_EIO;
}

static int32_t sys_linux_openat(registers_t *regs, int32_t dirfd,
                                const char *user_path, uint32_t flags,
                                uint32_t create_mode) {
    char path[VFS_MAX_PATH];
    uint32_t native, access=flags&3U;
    int32_t fd=linux_at_path(regs,dirfd,user_path,path);
    if (fd<0) return fd;
    if (access==3U) return -BK_EINVAL;
    char link_target[VFS_MAX_PATH];
    bool is_link=vfs_readlink(path,link_target,sizeof(link_target))>=0;
    if(is_link && (flags&0xc0U)==0xc0U)return -17;
    if(is_link && (flags&0x20000U))return -40;
    if (flags & ~(3U|0x40U|0x80U|0x200U|0x800U|0x8000U|0x10000U|0x20000U|0x80000U)) return -BK_ENOSYS;
    if(!kstrcmp(path,"/dev/null") || !kstrcmp(path,"/dev/blesgfx") ||
       !kstrcmp(path,"/dev/dsp") || !kstrcmp(path,"/dev/mixer")) {
        if(flags&0x10000U)return -20;
        syscall_resources_t *r=resource_for(task_current_process_id(),true);
        if(!r)return -BK_EMFILE;
        for(int n=0;n<SYSCALL_FD_SLOTS+3;n++) if(resource_fd(r,n)<0) {
            int raw=linux_device_open(!kstrcmp(path,"/dev/null")?2:
                !kstrcmp(path,"/dev/dsp")?4:!kstrcmp(path,"/dev/mixer")?5:3,
                                      flags&(3U|0x800U));if(raw<0)return raw;
            if(n<3)r->stdio_fds[n]=raw;else r->fds[n-3]=raw;
            r->linux_fd_flags[n]=(flags&0x80000U)?1:0;return n;
        }
        return -BK_EMFILE;
    }
    vfs_dir_entry_t entry;
    bool exists=vfs_stat(path,&entry);
    if ((flags&0xc0U)==0xc0U && exists) return -17;
    if ((flags&0x10000U) && (!exists || entry.type!=VFS_NODE_DIR)) return exists?-20:-BK_ENOENT;
    if (exists && entry.type==VFS_NODE_DIR && (access || (flags&0x240U))) return -21;
    native=access==0U ? VFS_O_RDONLY : access==1U ? VFS_O_WRONLY : VFS_O_RDWR;
    if (flags&0x40U) native|=VFS_O_CREATE;
    if (flags&0x200U) native|=VFS_O_TRUNC;
    if (exists && entry.type==VFS_NODE_DIR) native|=VFS_O_DIRECTORY;
    native|=flags&0x800U;
    syscall_resources_t *r=resource_for(task_current_process_id(),true);
    if (!r) return -BK_EMFILE;
    for (fd=3;fd<SYSCALL_FD_SLOTS+3;fd++) {
        if (r->fds[fd-3]>=0) continue;
        int raw=vfs_open(path,native);
        if (raw<0) {
            if (flags&0x40U) {
                uint64_t total_bytes=0,free_bytes=0;
                fat_type_t fs_type=FAT_TYPE_NONE;
                (void)vfs_get_space_path_type(path,&total_bytes,&free_bytes,&fs_type);
                kprintf("[LINUX:OPEN] create-fail pid=%u fs=%u free_kib=%u flags=%x path=%s vfs=%s\n",
                        task_current_process_id(),(uint32_t)fs_type,
                        (uint32_t)(free_bytes/1024U),flags,path,vfs_last_error_text());
            }
            return -BK_ENOENT;
        }
        if (!exists && (flags&0x40U) &&
            !vfs_set_mode(path,linux_creation_mode(create_mode))) {
            vfs_close(raw);
            (void)vfs_remove(path);
            return -BK_EIO;
        }
        uint64_t dev,ino;
        if (!vfs_fd_identity(raw,&dev,&ino)) { vfs_close(raw);return -BK_EIO; }
        r->fds[fd-3]=raw;
        r->linux_fd_flags[fd]=(flags&0x80000U)?1:0;
        r->linux_open_flags[fd]=flags&(3U|0x8000U|0x10000U|0x800U);
        return fd;
    }
    return -BK_EMFILE;
}

static int32_t sys_linux_mkdirat(registers_t *regs, int32_t dirfd,
                                 const char *user_path, uint32_t mode) {
    char path[VFS_MAX_PATH];
    vfs_dir_entry_t entry;
    int32_t result=linux_at_path(regs,dirfd,user_path,path);
    if(result<0)return result;
    if(vfs_stat(path,&entry))return -17; /* EEXIST */
    if(vfs_mkdir_mode(path,linux_creation_mode(mode)))return 0;
    {
        /* mkdir failures are rare and important during installers.  Keep this
         * diagnostic unconditional so a release build still tells us the
         * exact pathname, backend and remaining space. */
        uint64_t total_bytes=0,free_bytes=0;
        fat_type_t fs_type=FAT_TYPE_NONE;
        (void)vfs_get_space_path_type(path,&total_bytes,&free_bytes,&fs_type);
        kprintf("[LINUX:MKDIR] fail pid=%u fs=%u free_kib=%u path=%s vfs=%s\n",
                task_current_process_id(),(uint32_t)fs_type,
                (uint32_t)(free_bytes/1024U),path,vfs_last_error_text());
    }
    return -BK_EIO;
}

static int32_t sys_linux_chmodat(registers_t *regs, int32_t dirfd,
                                 const char *user_path, uint32_t mode) {
    char path[VFS_MAX_PATH];
    vfs_dir_entry_t entry;
    int32_t result=linux_at_path(regs,dirfd,user_path,path);
    if(result<0)return result;
    if(!vfs_stat(path,&entry))return -BK_ENOENT;
    return vfs_set_mode(path,(uint16_t)(mode&07777U))?0:-BK_EIO;
}

static int32_t sys_linux_fchmod(uint32_t fd,uint32_t mode) {
    syscall_resources_t *resource=
        resource_for(task_current_process_id(),false);
    int raw=resource_fd(resource,fd);
    if(raw<0)return -BK_EBADF;
    if(raw>=LINUX_IO_BASE)return -BK_EINVAL;
    return vfs_set_fd_mode(raw,(uint16_t)(mode&07777U))?0:-BK_EIO;
}

static int32_t sys_linux_utimensat(registers_t *regs, bool time64) {
    /* Wine's SetFileTime uses futimens: pathname=NULL and dirfd is the file
     * descriptor. FAT stores access dates and modification dates/times. */
    const uint32_t utime_now = 0x3fffffffU, utime_omit = 0x3ffffffeU;
    const char *path;
    char full[VFS_MAX_PATH];
    int raw = -1;
    uint64_t value[2] = {0U, 0U};
    bool set_time[2] = {true, true};
    uint32_t flags = regs->esi;
    if (flags & ~(0x100U | 0x1000U)) return -BK_EINVAL;
    if (!regs->ecx) {
        raw = resource_fd(resource_for(task_current_process_id(), false),
                          regs->ebx);
        if (raw < 0) return -BK_EBADF;
        if (raw >= LINUX_IO_BASE) return -BK_EINVAL;
        path = vfs_fd_path(raw);
        if (!path) return -BK_EBADF;
    } else {
        int32_t result = linux_at_path(regs, (int32_t)regs->ebx,
                                       (const char *)(uintptr_t)regs->ecx,
                                       full);
        if (result < 0) return result;
        path = full;
    }
    if (regs->edx) {
        if (time64) {
            linux_i386_timespec64_t times[2];
            if (!copy_from_user(times, (const void *)(uintptr_t)regs->edx,
                                sizeof(times))) return -BK_EFAULT;
            for (uint32_t i = 0U; i < 2U; i++) {
                uint32_t ns = (uint32_t)times[i].nanoseconds;
                if (ns == utime_omit) set_time[i] = false;
                else if (ns == utime_now) value[i] = linux_realtime_seconds();
                else if (times[i].nanoseconds < 0 ||
                         times[i].nanoseconds >= 1000000000LL) return -BK_EINVAL;
                else value[i] = times[i].seconds < 0 ? 0U :
                                (uint64_t)times[i].seconds;
            }
        } else {
            linux_i386_timespec_t times[2];
            if (!copy_from_user(times, (const void *)(uintptr_t)regs->edx,
                                sizeof(times))) return -BK_EFAULT;
            for (uint32_t i = 0U; i < 2U; i++) {
                uint32_t ns = (uint32_t)times[i].nanoseconds;
                if (ns == utime_omit) set_time[i] = false;
                else if (ns == utime_now) value[i] = linux_realtime_seconds();
                else if (times[i].nanoseconds < 0 ||
                         times[i].nanoseconds >= 1000000000) return -BK_EINVAL;
                else value[i] = times[i].seconds < 0 ? 0U :
                                (uint32_t)times[i].seconds;
            }
        }
    } else value[0] = value[1] = linux_realtime_seconds();
    if (raw >= 0 && !vfs_flush(raw)) return -BK_EIO;
    vfs_dir_entry_t entry;
    if (!vfs_stat(path, &entry)) return -BK_ENOENT;
    if (!set_time[0] && !set_time[1]) return 0;
    return vfs_set_times(path, set_time[0], value[0],
                         set_time[1], value[1]) ? 0 : -BK_EIO;
}

static int32_t sys_linux_faccessat(registers_t *regs, int32_t dirfd,
                                    const char *user_path, uint32_t access_mode,
                                    uint32_t flags) {
    /* Linux access(2) modes are F_OK=0, X_OK=1, W_OK=2, R_OK=4.
     * faccessat2 additionally accepts AT_EACCESS, AT_SYMLINK_NOFOLLOW and
     * AT_EMPTY_PATH. BlesKernOS currently exposes synthetic uid/gid 0 and FAT
     * has no symlinks, so AT_EACCESS/NOFOLLOW do not change path resolution.
     * Still validate the ABI flags so callers don't get false success. */
    const uint32_t allowed_flags = 0x200U | 0x100U | 0x1000U;
    char path[VFS_MAX_PATH];
    vfs_dir_entry_t entry;
    uint16_t mode = 0U;
    int32_t result;

    if (access_mode & ~7U) return -BK_EINVAL;
    if (flags & ~allowed_flags) return -BK_EINVAL;

    /* AT_EMPTY_PATH: operate on dirfd itself when pathname is empty. */
    if (flags & 0x1000U) {
        char user_copy[VFS_MAX_PATH];
        if (!copy_user_string(regs, user_path, user_copy, sizeof(user_copy)))
            return -BK_EFAULT;
        if (!user_copy[0]) {
            const char *fd_path;
            int raw = -1;
            if (dirfd == -100) {
                fd_path = vfs_getcwd();
                if (!fd_path || !*fd_path) return -BK_ENOENT;
            } else {
                raw = resource_fd(resource_for(task_current_process_id(), false),
                                  (uint32_t)dirfd);
                if (raw < 0) return -BK_EBADF;
                if (raw >= LINUX_IO_BASE) return -BK_EINVAL;
                fd_path = vfs_fd_path(raw);
                if (!fd_path) return -BK_EBADF;
            }
            if (!vfs_stat(fd_path, &entry)) return -BK_ENOENT;
            if (!access_mode) return 0;
            if (raw >= 0 ? !vfs_fd_mode(raw, &mode) : !vfs_get_mode(fd_path, &mode))
                mode = entry.type == VFS_NODE_DIR ? 0755U : 0644U;
            /* Synthetic root can read/write existing objects. Linux still
             * requires at least one execute bit for X_OK on regular files. */
            if ((access_mode & 1U) && entry.type != VFS_NODE_DIR &&
                !(mode & 0111U)) return -BK_EACCES;
            return 0;
        }
    }

    result = linux_at_path(regs, dirfd, user_path, path);
    if (result < 0) return result;
    if (!vfs_stat(path, &entry)) return -BK_ENOENT;
    if (!access_mode) return 0;
    if (!vfs_get_mode(path, &mode))
        mode = entry.type == VFS_NODE_DIR ? 0755U : 0644U;
    if ((access_mode & 1U) && entry.type != VFS_NODE_DIR && !(mode & 0111U))
        return -BK_EACCES;
    return 0;
}

/* Linux i386 statfs64 ABI.  On 32-bit x86 the kernel structure is 84 bytes:
 * 32-bit scalar words plus 64-bit block/file counters with 4-byte alignment. */
typedef struct PACKED {
    uint32_t type;
    uint32_t block_size;
    uint64_t blocks;
    uint64_t blocks_free;
    uint64_t blocks_available;
    uint64_t files;
    uint64_t files_free;
    uint32_t fsid[2];
    uint32_t name_length;
    uint32_t fragment_size;
    uint32_t flags;
    uint32_t spare[4];
} linux_i386_statfs64_t;
typedef char linux_i386_statfs64_must_be_84_bytes[
    sizeof(linux_i386_statfs64_t)==84U ? 1 : -1];

typedef struct PACKED {
    int64_t seconds;
    uint32_t nanoseconds;
    int32_t reserved;
} linux_i386_statx_timestamp_t;

typedef struct PACKED {
    uint32_t mask;
    uint32_t block_size;
    uint64_t attributes;
    uint32_t links;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint16_t spare0;
    uint64_t inode;
    uint64_t size;
    uint64_t blocks;
    uint64_t attributes_mask;
    linux_i386_statx_timestamp_t access_time;
    linux_i386_statx_timestamp_t birth_time;
    linux_i386_statx_timestamp_t change_time;
    linux_i386_statx_timestamp_t modify_time;
    uint32_t rdev_major;
    uint32_t rdev_minor;
    uint32_t dev_major;
    uint32_t dev_minor;
    uint64_t mount_id;
    uint32_t direct_io_memory_alignment;
    uint32_t direct_io_offset_alignment;
    uint64_t subvolume;
    uint32_t atomic_write_unit_min;
    uint32_t atomic_write_unit_max;
    uint32_t atomic_write_segments_max;
    uint32_t spare1;
    uint64_t spare3[9];
} linux_i386_statx_t;
typedef char linux_i386_statx_must_be_256_bytes[
    sizeof(linux_i386_statx_t)==256U ? 1 : -1];

static int32_t linux_stat64_kernel_path(const char *path,bool nofollow,
                                        linux_i386_stat64_t *status) {
    vfs_dir_entry_t entry;
    uint64_t device,inode;
    uint16_t mode;

    if(!path || !status)return -BK_EINVAL;
    if(nofollow) {
        char target[VFS_MAX_PATH];
        int n=vfs_readlink(path,target,sizeof(target));
        if(n>=0) {
            linux_fill_stat64(status,(uint32_t)n,false);
            status->mode=0120777U;
            if(!vfs_identity_nofollow(path,&device,&inode))return -BK_EIO;
            status->device=device;status->inode=inode;
            status->inode_legacy=(uint32_t)inode;
            return 0;
        }
        if(n!=-22 && n!=-2)return n;
    }
    if(linux_unix_stat(path,status))return 0;
    if(!vfs_stat(path,&entry))return -BK_ENOENT;
    linux_fill_stat64(status,entry.size,entry.type==VFS_NODE_DIR);
    linux_apply_vfs_times(status,path);
    if(vfs_get_mode(path,&mode))
        status->mode=(entry.type==VFS_NODE_DIR?0040000U:0100000U)|
                     (uint32_t)(mode&07777U);
    if(!vfs_identity(path,&device,&inode))return -BK_EIO;
    status->device=device;status->inode=inode;
    status->inode_legacy=(uint32_t)inode;
    return 0;
}

static void linux_statx_from_stat64(linux_i386_statx_t *out,
                                    const linux_i386_stat64_t *in) {
    kmemset(out,0,sizeof(*out));
    /* STATX_BASIC_STATS.  BlesKernOS does not yet persist a distinct birth
     * time or mount id, so those bits stay clear instead of fabricating data. */
    out->mask=0x000007ffU;
    out->block_size=in->block_size?in->block_size:PAGING_PAGE_SIZE;
    out->links=in->links;
    out->uid=in->uid;out->gid=in->gid;
    out->mode=(uint16_t)in->mode;
    out->inode=in->inode;
    out->size=(uint64_t)in->size;
    out->blocks=in->blocks;
    out->access_time.seconds=(int64_t)in->access_time;
    out->access_time.nanoseconds=in->access_nsec;
    out->modify_time.seconds=(int64_t)in->modify_time;
    out->modify_time.nanoseconds=in->modify_nsec;
    out->change_time.seconds=(int64_t)in->change_time;
    out->change_time.nanoseconds=in->change_nsec;
    /* Device ids in the compatibility VFS are synthetic scalar ids, not
     * Linux dev_t encodings. Expose them as a minor number consistently. */
    out->dev_minor=(uint32_t)in->device;
    out->rdev_minor=(uint32_t)in->rdevice;
}

static int32_t sys_linux_statx(registers_t *regs) {
    const uint32_t allowed_flags=0x100U|0x800U|0x1000U|0x2000U|0x4000U;
    uint32_t flags=regs->edx;
    char supplied[VFS_MAX_PATH],path[VFS_MAX_PATH];
    linux_i386_stat64_t status;
    linux_i386_statx_t out;
    int32_t result;

    if(flags&~allowed_flags)return -BK_EINVAL;
    if((flags&0x6000U)==0x6000U)return -BK_EINVAL;
    if(regs->esi&0x80000000U)return -BK_EINVAL; /* STATX__RESERVED */
    if(!user_mapping_write_ok(regs,(void *)(uintptr_t)regs->edi,sizeof(out)))
        return -BK_EFAULT;
    if(!copy_user_string(regs,(const char *)(uintptr_t)regs->ecx,
                         supplied,sizeof(supplied)))return -BK_EFAULT;

    if(!supplied[0]) {
        if(!(flags&0x1000U))return -BK_ENOENT; /* AT_EMPTY_PATH */
        if((int32_t)regs->ebx==-100) {
            const char *cwd=vfs_getcwd();
            if(!cwd || !*cwd)return -BK_ENOENT;
            kstrncpy(path,cwd,sizeof(path)-1U);path[sizeof(path)-1U]=0;
            result=linux_stat64_kernel_path(path,false,&status);
        } else {
            int raw=resource_fd(resource_for(task_current_process_id(),false),
                                regs->ebx);
            if(raw<0)return -BK_EBADF;
            if(raw>=LINUX_IO_BASE) {
                if(!linux_io_stat(raw,&status))return -BK_EBADF;
                result=0;
            } else {
                const char *fd_path=vfs_fd_path(raw);
                if(!fd_path)return -BK_EBADF;
                result=linux_stat64_kernel_path(fd_path,false,&status);
            }
        }
    } else {
        result=linux_at_path(regs,(int32_t)regs->ebx,
            (const char *)(uintptr_t)regs->ecx,path);
        if(result>=0)
            result=linux_stat64_kernel_path(path,(flags&0x100U)!=0,&status);
    }
    if(result<0)return result;
    linux_statx_from_stat64(&out,&status);
    return copy_to_user((void *)(uintptr_t)regs->edi,&out,sizeof(out))?0:-BK_EFAULT;
}

static int32_t sys_linux_statfs64(registers_t *regs,bool by_fd) {
    linux_i386_statfs64_t out;
    char path[VFS_MAX_PATH];
    uint64_t total_bytes,free_bytes;
    fat_type_t fs_type=FAT_TYPE_NONE;
    const char *selected=path;
    uint32_t block_size=PAGING_PAGE_SIZE;

    if(regs->ecx!=sizeof(out))return -BK_EINVAL;
    if(!user_mapping_write_ok(regs,(void *)(uintptr_t)regs->edx,sizeof(out)))
        return -BK_EFAULT;
    if(by_fd) {
        int raw=resource_fd(resource_for(task_current_process_id(),false),regs->ebx);
        const char *fd_path;
        if(raw<0)return -BK_EBADF;
        if(raw>=LINUX_IO_BASE)return -BK_EINVAL;
        fd_path=vfs_fd_path(raw);
        if(!fd_path)return -BK_EBADF;
        kstrncpy(path,fd_path,sizeof(path)-1U);path[sizeof(path)-1U]=0;
    } else {
        int32_t result=linux_at_path(regs,-100,
            (const char *)(uintptr_t)regs->ebx,path);
        if(result<0)return result;
    }
    bool optical=vfs_get_optical_space(selected,&total_bytes);
    if(optical) {free_bytes=0;block_size=2048U;}
    else if(!vfs_get_space_path_type(selected,&total_bytes,&free_bytes,&fs_type))
        return -BK_EIO;
    kmemset(&out,0,sizeof(out));
    /* Wine treats MSDOS_SUPER_MAGIC as proof that the host filesystem is
     * case-insensitive and therefore skips its Unicode directory-scan
     * fallback.  That is valid for the FAT backend, but not for BFS4: BFS4
     * stores UTF-8 bytes correctly yet currently folds only ASCII case.
     * Give BFS4 its own magic (little-endian "BFS4") so Wine performs its
     * normal case-insensitive lookup for names such as Diseños/DISEÑOS. */
    out.type=optical?0x9660U:fs_type==FAT_TYPE_BFS4 ? 0x34534642U : 0x4d44U;
    out.block_size=block_size;
    out.blocks=(total_bytes+block_size-1U)/block_size;
    out.blocks_free=free_bytes/block_size;
    out.blocks_available=out.blocks_free;
    out.fsid[0]=1U;
    out.name_length=optical?31U:255U;
    out.fragment_size=block_size;
    if(optical)out.flags=1U; /* ST_RDONLY */
    return copy_to_user((void *)(uintptr_t)regs->edx,&out,sizeof(out))?0:-BK_EFAULT;
}

static int32_t sys_linux_fstatat64(registers_t *regs) {
    char path[VFS_MAX_PATH];
    if (regs->esi & ~(0x100U|0x800U|0x1000U)) return -BK_EINVAL;
    if (!copy_user_string(regs,(const char *)(uintptr_t)regs->ecx,path,sizeof(path)))
        return -BK_EFAULT;
    if (!path[0] && (regs->esi&0x1000U))
        return sys_linux_fstat64(regs,regs->ebx,(void *)(uintptr_t)regs->edx);
    if (!path[0]) return -BK_ENOENT;
    int32_t error=linux_at_path(regs,regs->ebx,(const char *)(uintptr_t)regs->ecx,path);
    if (error<0) return error;
    if (!user_mapping_write_ok(regs,(void *)(uintptr_t)regs->edx,sizeof(linux_i386_stat64_t))) return -BK_EFAULT;
    return (regs->esi&0x100U)?linux_lstat_path(path,(void *)(uintptr_t)regs->edx):
        linux_stat_path(path,(void *)(uintptr_t)regs->edx);
}

static int32_t linux_getdents64(registers_t *regs) {
    int raw=resource_fd(resource_for(task_current_process_id(),false),regs->ebx);
    if (raw<0) return -BK_EBADF;
    if (!vfs_is_directory(raw)) return -20;
    if (!user_mapping_write_ok(regs,(void *)(uintptr_t)regs->ecx,regs->edx)) return -BK_EFAULT;
    vfs_dir_entry_t *entries=NULL;
    const fat_dir_entry_t *fat_entries=NULL;
    uint32_t capacity=128,count=0,used=0,index=0,cookie=(uint32_t)vfs_tell(raw);
    int32_t error=-BK_ENOMEM;
    bool fat = vfs_fat_directory(raw, &fat_entries, &count);
    if (!fat) do {
        if (entries) { kfree(entries);capacity*=2; }
        entries=kmalloc(capacity*sizeof(*entries));
        if (!entries) goto done;
        if (!vfs_listdir(vfs_fd_path(raw),entries,capacity,&count)) { error=-BK_EIO;goto done; }
    } while(count==capacity && capacity<8192);
    for (uint32_t n=0;n<count+2;n++) {
        const char *name=n<2?(n?"..":"."):(fat ? fat_entries[n-2].name : entries[n-2].name);
        if(n>=2 && (!kstrcmp(name,".") || !kstrcmp(name,"..")))continue;
        if(index++<cookie)continue;
        uint8_t record[280];char path[VFS_MAX_PATH],full[VFS_MAX_PATH];uint64_t dev,ino;
        uint32_t size=(19+kstrlen(name)+1+7)&~7U;
        if (size>regs->edx-used) { error=used?(int32_t)used:-BK_EINVAL;goto done; }
        uint16_t mode = 0040000U;
        if (n >= 2 && fat) {
            if (!vfs_fat_directory_identity(raw,n-2,&ino,&mode)) {error=-BK_EIO;goto done;}
        } else {
        const char *base=vfs_fd_path(raw);
        if(kstrlen(base)+kstrlen(name)+2>sizeof(path)) { error=-36;goto done; }
        kstrcpy(path,base);kstrcat(path,"/");kstrcat(path,name);
        if (!vfs_normalize_path(path,full) || !vfs_identity(full,&dev,&ino)) {error=-BK_EIO;goto done;}
        }
        kmemset(record,0,size);kmemcpy(record,&ino,8);
        uint64_t next=index;kmemcpy(record+8,&next,8);
        *(uint16_t *)(record+16)=(uint16_t)size;
        record[18]=n<2 ? 4 : fat ? (mode >> 12) : entries[n-2].type==VFS_NODE_DIR?4:8;
        kstrcpy((char *)record+19,name);
        if(!copy_to_user((void *)(uintptr_t)(regs->ecx+used),record,size)){error=-BK_EFAULT;goto done;}
        used+=size;vfs_seek(raw,(int32_t)index,0);
    }
    error=(int32_t)used;
done:
    if(entries)kfree(entries);
    return used?(int32_t)used:error;
}

/* Linux i386 pread64(2), syscall 180.  The offset is passed split as
 * pos_lo=ESI,pos_hi=EDI.  Unlike read(2), pread64 must not modify the shared
 * descriptor position, so keep the whole seek/read/restore sequence inside
 * KDOMAIN_VFS (the caller in i386_translate does that). */
static int32_t sys_linux_pread64(registers_t *regs) {
    syscall_resources_t *resource=resource_for(task_current_process_id(),false);
    int raw=resource_fd(resource,regs->ebx);
    uint32_t count=regs->edx;
    int64_t offset=((int64_t)(int32_t)regs->edi<<32)|(uint32_t)regs->esi;
    int32_t old_position,result;
    uint32_t total=0;

    if(raw<0)return -BK_EBADF;
    /* Pipes/socketpairs are not seekable. */
    if(raw>=LINUX_IO_BASE)return -29; /* ESPIPE */
    if(offset<0)return -BK_EINVAL;
    /* vfs_seek currently exposes a signed 31-bit file position.  Report the
     * representational limit explicitly instead of truncating a 64-bit offset. */
    if(offset>0x7fffffffLL)return -75; /* EOVERFLOW */
    if(count>0x7fffffffU)return -BK_EINVAL;
    if(count && !user_mapping_write_ok(regs,(void *)(uintptr_t)regs->ecx,count))
        return -BK_EFAULT;

    old_position=vfs_tell(raw);
    if(old_position<0)return -29; /* ESPIPE */
    if(vfs_seek(raw,(int32_t)offset,0U)<0)return -BK_EINVAL;

    result=0;
    while(total<count) {
        uint32_t chunk=count-total;
        uint32_t address=regs->ecx+total,pid=task_current_process_id();
        uint32_t page_bytes=4096U-(address&4095U);
        if(chunk>page_bytes)chunk=page_bytes;
        if(!pager_pin(pid,address,chunk,true)){result=total?(int32_t)total:-BK_EFAULT;break;}
        if(!user_access_ok((void *)(uintptr_t)address,chunk,true)) {
            pager_unpin(pid,address,chunk);result=total?(int32_t)total:-BK_EFAULT;break;
        }
        result=vfs_read(raw,(void *)(uintptr_t)address,chunk);
        pager_unpin(pid,address,chunk);
        if(result<0) {
            result=total?(int32_t)total:-BK_EIO;
            break;
        }
        total+=(uint32_t)result;
        if((uint32_t)result<chunk) { result=(int32_t)total;break; }
        result=(int32_t)total;
    }

    /* A positioned read must never leak its temporary seek to dup/fork users
     * sharing this VFS file object.  Failure here indicates VFS corruption. */
    if(vfs_seek(raw,old_position,0U)<0 && result>=0)return -BK_EIO;
    return result;
}

/* Linux i386 pwrite64(2), syscall 181.  Like pread64, the 64-bit offset
 * is split as pos_lo=ESI,pos_hi=EDI.  The write is positioned: it must not
 * change the shared file offset observed by dup/fork users.  The translator
 * keeps this helper under KDOMAIN_VFS for the whole seek/write/restore
 * sequence. */
static int32_t sys_linux_pwrite64(registers_t *regs) {
    syscall_resources_t *resource=resource_for(task_current_process_id(),false);
    int raw=resource_fd(resource,regs->ebx);
    uint32_t count=regs->edx;
    int64_t offset=((int64_t)(int32_t)regs->edi<<32)|(uint32_t)regs->esi;
    int32_t old_position,result,flags;
    uint32_t total=0;

    if(raw<0)return -BK_EBADF;
    /* Pipes/socketpairs are valid descriptors, but positioned I/O is not
     * defined for them. */
    if(raw>=LINUX_IO_BASE)return -29; /* ESPIPE */
    if(offset<0)return -BK_EINVAL;
    if(offset>0x7fffffffLL)return -75; /* EOVERFLOW: VFS offset limit */
    if(count>0x7fffffffU)return -BK_EINVAL;
    if(count && !user_mapping_range_ok(regs,(const void *)(uintptr_t)regs->ecx,count))
        return -BK_EFAULT;
    if(vfs_is_directory(raw))return -21; /* EISDIR */
    flags=vfs_status_flags(raw);
    if(flags<0)return -BK_EBADF;
    /* vfs_status_flags exposes Linux access bits: 0=RDONLY, 1=WRONLY,
     * 2=RDWR.  Writing through an O_RDONLY descriptor is EBADF. */
    if((flags&3)==0)return -BK_EBADF;

    old_position=vfs_tell(raw);
    if(old_position<0)return -29; /* ESPIPE */
    if(vfs_seek(raw,(int32_t)offset,0U)<0)return -BK_EINVAL;

    result=0;
    while(total<count) {
        uint32_t chunk=count-total;
        uint32_t address=regs->ecx+total,pid=task_current_process_id();
        uint32_t page_bytes=4096U-(address&4095U);
        if(chunk>page_bytes)chunk=page_bytes;
        if(!pager_pin(pid,address,chunk,false)){result=total?(int32_t)total:-BK_EFAULT;break;}
        if(!user_access_ok((const void *)(uintptr_t)address,chunk,false)) {
            pager_unpin(pid,address,chunk);result=total?(int32_t)total:-BK_EFAULT;break;
        }
        result=vfs_write(raw,(const void *)(uintptr_t)address,chunk);
        pager_unpin(pid,address,chunk);
        if(result<0) {
            if(total)result=(int32_t)total;
            else if(result==-27)result=-27; /* EFBIG from shared mapping */
            else result=-BK_EIO;
            break;
        }
        total+=(uint32_t)result;
        if((uint32_t)result<chunk) { result=(int32_t)total;break; }
        result=(int32_t)total;
    }

    if(vfs_seek(raw,old_position,0U)<0 && result>=0)return -BK_EIO;
    return result;
}

/* Linux fchdir(2), syscall 133.  Directory descriptors in BlesKernOS retain
 * their normalized VFS path, and vfs_rename() updates that path for open
 * handles.  Re-use the normal chdir validation after verifying the descriptor
 * really denotes a directory. */
static int32_t sys_linux_fchdir(uint32_t fd) {
    syscall_resources_t *resource=resource_for(task_current_process_id(),false);
    int raw=resource_fd(resource,fd);
    const char *path;

    if(raw<0)return -BK_EBADF;
    if(raw>=LINUX_IO_BASE)return -20; /* ENOTDIR */
    if(!vfs_is_directory(raw))return -20; /* ENOTDIR */
    path=vfs_fd_path(raw);
    if(!path)return -BK_EBADF;
    return vfs_chdir(path)?0:-BK_ENOENT;
}

static int32_t sys_linux_writev(registers_t *regs) {
    struct { uint32_t base,length; } item;
    uint32_t total=0U;
    if (regs->edx>1024U) return -BK_EINVAL;
    if (!user_mapping_range_ok(regs,(const void *)(uintptr_t)regs->ecx,regs->edx*8U))
        return -BK_EFAULT;
    for (uint32_t i=0;i<regs->edx;++i) {
        if (!copy_from_user(&item,(const void *)(uintptr_t)(regs->ecx+i*8U),8U))
            return total ? (int32_t)total : -BK_EFAULT;
        if (item.length>0x7fffffffU-total) return total ? (int32_t)total : -BK_EINVAL;
        while (item.length) {
            uint32_t chunk=item.length>SYSCALL_WRITE_MAX ? SYSCALL_WRITE_MAX : item.length;
            int32_t result=sys_write(regs,regs->ebx,(const void *)(uintptr_t)item.base,chunk);
            if (result<0) return total ? (int32_t)total : result;
            total+=(uint32_t)result;
            if ((uint32_t)result<chunk) return (int32_t)total;
            item.base+=chunk;item.length-=chunk;
        }
    }
    return (int32_t)total;
}
