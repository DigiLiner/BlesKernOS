#!/usr/bin/env python3
"""Run the actual asynchronous operation worker against an isolated filesystem."""
from pathlib import Path
import ast, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
# Reuse only the filesystem shim, without executing the other test module.
tree=ast.parse((root/'tests/desktop/test_file_ops.py').read_text())
fs=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign)
        and any(isinstance(t,ast.Name) and t.id=='shim' for t in n.targets))
fs=fs.replace('int type;char name[256];','int type;uint32_t size;char name[256];')
fs=fs.replace('entry->type=S_ISDIR(s.st_mode)?2:1;', 'entry->size=s.st_size;entry->type=S_ISDIR(s.st_mode)?2:1;')
ops=(root/'system/desktop/file_ops.h').read_text()
ops=ops[ops.index('typedef void (*bk_shell_progress_fn)'):ops.rindex('#endif')]
worker=(root/'system/desktop/file_progress.h').read_text()
worker=worker[worker.index('enum {BK_SHELL_JOB_MOVE'):worker.rindex('#endif')]
gui=r'''
#define UNUSED __attribute__((unused))
#define GUI_EVENT_CLOSE 9
#define BK_GUI_PROGRESS_CLASSIC 2
#define bk_runtime_strncpy strncpy
#define bk_runtime_strcpy strcpy
#define BK_DESKTOP_FILES "desktop"
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {unsigned unused;} gui_surface_t;
typedef struct {int type;} gui_event_t;
typedef struct {uint32_t *pixels;unsigned width,height;} gui_image_t;
typedef struct {unsigned unused;} gui_desktop_t;
typedef struct gui_window gui_window_t;
struct gui_window {unsigned owner_pid;bool dirty,resizable;void *context;
 void (*paint)(gui_window_t*,gui_surface_t*,void*);};
static gui_window_t *active_window;
static unsigned ticks,painted,errors,changes,spawned,last_percent;
static bool monotonic;
static char resource[32],last_message[320];
static void (*pending)(void*);static void *pending_argument;
static bool bk_graphics_icon_load(const char *name,gui_image_t *image){
 strcpy(resource,name);image->pixels=calloc(1,4);image->width=image->height=1;return true;}
static void bk_gui_image_free(gui_image_t *image){free(image->pixels);}
static gui_rect_t bk_gui_window_content_rect_raw(gui_window_t *w){(void)w;return (gui_rect_t){0,0,440,110};}
static void bk_gui_gfx_fill_rect(gui_surface_t *s,gui_rect_t r,unsigned c){(void)s;(void)r;(void)c;}
static void bk_gui_surface_draw_image(gui_surface_t *s,gui_rect_t a,gui_rect_t b,gui_image_t *image){(void)s;(void)a;(void)b;assert(image->pixels);}
static void bk_gui_font_draw_string_clipped(gui_surface_t *s,int x,int y,const char *text,unsigned color,gui_rect_t clip){
 (void)s;(void)x;(void)color;(void)clip;if(y==16)strncpy(last_message,text,sizeof(last_message)-1);}
static void bk_gui_surface_draw_progress(gui_surface_t *s,gui_rect_t r,unsigned percent,unsigned style,unsigned phase){
 (void)s;(void)r;(void)style;(void)phase;assert(percent<=100);if(monotonic)assert(percent>=last_percent);last_percent=percent;++painted;}
static gui_window_t *bk_gui_create_window(gui_desktop_t *d,int x,int y,int w,int h,const char *title){
 (void)d;(void)x;(void)y;(void)w;(void)h;assert(title && !active_window);return active_window=calloc(1,sizeof(*active_window));}
static void bk_gui_set_window_content(gui_window_t *w,void (*paint)(gui_window_t*,gui_surface_t*,void*),void *context){w->paint=paint;w->context=context;}
static void bk_gui_set_window_event_handler(gui_window_t *w,bool (*event)(gui_window_t*,const gui_event_t*,void*),void *context){
 gui_event_t close={GUI_EVENT_CLOSE};assert(event(w,&close,context));}
static void bk_gui_window_set_extended_events(gui_window_t *w,bool enabled){(void)w;assert(enabled);}
static void bk_gui_focus_window(gui_desktop_t *d,gui_window_t *w){(void)d;assert(w==active_window);}
static void bk_proc_bind_window(gui_window_t *w){(void)w;}
static unsigned bk_sys_ticks(void){return ticks+=10;}
static unsigned bk_sys_tick_frequency(void){return 100;}
static void bk_gui_request_paint(void){if(active_window && active_window->paint){gui_surface_t surface={0};active_window->paint(active_window,&surface,active_window->context);}}
static void bk_sys_sleep_ticks(unsigned count){ticks+=count;}
static void bk_sys_sleep_ms(unsigned count){ticks+=count/10;}
static void bk_gui_destroy_window(gui_desktop_t *d,gui_window_t *w){(void)d;assert(w==active_window);free(w);active_window=NULL;}
static void bk_gui_files_changed(void){++changes;}
static bool bk_gui_error(const char *title,const char *message){assert(title && message);++errors;return true;}
static int bk_proc_spawn_thread(const char *name,void (*entry)(void*),void *argument){
 assert(name && !pending);pending=entry;pending_argument=argument;++spawned;return 1;}
static void bk_proc_exit(void){}
'''
tests=r'''
static void run(void){assert(pending);void (*entry)(void*)=pending;void *arg=pending_argument;
 pending=NULL;last_percent=0;monotonic=true;entry(arg);assert(!active_window);}
int main(void){
 gui_desktop_t desktop={0};assert(bk_file_mkdir("desktop"));assert(bk_file_mkdir("dest"));
 char path[260]="original";assert(bk_file_write_all(path,"data",4));
 assert(bk_shell_job_one(&desktop,BK_SHELL_JOB_MOVE,path,"dest"));
 assert(!access(path,F_OK) && access("dest/original",F_OK)); /* Deferred disk operation. */
 strcpy(path,"changed-by-caller");run();assert(access("original",F_OK) && !access("dest/original",F_OK));
 assert(changes==1 && !errors && painted>=2 && last_percent==100 && !strcmp(resource,"Folder"));
 assert(strstr(last_message,"Moviendo original"));
 assert(bk_shell_job_one(&desktop,BK_SHELL_JOB_TRASH,"dest/original",NULL));run();
 assert(!access("trash/original",F_OK) && !strcmp(resource,"RecycleEmpty") && last_percent==100);
 assert(bk_file_mkdir("dest/tree"));assert(bk_file_mkdir("dest/tree/sub"));
 char large[40000];memset(large,'x',sizeof(large));assert(bk_file_write_all("dest/tree/sub/large",large,sizeof(large)));
 assert(bk_shell_job_one(&desktop,BK_SHELL_JOB_COPY,"dest/tree","desktop"));run();
 assert(!access("desktop/tree/sub/large",F_OK) && last_percent==100);
 assert(bk_shell_job_one(&desktop,BK_SHELL_JOB_DELETE,"desktop/tree",NULL));run();
 assert(access("desktop/tree",F_OK) && last_percent==100 && !strcmp(resource,"RecycleEmpty"));
 assert(bk_file_write_all("trash/.bk-trash-original","metadata",9));
 assert(bk_shell_job_one(&desktop,BK_SHELL_JOB_DELETE,"trash/original",NULL));run();
 assert(access("trash/.bk-trash-original",F_OK));
 assert(bk_shell_job_one(&desktop,BK_SHELL_JOB_MOVE,"missing","dest"));run();
 assert(errors==1 && last_percent<100); /* A failure never reports success. */
 assert(!bk_shell_job_start(bk_shell_job_create(&desktop,BK_SHELL_JOB_MOVE,"dest")));
 assert(!bk_shell_job_one(&desktop,BK_SHELL_JOB_MOVE,"","dest"));
 bk_shell_job_t *job=bk_shell_job_create(&desktop,BK_SHELL_JOB_MOVE,"dest");
 for(int i=0;i<128;++i)assert(bk_shell_job_add(job,"valid"));assert(!bk_shell_job_add(job,"overflow"));free(job);
 puts("PASS asynchronous file progress, owned paths, recursive copy/delete, trash metadata and failure reporting");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-file-progress-') as temp:
 source=Path(temp)/'test.c';source.write_text(fs+gui+ops+worker+tests)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',str(source),'-o',temp+'/test'],check=True)
 subprocess.run([temp+'/test'],cwd=temp,check=True)
