#include <bleskernos.h>
#include <bleskernos_api.h>
#include <bleskernos_admin.h>
static void report(const char *prefix,const char *path){bk_sys_log(prefix);bk_sys_log(path);bk_sys_log("\n");}
void bleskernos_program_main(void *unused){(void)unused;
const char *paths[]={"/SYSTEM/PROGRAMS/FILE.BEX","/SYSTEM/PROGRAMS/TEXTEDITOR.BEX","/SYSTEM/PROGRAMS/CALCULATOR.BEX","/SYSTEM/PROGRAMS/FONT.BEX","/SYSTEM/PROGRAMS/MIDAMP.BEX","/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX","/SYSTEM/PROGRAMS/CALENDAR.BEX","/SYSTEM/PROGRAMS/ABOUT.BEX","/SYSTEM/PROGRAMS/RUNBOX.BEX","/SYSTEM/PROGRAMS/IMAGEVIEWER.BEX","/SYSTEM/PROGRAMS/VIEWER.BEX","/SYSTEM/PROGRAMS/PAINT.BEX","/SYSTEM/PROGRAMS/SCANDISK.BEX","/SYSTEM/CONTROL/CONTROL.BEX","/SYSTEM/CONTROL/APPEAR.CPL","/SYSTEM/CONTROL/DISPLAY.CPL","/SYSTEM/CONTROL/SOUND.CPL","/SYSTEM/CONTROL/DATETIME.CPL","/SYSTEM/CONTROL/MOUSE.CPL","/SYSTEM/CONTROL/KEYBOARD.CPL","/SYSTEM/CONTROL/SYSTEM.CPL","/SYSTEM/CONTROL/DEVMGR.CPL","/SYSTEM/CONTROL/INTERNET.CPL","/SYSTEM/CONTROL/MODEM.CPL","/SYSTEM/CONTROL/LANGUAGE.CPL","/SYSTEM/PROGRAMS/NETSURF.BEX","/SYSTEM/PROGRAMS/HELP_CENTER.BEX","/SYSTEM/PROGRAMS/FIND_FILES.BEX","/SYSTEM/PROGRAMS/ARCHIVE_MANAGER.BEX","/SYSTEM/PROGRAMS/DISK_TOOLS.BEX","/SYSTEM/PROGRAMS/CLIPBOARD_VIEWER.BEX","/SYSTEM/PROGRAMS/DEFRAG.BEX","/SYSTEM/PROGRAMS/EVENT_VIEWER.BEX","/SYSTEM/PROGRAMS/PERFORMANCE_MONITOR.BEX","/SYSTEM/PROGRAMS/SHELL.BEX","/SYSTEM/PROGRAMS/NETWORK_STATUS.BEX"};
for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
 report("[LBSMIG] starting ",paths[i]);int pid=bk_spawn(paths[i],0);
 if(pid<0){report("[LBSMIG] FAIL spawn ",paths[i]);bk_exit(1);}
 bk_sys_sleep_ms(8000);
 bk_process_info_t tasks[128];unsigned count=bk_proc_snapshot(tasks,128);int alive=0;
 for(unsigned j=0;j<count;j++)if(tasks[j].process_id==(unsigned)pid && tasks[j].state!=BK_PROCESS_ZOMBIE && tasks[j].state!=BK_PROCESS_UNUSED)alive=1;
 if(!alive){report("[LBSMIG] FAIL lifetime ",paths[i]);bk_exit(2);}
 report("[LBSMIG] PASS startup ",paths[i]);bk_sys_sleep_ms(700);
 if(!bk_proc_end_process(pid)){report("[LBSMIG] FAIL cleanup ",paths[i]);bk_exit(3);}
 int status;
 for(unsigned attempt=0;attempt<100;attempt++){
  int live=0;count=bk_proc_snapshot(tasks,128);
  for(unsigned j=0;j<count;j++)if(tasks[j].process_id==(unsigned)pid && tasks[j].state!=BK_PROCESS_ZOMBIE && tasks[j].state!=BK_PROCESS_UNUSED)live=1;
  if(!live)break;
  if(attempt==99){report("[LBSMIG] FAIL surviving thread ",paths[i]);bk_exit(4);}
  bk_sys_sleep_ms(100);
 }
 bk_waitpid(pid,&status);
 report("[LBSMIG] PASS cleanup ",paths[i]);
}bk_sys_log("[LBSMIG] ALL PASS\n");bk_exit(0);}
