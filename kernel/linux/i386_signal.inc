/* Implementacion propia del ABI publico Linux i386 (signal_32/sigcontext).
 * Incluido por task.c: vida util, destinatarios y acciones bajo task_table_lock.
 * Estado supervisor asignado solamente a tareas Linux; no consume BSS por DSO.
 */
typedef struct { uint32_t handler, flags, restorer; uint64_t mask; } linux_action;
typedef struct { uint32_t sp, flags, size; } linux_stack;
typedef struct {
    uint32_t gs, fs, es, ds, di, si, bp, sp, bx, dx, cx, ax;
    uint32_t trap, error, ip, cs, flags, sp_signal, ss, fp, mask, cr2;
} linux_context;
typedef struct {
    uint32_t flags, link;
    linux_stack stack;
    linux_context cpu;
    uint64_t mask;
} linux_ucontext;
typedef struct {
    uint32_t ret, sig, info_ptr, context_ptr;
    uint32_t info[32];
    linux_ucontext uc;
    uint8_t code[8];
} linux_rt_frame;
typedef struct {
    uint32_t ret, sig;
    linux_context cpu;
    uint8_t unused_fp[624];
    uint32_t mask_hi;
    uint8_t code[8];
} linux_old_frame;
typedef struct { uint32_t sig; int32_t code; uint32_t data; } linux_pending;
typedef struct {
    void *allocation; uint32_t size, refs;
    linux_action actions[64];
    char cwd[TASK_CWD_LEN];
} linux_actions;
typedef struct linux_signals {
    void *allocation; uint32_t size;
    linux_actions *shared;
    uint64_t mask;
    linux_stack stack;
    uint32_t futex_key, futex_process, futex_mask;
    registers_t *futex_return_frame;
    bool futex_return_pending, futex_return_timed;
    uint32_t user_stack_address;
    uint32_t robust_head;
    linux_pending queue[32];
} linux_signals;
_Static_assert(sizeof(linux_action)==20 && sizeof(linux_context)==88 &&
               sizeof(linux_ucontext)==116 && sizeof(linux_rt_frame)==268,
               "ABI de senales i386");
#define LINUX_UNBLOCKABLE ((1ULL<<8)|(1ULL<<18))
#define LINUX_SA_ONSTACK 0x08000000U
#define LINUX_SA_RESTORER 0x04000000U
#define LINUX_SA_NODEFER 0x40000000U
#define LINUX_SA_RESETHAND 0x80000000U
static void linux_wake_locked(task_t *t,int32_t result);

static bool linux_signal_init(task_t *task, task_t *parent) {
    linux_signals *s;
    void *allocation; uint32_t size;
    if (task->linux_signals) return true;
    s=mm_alloc_guarded_stack(sizeof(*s),false,&allocation,&size);
    if (!s) return false;
    kmemset(s,0,sizeof(*s)); s->allocation=allocation; s->size=size;
    s->user_stack_address=(uint32_t)task->user_stack;
    s->stack.flags=2;
    if (parent && parent->linux_signals) {
        s->shared=parent->linux_signals->shared;
        uint32_t flags=kspin_lock_irqsave(&g_task_table_lock);
        s->shared->refs++;
        kspin_unlock_irqrestore(&g_task_table_lock,flags);
        s->mask=parent->linux_signals->mask;
    } else {
        linux_actions *a=mm_alloc_guarded_stack(sizeof(*a),false,&allocation,&size);
        if (!a) { mm_free_guarded_stack(s->allocation,s,s->size); return false; }
        kmemset(a,0,sizeof(*a));a->allocation=allocation;a->size=size;a->refs=1;
        s->shared=a;
        kstrncpy(a->cwd,task->cwd?task->cwd:"/",sizeof(a->cwd)-1);
    }
    task->linux_signals=s;
    return true;
}

