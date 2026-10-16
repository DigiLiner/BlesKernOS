/* Linux inotify ABI over successful VFS operations. Included after devices.
 * Queue/watch storage is allocated only for an open instance. No user copies,
 * VFS lookups, allocation or scheduler calls occur under linux_io_lock. */
#include <string.h>
#define LINUX_INOTIFY_WATCHES 64U
#define LINUX_INOTIFY_BYTES 8192U
#define LINUX_IN_ALL_EVENTS 0x00000fffU
#define LINUX_IN_IGNORED 0x00008000U
#define LINUX_IN_OVERFLOW 0x00004000U
#define LINUX_IN_ISDIR 0x40000000U
#define LINUX_IN_ONESHOT 0x80000000U
typedef struct {int32_t wd;uint32_t mask,cookie,length;} linux_inotify_event;
typedef struct {
    uint64_t device,inode;
    uint32_t wd,mask;
    bool directory;
    char path[VFS_MAX_PATH];
} linux_inotify_watch;
struct linux_inotify {
    uint32_t used,last,next_wd;
    bool overflow;
    linux_inotify_watch watches[LINUX_INOTIFY_WATCHES];
    uint8_t queue[LINUX_INOTIFY_BYTES];
};
static volatile uint32_t linux_inotify_instances;
static uint32_t linux_inotify_cookie;

static bool linux_inotify_prefix(const char *a,const char *b,uint32_t length) {
    for(uint32_t i=0;i<length;i++) {
        unsigned char x=a[i],y=b[i];
        if(x>='A' && x<='Z')x+='a'-'A';
        if(y>='A' && y<='Z')y+='a'-'A';
        if(x!=y || !x)return false;
    }
    return true;
}
static bool linux_inotify_equal(const char *a,const char *b) {
    uint32_t n=kstrlen(a);return n==kstrlen(b) && linux_inotify_prefix(a,b,n);
}
static const char *linux_inotify_child(const char *parent,const char *path) {
    uint32_t n=kstrlen(parent);
    if(!linux_inotify_prefix(parent,path,n))return NULL;
    const char *name;
    if(n==1 && parent[0]=='/')name=path+1;
    else {if(path[n]!='/')return NULL;name=path+n+1;}
    if(!*name)return NULL;
    for(const char *p=name;*p;p++)if(*p=='/')return NULL;
    return name;
}
/* Reserve one header for a visible overflow event, never silently truncate a
 * variable-length name. Consecutive identical unread events coalesce. */
