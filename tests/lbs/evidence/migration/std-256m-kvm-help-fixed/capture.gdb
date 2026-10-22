set pagination off
set confirm off
file "/home/lucas/Escritorio/BlesKernOS 0.10/build/kernel.elf"
add-symbol-file "/home/lucas/Escritorio/BlesKernOS 0.10/tests/lbs/evidence/migration/std-256m-kvm-help-fixed/types.o" 0
target remote :12369
info threads
thread apply all info registers eax ebx ecx edx esi edi eip esp ebp eflags cr3
p *(kmutex_t(*)[9])&g_domains
p *(kmutex_t*)&g_desktop_paint_lock
p *(kmutex_t*)&g_bfs_lock
p *(kspinlock_t*)&g_mm_heap_lock
p *(unsigned*)&kernel_ticks
p *(unsigned*)&g_vision_frame_tid
p *(unsigned*)&g_vision_waiting_tid
set $i=0
while $i<128
set $t=((task_t*)&tasks)+$i
if $t->state && $t->state!=5
printf "\nTASK pid=%u process=%u state=%u name=%s api=%s\n",$t->pid,$t->process_id,$t->state,$t->name,$t->api_guard_name
p *$t->context
set $bp=$t->context->ebp
set $depth=0
while $bp>=(unsigned)$t->stack && $bp+8<=(unsigned)$t->stack+$t->stack_size && $depth<32
set $pc=*(unsigned*)($bp+4)
printf "frame bp=%x pc=%x ",$bp,$pc
info symbol $pc
set $next=*(unsigned*)$bp
if $next<=$bp
set $depth=32
else
set $bp=$next
set $depth=$depth+1
end
end
end
set $i=$i+1
end
set $b=*(heap_block_t**)&heap_head
set $n=0
while $b && $n<20000
if $b->magic!=0xb1e5c0de
printf "INVALID HEAP HEADER at %x after %u blocks\n",$b,$n
x/24wx $b-1
set $b=0
else
set $b=$b->next
set $n=$n+1
end
end
printf "Heap headers inspected: %u\n",$n
detach
quit
