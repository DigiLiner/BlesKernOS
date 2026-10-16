#ifndef BLESKERNOS_X11_X_H
#define BLESKERNOS_X11_X_H
typedef unsigned long XID;
typedef XID Window;
typedef XID Drawable;
typedef XID Pixmap;
typedef XID Atom;
typedef unsigned long Time;
typedef int Bool;
typedef int Status;
#define True 1
#define False 0
#define None 0L
#define CurrentTime 0L
#define KeyPress 2
#define KeyRelease 3
#define ButtonPress 4
#define ButtonRelease 5
#define MotionNotify 6
#define FocusIn 9
#define FocusOut 10
#define Expose 12
#define DestroyNotify 17
#define ConfigureNotify 22
#define ClientMessage 33
#define NoEventMask 0L
#define KeyPressMask (1L<<0)
#define KeyReleaseMask (1L<<1)
#define ButtonPressMask (1L<<2)
#define ButtonReleaseMask (1L<<3)
#define PointerMotionMask (1L<<6)
#define ExposureMask (1L<<15)
#define StructureNotifyMask (1L<<17)
#define FocusChangeMask (1L<<21)
#define Button1 1
#define Button2 2
#define Button3 3
#define GCForeground (1L<<2)
#define GCBackground (1L<<3)
#endif
