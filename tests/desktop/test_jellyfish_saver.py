#!/usr/bin/env python3
"""Test the real saver lifecycle, input-only GUI registration and fallback policy."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'system/screensavers/ss_jelly.c').read_text().replace('#include "kernel/include/api.h"','')
def function(text,signature):
 start=text.index(signature);end=text.index('\n}',start)+2;return text[start:end]
register=function((ROOT/'gui/desktop.c').read_text(),'gui_program_t *gui_desktop_register_program(')
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
typedef struct {uint32_t width,height;} gui_surface_t;
typedef struct {int type;} gui_event_t;
typedef struct gui_program gui_program_t;
typedef struct gui_desktop {gui_surface_t surface;unsigned next_program_id;gui_program_t *first_program,*last_program;} gui_desktop_t;
typedef void (*gui_program_paint_t)(gui_program_t*,gui_desktop_t*,gui_surface_t*);
typedef bool (*gui_program_event_t)(gui_program_t*,gui_desktop_t*,const gui_event_t*);
typedef void (*gui_program_destroy_t)(gui_program_t*,gui_desktop_t*);
struct gui_program {void *state;unsigned id,callback_pid;gui_program_paint_t paint;gui_program_event_t handle_event;gui_program_destroy_t destroy;bool paint_requested;char name[48];gui_program_t *prev,*next;};
typedef struct {uint32_t pid,process_id,parent_pid;int state;char name[24];} task_t;
enum {GUI_EVENT_KEY=5,GUI_EVENT_MOUSE_MOVE=1,GUI_EVENT_MOUSE_DOWN=2,GUI_EVENT_MOUSE_UP=3,GUI_EVENT_MOUSE_WHEEL=4,TASK_UNUSED=0,TASK_ZOMBIE=4};
#define HEAP_START 1U
static gui_desktop_t desktop={.surface={1024,768}};
static task_t tasks[8];static unsigned count,now,launches,registrations,removals,clear_count,requests[8],request_count;
static int child_status;static bool spawn_fail;static gui_program_t *current;static char message[128];static jmp_buf worker_exit;
static void *kzalloc(size_t n){return calloc(1,n);}
static void kfree(void *p){free(p);}
static bool mm_set_allocation_owner(void *p,unsigned n){(void)p;(void)n;return true;}
static bool task_current_is_user(void){return true;}
static unsigned task_current_pid(void){return 42;}
static int task_set_affinity_mask(unsigned p,unsigned m){assert(p==42 && m==1);return 0;}
static void copy_text(char *d,size_t n,const char *s){snprintf(d,n,"%s",s);}
static bool bk_vision_resource_register(void *p,bool program){assert(p && program);return true;}
''' + register + r'''
static unsigned bk_proc_count(void){return count;}
static const task_t *bk_proc_get(unsigned i){return &tasks[i];}
static bool bk_proc_request_exit(unsigned pid){requests[request_count++]=pid;return true;}
static bool bk_proc_exit_requested(void){return false;}
static unsigned bk_sys_getpid(void){return 42;}
static unsigned bk_sys_uptime_ms(void){return now;}
static void *bk_sys_alloc_zero(size_t n){return calloc(1,n);}
static void bk_sys_free(void *p){free(p);}
static int bk_runtime_strcmp(const char *a,const char *b){return strcmp(a,b);}
static gui_desktop_t *bk_gui_desktop(void){return &desktop;}
static void bk_gui_invalidate_all(gui_desktop_t *d){assert(d==&desktop);}
static void bk_gui_request_paint(void){}
static void bk_gui_gfx_invalidate_front(void){}
static void bk_gui_gfx_clear(gui_surface_t *s,unsigned color){assert(s && color==0);clear_count++;}
static void bk_gui_font_draw_string(gui_surface_t *s,int x,int y,const char *m,unsigned color,unsigned bg,bool opaque){assert(s && x>=0 && y>=0 && color==0xffffff && bg==0 && !opaque);snprintf(message,sizeof(message),"%s",m);}
static gui_program_t *bk_gui_register_program(gui_desktop_t *d,const char *n,void *s,gui_program_paint_t p,gui_program_event_t e,gui_program_destroy_t destroy){
 registrations++;current=gui_desktop_register_program(d,n,s,p,e,destroy);assert(current && current->callback_pid==42);return current;
}
static void bk_gui_unregister_program(gui_desktop_t *d,gui_program_t *p){assert(p && d==&desktop);removals++;free(p);current=NULL;d->first_program=d->last_program=NULL;}
static int bk_app_run_sync(const char *path,const char *args){assert(!strcmp(path,"/SYSTEM/PROGRAMS/GLMARK2.ELF"));assert(!strcmp(args,"--bkg-screensaver --size 640x480"));launches++;return child_status;}
static void bk_proc_exit(void){longjmp(worker_exit,1);}
static int bk_proc_spawn_thread(const char *name,void (*fn)(void*),void *arg){assert(!strcmp(name,"jelly-launch"));if(spawn_fail)return -1;if(!setjmp(worker_exit))fn(arg);return 99;}
static void bk_sys_sleep_ms(unsigned ms){
 assert(ms==10 || ms==20);now+=1000;
 if(current && current->paint){current->paint(current,&desktop,&desktop.surface);gui_event_t e={GUI_EVENT_KEY};assert(current->handle_event(current,&desktop,&e));}
}
''' + source + r'''
static void reset(void){count=now=launches=registrations=removals=clear_count=request_count=0;message[0]=0;spawn_fail=false;}
int main(void){
 assert(!gui_desktop_register_program(&desktop,"invalid",NULL,NULL,NULL,NULL));
 reset();child_status=0;bleskernos_program_main(&desktop);assert(launches==1 && registrations==1 && removals==1 && !clear_count);
 reset();child_status=512;bleskernos_program_main(&desktop);assert(launches==1 && registrations==2 && removals==2 && clear_count==1 && !strcmp(message,"Gallium no esta disponible"));
 reset();child_status=768;bleskernos_program_main(&desktop);assert(!strcmp(message,"No se pudo cargar la medusa"));
 reset();spawn_fail=true;bleskernos_program_main(&desktop);assert(!launches && !strcmp(message,"Gallium no esta disponible"));
 reset();tasks[0]=(task_t){.pid=7,.process_id=7,.state=1,.name="SSJELLY.SCV"};count=1;bleskernos_program_main(&desktop);assert(!launches && !registrations);
 reset();ss_jelly_state state={.owner=42,.worker=99,.input_armed_ms=500};gui_program_t p={.state=&state};gui_event_t event={GUI_EVENT_MOUSE_MOVE};
 assert(ss_jelly_event(&p,&desktop,&event) && !state.dismissed);now=501;assert(ss_jelly_event(&p,&desktop,&event) && state.dismissed);
 event.type=99;assert(!ss_jelly_event(&p,&desktop,&event));
 tasks[0]=(task_t){.pid=76,.process_id=76,.parent_pid=42,.state=1};
 tasks[1]=(task_t){.pid=77,.process_id=77,.parent_pid=1,.state=1};
 tasks[2]=(task_t){.pid=78,.process_id=78,.parent_pid=99,.state=1};
 tasks[3]=(task_t){.pid=79,.process_id=79,.parent_pid=42,.state=TASK_ZOMBIE};count=4;
 ss_jelly_stop_child(&state);assert(request_count==2 && requests[0]==76 && requests[1]==78);
 puts("PASS: input-only registration, Gallium child lifecycle, unavailable/resource-error white text, no software substitution, input grace, singleton and descendant-only dismissal");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-jelly-saver-') as directory:
 p=Path(directory);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