static void linux_signal_release(task_t *task) {
    linux_signals *s=task->linux_signals;
    linux_actions *a;
    uint32_t flags;
    if (!s) return;
    flags=kspin_lock_irqsave(&g_task_table_lock);
    task->linux_signals=NULL;a=s->shared;
    bool last=--a->refs==0;
    kspin_unlock_irqrestore(&g_task_table_lock,flags);
    if (last) mm_free_guarded_stack(a->allocation,a,a->size);
    mm_free_guarded_stack(s->allocation,s,s->size);
}

static bool linux_on_stack(linux_stack *s,uint32_t sp) {
    return !(s->flags&2) && sp-s->sp<s->size;
}

bool task_linux_signal_pending(void) {
    task_t *t=task_current_entry();bool pending=false;
    if(!t || !t->linux_signals)return false;
    uint32_t lock=kspin_lock_irqsave(&g_task_table_lock);
    linux_signals *s=t->linux_signals;
    for(unsigned i=0;i<32;i++)if(s->queue[i].sig &&
        !(s->mask&(1ULL<<(s->queue[i].sig-1)))) {pending=true;break;}
    kspin_unlock_irqrestore(&g_task_table_lock,lock);return pending;
}

uint64_t task_linux_mask_swap(uint64_t mask) {
    task_t *t=task_current_entry();
    if(!t->linux_signals && !linux_signal_init(t,NULL))return 0;
    uint64_t old=t->linux_signals->mask;
    t->linux_signals->mask=mask&~LINUX_UNBLOCKABLE;return old;
}

/* Retorna errno real. Las senales estandar coalescen; las RT conservan FIFO
 * por numero, con EAGAIN si la cola acotada esta llena. */
static int32_t linux_signal_queue(task_t *t,uint32_t sig,int32_t code,uint32_t data) {
    linux_signals *s=t->linux_signals;
    if (!sig) return 0;
    if (!s) return -BK_EAGAIN;
    /* El control de trabajos aun no tiene estados STOPPED/wait de parada.
     * Rechazarlo explicitamente, nunca convertir SIGSTOP en terminacion. */
    if ((sig==19 || sig==20 || sig==21 || sig==22) && !s->shared->actions[sig-1].handler)
        return -BK_ENOSYS;
    int free_slot=-1;
    for (unsigned i=0;i<32;i++) {
        if (sig<32 && s->queue[i].sig==sig) return 0;
        if (!s->queue[i].sig && free_slot<0) free_slot=(int)i;
    }
    if (free_slot<0) { if(sig==9)free_slot=31;else return -BK_EAGAIN; }
    s->queue[free_slot]=(linux_pending){sig,code,data};
    if (!(s->mask&(1ULL<<(sig-1)))) {
        if (s->futex_key) linux_wake_locked(t,-4);
        /* Queueing holds the task-table lock. Latch an IPC interruption even
         * between prepare and commit; the scheduler publishes a sleeping
         * waiter only after its old CPU releases the kernel stack. */
        if (t->event_wait_key) {
            t->event_wait_fired=true;
            if (t->state==TASK_SLEEPING) t->wake_tick=pit_get_ticks();
        }
    }
    return 0;
}

