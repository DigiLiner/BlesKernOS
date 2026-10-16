/* Freestanding i386 diagnostic: no libc, brk, DLLs or graphics surfaces. */
#include "../../ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"
static int call3(int n,int a,int b,int c) {
    int r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c):"memory");return r;
}
static char line[1024];static unsigned used;
static void text(const char *s){while(*s && used<sizeof(line)-1)line[used++]=*s++;}
static void number(unsigned n){char digits[11];unsigned count=0;do{digits[count++]='0'+n%10;n/=10;}while(n);while(count)line[used++]=digits[--count];}
static void field(const char *s,unsigned n){text(" ");text(s);text("=");number(n);}
static void sample(int fd,unsigned pid,const char *stage) {
    bles_gfx_wine_memory_t m={.pid=pid};used=0;
    int result=call3(54,fd,BLES_GFX_WINE_MEMORY,(int)&m);
    text("[WINEMEM] stage=");text(stage);field("pid",pid);field("status",result);
    if(result==0) {
        field("heap_pages",m.heap_resident_pages);field("vm_pages",m.vm_allocated_pages);
        field("system_bytes",m.system_allocated_bytes);field("owned_heap",m.process_heap_bytes);
        field("virtual",m.virtual_bytes);field("committed",m.committed_bytes);
        field("mapped",m.mapped_pages);field("rss",m.resident_pages);field("writable",m.writable_pages);
        field("cow",m.cow_pages);field("tables",m.table_pages);
        field("paged_virtual",m.paged_virtual_pages);field("paged_rss",m.paged_resident_pages);
        field("swapped",m.swapped_pages);field("brk_reserved",m.brk_capacity);field("brk_committed",m.brk_committed);
    }
    text("\n");call3(4,1,(int)line,used);
    {
        bles_gfx_wine_pager_perf_t parts={.pid=pid};
        int result=call3(54,fd,BLES_GFX_WINE_PAGER_PERF,(int)&parts);used=0;
        text("[WINEMEM:PAGER] stage=");text(stage);field("pid",pid);field("status",result);
        if(!result) {
            field("anon",parts.anonymous_pages);field("file_private",parts.file_private_pages);
            field("file_clean",parts.file_clean_pages);field("borrowed",parts.borrowed_pages);
        }
        text("\n");call3(4,1,(int)line,used);
    }
    if(!pid) {
        bles_gfx_heap_subsystems_t heap={0};
        int hr=call3(54,fd,BLES_GFX_HEAP_SUBSYSTEMS,(int)&heap);used=0;
        const char *names[8]={"other","gui","vfs","vm","process","audio","network","driver"};
        text("[WINEMEM:HEAP] stage=");text(stage);field("status",hr);
        if(!hr)for(unsigned i=0;i<8;i++)field(names[i],heap.bytes[i]);
        text("\n");call3(4,1,(int)line,used);
        bles_gfx_wine_cache_perf_t cache={0};
        int result=call3(54,fd,BLES_GFX_WINE_CACHE_PERF,(int)&cache);used=0;
        text("[WINEMEM:CACHE] stage=");text(stage);field("status",result);
        if(!result) {
            field("entries",cache.cache_entries);field("bytes",cache.cache_bytes);
            field("released",cache.cache_evictions);field("handles",cache.vfs_handles);
            field("handles_peak",cache.vfs_handles_peak);field("private_fds",cache.private_fds);
        }
        text("\n");call3(4,1,(int)line,used);
    }
}
void run(unsigned *stack) {
    int argc=stack[0];char **argv=(char **)(stack+1);
    int fd=call3(5,(int)"/dev/blesgfx",2,0);
    if(fd<0)call3(1,2,0,0);
    char stage[32];unsigned n=0;const char *args=argc>1?argv[1]:"unknown";
    while(*args && *args!=' ' && n<sizeof(stage)-1)stage[n++]=*args++;
    stage[n]=0;sample(fd,0,stage);
    /* BlesK launch_arg is a single argv element, unlike a shell's argv. */
    while(*args) {
        while(*args==' ')args++;
        unsigned pid=0;while(*args>='0' && *args<='9')pid=pid*10+*args++-'0';
        if(pid)sample(fd,pid,stage);
        if(*args && *args!=' ')break;
    }
    call3(6,fd,0,0);call3(1,0,0,0);for(;;);
}
__asm__(".global _start\n_start:\n mov %esp,%eax\n push %eax\n call run\n");
