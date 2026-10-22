#include <bleskernos.h>
#include <bleskernos_api.h>
void bleskernos_program_main(void *arg){(void)arg;int pid=bk_spawn("/SYSTEM/PROGRAMS/TEXTEDITOR.BEX",0);int status=-1;if(pid<0 || bk_waitpid(pid,&status)!=pid || status!=127){bk_sys_log("[LBSMISSING] FAIL exit status\n");bk_exit(1);}bk_sys_log("[LBSMISSING] PASS exit status 127\n");bk_exit(0); }