int32_t task_linux_signal_syscall(registers_t *r) {
    task_t *t=task_current_entry();
    uint32_t nr=r->eax, flags;
    int32_t result=0;
    if (!t || !t->linux_abi) return -BK_ENOSYS;
    if (!t->linux_signals && !linux_signal_init(t,NULL)) return -BK_ENOMEM;
    linux_signals *s=t->linux_signals;
    if (nr==174) {
        linux_action a;
        if (r->esi!=8 || !r->ebx || r->ebx>64 || r->ebx==9 || r->ebx==19)
            return -BK_EINVAL;
        if (r->ecx && !copy_from_user(&a,(void *)r->ecx,sizeof(a))) return -BK_EFAULT;
        uint32_t pid=task_current_process_id();
        if(r->edx) {
            if(!pager_pin(pid,r->edx,sizeof(linux_action),true))return -BK_EFAULT;
            if(!user_access_ok((void *)r->edx,sizeof(linux_action),true)) {
                pager_unpin(pid,r->edx,sizeof(linux_action));return -BK_EFAULT;
            }
        }
        flags=kspin_lock_irqsave(&g_task_table_lock);
        linux_action old=s->shared->actions[r->ebx-1];
        if(r->edx)kmemcpy((void *)r->edx,&old,sizeof(old));
        if (r->ecx) { a.mask&=~LINUX_UNBLOCKABLE;s->shared->actions[r->ebx-1]=a; }
        kspin_unlock_irqrestore(&g_task_table_lock,flags);
        if(r->edx)pager_unpin(pid,r->edx,sizeof(linux_action));
    } else if (nr==175) {
        uint64_t mask=0,old=s->mask;
        if (r->esi!=8 || (r->ecx && r->ebx>2)) return -BK_EINVAL;
        if (r->ecx && !copy_from_user(&mask,(void *)r->ecx,8)) return -BK_EFAULT;
        if (r->edx && !copy_to_user((void *)r->edx,&old,8)) return -BK_EFAULT;
        if (r->ecx) {
            if (r->ebx==0) mask|=old;
            else if (r->ebx==1) mask=old&~mask;
            s->mask=mask&~LINUX_UNBLOCKABLE;
        }
    } else if (nr==186) {
        linux_stack st,old=s->stack;
        bool on=linux_on_stack(&old,r->useresp);
        if (on) old.flags=1;
        if (r->ebx) {
            if (!copy_from_user(&st,(void *)r->ebx,sizeof(st))) return -BK_EFAULT;
            if (on) return -1; /* EPERM */
            if (st.flags!=0 && st.flags!=2) return -BK_EINVAL;
            if (!st.flags && st.size<2048) return -BK_ENOMEM;
            if (!st.flags && st.sp+st.size<st.sp) return -BK_EINVAL;
            if (st.flags==2) st.sp=st.size=0;
        }
        if (r->ecx && !copy_to_user((void *)r->ecx,&old,sizeof(old))) return -BK_EFAULT;
        if (r->ebx) s->stack=st;
    } else if (nr==176) { /* rt_sigpending */
        uint64_t pending=0;
        if (r->ecx!=8) return -BK_EINVAL;
        flags=kspin_lock_irqsave(&g_task_table_lock);
        for (unsigned i=0;i<32;i++) if (s->queue[i].sig) pending|=1ULL<<(s->queue[i].sig-1);
        kspin_unlock_irqrestore(&g_task_table_lock,flags);
        pending&=s->mask;
        if (!copy_to_user((void *)r->ebx,&pending,8)) return -BK_EFAULT;
    } else if (nr==37 || nr==238 || nr==270) {
        uint32_t id=nr==270?r->ecx:r->ebx,sig=nr==270?r->edx:r->ecx;
        if (sig>64 || (nr==270 && ((int32_t)r->ebx<=0 || (int32_t)id<=0))) return -BK_EINVAL;
        if ((int32_t)id<=0) return -BK_ENOSYS; /* grupos de procesos pendientes */
        result=-BK_ESRCH;
        flags=kspin_lock_irqsave(&g_task_table_lock);
        for (int i=1;i<TASK_MAX;i++) {
            task_t *target=&tasks[i];
            if (!target->linux_abi || target->state==TASK_UNUSED || target->state==TASK_ZOMBIE) continue;
            if ((nr==37?target->process_id:target->pid)!=id ||
                (nr==270 && target->process_id!=r->ebx)) continue;
            result=linux_signal_queue(target,sig,nr==37?0:-6,t->process_id);
            break;
        }
        kspin_unlock_irqrestore(&g_task_table_lock,flags);
    } else result=-BK_ENOSYS;
    return result;
}

