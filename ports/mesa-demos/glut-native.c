/* SPDX-License-Identifier: MIT */
#include <GL/glut.h>
#include "../../libs/src/font8x8_basic.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../gallium/include/bleskernos_gallium.h"
#include "../tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"
#include "../../libs/Mesa-3.5/include/GL/osmesa.h"
static bkg_context *ctx;
static bkg_context *(*create)(unsigned,unsigned);
static void (*destroy)(bkg_context *);
static int (*present)(bkg_context *);
static int (*poll_event)(bkg_context *,void *);
static void *(*get_proc)(const char *);
static int (*classic_stats)(bkg_context *,OSMesaBleskStats *);
static void (*display)(void),(*idle)(void);
static void (*reshape)(int,int),(*visibility)(int);
static void (*keyboard)(unsigned char,int,int),(*special)(int,int,int);
static void (*mouse)(int,int,int,int),(*motion)(int,int),(*menu)(int);
static unsigned width=640,height=480,frames,redisplay=1,closed,seconds=0;
static int bench_size,menu_count,menu_open,menu_x,menu_y,menu_button=2;
static struct {char title[64];int value;} entries[16];
static double epoch,started;
static const char *backend="gallium",*title;
static int fixed_time,close_check;
static const char *snapshot;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static void die(const char *s){fprintf(stderr,"[MESADEMOS] FAIL %s\n",s);exit(1);}
int gladLoadGL(void){return get_proc && gladLoadGLLoader(get_proc);}
void glutInitWindowSize(int w,int h){if(!bench_size){width=w;height=h;}}
void glutInitDisplayMode(unsigned mode){if(mode&~(GLUT_DOUBLE|GLUT_DEPTH))die("unsupported visual");}
void glutInit(int *argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    epoch=now();
    char **args=argv;int count=*argc;
    int n=1;
    for(int i=1;i<count;i++) {
        if(!strcmp(args[i],"--backend") && i+1<count)backend=args[++i];
        else if(!strcmp(args[i],"--seconds") && i+1<count)seconds=atoi(args[++i]);
        else if(!strcmp(args[i],"--size") && i+1<count){if(sscanf(args[++i],"%ux%u",&width,&height)!=2)die("size");bench_size=1;}
        else if(!strcmp(args[i],"--bkg-trace"))setenv("BKG_MESA35_TRACE","1",1);
        else if(!strcmp(args[i],"--close-check"))close_check=1;
        else if(!strcmp(args[i],"--fixed-time"))fixed_time=1;
        else if(!strcmp(args[i],"--snapshot") && i+1<count)snapshot=args[++i];
        else args[n++]=args[i];
    }
    args[n]=NULL;
    /* Original demos only consume their small remaining option list. */
    for(int i=0;i<n;i++)argv[i]=args[i];*argc=n;
    if(strcmp(backend,"gallium") && strcmp(backend,"mesa35") && strcmp(backend,"software") && strcmp(backend,"auto"))die("unknown backend");
    int classic=strcmp(backend,"gallium")!=0;
    if(classic)setenv("BKG_MESA35_MODE",!strcmp(backend,"software")?"software":!strcmp(backend,"auto")?"auto":"strict",1);
    void *library=dlopen(classic?"/SYSTEM/LIBS/GALLIUM/libbkgmesa.so.1":"/SYSTEM/LIBS/GALLIUM/libbkgfx.so.1",RTLD_NOW|RTLD_LOCAL);
    if(!library){fprintf(stderr,"%s\n",dlerror());die("backend load");}
#define LOAD(name,symbol) do{*(void **)(&name)=dlsym(library,symbol);if(!name)die(symbol);}while(0)
    LOAD(create,"bkg_create");LOAD(destroy,"bkg_destroy");LOAD(present,"bkg_present");LOAD(poll_event,"bkg_poll_event");LOAD(get_proc,"bkg_get_proc");
    *(void **)(&classic_stats)=dlsym(library,"bkg_classic_stats");
#undef LOAD
}
int glutCreateWindow(const char *name) {
    if(ctx)die("one window supported");
    title=name;ctx=create(width,height);if(!ctx)die("context creation");
    if(!gladLoadGL())die("GL entry points");
    fprintf(stderr,"[MESADEMOS] START %s backend=%s size=%ux%u GL=%s renderer=%s\n",title,backend,width,height,glGetString(GL_VERSION),glGetString(GL_RENDERER));
    return 1;
}
void glutDestroyWindow(int id){(void)id;closed=1;}
void glutDisplayFunc(void (*f)(void)){display=f;}
void glutReshapeFunc(void (*f)(int,int)){reshape=f;}
void glutKeyboardFunc(void (*f)(unsigned char,int,int)){keyboard=f;}
void glutSpecialFunc(void (*f)(int,int,int)){special=f;}
void glutVisibilityFunc(void (*f)(int)){visibility=f;}
void glutIdleFunc(void (*f)(void)){idle=f;}
void glutMouseFunc(void (*f)(int,int,int,int)){mouse=f;}
void glutMotionFunc(void (*f)(int,int)){motion=f;}
void glutPostRedisplay(void){redisplay=1;}
int glutGet(unsigned what) {
    if(what==GLUT_ELAPSED_TIME)return fixed_time?1000:(int)((now()-epoch)*1000);
    if(what==GLUT_WINDOW_WIDTH)return width;if(what==GLUT_WINDOW_HEIGHT)return height;
    die("unsupported glutGet");return 0;
}
int glutExtensionSupported(const char *ext){const char *list=(const char *)glGetString(GL_EXTENSIONS);size_t len=strlen(ext);while(list && (list=strstr(list,ext))){if((list==(const char *)glGetString(GL_EXTENSIONS) || list[-1]==' ') && (!list[len] || list[len]==' '))return 1;list+=len;}return 0;}
void glutFullScreen(void){die("fullscreen unsupported; use --size");}
int glutCreateMenu(void (*f)(int)){menu=f;menu_count=0;return 1;}
void glutAddMenuEntry(const char *name,int value){if(menu_count>=16)die("menu capacity");snprintf(entries[menu_count].title,64,"%s",name);entries[menu_count++].value=value;}
void glutAttachMenu(int button){menu_button=button;}
/* Menu text is ordinary triangles, avoiding glBitmap's software fallback. */
static void menu_quad(float x,float y,float w,float h) {
    glVertex2f(x,y);glVertex2f(x+w,y);glVertex2f(x+w,y+h);glVertex2f(x,y+h);
}
static void menu_draw(void) {
    if(!menu_open)return;
    GLint matrix;glGetIntegerv(GL_MATRIX_MODE,&matrix);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);glDisable(GL_DEPTH_TEST);glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glDisable(GL_FOG);
    glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
    glMatrixMode(GL_PROJECTION);glPushMatrix();glLoadIdentity();glOrtho(0,width,height,0,-1,1);
    glMatrixMode(GL_MODELVIEW);glPushMatrix();glLoadIdentity();
    glColor4f(.12f,.14f,.18f,1);glBegin(GL_QUADS);menu_quad(menu_x,menu_y,240,menu_count*22+8);glEnd();
    glColor4f(1,1,1,1);glBegin(GL_QUADS);
    for(int i=0;i<menu_count;i++) {
        char line[80];snprintf(line,sizeof(line),"%d  %s",i+1,entries[i].title);
        for(int c=0;line[c] && c<28;c++)for(int y=0;y<8;y++)for(int x=0;x<8;x++)
            if(font8x8_basic[(unsigned char)line[c]&127][y]&(1<<x))menu_quad(menu_x+6+c*8+x,menu_y+7+i*22+y,1,1);
    }
    glEnd();glPopMatrix();glMatrixMode(GL_PROJECTION);glPopMatrix();glMatrixMode(matrix);glPopAttrib();
}
/* Right click opens, click or 1..9 selects, Escape dismisses. */
static void events(void) {
    bles_gfx_event_t e;
    while(poll_event(ctx,&e)) {
        if(e.type==BLES_GFX_CLOSE)closed=1;
        else if(e.type==BLES_GFX_KEY) {
            if(menu_open){redisplay=1;if(e.key>='1' && e.key<'1'+menu_count){menu(entries[e.key-'1'].value);menu_open=0;}else if(e.key==27)menu_open=0;continue;}
            int key=e.key==0x90?GLUT_KEY_UP:e.key==0x91?GLUT_KEY_DOWN:e.key==0x92?GLUT_KEY_LEFT:e.key==0x93?GLUT_KEY_RIGHT:0;
            if(key && special)special(key,e.x,e.y);else if(keyboard)keyboard(e.key,e.x,e.y);else if(e.key==27)closed=1;
        } else if(e.type==BLES_GFX_MOUSE_MOVE){if(e.buttons && motion)motion(e.x,e.y);}
        else if(e.type==BLES_GFX_MOUSE_DOWN || e.type==BLES_GFX_MOUSE_UP) {
            int button=e.button==1?0:e.button==2?2:1;
            if(menu_open && e.type==BLES_GFX_MOUSE_DOWN && button==0){int row=(e.y-menu_y-4)/22;if(e.x>=menu_x && e.x<menu_x+240 && e.y>=menu_y+4 && row>=0 && row<menu_count)menu(entries[row].value);menu_open=0;redisplay=1;}
            else if(menu && button==menu_button && e.type==BLES_GFX_MOUSE_DOWN){menu_open=1;menu_x=e.x;menu_y=e.y;if(menu_x+240>(int)width)menu_x=width-240;if(menu_y+menu_count*22+8>(int)height)menu_y=height-menu_count*22-8;redisplay=1;fprintf(stderr,"[MESADEMOS] menu at %d,%d\n",menu_x,menu_y);for(int i=0;i<menu_count;i++)fprintf(stderr,"  %d: %s\n",i+1,entries[i].title);}
            else if(mouse)mouse(button,e.type==BLES_GFX_MOUSE_DOWN?GLUT_DOWN:GLUT_UP,e.x,e.y);
        }
    }
}
void glutSwapBuffers(void) {
    menu_draw();
    GLenum error=glGetError();if(error!=GL_NO_ERROR){fprintf(stderr,"GL error 0x%x\n",error);die("OpenGL error");}
    if(!present(ctx))die("present");frames++;
}
static void save_snapshot(void) {
    if(!snapshot)return;
    unsigned char *data=malloc(width*height*3);if(!data)die("snapshot memory");
    glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,data);
    FILE *f=fopen(snapshot,"wb");if(!f)die("snapshot path");
    fprintf(f,"P6\n%u %u\n255\n",width,height);
    for(unsigned y=height;y>0;y--)fwrite(data+(y-1)*width*3,1,width*3,f);
    fflush(f);fsync(fileno(f));fclose(f);sync();free(data);
}
void glutMainLoop(void) {
    if(!display)die("no display callback");
    if(reshape)reshape(width,height);if(visibility)visibility(GLUT_VISIBLE);
    frames=0;started=now();
    while(!closed && (!seconds || now()-started<seconds)) {
        events();if(closed)break;if(idle)idle();
        if(redisplay){redisplay=0;display();}else usleep(1000);
        if(fixed_time && frames>=3)break;
    }
    double elapsed=now()-started;
    OSMesaBleskStats stats={0};if(classic_stats)classic_stats(ctx,&stats);
    fprintf(stderr,"[MESADEMOS] RESULT backend=%s frames=%u seconds=%.3f fps=%.2f draws=%u triangles=%u uploads=%u downloads=%u fallbacks=%u strict_failures=%u textures=%u\n",backend,frames,elapsed,frames/elapsed,stats.draw_calls,stats.triangles,stats.uploads,stats.downloads,stats.fallbacks,stats.strict_failures,stats.texture_uploads);
    save_snapshot();
    if(fixed_time){
        fprintf(stderr,"[MESADEMOS] SNAPSHOT_READY\n");
        if(close_check){
            double deadline=now()+30;
            while(!closed && now()<deadline){events();usleep(10000);}
            if(!closed)die("native close event timeout");
            fprintf(stderr,"[MESADEMOS] CLOSE_CONFIRMED\n");
        }else usleep(2500000);
    }
    destroy(ctx);ctx=NULL;
    fprintf(stderr,"[MESADEMOS] CLOSED %s\n",title);
}

extern int bkg_demo_main(int,char **);
int main(int argc,char **argv) {
    char *args[128];int count=0;args[count++]=argv[0];
    for(int i=1;i<argc;i++) {
        char *copy=strdup(argv[i]),*save=NULL;
        for(char *s=strtok_r(copy," \t",&save);s && count<127;s=strtok_r(NULL," \t",&save))args[count++]=s;
    }
    args[count]=NULL;
    return bkg_demo_main(count,args);
}