static bool linux_inotify_enqueue(struct linux_inotify *s,int32_t wd,
                                  uint32_t mask,uint32_t cookie,const char *name) {
    uint32_t length=name?(kstrlen(name)+1U+3U)&~3U:0U;
    uint32_t size=sizeof(linux_inotify_event)+length;
    linux_inotify_event e={wd,mask,cookie,length};
    if(s->used) {
        linux_inotify_event *last=(void *)(s->queue+s->last);
        if(last->wd==wd && last->mask==mask && last->cookie==cookie && last->length==length &&
           (!name || !kmemcmp(last+1,name,kstrlen(name)+1U)))return false;
    }
    if(s->used>LINUX_INOTIFY_BYTES-sizeof(e) ||
       size>LINUX_INOTIFY_BYTES-sizeof(e)-s->used) {
        if(s->overflow)return false;
        s->last=s->used;e=(linux_inotify_event){-1,LINUX_IN_OVERFLOW,0,0};
        kmemcpy(s->queue+s->used,&e,sizeof(e));s->used+=sizeof(e);s->overflow=true;
        return true;
    }
    s->last=s->used;kmemcpy(s->queue+s->used,&e,sizeof(e));
    if(length) {
        kmemset(s->queue+s->used+sizeof(e),0,length);
        kmemcpy(s->queue+s->used+sizeof(e),name,kstrlen(name));
    }
    s->used+=size;return true;
}
static bool linux_inotify_drop(struct linux_inotify *s,linux_inotify_watch *w) {
    bool changed=linux_inotify_enqueue(s,w->wd,LINUX_IN_IGNORED,0,NULL);
    w->wd=0;return changed;
}
static bool linux_inotify_emit(struct linux_inotify *s,linux_inotify_watch *w,
                               uint32_t mask,uint32_t cookie,const char *name,bool directory) {
    if(!(w->mask&mask))return false;
    bool changed=linux_inotify_enqueue(s,w->wd,mask|(directory?LINUX_IN_ISDIR:0),cookie,name);
    if(w->mask&LINUX_IN_ONESHOT)changed|=linux_inotify_drop(s,w);
    return changed;
}
static void linux_inotify_changed(const char *path,const char *renamed,
                                  uint32_t mask,bool directory) {
    if(!linux_inotify_instances)return;
    bool changed=false;
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock),cookie=0;
    if(renamed){if(!++linux_inotify_cookie)++linux_inotify_cookie;cookie=linux_inotify_cookie;}
    for(unsigned i=0;i<128;i++) {
        linux_device *d=&linux_devices[i];struct linux_inotify *s=d->inotify;
        if(!d->refs || d->kind!=6 || !s)continue;
        for(unsigned j=0;j<LINUX_INOTIFY_WATCHES;j++) {
            linux_inotify_watch *w=&s->watches[j];if(!w->wd)continue;
            bool self=linux_inotify_equal(w->path,path);
            const char *child=w->directory?linux_inotify_child(w->path,path):NULL;
            if(renamed) {
                if(child)changed|=linux_inotify_emit(s,w,VFS_NOTIFY_MOVED_FROM,cookie,child,directory);
                if(!w->wd)continue;
                const char *next=w->directory?linux_inotify_child(w->path,renamed):NULL;
                if(next)changed|=linux_inotify_emit(s,w,VFS_NOTIFY_MOVED_TO,cookie,next,directory);
                if(w->wd && self)changed|=linux_inotify_emit(s,w,VFS_NOTIFY_MOVE_SELF,0,NULL,directory);
                if(!w->wd)continue;
                uint32_t old=kstrlen(path),n=kstrlen(renamed),tail=kstrlen(w->path);
                if(tail>=old && linux_inotify_prefix(w->path,path,old) &&
                   (tail==old || w->path[old]=='/')) {
                    if(n+tail-old>=sizeof(w->path))changed|=linux_inotify_drop(s,w);
                    else {
                        memmove(w->path+n,w->path+old,tail-old+1U);
                        kmemcpy(w->path,renamed,n);
                    }
                }
            } else {
                if(child && mask!=VFS_NOTIFY_DELETE_SELF)
                    changed|=linux_inotify_emit(s,w,mask,0,child,directory);
                if(w->wd && self) {
                    uint32_t event=mask==VFS_NOTIFY_DELETE?VFS_NOTIFY_DELETE_SELF:mask;
                    changed|=linux_inotify_emit(s,w,event,0,NULL,w->directory);
                    if(w->wd && event==VFS_NOTIFY_DELETE_SELF)changed|=linux_inotify_drop(s,w);
                }
            }
        }
    }
    kspin_unlock_irqrestore(&linux_io_lock,lock);
    if(changed)task_event_signal(linux_io_event_key());
}

