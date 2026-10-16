/* Freestanding Linux i386 regression for VMA growth, fork and teardown. */
#define MAPS 1537U
static volatile unsigned *pages[MAPS];
static int call3(int n,int a,int b,int c) {
    int result;
    __asm__ volatile("int $0x80":"=a"(result):"a"(n),"b"(a),"c"(b),"d"(c):"memory");
    return result;
}
static void quit(int status) {call3(1,status,0,0);for(;;);}
static int map(void) {
    unsigned args[6]={0,4096,3,0x22U,(unsigned)-1,0};
    return call3(90,(int)args,0,0);
}
static void dense_commits(void) {
    unsigned args[6]={0,128U*4096U,0,0x22U,(unsigned)-1,0};
    int base=call3(90,(int)args,0,0);
    if((unsigned)base>=0xfffff000U)quit(21);
    volatile unsigned *memory=(volatile unsigned *)base;
    /* Wine grows heaps by committing a span that includes older commits. */
    for(unsigned i=0;i<128;i++) {
        if(call3(125,base,(i+1)*4096,3))quit(22);
        for(unsigned j=0;j<i;j++)if(memory[j*1024]!=0xbeef0000U+j)quit(23);
        memory[i*1024]=0xbeef0000U+i;
    }
    if(call3(125,base,128*4096,1) || call3(125,base,128*4096,3))quit(24);
    for(unsigned j=0;j<128;j++)if(memory[j*1024]!=0xbeef0000U+j)quit(25);
    if(call3(91,base,128*4096,0))quit(26);
}
void _start(void) {
    /* Repeat growth and hole reuse; the final round deliberately leaves
     * mappings alive so exit must reclaim its descriptors and backing. */
    for(unsigned round=0;round<3;round++) {
        for(unsigned i=0;i<MAPS;i++) {
            int address=map();if((unsigned)address>=0xfffff000U)quit(10);
            pages[i]=(volatile unsigned *)address;*pages[i]=0xb1e50000U+i;
        }
        dense_commits();
        if(call3(125,(int)pages[0],4096,1) || *pages[0]!=0xb1e50000U)quit(11);
        if(call3(125,(int)pages[0],4096,3))quit(12);
        /* clone(SIGCHLD) is the fork entry supported by the current ABI. */
        int child=call3(120,17,0,0);if(child<0)quit(13);
        if(!child) {
            for(unsigned i=0;i<MAPS;i++)if(*pages[i]!=0xb1e50000U+i)quit(14);
            *pages[0]=0xcafef00dU;
            /* Exercise child growth after inheriting a full parent table. */
            for(unsigned i=0;i<MAPS;i++)if((unsigned)map()>=0xfffff000U)quit(15);
            quit(0);
        }
        int status=-1;
        if(call3(7,child,(int)&status,0)!=child || status || *pages[0]!=0xb1e50000U)quit(16);
        for(unsigned i=0;i<MAPS;i+=2) {
            if(call3(91,(int)pages[i],4096,0))quit(17);
            int address=map();if((unsigned)address>=0xfffff000U)quit(18);
            pages[i]=(volatile unsigned *)address;*pages[i]=0xb1e50000U+i;
        }
        for(unsigned i=0;i<MAPS;i++)if(*pages[i]!=0xb1e50000U+i)quit(19);
        if(round<2)for(unsigned i=0;i<MAPS;i++)if(call3(91,(int)pages[i],4096,0))quit(20);
    }
    static const char pass[]="[VMMETATEST:LINUX] 1537 maps, dense commits, fork/isolation/reuse PASS\n";
    call3(4,1,(int)pass,sizeof(pass)-1);quit(0);
}
