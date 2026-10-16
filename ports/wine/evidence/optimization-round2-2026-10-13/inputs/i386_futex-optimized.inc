int32_t task_linux_robust(registers_t *r,bool get) {
    task_t *t=task_current_entry();
    if(!get) {
        if(r->ecx!=12)return -BK_EINVAL;
        if(!t->linux_signals && !linux_signal_init(t,NULL))return -BK_ENOMEM;
        t->linux_signals->robust_head=r->ebx;return 0;
    }
    uint32_t flags=kspin_lock_irqsave(&g_task_table_lock),head=0,length=12;
    bool found=false;
    for(int i=1;i<TASK_MAX;i++) {
        task_t *p=&tasks[i];
        if(p->state!=TASK_UNUSED && p->state!=TASK_ZOMBIE && p->linux_abi &&
            p->pid==(r->ebx?r->ebx:t->pid)) {
            found=true;head=p->linux_signals?p->linux_signals->robust_head:0;break;
        }
    }
    kspin_unlock_irqrestore(&g_task_table_lock,flags);
    if(!found)return -BK_ESRCH;
    return copy_to_user((void *)r->ecx,&head,4) && copy_to_user((void *)r->edx,&length,4)?0:-BK_EFAULT;
}

static void linux_robust_dead(uint32_t address,uint32_t tid) {
    uint32_t old;
    if((address&3) || !user_access_ok((void *)address,4,true) ||
        !copy_from_user(&old,(void *)address,4))return;
    while((old&0x3fffffffU)==tid) {
        uint32_t expected=old,value=(old&0x80000000U)|0x40000000U;
        __asm__ volatile("lock cmpxchgl %2,%1":"+a"(old),"+m"(*(uint32_t *)address):"r"(value):"memory");
        if(old==expected) {
            if(old&0x80000000U)(void)task_linux_futex_wake(address,1,~0U,false);
            return;
        }
    }
}
static void linux_robust_exit(task_t *t) {
    if(!t->linux_signals || !t->linux_signals->robust_head)return;
    uint32_t head=t->linux_signals->robust_head,data[3],next;
    t->linux_signals->robust_head=0;
    if(!copy_from_user(data,(void *)head,12))return;
    next=data[0];
    for(unsigned i=0;i<2048 && (next&~1U)!=head;i++) {
        uint32_t entry=next&~1U,following;
        if(!entry || !copy_from_user(&following,(void *)entry,4))break;
        if(!(next&1) && entry!=(data[2]&~1U))linux_robust_dead(entry+data[1],t->pid);
        next=following;
    }
    if(data[2] && !(data[2]&1))linux_robust_dead(data[2]+data[1],t->pid);
}

/* La comparacion y el registro del waiter comparten el lock con WAKE.
 * Ningun intervalo entre observar el valor y dormir puede perder un wake. */
static void linux_wake_locked(task_t *t,int32_t result) {
    if (!t->linux_signals || !t->linux_signals->futex_key || t->state!=TASK_SLEEPING) return;
    t->linux_signals->futex_key=0;
    t->context->eax=(uint32_t)result;
    t->wake_tick=pit_get_ticks();
    /* El scheduler publica READY cuando el CPU anterior abandono la pila;
     * esto tambien evita encolar el mismo stack en dos CPUs. */
}

int32_t task_linux_futex_wait(registers_t *regs,uint32_t address,uint32_t value,uint32_t mask,
                             bool private_key,uint32_t ticks) {
    task_t *t=task_current_entry();
    uint32_t actual,key,scope;
    if (!t) return -BK_EINVAL;
    if (!t->linux_signals && !linux_signal_init(t,NULL)) return -BK_ENOMEM;
    /* Materialization can allocate or perform swap I/O. Never do it while
     * holding the task-table spinlock; keep the word resident until the
     * compare-and-enqueue has completed. */
    uint32_t pid=task_current_process_id();
    void *pin=NULL;
    if(!pager_pin_word(pid,address,&pin))return -BK_EFAULT;
    if(!user_access_ok((void *)address,4,false)) {
        pager_release_word(pin);return -BK_EFAULT;
    }
    if(!syscall_linux_futex_key(address,private_key,&key,&scope)) {
        pager_release_word(pin);return -BK_EFAULT;
    }
    uint32_t flags=kspin_lock_irqsave(&g_task_table_lock);
    int32_t result=0;
    actual=*(volatile uint32_t *)address;
    if (actual!=value) result=-BK_EAGAIN;
    else if (!ticks) result=-110;
    else {
        linux_signals *s=t->linux_signals;
        /* Track only a registered sleep, so rejected WAIT arguments do not
         * produce a spurious resumption warning. Healthy returns stay quiet. */
        s->futex_return_frame=regs;s->futex_return_pending=true;
        s->futex_return_timed=regs->esi!=0;
        s->futex_key=key;s->futex_process=scope;
        s->futex_mask=mask;t->wake_tick=pit_get_ticks()+ticks;t->context=regs;
        t->state=TASK_SLEEPING;
        for (unsigned i=0;i<32;i++) if (s->queue[i].sig &&
            !(s->mask&(1ULL<<(s->queue[i].sig-1)))) {
            s->futex_key=0;t->state=TASK_RUNNING;result=-4;break;
        }
    }
    kspin_unlock_irqrestore(&g_task_table_lock,flags);
    pager_release_word(pin);return result;
}

int32_t task_linux_futex_wake(uint32_t address,uint32_t count,uint32_t mask,
                             bool private_key) {
    int wake_slots[TASK_MAX];
    uint32_t wake_count=0,running_mask=0;
    uint32_t key,process,woken=0;
    if(!syscall_linux_futex_key(address,private_key,&key,&process))return -BK_EFAULT;
    uint32_t flags=kspin_lock_irqsave(&g_task_table_lock);
    for (int i=1;i<TASK_MAX && woken<count;i++) {
        task_t *t=&tasks[i];linux_signals *s=t->linux_signals;
        if (t->state!=TASK_SLEEPING || !s || s->futex_key!=key ||
            s->futex_process!=process || !(s->futex_mask&mask)) continue;
        linux_wake_locked(t,0);woken++;
        /* A retired stack can run immediately, as with event-backed IPC.
         * A waiter still leaving WAIT keeps its stack until the scheduler
         * completes the handoff; never enqueue that stack from here. */
        if(t->running_cpu==-1) {
            t->state=TASK_READY;
            if(t->queued_cpu<0)wake_slots[wake_count++]=i;
        } else if(t->running_cpu>=0 && t->running_cpu<32) {
            running_mask|=1U<<(uint32_t)t->running_cpu;
        }
    }
    kspin_unlock_irqrestore(&g_task_table_lock,flags);
    if(smp_scheduler_started()) {
        for(uint32_t i=0;i<wake_count;i++)
            (void)scheduler_smp_enqueue(wake_slots[i],-1,true);
        smp_reschedule_mask(running_mask);
    }
    return (int32_t)woken;
}
