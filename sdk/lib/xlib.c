#include "bleskernos_api.h"
#include <X11/Xlib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define X_WINDOW_LIMIT 16U
#define X_EVENT_LIMIT 128U
typedef struct {
    Window id; bk_gui_window_t *native; unsigned int width,height;
    int x,y; unsigned long background,event_mask; char title[96];
    uint32_t *pixels; Bool mapped;
} x_window_t;
struct _XDisplay {
    bk_gui_desktop_t *desktop; x_window_t windows[X_WINDOW_LIMIT];
    XEvent events[X_EVENT_LIMIT]; volatile unsigned int head,tail;
};
struct _XGC { unsigned long foreground,background; int line_width; };

static x_window_t *window_for(Display*d,Window id){if(!d||!id||id>X_WINDOW_LIMIT)return 0;return d->windows[id-1U].id?&d->windows[id-1U]:0;}
static void push_event(Display*d,const XEvent*event){unsigned int next;if(!d||!event)return;next=(d->tail+1U)%X_EVENT_LIMIT;if(next==d->head)return;d->events[d->tail]=*event;__sync_synchronize();d->tail=next;}
static void expose(x_window_t*w,Display*d){XEvent e;if(!w||!(w->event_mask&ExposureMask))return;memset(&e,0,sizeof(e));e.xexpose.type=Expose;e.xexpose.display=d;e.xexpose.window=w->id;e.xexpose.width=(int)w->width;e.xexpose.height=(int)w->height;push_event(d,&e);}
static void x_paint(bk_gui_window_t*n,bk_gui_surface_t*s,void*opaque){x_window_t*w=(x_window_t*)opaque;bk_gui_rect_t c;bk_gui_image_t image;(void)n;if(!w||!s||!w->pixels||!bk_gui_window_content_rect(w->native,&c))return;image.pixels=w->pixels;image.width=(uint16_t)w->width;image.height=(uint16_t)w->height;bk_gui_surface_draw_image(s,(bk_gui_rect_t){c.x,c.y,(int)w->width,(int)w->height},c,&image);}
static bool x_event(bk_gui_window_t*n,const bk_gui_event_t*source,void*opaque){x_window_t*w=(x_window_t*)opaque;Display*d;XEvent e;bk_gui_rect_t c;(void)n;if(!w||!source)return false;d=(Display*)(uintptr_t)w->pixels[-1];memset(&e,0,sizeof(e));e.xany.display=d;e.xany.window=w->id;(void)bk_gui_window_content_rect(w->native,&c);
    if(source->type==BK_GUI_EVENT_CLOSE){e.type=ClientMessage;push_event(d,&e);return true;}
    if(source->type==BK_GUI_EVENT_RESIZE&&(w->event_mask&StructureNotifyMask)){e.type=ConfigureNotify;e.xconfigure.width=source->x;e.xconfigure.height=source->y;push_event(d,&e);return true;}
    if((source->type==BK_GUI_EVENT_KEY||source->type==BK_GUI_EVENT_KEY_UP)&&w->event_mask&(source->type==BK_GUI_EVENT_KEY?KeyPressMask:KeyReleaseMask)){e.type=source->type==BK_GUI_EVENT_KEY?KeyPress:KeyRelease;e.xkey.keycode=(unsigned char)source->key;e.xkey.state=(source->shift?1U:0U)|(source->ctrl?4U:0U)|(source->alt?8U:0U);push_event(d,&e);return true;}
    if(source->type==BK_GUI_EVENT_MOUSE_MOVE&&(w->event_mask&PointerMotionMask)){e.type=MotionNotify;e.xmotion.x=source->x-c.x;e.xmotion.y=source->y-c.y;e.xmotion.state=source->buttons;push_event(d,&e);return true;}
    if((source->type==BK_GUI_EVENT_MOUSE_DOWN||source->type==BK_GUI_EVENT_MOUSE_UP)&&w->event_mask&(source->type==BK_GUI_EVENT_MOUSE_DOWN?ButtonPressMask:ButtonReleaseMask)){e.type=source->type==BK_GUI_EVENT_MOUSE_DOWN?ButtonPress:ButtonRelease;e.xbutton.x=source->x-c.x;e.xbutton.y=source->y-c.y;e.xbutton.keycode=source->button;push_event(d,&e);return true;}
    return false;
}
Display *XOpenDisplay(const char*name){Display*d;(void)name;d=(Display*)calloc(1,sizeof(*d));if(!d)return 0;d->desktop=bk_gui_desktop();if(!d->desktop){free(d);return 0;}return d;}
int XCloseDisplay(Display*d){if(!d)return 0;for(unsigned int i=0;i<X_WINDOW_LIMIT;i++)if(d->windows[i].id)XDestroyWindow(d,d->windows[i].id);free(d);return 0;}
Window XDefaultRootWindow(Display*d){(void)d;return 0;}int XDefaultScreen(Display*d){(void)d;return 0;}unsigned long XBlackPixel(Display*d,int s){(void)d;(void)s;return 0;}unsigned long XWhitePixel(Display*d,int s){(void)d;(void)s;return 0xFFFFFFUL;}
Window XCreateSimpleWindow(Display*d,Window parent,int x,int y,unsigned int width,unsigned int height,unsigned int border_width,unsigned long border,unsigned long background){(void)parent;(void)border_width;(void)border;if(!d||!width||!height||width>1600U||height>1200U)return 0;for(unsigned int i=0;i<X_WINDOW_LIMIT;i++)if(!d->windows[i].id){x_window_t*w=&d->windows[i];uint32_t*allocation=(uint32_t*)calloc((size_t)width*height+1U,sizeof(uint32_t));if(!allocation)return 0;w->id=i+1U;w->x=x;w->y=y;w->width=width;w->height=height;w->background=background;w->pixels=allocation+1;allocation[0]=(uint32_t)(uintptr_t)d;strcpy(w->title,"X11 application");for(size_t p=0;p<(size_t)width*height;p++)w->pixels[p]=(uint32_t)background;return w->id;}return 0;}
static int map_window(Display*d,x_window_t*w){if(!d||!w)return 0;if(w->mapped)return 1;w->native=bk_gui_create_window(d->desktop,w->x,w->y,(int)w->width+16,(int)w->height+40,w->title);if(!w->native)return 0;bk_gui_window_set_owner(w->native,bk_sys_getpid());bk_gui_window_set_extended_events(w->native,true);bk_gui_set_window_content(w->native,x_paint,w);bk_gui_set_window_event_handler(w->native,x_event,w);bk_proc_bind_window(w->native);w->mapped=True;bk_gui_window_invalidate(w->native);expose(w,d);return 1;}
int XMapWindow(Display*d,Window id){return map_window(d,window_for(d,id))?0:-1;}int XMapRaised(Display*d,Window id){return XMapWindow(d,id);}int XUnmapWindow(Display*d,Window id){x_window_t*w=window_for(d,id);if(!w||!w->native)return-1;bk_gui_close_window(w->native);w->mapped=False;w->native=0;return 0;}
int XDestroyWindow(Display*d,Window id){x_window_t*w=window_for(d,id);if(!w)return-1;if(w->native)bk_gui_destroy_window(d->desktop,w->native);free(w->pixels-1);memset(w,0,sizeof(*w));return 0;}
int XStoreName(Display*d,Window id,const char*name){x_window_t*w=window_for(d,id);if(!w||!name)return 0;strncpy(w->title,name,sizeof(w->title)-1U);w->title[sizeof(w->title)-1U]=0;return 1;}
int XSelectInput(Display*d,Window id,long mask){x_window_t*w=window_for(d,id);if(!w)return 0;w->event_mask=(unsigned long)mask;return 1;}
int XPending(Display*d){if(!d)return 0;return d->tail>=d->head?(int)(d->tail-d->head):(int)(X_EVENT_LIMIT-d->head+d->tail);}
int XNextEvent(Display*d,XEvent*e){if(!d||!e)return-1;while(!XPending(d))bk_sys_yield();*e=d->events[d->head];__sync_synchronize();d->head=(d->head+1U)%X_EVENT_LIMIT;return 0;}
int XPeekEvent(Display*d,XEvent*e){if(!d||!e)return-1;while(!XPending(d))bk_sys_yield();*e=d->events[d->head];return 0;}
int XFlush(Display*d){if(d)bk_gui_request_paint();return 0;}int XSync(Display*d,Bool discard){if(d&&discard)d->head=d->tail;return XFlush(d);}
GC XCreateGC(Display*d,Drawable id,unsigned long mask,XGCValues*v){GC gc;(void)d;(void)id;gc=(GC)calloc(1,sizeof(*gc));if(!gc)return 0;gc->foreground=(mask&GCForeground)&&v?v->foreground:0;gc->background=(mask&GCBackground)&&v?v->background:0xFFFFFFUL;gc->line_width=1;return gc;}
int XFreeGC(Display*d,GC gc){(void)d;free(gc);return 0;}int XSetForeground(Display*d,GC gc,unsigned long c){(void)d;if(!gc)return 0;gc->foreground=c;return 1;}int XSetBackground(Display*d,GC gc,unsigned long c){(void)d;if(!gc)return 0;gc->background=c;return 1;}
static void put_pixel(x_window_t*w,int x,int y,uint32_t c){if(w&&x>=0&&y>=0&&(unsigned)x<w->width&&(unsigned)y<w->height)w->pixels[(unsigned)y*w->width+(unsigned)x]=c;}
int XClearWindow(Display*d,Window id){x_window_t*w=window_for(d,id);if(!w)return-1;for(size_t i=0;i<(size_t)w->width*w->height;i++)w->pixels[i]=(uint32_t)w->background;if(w->native)bk_gui_window_invalidate(w->native);expose(w,d);return 0;}
int XFillRectangle(Display*d,Drawable id,GC gc,int x,int y,unsigned int width,unsigned int height){x_window_t*w=window_for(d,id);if(!w||!gc)return-1;for(unsigned int yy=0;yy<height;yy++)for(unsigned int xx=0;xx<width;xx++)put_pixel(w,x+(int)xx,y+(int)yy,(uint32_t)gc->foreground);if(w->native)bk_gui_window_invalidate(w->native);return 0;}
int XDrawLine(Display*d,Drawable id,GC gc,int x0,int y0,int x1,int y1){x_window_t*w=window_for(d,id);int dx=x1>x0?x1-x0:x0-x1,sx=x0<x1?1:-1,dy=-(y1>y0?y1-y0:y0-y1),sy=y0<y1?1:-1,err=dx+dy;if(!w||!gc)return-1;for(;;){put_pixel(w,x0,y0,(uint32_t)gc->foreground);if(x0==x1&&y0==y1)break;int twice=2*err;if(twice>=dy){err+=dy;x0+=sx;}if(twice<=dx){err+=dx;y0+=sy;}}if(w->native)bk_gui_window_invalidate(w->native);return 0;}
int XDrawRectangle(Display*d,Drawable id,GC gc,int x,int y,unsigned int width,unsigned int height){XDrawLine(d,id,gc,x,y,x+(int)width,y);XDrawLine(d,id,gc,x+(int)width,y,x+(int)width,y+(int)height);XDrawLine(d,id,gc,x+(int)width,y+(int)height,x,y+(int)height);return XDrawLine(d,id,gc,x,y+(int)height,x,y);}
int XDrawString(Display*d,Drawable id,GC gc,int x,int y,const char*text,int length){x_window_t*w=window_for(d,id);bk_gui_surface_t*s;bk_gui_rect_t c,clip;char copy[256];if(!w||!w->native||!gc||!text||length<0)return-1;if(length>255)length=255;memcpy(copy,text,(size_t)length);copy[length]=0;if(!bk_gui_window_content_rect(w->native,&c)||!bk_gui_window_begin_immediate_paint(w->native,&s))return-1;clip=c;bk_gui_surface_draw_text_px(s,c.x+x,c.y+y-14,copy,(uint32_t)length,(uint32_t)gc->foreground,14U,false,false,true,clip);bk_gui_window_end_immediate_paint(w->native,s);return 0;}
int XGetWindowAttributes(Display*d,Window id,XWindowAttributes*a){x_window_t*w=window_for(d,id);if(!w||!a)return 0;memset(a,0,sizeof(*a));a->x=w->x;a->y=w->y;a->width=(int)w->width;a->height=(int)w->height;a->depth=32;a->your_event_mask=(long)w->event_mask;return 1;}
Atom XInternAtom(Display*d,const char*name,Bool only){unsigned long hash=5381U;(void)d;(void)only;if(!name)return None;while(*name)hash=((hash<<5)+hash)^(unsigned char)*name++;return hash?hash:1U;}Status XSetWMProtocols(Display*d,Window w,Atom*p,int count){(void)d;(void)w;(void)p;(void)count;return 1;}
int XLookupString(XKeyEvent*e,char*b,int bytes,KeySym*sym,void*compose){(void)compose;if(!e)return 0;if(sym)*sym=(KeySym)e->keycode;if(b&&bytes>0&&e->keycode<256U){b[0]=(char)e->keycode;return 1;}return 0;}
static int hex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return-1;}
Status XParseColor(Display*d,unsigned long map,const char*spec,XColor*c){unsigned int rgb=0;(void)d;(void)map;if(!spec||!c)return 0;if(spec[0]=='#'&&strlen(spec)==7U){for(int i=1;i<7;i++){int digit=hex(spec[i]);if(digit<0)return 0;rgb=(rgb<<4)|(unsigned)digit;}}else if(!strcmp(spec,"black"))rgb=0;else if(!strcmp(spec,"white"))rgb=0xFFFFFFU;else if(!strcmp(spec,"red"))rgb=0xFF0000U;else if(!strcmp(spec,"green"))rgb=0x00FF00U;else if(!strcmp(spec,"blue"))rgb=0x0000FFU;else return 0;c->pixel=rgb;c->red=(unsigned short)(((rgb>>16)&255U)*257U);c->green=(unsigned short)(((rgb>>8)&255U)*257U);c->blue=(unsigned short)((rgb&255U)*257U);return 1;}Status XAllocColor(Display*d,unsigned long map,XColor*c){(void)d;(void)map;if(!c)return 0;c->pixel=((unsigned long)(c->red>>8)<<16)|((unsigned long)(c->green>>8)<<8)|(c->blue>>8);return 1;}
