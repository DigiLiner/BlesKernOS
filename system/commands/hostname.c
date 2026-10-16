#include "common.h"
#define HOST_FILE "/SYSTEM/USER/CONFIG/HOSTNAME"
static bool valid_name(const char *s) {
    uint32_t n=bk_runtime_strlen(s);if(!n||n>63||s[0]=='-'||s[n-1]=='-')return false;
    for(uint32_t i=0;i<n;i++)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||(s[i]>='0'&&s[i]<='9')||s[i]=='-'))return false;
    return true;
}
static int run(int argc,char **argv) {
    if(argc==3 && command_is(argv[1],"-s")) {
        if(!valid_name(argv[2]))return command_error("hostname","nombre invalido (1..63 letras, numeros y guiones)");
        return bk_file_write_all(HOST_FILE,argv[2],bk_runtime_strlen(argv[2]))?0:2;
    }
    if(argc!=1)return command_error("hostname","uso: hostname [-s nombre]");
    int fd=bk_file_open(HOST_FILE,BK_FILE_READ_ONLY);char host[64];int n=fd<0?-1:bk_file_read(fd,host,63);
    if(fd>=0)bk_file_close(fd);
    if(n<=0){bk_console_write("BLESKERNOS\n");return 0;}
    host[n]=0;if(!valid_name(host))return command_error("hostname","configuracion invalida");
    kprintf("%s\n",host);return 0;
}
BK_COMMAND_MAIN(run)
