#include <bleskernos.h>
#include <bleskernos_api.h>
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <bleskernos_lbs.h>
extern int __bk_lbs_vm_protect(void *, unsigned int, unsigned int);
typedef struct { unsigned address, bytes; char name[260]; } share_record_t;
_Static_assert(sizeof(share_record_t)==268,"sharing probe record ABI");
static share_record_t records[64];
static unsigned record_count;
static const char *self = "/SYSTEM/TESTS/NATIVE/LBSSHARE.BEX";
static void fail(const char *why) { bk_sys_log("[LBSSHARE] FAIL "); bk_sys_log(why); bk_sys_log("\n"); bk_exit(1); }
static int exists(const char *path) { bk_file_entry_t info; return bk_file_stat(path, &info); }
static void marker(const char *path) { if (!bk_file_write_all(path, "ok", 2)) fail("marker write"); }
static void until(const char *path) { unsigned start=bk_uptime_ms(); while(!exists(path)) { if(bk_uptime_ms()-start>60000)fail("marker timeout"); bk_sys_sleep_ms(30); } }
static int mappings(struct dl_phdr_info *info, unsigned size, void *who) {
    (void)size; (void)who;
    for(unsigned j=0;j<info->dlpi_phnum;j++) {
        const Elf32_Phdr *p=&info->dlpi_phdr[j];
        if(p->p_type!=PT_LOAD || !p->p_memsz || (p->p_flags&PF_W))continue;
        unsigned first=p->p_vaddr&~4095U, end=(p->p_vaddr+p->p_memsz+4095U)&~4095U;
        if(record_count>=64)fail("mapping record limit");
        share_record_t *record=&records[record_count++];
        record->address=info->dlpi_addr+first;record->bytes=end-first;
        snprintf(record->name,sizeof(record->name),"%s",info->dlpi_name);
    }
    return 0;
}
void bleskernos_program_main(void *unused) {
    char arg[40],line[240]; (void)unused;
    bk_proc_launch_arg_copy(arg,sizeof(arg));
    if(!arg[0]) {
        const char *files[]={"ONE.OK","TWO.OK","GO","COW.OK","ONE.DONE"};
        for(unsigned i=0;i<5;i++){snprintf(line,sizeof(line),"/SYSTEM/TESTS/NATIVE/%s",files[i]);bk_file_remove(line);}
        bk_sys_log("[LBSSHARE] BASELINE\n"); bk_sys_sleep_ms(1500);
        int a=bk_spawn(self,"one"),status=-1;
        if(a<0)fail("first spawn");
        until("/SYSTEM/TESTS/NATIVE/ONE.OK");
        int b=bk_spawn(self,"two");if(b<0)fail("second spawn");
        until("/SYSTEM/TESTS/NATIVE/TWO.OK");
        bk_sys_log("[LBSSHARE] BOTH READY\n"); bk_sys_sleep_ms(3500);
        marker("/SYSTEM/TESTS/NATIVE/GO");until("/SYSTEM/TESTS/NATIVE/COW.OK");
        bk_sys_log("[LBSSHARE] COW READY\n"); bk_sys_sleep_ms(3500);
        if(bk_waitpid(a,&status)!=a || status)fail("first exit");
        marker("/SYSTEM/TESTS/NATIVE/ONE.DONE");
        if(bk_waitpid(b,&status)!=b || status)fail("survivor exit");
        bk_sys_log("[LBSSHARE] RELEASED\n");bk_sys_sleep_ms(2000);
        int fault=bk_spawn(self,"fault");
        if(fault<0 || bk_waitpid(fault,&status)!=fault || !status)fail("CPU write protection");
        bk_sys_log("[LBSSHARE] ALL PASS\n");bk_exit(0);
    }
    void *fixture=dlopen("/SYSTEM/TESTS/NATIVE/FIXSHARE.LBS",RTLD_NOW|RTLD_LOCAL);
    if(!fixture){char *error=dlerror();if(error)bk_sys_log(error);fail("fixture load");}
    int (*get)(void)=dlsym(fixture,"share_get");
    volatile int *state=dlsym(fixture,"share_state");
    const volatile unsigned char *constant=dlsym(fixture,"share_constant");
    if(!get || !state || !constant || *constant!=42)fail("fixture exports");
    if(!strcmp(arg,"fault")) {
        snprintf(line,sizeof(line),"[LBSSHARE] EXPECT WRITE FAULT pid=%u address=%x\n",bk_getpid(),(unsigned)get);bk_sys_log(line);
        *(volatile unsigned char *)(void *)get=0xcc;fail("shared code writable");
    }
    unsigned who=!strcmp(arg,"one")?1U:2U;
    void *heavy=bk_lbs_open("NGS3D.LBS",1);if(!heavy)fail("NGS3D load");
    *state=who*111;if(get()!=(int)(who*111))fail("private data");
    int fd=bk_open("/README.TXT",BK_O_RDONLY);
    if(fd<0 || bk_read(fd,(void *)get,1)>=0)fail("kernel write into shared code");bk_close(fd);
    dl_iterate_phdr(mappings,(void *)who);
    snprintf(line,sizeof(line),"[LBSSHARE] READY who=%u pid=%u code=%x constant=%x data=%x value=%u maps=%x count=%u\n",who,bk_getpid(),(unsigned)get,(unsigned)constant,(unsigned)state,(unsigned)get(),(unsigned)records,record_count);bk_sys_log(line);bk_sys_log(line);
    marker(who==1?"/SYSTEM/TESTS/NATIVE/ONE.OK":"/SYSTEM/TESTS/NATIVE/TWO.OK");
    until("/SYSTEM/TESTS/NATIVE/GO");
    if(who==1) {
        if(__bk_lbs_vm_protect((void *)constant,4096,3))fail("private COW protect");
        *(volatile unsigned char *)constant=99;
        if(*constant!=99)fail("private COW write");
        marker("/SYSTEM/TESTS/NATIVE/COW.OK");bk_sys_sleep_ms(4500);
    } else {
        until("/SYSTEM/TESTS/NATIVE/ONE.DONE");
        if(*constant!=42 || get()!=222)fail("cross-process modification");
        bk_sys_log("[LBSSHARE] survivor data and code isolated\n");
    }
    if(dlclose(heavy) || dlclose(fixture))fail("unload");bk_exit(0);
}
