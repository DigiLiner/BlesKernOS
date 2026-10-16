#include <bleskernos_api.h>
void shell_execute_line(const char *line);
int snprintf(char *,size_t,const char *,...);
static uint32_t failures,checks;
static void check(bool ok,const char *name) {
    char line[256];checks++;
    snprintf(line,sizeof(line),"[CMDTEST] %s %s\n",ok?"PASS":"FAIL",name);
    bk_sys_log(line);if(!ok)failures++;
}
static void command(const char *name,const char *arguments,int expected) {
    char path[260],line[2048];
    snprintf(path,sizeof(path),"/SYSTEM/COMMANDS/%s.BEX",name);
    snprintf(line,sizeof(line),"%s --cwd /SYSTEM/USER/CMDTEST %s",name,arguments);
    int status=bk_app_run_sync(path,line);
    char label[1024];snprintf(label,sizeof(label),"%s %s rc=%d esperado=%d",name,arguments,status,expected);
    check(status==expected,label);
}
static bool file_equals(const char *path,const void *data,uint32_t size) {
    void *raw=NULL;uint32_t n=0;
    if(!bk_file_read_all(path,&raw,&n))return false;
    bool ok=n==size && !bk_memory_compare(raw,data,size);bk_sys_free(raw);return ok;
}
void bleskernos_program_main(void *desktop UNUSED) {
    bk_sys_log("[CMDTEST] START\n");
    check(bk_sys_api_version()>=40,"API 40");
    check(bk_file_mkdir("/SYSTEM/USER/CMDTEST"),"crear directorio de prueba");
    const char *sample="alpha beta\nGamma\nz";
    check(bk_file_write_all("/SYSTEM/USER/CMDTEST/source",sample,18),"crear fixture");
    command("help","",0);command("help","diskpart",0);command("help","touch",0);
    command("touch","source empty",0);
    check(file_equals("/SYSTEM/USER/CMDTEST/source",sample,18),"touch preserva datos");
    command("cp","-v source copied",0);command("cmp","source copied",0);
    command("cp","source ./source",2);command("cp","-n empty copied",2);
    command("head","-n 1 source",0);command("tail","-n 1 source",0);
    command("wc","source",0);command("grep","-in GAMMA source",0);command("grep","ausente source",1);
    command("diff","source empty",1);command("rmdir","source",2);
    command("mkdir","-p folder/deep",0);command("find",". -name source -type f",0);
    command("tree",". -d 2",0);command("du",".",0);command("df","/",0);
    command("stat","source",0);command("chmod","640 source",0);
    uint16_t mode=0;check(bk_file_get_mode("/SYSTEM/USER/CMDTEST/source",&mode)&&mode==0640,"chmod persistente");
    command("attrib","-r source",0);command("which","touch pwd",0);
    command("calc","1 - 2",0);command("calc","2147483647 + 1",2);
    command("sleep","4294967296",2);command("compress","source packed",0);command("extract","packed restored",0);
    command("cmp","source restored",0);command("netstat","-a",0);command("lsusb","-v",0);
    char args[512];
    snprintf(args,sizeof(args),"-L http://10.0.2.2:%u/redirect",CMD_HTTP_PORT);command("curl",args,0);
    snprintf(args,sizeof(args),"-I http://10.0.2.2:%u/data",CMD_HTTP_PORT);command("curl",args,0);
    snprintf(args,sizeof(args),"http://10.0.2.2:%u/large large.bin",CMD_HTTP_PORT);command("wget",args,0);
    bk_file_entry_t e;check(bk_file_stat("/SYSTEM/USER/CMDTEST/large.bin",&e)&&e.size==33280,"descarga HTTP > 8 KiB completa");
    snprintf(args,sizeof(args),"10.0.2.2:%u get remote.bin ftp.bin",CMD_FTP_PORT);command("ftp",args,0);
    check(bk_file_stat("/SYSTEM/USER/CMDTEST/ftp.bin",&e)&&e.size==33280,"descarga FTP completa");
    snprintf(args,sizeof(args),"10.0.2.2:%u put source uploaded.txt",CMD_FTP_PORT);command("ftp",args,0);
    command("diskpart","list disk",0);command("diskpart","list partition",0);
    command("diskpart","create ata0 1 2048 32768 fat32 /YES",2);
    command("diskpart","create ata1 1 2048 131072 fat32 /YES",0);
    command("diskpart","create ata1 2 30000 65536 fat32 /YES",2);
    command("diskpart","detail ata1",0);
    command("format","ata1p1 CMDTEST /YES",0);
    command("mount","ata1p1",0);command("df","/ata1p1",0);
    command("unmount","ata0",2);command("unmount","ata1p1",0);
    bk_file_entry_t detached;check(!bk_file_stat("/ata1p1",&detached),"volumen desmontado no se remonta solo");
    command("mount","ata1p1",0);
    check(bk_file_write_all("/ata1p1/probe","diskpart",8),"escritura en particion nueva");
    int busy=bk_file_open("/ata1p1/probe",BK_FILE_READ_ONLY);
    check(busy>=0,"abrir particion para verificar ocupado");
    command("diskpart","delete ata1 1 /YES",2);command("unmount","ata1p1",2);
    if(busy>=0)bk_file_close(busy);
    command("unmount","ata1p1",0);command("diskpart","delete ata1 1 /YES",0);
    command("hostname","-s CMD-TEST",0);command("hostname","",0);
    command("hostname","-s bad.name",2);
    command("find",". -name *.bin -type f",0);
    command("sh","-c \"cmp source restored\"",0);
    shell_execute_line("alias c 'cp -v'");
    shell_execute_line("c '/SYSTEM/USER/CMDTEST/source' '/SYSTEM/USER/CMDTEST/alias copied'");
    check(file_equals("/SYSTEM/USER/CMDTEST/alias copied",sample,18),"alias preserva argumentos con espacios");
    shell_execute_line("help diskpart");shell_execute_line("mem -m");
    command("sync","",0);
    char summary[128];snprintf(summary,sizeof(summary),"[CMDTEST] DONE checks=%u failures=%u\n",checks,failures);bk_sys_log(summary);
    bk_proc_exit_status(failures?1:0);
}
