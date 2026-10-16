#ifndef BLESKERNOS_X11_XUTIL_H
#define BLESKERNOS_X11_XUTIL_H
#include <X11/Xlib.h>
typedef struct { long flags; int x,y,width,height; } XSizeHints;
#define PPosition (1L<<2)
#define PSize (1L<<3)
static inline int XSetNormalHints(Display*d,Window w,XSizeHints*h){(void)d;(void)w;(void)h;return 1;}
#endif
