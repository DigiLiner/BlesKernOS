#include <bleskernos_lbs.h>
#include <bleskernos_api.h>
#include <dlfcn.h>
extern void bk_lbs_application_main(void *);
void *__bk_lbs_app_slots[12];
void bleskernos_program_main(void *arg) {
 void *handles[4]={0};
 unsigned loaded=0;
 const char *failed="opening module";
 failed="FONT.LBS";
 handles[0]=bk_lbs_open(failed,BK_LBS_ABI_MAJOR);
 if(!handles[0])goto fail;
 loaded++;
 failed="NGS.LBS";
 handles[1]=bk_lbs_open(failed,BK_LBS_ABI_MAJOR);
 if(!handles[1])goto fail;
 loaded++;
 failed="SYSTEM.LBS";
 handles[2]=bk_lbs_open(failed,BK_LBS_ABI_MAJOR);
 if(!handles[2])goto fail;
 loaded++;
 failed="VISION.LBS";
 handles[3]=bk_lbs_open(failed,BK_LBS_ABI_MAJOR);
 if(!handles[3])goto fail;
 loaded++;
 failed="bk_gui_desktop";
 __bk_lbs_app_slots[0]=dlsym(handles[3],failed);
 if(!__bk_lbs_app_slots[0])goto fail;
 failed="bk_gui_invalidate_all";
 __bk_lbs_app_slots[1]=dlsym(handles[3],failed);
 if(!__bk_lbs_app_slots[1])goto fail;
 failed="bk_gui_request_paint";
 __bk_lbs_app_slots[2]=dlsym(handles[3],failed);
 if(!__bk_lbs_app_slots[2])goto fail;
 failed="bk_proc_exit_requested";
 __bk_lbs_app_slots[3]=dlsym(handles[2],failed);
 if(!__bk_lbs_app_slots[3])goto fail;
 failed="bk_sys_alloc_zero";
 __bk_lbs_app_slots[4]=dlsym(handles[2],failed);
 if(!__bk_lbs_app_slots[4])goto fail;
 failed="bk_sys_free";
 __bk_lbs_app_slots[5]=dlsym(handles[2],failed);
 if(!__bk_lbs_app_slots[5])goto fail;
 failed="bk_sys_sleep_ms";
 __bk_lbs_app_slots[6]=dlsym(handles[2],failed);
 if(!__bk_lbs_app_slots[6])goto fail;
 failed="bk_sys_uptime_ms";
 __bk_lbs_app_slots[7]=dlsym(handles[2],failed);
 if(!__bk_lbs_app_slots[7])goto fail;
 failed="gui_font_draw_string";
 __bk_lbs_app_slots[8]=dlsym(handles[0],failed);
 if(!__bk_lbs_app_slots[8])goto fail;
 failed="gui_font_draw_string_scaled";
 __bk_lbs_app_slots[9]=dlsym(handles[0],failed);
 if(!__bk_lbs_app_slots[9])goto fail;
 failed="gui_gfx_draw_rect";
 __bk_lbs_app_slots[10]=dlsym(handles[1],failed);
 if(!__bk_lbs_app_slots[10])goto fail;
 failed="gui_gfx_fill_rect";
 __bk_lbs_app_slots[11]=dlsym(handles[1],failed);
 if(!__bk_lbs_app_slots[11])goto fail;
 for(unsigned i=0;i<loaded;i++)if(bk_dl_pin(handles[i]))goto fail;
 bk_sys_log("[LBS:APP] /SYSTEM/PROGRAMS/SAVERPROBE.BEX modules=FONT,NGS,SYSTEM,VISION symbols=12 ready\n");
 bk_lbs_application_main(arg);
 return;
fail:
 bk_sys_log("[LBS:APP] FAIL /SYSTEM/PROGRAMS/SAVERPROBE.BEX: ");
 bk_sys_log(failed); bk_sys_log("\n");
 {const char *error=dlerror();if(error)bk_sys_log(error);bk_sys_log("\n");}
 while(loaded)dlclose(handles[--loaded]);
 bk_exit(127);
}