static void linux_save_context(linux_context *c,registers_t *r,uint32_t fp,uint64_t mask,uint32_t addr) {
    c->gs=r->gs&0xffff;c->fs=r->fs&0xffff;c->es=r->es&0xffff;c->ds=r->ds&0xffff;
    c->di=r->edi;c->si=r->esi;c->bp=r->ebp;c->sp=r->useresp;
    c->bx=r->ebx;c->dx=r->edx;c->cx=r->ecx;c->ax=r->eax;
    c->trap=r->int_no==128?0:r->int_no;c->error=r->err_code;
    c->ip=r->eip;c->cs=r->cs;c->flags=r->eflags;c->sp_signal=r->useresp;
    c->ss=r->ss;c->fp=fp;c->mask=(uint32_t)mask;c->cr2=addr;
}

/* FSAVE ofrece el formato x87 publico; el bloque FXSR empieza en +112.
 * Asi los cambios del handler a x87 y XMM son consumidos por sigreturn. */
static bool linux_save_fp(uint32_t address) {
    uint8_t fp[624] __attribute__((aligned(16)));
    kmemset(fp,0,sizeof(fp));
    __asm__ volatile("fnsave (%0); fwait; frstor (%0)"::"r"(fp):"memory");
    *(uint16_t *)(fp+108)=*(uint16_t *)(fp+4);
    *(uint16_t *)(fp+110)=g_fpu_fxsr?0:0xffff;
    if (g_fpu_fxsr) __asm__ volatile("fxsave (%0)"::"r"(fp+112):"memory");
    return copy_to_user((void *)address,fp,sizeof(fp));
}

static bool linux_restore_fp(uint32_t address) {
    uint8_t fp[624] __attribute__((aligned(16)));
    if (!address) {
        if (g_fpu_fxsr) __asm__ volatile("fxrstor (%0)"::"r"(g_initial_fpu_state):"memory");
        else __asm__ volatile("frstor (%0)"::"r"(g_initial_fpu_state):"memory");
        return true;
    }
    if (!copy_from_user(fp,(void *)address,sizeof(fp))) return false;
    if (*(uint16_t *)(fp+110)==0) {
        uint32_t mask=*(uint32_t *)(g_initial_fpu_state+28);
        if (!mask) mask=0xffbf;
        if (!g_fpu_fxsr || (*(uint32_t *)(fp+136)&~mask)) return false;
        __asm__ volatile("fxrstor (%0)"::"r"(fp+112):"memory");
    } else if (*(uint16_t *)(fp+110)!=0xffff) return false;
    __asm__ volatile("frstor (%0)"::"r"(fp):"memory");
    return true;
}

static bool linux_data_selector(uint32_t value) {
    return value==0 || value==GDT_USER_DATA || value==GDT_USER_TLS0 ||
        value==GDT_USER_TLS1 || value==GDT_USER_TLS2;
}

void task_linux_sigreturn(registers_t *r,bool rt) {
    linux_context c;
    linux_ucontext uc;
    uint64_t mask;
    uint32_t hi;
    task_t *t=task_current_entry();
    uint32_t base=r->useresp-(rt?4:8);
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[LINUX:SIGRETURN] pid=%u phase=enter rt=%u useresp=%x base=%x\n",
            t?t->pid:0U,rt?1U:0U,r?r->useresp:0U,base);
