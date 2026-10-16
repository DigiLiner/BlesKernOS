/* Linux i386: every byte survives eviction/page faults and fork isolation. */
#ifndef SWAP_PRESSURE_MIB
#define SWAP_PRESSURE_MIB 6U
#endif
static int call3(int n,int a,int b,int c) {
    int r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c):"memory");return r;
}
static unsigned char pattern(unsigned page,unsigned offset) {
    return (unsigned char)((page*37U)^(offset*13U)^(page>>8));
}
static void say(const char *s,unsigned n){call3(4,1,(int)s,n);}
static void quit(int status){call3(1,status,0,0);for(;;);}
void _start(void) {
    /* Wine commits this exact page above the automatic mmap arena. Keep its
     * backing alive during eviction and fork; it must not be treated as an
     * automatic-arena allocation or rejected merely because of its address. */
    unsigned fixed[6]={0x7ffe0000U,4096U,3U,0x32U,(unsigned)-1,0U};
    if((unsigned)call3(90,(int)fixed,0,0)!=fixed[0])quit(15);
    volatile unsigned *shared_data=(volatile unsigned *)fixed[0];
    *shared_data=0xb1e50004U;
    unsigned length=SWAP_PRESSURE_MIB*1024U*1024U;
    unsigned args[6]={0,length,3,0x22U,(unsigned)-1,0};
    int result=call3(90,(int)args,0,0);
    if((unsigned)result>=0xfffff000U)quit(1);
    volatile unsigned char *p=(volatile unsigned char *)result;
    say("[SWAPTEST:LINUX] touching private pages\n",sizeof("[SWAPTEST:LINUX] touching private pages\n")-1);
    for(unsigned page=0;page<length/4096U;page++)
        for(unsigned i=0;i<4096U;i++)p[page*4096U+i]=pattern(page,i);
    for(unsigned pass=0;pass<2;pass++)for(unsigned page=0;page<length/4096U;page++) {
        unsigned index=pass?length/4096U-1U-page:page;
        for(unsigned i=0;i<4096U;i++)if(p[index*4096U+i]!=pattern(index,i))quit(2);
    }
    int child=call3(120,17,0,0); /* clone(SIGCHLD): fork semantics */
    if(child<0)quit(11);
    if(!child) {
        if(*shared_data!=0xb1e50004U)quit(16);
        *shared_data=0x12345678U;
        for(unsigned page=0;page<length/4096U;page++) {
            if(p[page*4096U]!=pattern(page,0))quit(12);
            p[page*4096U]=(unsigned char)(pattern(page,0)^0x5a);
        }
        quit(0);
    }
    int status=0;
    if(call3(7,child,(int)&status,0)!=child || status)quit(13);
    if(*shared_data!=0xb1e50004U || call3(91,(int)fixed[0],4096,0))quit(17);
    for(unsigned page=0;page<length/4096U;page++)
        for(unsigned i=0;i<4096U;i++)if(p[page*4096U+i]!=pattern(page,i))quit(14);
    int fd=call3(5,(int)"/dev/null",1,0);
    if(fd<0 || call3(4,fd,result,4096)!=4096 || call3(6,fd,0,0))quit(10);
    /* Permissions survive swapped/non-present state and restoration. */
    if(call3(125,result,length,1))quit(3);
    for(unsigned page=0;page<length/4096U;page++)
        if(p[page*4096U]!=pattern(page,0))quit(4);
    if(call3(125,result,length,3))quit(5);
    /* Split a VMA, release its middle slots, then verify both survivors. */
    if(call3(91,result+2U*1024U*1024U,length-4U*1024U*1024U,0))quit(6);
    if(p[4096U]!=pattern(1,0) || p[length-1024U*1024U]!=pattern(length/4096U-256U,0))quit(7);
    if(call3(91,result,length,0))quit(8);
    say("[SWAPTEST:LINUX] integrity PASS\n",sizeof("[SWAPTEST:LINUX] integrity PASS\n")-1);
    /* Termination must release these pages/slots without munmap. */
    result=call3(90,(int)args,0,0);
    if((unsigned)result>=0xfffff000U)quit(9);
    p=(volatile unsigned char *)result;
    for(unsigned page=0;page<length/4096U;page++)p[page*4096U]=pattern(page,0);
    quit(0);
}
