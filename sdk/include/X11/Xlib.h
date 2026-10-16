#ifndef BLESKERNOS_X11_XLIB_H
#define BLESKERNOS_X11_XLIB_H
#include <X11/X.h>

typedef struct _XDisplay Display;
typedef struct _XGC *GC;
typedef unsigned long KeySym;
typedef struct { int type; unsigned long serial; Bool send_event; Display *display; Window window; } XAnyEvent;
typedef struct { int type; unsigned long serial; Bool send_event; Display*display;Window window,root,subwindow;Time time;int x,y,x_root,y_root;unsigned int state;unsigned int keycode;Bool same_screen; } XKeyEvent;
typedef XKeyEvent XButtonEvent;
typedef XKeyEvent XMotionEvent;
typedef struct { int type;unsigned long serial;Bool send_event;Display*display;Window window;int x,y,width,height,count; } XExposeEvent;
typedef struct { int type;unsigned long serial;Bool send_event;Display*display;Window event,window;int x,y,width,height,border_width;Window above;Bool override_redirect; } XConfigureEvent;
typedef union _XEvent { int type; XAnyEvent xany; XKeyEvent xkey; XButtonEvent xbutton; XMotionEvent xmotion; XExposeEvent xexpose; XConfigureEvent xconfigure; long pad[24]; } XEvent;
typedef struct { unsigned long foreground,background; int line_width; } XGCValues;
typedef struct { int x,y,width,height,border_width,depth; Window root; int class; long your_event_mask; } XWindowAttributes;
typedef struct { unsigned long pixel; unsigned short red,green,blue; char flags,pad; } XColor;

Display *XOpenDisplay(const char *name);
int XCloseDisplay(Display *display);
Window XDefaultRootWindow(Display *display);
int XDefaultScreen(Display *display);
unsigned long XBlackPixel(Display *display,int screen);
unsigned long XWhitePixel(Display *display,int screen);
Window XCreateSimpleWindow(Display *display,Window parent,int x,int y,unsigned int width,unsigned int height,unsigned int border_width,unsigned long border,unsigned long background);
int XDestroyWindow(Display *display,Window window);
int XMapWindow(Display *display,Window window);
int XMapRaised(Display *display,Window window);
int XUnmapWindow(Display *display,Window window);
int XStoreName(Display *display,Window window,const char *name);
int XSelectInput(Display *display,Window window,long event_mask);
int XPending(Display *display);
int XNextEvent(Display *display,XEvent *event);
int XPeekEvent(Display *display,XEvent *event);
int XFlush(Display *display);
int XSync(Display *display,Bool discard);
GC XCreateGC(Display *display,Drawable drawable,unsigned long mask,XGCValues *values);
int XFreeGC(Display *display,GC gc);
int XSetForeground(Display *display,GC gc,unsigned long color);
int XSetBackground(Display *display,GC gc,unsigned long color);
int XClearWindow(Display *display,Window window);
int XFillRectangle(Display *display,Drawable drawable,GC gc,int x,int y,unsigned int width,unsigned int height);
int XDrawRectangle(Display *display,Drawable drawable,GC gc,int x,int y,unsigned int width,unsigned int height);
int XDrawLine(Display *display,Drawable drawable,GC gc,int x1,int y1,int x2,int y2);
int XDrawString(Display *display,Drawable drawable,GC gc,int x,int y,const char *text,int length);
int XGetWindowAttributes(Display *display,Window window,XWindowAttributes *attributes);
Atom XInternAtom(Display *display,const char *name,Bool only_if_exists);
Status XSetWMProtocols(Display *display,Window window,Atom *protocols,int count);
int XLookupString(XKeyEvent *event,char *buffer,int bytes,KeySym *keysym,void *compose);
Status XParseColor(Display *display,unsigned long colormap,const char *spec,XColor *color);
Status XAllocColor(Display *display,unsigned long colormap,XColor *color);

#define DefaultRootWindow(d) XDefaultRootWindow(d)
#define DefaultScreen(d) XDefaultScreen(d)
#define BlackPixel(d,s) XBlackPixel((d),(s))
#define WhitePixel(d,s) XWhitePixel((d),(s))
#define DefaultColormap(d,s) 0UL
#endif