#endif
    if (!t || !t->linux_signals) goto bad;
    if (rt) {
        if (!copy_from_user(&uc,(void *)(base+144),sizeof(uc))) goto bad;
        c=uc.cpu;mask=uc.mask;
        if (uc.stack.flags>2 || (!(uc.stack.flags&2) &&
            (uc.stack.size<2048 || uc.stack.sp+uc.stack.size<uc.stack.sp))) goto bad;
    } else {
        if (!copy_from_user(&c,(void *)(base+8),sizeof(c)) ||
            !copy_from_user(&hi,(void *)(base+720),4)) goto bad;
        mask=((uint64_t)hi<<32)|c.mask;
    }
    if (c.cs!=GDT_USER_CODE || c.ss!=GDT_USER_DATA ||
        !linux_data_selector(c.gs) || !linux_data_selector(c.fs) ||
        !linux_data_selector(c.es) || !linux_data_selector(c.ds) ||
        !user_access_ok((void *)c.ip,1,false) ||
        !user_access_ok((void *)(c.sp-1),1,true) || !linux_restore_fp(c.fp)) goto bad;
    r->gs=c.gs;r->fs=c.fs;r->es=c.es;r->ds=c.ds;
    r->edi=c.di;r->esi=c.si;r->ebp=c.bp;r->useresp=c.sp;
    r->ebx=c.bx;r->edx=c.dx;r->ecx=c.cx;r->eax=c.ax;
    r->eip=c.ip;r->cs=c.cs;r->ss=c.ss;
    r->eflags=(c.flags&0x00050dd5U)|0x202U;
    t->linux_signals->mask=mask&~LINUX_UNBLOCKABLE;
    if (rt) { uc.stack.flags&=2;t->linux_signals->stack=uc.stack; }
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[LINUX:SIGRETURN] pid=%u phase=restore eip=%x esp=%x eax=%x ebp=%x\n",
            t->pid,r->eip,r->useresp,r->eax,r->ebp);
#endif
    return;
bad:
    task_request_exit_process(task_current_process_id(),11);
    task_exit_from_interrupt(11);
}

static bool linux_signal_deliver(registers_t *r,linux_pending p) {
    task_t *t=task_current_entry();
    linux_signals *s=t->linux_signals;
    uint32_t flags=kspin_lock_irqsave(&g_task_table_lock);
    linux_action action=s->shared->actions[p.sig-1];
    if (action.flags&LINUX_SA_RESETHAND) s->shared->actions[p.sig-1].handler=0;
    kspin_unlock_irqrestore(&g_task_table_lock,flags);
    if (action.handler==1 || (!action.handler && (p.sig==17 || p.sig==18 || p.sig==23 || p.sig==28))) return true;
    if (!action.handler || p.sig==9 || p.sig==19) goto fatal;
    bool rt=(action.flags&4)!=0;
    uint32_t sp=r->useresp;
    bool on=linux_on_stack(&s->stack,sp);
    if ((action.flags&LINUX_SA_ONSTACK) && !on && !(s->stack.flags&2)) sp=s->stack.sp+s->stack.size;
    uint32_t fp=(sp-624U)&~15U;
    uint32_t base=((fp-(rt?sizeof(linux_rt_frame):sizeof(linux_old_frame)))&~15U)-4U;
    if (sp<1024 || fp>sp || base>fp ||
        ((on || ((action.flags&LINUX_SA_ONSTACK) && !(s->stack.flags&2))) && base<s->stack.sp) ||
        !linux_save_fp(fp)) goto fatal;
    if (rt) {
        linux_rt_frame f;
        kmemset(&f,0,sizeof(f)); f.ret=(action.flags&LINUX_SA_RESTORER)?action.restorer:base+260;
        f.sig=p.sig;f.info_ptr=base+16;f.context_ptr=base+144;
        f.info[0]=p.sig;f.info[2]=(uint32_t)p.code;f.info[3]=p.data;
        f.uc.stack=s->stack;if(on) f.uc.stack.flags=1;
        f.uc.mask=s->mask;
        linux_save_context(&f.uc.cpu,r,fp,s->mask,p.code>0?p.data:0);
        f.code[0]=0xb8;f.code[1]=173;f.code[5]=0xcd;f.code[6]=0x80;
        if (!copy_to_user((void *)base,&f,sizeof(f))) goto fatal;
        r->edx=f.info_ptr;r->ecx=f.context_ptr;
    } else {
        linux_old_frame f;
        kmemset(&f,0,sizeof(f));f.ret=(action.flags&LINUX_SA_RESTORER)?action.restorer:base+724;
        f.sig=p.sig;f.mask_hi=(uint32_t)(s->mask>>32);
        linux_save_context(&f.cpu,r,fp,s->mask,p.code>0?p.data:0);
        f.code[0]=0x58;f.code[1]=0xb8;f.code[2]=119;f.code[6]=0xcd;f.code[7]=0x80;
        if (!copy_to_user((void *)base,&f,sizeof(f))) goto fatal;
        r->edx=r->ecx=0;
    }
    s->mask|=action.mask;
    if (!(action.flags&LINUX_SA_NODEFER)) s->mask|=1ULL<<(p.sig-1);
    s->mask&=~LINUX_UNBLOCKABLE;
#ifdef BLESK_WINE_VERBOSE_TRACE
    kprintf("[LINUX:SIGDELIVER] pid=%u sig=%u handler=%x flags=%x rt=%u frame=%x oldeip=%x oldesp=%x\n",
            t->pid,p.sig,action.handler,action.flags,rt?1U:0U,base,r->eip,r->useresp);
#endif
    r->eip=action.handler;r->useresp=base;r->eax=p.sig;
    r->cs=GDT_USER_CODE;r->ss=r->ds=r->es=GDT_USER_DATA;
    r->eflags&=~(0x100U|0x400U|0x10000U);
    return true;
fatal:
    task_request_exit_process(t->process_id,(int)p.sig);
    task_exit_from_interrupt((int)p.sig);return false;
}