static int32_t linux_inotify_init(uint32_t flags) {
    if(flags&~(0x800U|0x80000U))return -BK_EINVAL;
    syscall_resources_t *r=resource_for(task_current_process_id(),true);
    if(!r)return -BK_ENOMEM;
    int fd;
    for(fd=0;fd<SYSCALL_FD_SLOTS+3;fd++)if(resource_fd(r,fd)<0)break;
    if(fd==SYSCALL_FD_SLOTS+3)return -BK_EMFILE;
    struct linux_inotify *s=kzalloc(sizeof(*s));if(!s)return -BK_ENOMEM;
    (void)mm_set_allocation_owner(s,0U);s->next_wd=1;
    int raw=linux_device_open(6,flags&0x800U);
    if(raw<0){kfree(s);return raw;}
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
    linux_device_for(raw)->inotify=s;linux_inotify_instances++;
    kspin_unlock_irqrestore(&linux_io_lock,lock);
    vfs_observe_changes(linux_inotify_changed);
    if(fd<3)r->stdio_fds[fd]=raw;else r->fds[fd-3]=raw;
    r->linux_fd_flags[fd]=(flags&0x80000U)?1:0;
    r->linux_open_flags[fd]=0;
    return fd;
}
static int32_t linux_inotify_add(registers_t *regs) {
    int raw=resource_fd(resource_for(task_current_process_id(),false),regs->ebx);
    linux_device *d=linux_device_for(raw);
    if(!d || d->kind!=6)return raw<0?-BK_EBADF:-BK_EINVAL;
    uint32_t mask=regs->edx;
    if(!(mask&LINUX_IN_ALL_EVENTS) || (mask&~0xb7000fffU) ||
       (mask&0x30000000U)==0x30000000U)return -BK_EINVAL;
    char input[VFS_MAX_PATH],path[VFS_MAX_PATH];
    int32_t result=linux_at_path(regs,-100,(void *)(uintptr_t)regs->ecx,input);
    if(result<0)return result;
    bool nofollow=(mask&0x02000000U)!=0;
    if(!(nofollow?vfs_normalize_nofollow(input,path):vfs_normalize_path(input,path)))return -40;
    vfs_dir_entry_t entry;char target[VFS_MAX_PATH];
    bool link=nofollow && vfs_readlink(path,target,sizeof(target))>=0;
    if(!link && !vfs_stat(path,&entry))return -BK_ENOENT;
    bool directory=!link && entry.type==VFS_NODE_DIR;
    if((mask&0x01000000U) && !directory)return -20;
    uint64_t device,inode;
    if(!(nofollow?vfs_identity_nofollow(path,&device,&inode):vfs_identity(path,&device,&inode)))return -BK_ENOENT;
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
    struct linux_inotify *s=d->inotify;linux_inotify_watch *slot=NULL;
    result=-28; /* ENOSPC: bounded watch table. */
    for(unsigned i=0;i<LINUX_INOTIFY_WATCHES;i++) {
        linux_inotify_watch *w=&s->watches[i];
        if(w->wd && w->device==device && w->inode==inode) {
            if(mask&0x10000000U){result=-17;goto done;}
            w->mask=(mask&0x20000000U)?w->mask|(mask&(LINUX_IN_ALL_EVENTS|LINUX_IN_ONESHOT))
                :mask&(LINUX_IN_ALL_EVENTS|LINUX_IN_ONESHOT);
            result=w->wd;goto done;
        }
        if(!w->wd && !slot)slot=w;
    }
    if(slot && s->next_wd<=0x7fffffffU) {
        *slot=(linux_inotify_watch){.device=device,.inode=inode,.wd=s->next_wd++,
            .mask=mask&(LINUX_IN_ALL_EVENTS|LINUX_IN_ONESHOT),.directory=directory};
        kstrcpy(slot->path,path);result=slot->wd;
    }
done:
    kspin_unlock_irqrestore(&linux_io_lock,lock);return result;
}
static int32_t linux_inotify_remove(uint32_t fd,uint32_t wd) {
    int raw=resource_fd(resource_for(task_current_process_id(),false),fd);
    linux_device *d=linux_device_for(raw);
    if(!d || d->kind!=6)return raw<0?-BK_EBADF:-BK_EINVAL;
    bool changed=false;int32_t result=-BK_EINVAL;
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
    for(unsigned i=0;i<LINUX_INOTIFY_WATCHES;i++) {
        linux_inotify_watch *w=&d->inotify->watches[i];
        if(w->wd && w->wd==wd){changed=linux_inotify_drop(d->inotify,w);result=0;break;}
    }
    kspin_unlock_irqrestore(&linux_io_lock,lock);
    if(changed)task_event_signal(linux_io_event_key());
    return result;
}
static int32_t linux_inotify_read(int raw,void *buffer,uint32_t length) {
    if(length<sizeof(linux_inotify_event))return -BK_EINVAL;
    uint32_t pid=task_current_process_id();
    if(!pager_pin(pid,(uint32_t)(uintptr_t)buffer,length,true))return -BK_EFAULT;
    if(!user_access_ok(buffer,length,true) || !sys_raw_retain(raw)) {
        pager_unpin(pid,(uint32_t)(uintptr_t)buffer,length);return -BK_EFAULT;
    }
    int32_t result;uint32_t empty_rounds=0,key=linux_io_event_key();
    for(;;) {
        bool armed=linux_io_wait_arm(key);
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
        linux_device *d=linux_device_for(raw);struct linux_inotify *s=d->inotify;
        if(s->used) {
            uint32_t take=0;
            while(take<s->used) {
                linux_inotify_event *e=(void *)(s->queue+take);
                uint32_t size=sizeof(*e)+e->length;
                if(size>length-take)break;
                if(e->mask==LINUX_IN_OVERFLOW)s->overflow=false;
                take+=size;
            }
            result=take?(int32_t)take:-BK_EINVAL;
            if(take) {
                kmemcpy(buffer,s->queue,take);s->used-=take;
                memmove(s->queue,s->queue+take,s->used);s->last=0;
                for(uint32_t n=0;n<s->used;) {
                    s->last=n;linux_inotify_event *e=(void *)(s->queue+n);n+=sizeof(*e)+e->length;
                }
            }
        } else result=(d->flags&0x800U)?-BK_EAGAIN:0;
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        if(result) {linux_io_wait_cancel(armed,key);break;}
        if(task_linux_signal_pending()){linux_io_wait_cancel(armed,key);result=-4;break;}
        linux_io_wait(&empty_rounds,armed,key,pit_get_frequency_hz());
    }
    sys_raw_close(raw);pager_unpin(pid,(uint32_t)(uintptr_t)buffer,length);return result;
}
