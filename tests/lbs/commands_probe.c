#include <bleskernos.h>
#include <bleskernos_api.h>
#include <string.h>
static void require(bool ok,const char *what) {
    if (!ok) {bk_sys_log("[LBS:COMMANDS] FAIL ");bk_sys_log(what);bk_sys_log("\n");bk_exit(1);}
}
static void command(const char *path,const char *args) {
    int pid=bk_spawn(path,args),status=-1;
    require(pid>0,"spawn");require(bk_waitpid(pid,&status)==pid && status==0,path);
}
void bleskernos_program_main(void *unused) {
    (void)unused;
    static const char data[]="shared-code shared-code shared-code\n";
    require(bk_file_write_all("/TEMP/LBSRAW.TXT",data,sizeof(data)),"write source");
    command("/SYSTEM/COMMANDS/compress.BEX","compress /TEMP/LBSRAW.TXT /TEMP/LBSRAW.BKZ");
    command("/SYSTEM/COMMANDS/extract.BEX","extract /TEMP/LBSRAW.BKZ /TEMP/LBSOUT.TXT");
    command("/SYSTEM/COMMANDS/copy.BEX","copy /TEMP/LBSOUT.TXT /TEMP/LBSCOPY.TXT");
    void *decoded=0;uint32_t size=0;
    require(bk_file_read_all("/TEMP/LBSCOPY.TXT",&decoded,&size) && size==sizeof(data) && !memcmp(decoded,data,size),"round trip bytes");
    bk_sys_free(decoded);
    command("/SYSTEM/COMMANDS/echo.BEX","echo LBS command shared parser");
    command("/SYSTEM/COMMANDS/checksum.BEX","checksum /TEMP/LBSCOPY.TXT");
    require(bk_file_write_all("/TEMP/LBSCMD.OK", "PASS", 4), "write completion");
    for (unsigned i=0;i<3;i++) { bk_sys_log("[LBS:COMMANDS] ALL PASS\n");bk_sys_sleep_ms(100); }
    bk_exit(0);
}