/* Llamado con CR3 y FPU de la tarea elegida, al ultimo retorno a Ring 3.
 * Nunca decorar un frame antes de que syscall_handler restaure argumentos. */
registers_t *task_linux_signal_return(registers_t *r) {
    task_t *t=task_current_entry();
    if (!r || (r->cs&3)!=3 || !t || !t->linux_abi) return r;
    if (t->state==TASK_ZOMBIE) return task_schedule(r);
    linux_signals *s=t->linux_signals;
    if (!s) return r;
    if (s->futex_return_pending && s->futex_return_frame==r) {
        int32_t result=(int32_t)r->eax;
        s->futex_return_pending=false;
        if (result!=0 && result!=-4 && result!=-BK_EAGAIN &&
            (result!=-110 || !s->futex_return_timed))
            kprintf("[FUTEX:RETURN] tid=%u eax=%d frame=%x ctx=%x key=%x wake=%u now=%u\n",
                    t->pid,result,(uint32_t)r,(uint32_t)t->context,s->futex_key,t->wake_tick,pit_get_ticks());
    }
    s->futex_key=0;
    uint32_t flags=kspin_lock_irqsave(&g_task_table_lock);
    int chosen=-1;
    for (int i=0;i<32;i++) if (s->queue[i].sig &&
        !(s->mask&(1ULL<<(s->queue[i].sig-1))) &&
        (chosen<0 || s->queue[i].sig<s->queue[chosen].sig)) chosen=i;
    linux_pending p={0,0,0};
    if (chosen>=0) {
        p=s->queue[chosen];
        for (int i=chosen;i<31;i++) s->queue[i]=s->queue[i+1];
        s->queue[31].sig=0;
    }
    kspin_unlock_irqrestore(&g_task_table_lock,flags);
    if (p.sig) {
        /* The assembly epilogue disabled IRQs before restoring segments.
         * Building a signal frame can fault, take the pager/VFS mutex, or
         * wait for remote TLB invalidation. Run that work in process context
         * on the selected task's stack, then make final return atomic again. */
        __asm__ volatile("sti" ::: "memory");
        bool delivered=linux_signal_deliver(r,p);
        __asm__ volatile("cli" ::: "memory");
        if(!delivered)return task_schedule(r);
    }
    return r;
}

registers_t *task_linux_signal_fault(registers_t *r,uint32_t address) {
    task_t *t=task_current_entry();
    uint32_t sig=11;int32_t code=1;
    if(r && t && r->int_no==14U && pager_fault_is_bus(t->process_id,address,(r->err_code&2U)!=0)) {
        sig=7;code=2; /* SIGBUS/BUS_ADRERR for a file page beyond EOF. */
    }
#ifdef BLESK_WINE_VERBOSE_TRACE
    uint32_t frame[2]={0,0};
    uint32_t code_words[4]={0,0,0,0};
    uint32_t stack_words[12]={0};
    bool frame_ok=false;
    bool code_ok=false;
    bool stack_ok=false;

    /* W3-40 diagnostic: Wine installs a SIGSEGV handler, so handled user
     * faults normally skip the fatal [LINUX:FAULT] print below. Preserve the
     * pre-signal machine state and the conventional EBP return address before
     * building Wine's signal frame. This changes no signal semantics. */
    if (r && r->int_no==14U) {
        if (r->ebp) frame_ok=copy_from_user(frame,(void *)(uintptr_t)r->ebp,sizeof(frame));
        if (r->eip>=8U) code_ok=copy_from_user(code_words,(void *)(uintptr_t)(r->eip-8U),sizeof(code_words));
        if (r->useresp) stack_ok=copy_from_user(stack_words,(void *)(uintptr_t)r->useresp,sizeof(stack_words));
        kprintf("[LINUX:FAULTPRE] pid=%u eip=%x addr=%x eax=%x ebx=%x ecx=%x edx=%x esi=%x edi=%x ebp=%x esp=%x ret=%x frame=%u gs=%x tls=%x\n",
            t?t->pid:0U,r->eip,address,r->eax,r->ebx,r->ecx,r->edx,r->esi,r->edi,
            r->ebp,r->useresp,frame_ok?frame[1]:0U,frame_ok?1U:0U,r->gs,t?t->user_fs_base:0U);
        kprintf("[LINUX:FAULTCODE] ok=%u start=%x w0=%x w1=%x w2=%x w3=%x\n",
            code_ok?1U:0U,r->eip-8U,code_words[0],code_words[1],code_words[2],code_words[3]);
        kprintf("[LINUX:FAULTSTACK] ok=%u sp=%x 00=%x 04=%x 08=%x 0c=%x 10=%x 14=%x 18=%x 1c=%x 20=%x 24=%x 28=%x 2c=%x\n",
            stack_ok?1U:0U,r->useresp,stack_words[0],stack_words[1],stack_words[2],stack_words[3],
            stack_words[4],stack_words[5],stack_words[6],stack_words[7],stack_words[8],stack_words[9],
            stack_words[10],stack_words[11]);
    }
#endif
    if (r->int_no==0 || r->int_no==16 || r->int_no==19) {sig=8;code=1;}
    else if (r->int_no==6) {sig=4;code=2;}
    else if (r->int_no==1 || r->int_no==3) {sig=5;code=r->int_no==1?2:1;}
    else if (r->int_no==17) {sig=7;code=1;}
    else if (r->int_no==14 && sig==11) code=(r->err_code&1)?2:1;
    if (!t->linux_signals && !linux_signal_init(t,NULL)) goto fatal;
    if ((t->linux_signals->mask&(1ULL<<(sig-1))) ||
        t->linux_signals->shared->actions[sig-1].handler<=1) goto fatal;
    if (!linux_signal_deliver(r,(linux_pending){sig,code,r->int_no==14?address:r->eip})) {
        /* Publish the next task only with interrupts disabled: the assembly
         * return has not switched to its saved stack yet. */
        __asm__ volatile("cli" ::: "memory");
        return task_schedule(r);
    }
    return r;
fatal:
    kprintf("[LINUX:FAULT] pid=%u sig=%u eip=%x addr=%x eax=%x ebx=%x ecx=%x edx=%x esi=%x edi=%x gs=%x tls=%x\n",
        t->pid,sig,r->eip,address,r->eax,r->ebx,r->ecx,r->edx,r->esi,r->edi,r->gs,t->user_fs_base);
    task_request_exit_process(t->process_id,(int)sig);
    task_exit_from_interrupt((int)sig);
    __asm__ volatile("cli" ::: "memory");
    return task_schedule(r);
}
