#include "bleskernos_api.h"
#include <SDL2/SDL_blesk.h>
#include <stdlib.h>
#include <string.h>

#define SDL_EVENT_CAPACITY 64U
struct SDL_Window {
    bk_gui_window_t *native;
    SDL_Surface surface;
    SDL_PixelFormat format;
    SDL_Event events[SDL_EVENT_CAPACITY];
    volatile Uint32 head, tail;
    int width, height, last_mouse_x, last_mouse_y;
    Uint32 mouse_buttons;
    SDL_Renderer *renderer;
    struct SDL_Window *next;
};
struct SDL_Renderer { SDL_Window *window; Uint32 color; Uint32 flags; };
struct SDL_Texture {
    SDL_Renderer *renderer;
    Uint32 format;
    int access, width, height, pitch, locked;
    Uint32 *pixels;
};
static SDL_Window *windows;
static SDL_Window *legacy_window;
static Uint8 keyboard_state[SDL_NUM_SCANCODES];
static char last_error[96];

static void error_set(const char *text){unsigned i=0;while(text&&text[i]&&i+1<sizeof(last_error)){last_error[i]=text[i];i++;}last_error[i]=0;}
static void event_push(SDL_Window*w,const SDL_Event*e){Uint32 next=(w->tail+1U)%SDL_EVENT_CAPACITY;if(next==w->head)return;w->events[w->tail]=*e;__sync_synchronize();w->tail=next;}
static int event_pop(SDL_Event*e){for(SDL_Window*w=windows;w;w=w->next)if(w->head!=w->tail){*e=w->events[w->head];__sync_synchronize();w->head=(w->head+1U)%SDL_EVENT_CAPACITY;return 1;}return 0;}
static SDL_Window *event_window(void){return windows;}
static void sdl_paint(bk_gui_window_t*n,bk_gui_surface_t*s,void*context){SDL_Window*w=(SDL_Window*)context;bk_gui_rect_t c;bk_gui_image_t image;(void)n;if(!w||!s||!bk_gui_window_content_rect(w->native,&c))return;image.pixels=(uint32_t*)w->surface.pixels;image.width=(uint16_t)w->width;image.height=(uint16_t)w->height;bk_gui_surface_fill_rect(s,c,0);bk_gui_surface_draw_image(s,(bk_gui_rect_t){c.x,c.y,w->width,w->height},c,&image);}
static bool sdl_event(bk_gui_window_t*n,const bk_gui_event_t*source,void*context){SDL_Window*w=(SDL_Window*)context;SDL_Event e;bk_gui_rect_t c;(void)n;if(!w||!source)return false;memset(&e,0,sizeof(e));(void)bk_gui_window_content_rect(w->native,&c);e.type=0;if(source->type==BK_GUI_EVENT_CLOSE){e.type=SDL_QUIT;e.window.event=SDL_WINDOWEVENT_CLOSE;}else if(source->type==BK_GUI_EVENT_RESIZE){e.type=SDL_WINDOWEVENT;e.window.event=SDL_WINDOWEVENT_RESIZED;e.window.data1=source->x;e.window.data2=source->y;}else if(source->type==BK_GUI_EVENT_KEY||source->type==BK_GUI_EVENT_KEY_UP){e.type=source->type==BK_GUI_EVENT_KEY?SDL_KEYDOWN:SDL_KEYUP;e.key.sym=(unsigned char)source->key;e.key.mod=(source->shift?1U:0U)|(source->ctrl?0x40U:0U)|(source->alt?0x100U:0U);keyboard_state[(unsigned char)source->key]=source->type==BK_GUI_EVENT_KEY;}else if(source->type==BK_GUI_EVENT_MOUSE_MOVE){e.type=SDL_MOUSEMOTION;e.motion.x=source->x-c.x;e.motion.y=source->y-c.y;e.motion.xrel=e.motion.x-w->last_mouse_x;e.motion.yrel=e.motion.y-w->last_mouse_y;w->last_mouse_x=e.motion.x;w->last_mouse_y=e.motion.y;}else if(source->type==BK_GUI_EVENT_MOUSE_DOWN||source->type==BK_GUI_EVENT_MOUSE_UP){e.type=source->type==BK_GUI_EVENT_MOUSE_DOWN?SDL_MOUSEBUTTONDOWN:SDL_MOUSEBUTTONUP;e.button.button=source->button;e.button.x=source->x-c.x;e.button.y=source->y-c.y;if(source->button>0&&source->button<32){if(source->type==BK_GUI_EVENT_MOUSE_DOWN)w->mouse_buttons|=SDL_BUTTON(source->button);else w->mouse_buttons&=~SDL_BUTTON(source->button);}}else if(source->type==BK_GUI_EVENT_MOUSE_WHEEL){e.type=SDL_MOUSEWHEEL;e.wheel.y=source->dy;}if(e.type){e.window.timestamp=bk_sys_uptime_ms();event_push(w,&e);return true;}return false;}

int SDL_Init(Uint32 flags){(void)flags;last_error[0]=0;return 0;}
void SDL_Quit(void){while(windows)SDL_DestroyWindow(windows);}
const char*SDL_GetError(void){return last_error;}
SDL_Window*SDL_CreateWindow(const char*t,int x,int y,int width,int height,Uint32 flags){SDL_Window*w;bk_gui_desktop_t*d=bk_gui_desktop();if(!d||width<1||height<1||width>1600||height>1200){error_set("invalid window or video unavailable");return 0;}w=(SDL_Window*)calloc(1,sizeof(*w));if(!w)return 0;w->surface.pixels=calloc((size_t)width*(size_t)height,4U);if(!w->surface.pixels){free(w);return 0;}w->width=width;w->height=height;w->format.format=SDL_PIXELFORMAT_ARGB8888;w->format.BitsPerPixel=32;w->format.BytesPerPixel=4;w->format.Rmask=0x00FF0000U;w->format.Gmask=0x0000FF00U;w->format.Bmask=0x000000FFU;w->format.Amask=0xFF000000U;w->surface.format=&w->format;w->surface.w=width;w->surface.h=height;w->surface.pitch=width*4;w->surface.window=w;if(x==SDL_WINDOWPOS_UNDEFINED)x=80;if(y==SDL_WINDOWPOS_UNDEFINED)y=60;w->native=bk_gui_create_window(d,x,y,width+16,height+40,t?t:"SDL application");if(!w->native){free(w->surface.pixels);free(w);error_set("window creation failed");return 0;}bk_gui_window_set_owner(w->native,bk_sys_getpid());bk_gui_window_set_extended_events(w->native,true);if(!(flags&SDL_WINDOW_RESIZABLE))bk_gui_set_window_min_size(w->native,width+16,height+40);bk_gui_set_window_content(w->native,sdl_paint,w);bk_gui_set_window_event_handler(w->native,sdl_event,w);bk_proc_bind_window(w->native);w->next=windows;windows=w;bk_gui_window_invalidate(w->native);return w;}
void SDL_DestroyWindow(SDL_Window*w){SDL_Window**p=&windows;if(!w)return;if(legacy_window==w)legacy_window=0;while(*p&&*p!=w)p=&(*p)->next;if(*p)*p=w->next;if(w->native)bk_gui_destroy_window(bk_gui_desktop(),w->native);free(w->surface.pixels);free(w);}
SDL_Surface*SDL_GetWindowSurface(SDL_Window*w){return w?&w->surface:0;}
int SDL_UpdateWindowSurface(SDL_Window*w){if(!w||!w->native){error_set("invalid window");return-1;}bk_gui_window_invalidate(w->native);return 0;}
int SDL_UpdateWindowSurfaceRects(SDL_Window*w,const SDL_Rect*r,int n){(void)r;(void)n;return SDL_UpdateWindowSurface(w);}
int SDL_PollEvent(SDL_Event*e){if(!e)return 0;return event_pop(e);}
int SDL_WaitEventTimeout(SDL_Event*e,int timeout){Uint32 start=SDL_GetTicks();while(!event_pop(e)){if(timeout>=0&&(Uint32)(SDL_GetTicks()-start)>=(Uint32)timeout)return 0;SDL_Delay(1);}return 1;}
int SDL_WaitEvent(SDL_Event*e){return SDL_WaitEventTimeout(e,-1);}
void SDL_PumpEvents(void){bk_sys_yield();}
Uint32 SDL_GetTicks(void){return bk_sys_uptime_ms();}
Uint64 SDL_GetPerformanceCounter(void){return(Uint64)bk_sys_uptime_ms();}
Uint64 SDL_GetPerformanceFrequency(void){return 1000U;}
void SDL_Delay(Uint32 ms){bk_sys_sleep_ms(ms);}
const Uint8 *SDL_GetKeyboardState(int*count){if(count)*count=SDL_NUM_SCANCODES;return keyboard_state;}
Uint32 SDL_GetMouseState(int*x,int*y){SDL_Window*w=windows;if(x)*x=w?w->last_mouse_x:0;if(y)*y=w?w->last_mouse_y:0;return w?w->mouse_buttons:0;}
Uint32 SDL_MapRGB(const SDL_PixelFormat*f,Uint8 r,Uint8 g,Uint8 b){(void)f;return((Uint32)r<<16)|((Uint32)g<<8)|b;}
Uint32 SDL_MapRGBA(const SDL_PixelFormat*f,Uint8 r,Uint8 g,Uint8 b,Uint8 a){return SDL_MapRGB(f,r,g,b)|((Uint32)a<<24);}
int SDL_FillRect(SDL_Surface*s,const SDL_Rect*r,Uint32 color){SDL_Rect q;if(!s||!s->pixels)return-1;q=r?*r:(SDL_Rect){0,0,s->w,s->h};if(q.x<0){q.w+=q.x;q.x=0;}if(q.y<0){q.h+=q.y;q.y=0;}if(q.x+q.w>s->w)q.w=s->w-q.x;if(q.y+q.h>s->h)q.h=s->h-q.y;for(int y=0;y<q.h;y++){Uint32*p=(Uint32*)((Uint8*)s->pixels+(q.y+y)*s->pitch)+q.x;for(int x=0;x<q.w;x++)p[x]=color;}return 0;}
int SDL_BlitSurface(SDL_Surface*src,const SDL_Rect*sr,SDL_Surface*dst,SDL_Rect*dr){SDL_Rect a=sr?*sr:(SDL_Rect){0,0,src?src->w:0,src?src->h:0};int dx=dr?dr->x:0,dy=dr?dr->y:0;if(!src||!dst||!src->pixels||!dst->pixels)return-1;if(a.x<0||a.y<0||dx<0||dy<0)return-1;if(a.x+a.w>src->w)a.w=src->w-a.x;if(a.y+a.h>src->h)a.h=src->h-a.y;if(dx+a.w>dst->w)a.w=dst->w-dx;if(dy+a.h>dst->h)a.h=dst->h-dy;for(int y=0;y<a.h;y++)memcpy((Uint8*)dst->pixels+(dy+y)*dst->pitch+dx*4,(Uint8*)src->pixels+(a.y+y)*src->pitch+a.x*4,a.w*4);if(dr){dr->w=a.w;dr->h=a.h;}return 0;}
int SDL_LockSurface(SDL_Surface*s){if(!s)return-1;s->locked++;return 0;}void SDL_UnlockSurface(SDL_Surface*s){if(s&&s->locked)s->locked--;}

SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags,int width,int height,
                                            int depth,Uint32 format) {
    SDL_Surface *surface;
    SDL_PixelFormat *pixel_format;
    (void)flags;
    if (width <= 0 || height <= 0 || depth != 32 ||
        format != SDL_PIXELFORMAT_ARGB8888) {
        error_set("only ARGB8888 surfaces are supported");
        return 0;
    }
    surface = (SDL_Surface *)calloc(1, sizeof(*surface));
    pixel_format = (SDL_PixelFormat *)calloc(1, sizeof(*pixel_format));
    if (!surface || !pixel_format) { free(surface); free(pixel_format); return 0; }
    surface->pixels = calloc((size_t)width * (size_t)height, 4U);
    if (!surface->pixels) { free(pixel_format); free(surface); return 0; }
    pixel_format->format=format; pixel_format->BitsPerPixel=32;
    pixel_format->BytesPerPixel=4; pixel_format->Rmask=0x00FF0000U;
    pixel_format->Gmask=0x0000FF00U; pixel_format->Bmask=0x000000FFU;
    pixel_format->Amask=0xFF000000U;
    surface->format=pixel_format; surface->w=width; surface->h=height;
    surface->pitch=width*4; surface->userdata=pixel_format;
    return surface;
}

void SDL_FreeSurface(SDL_Surface *surface) {
    if (!surface || surface->window) return;
    free(surface->pixels); free(surface->userdata); free(surface);
}

int SDL_BlitScaled(SDL_Surface *source,const SDL_Rect *source_rect,
                   SDL_Surface *destination,SDL_Rect *destination_rect) {
    SDL_Rect s, d;
    if (!source || !destination || !source->pixels || !destination->pixels)
        return -1;
    s=source_rect?*source_rect:(SDL_Rect){0,0,source->w,source->h};
    d=destination_rect?*destination_rect:(SDL_Rect){0,0,destination->w,destination->h};
    if (s.x<0||s.y<0||s.w<=0||s.h<=0||d.x<0||d.y<0||d.w<=0||d.h<=0)
        return -1;
    if(s.x+s.w>source->w)s.w=source->w-s.x;
    if(s.y+s.h>source->h)s.h=source->h-s.y;
    if(d.x+d.w>destination->w)d.w=destination->w-d.x;
    if(d.y+d.h>destination->h)d.h=destination->h-d.y;
    for(int y=0;y<d.h;y++){
        Uint32 *out=(Uint32*)((Uint8*)destination->pixels+(d.y+y)*destination->pitch)+d.x;
        Uint32 *in=(Uint32*)((Uint8*)source->pixels+(s.y+(y*s.h)/d.h)*source->pitch)+s.x;
        for(int x=0;x<d.w;x++)out[x]=in[(x*s.w)/d.w];
    }
    if(destination_rect)*destination_rect=d;
    return 0;
}

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window,int index,Uint32 flags) {
    SDL_Renderer *renderer;
    (void)index;
    if(!window){error_set("invalid renderer window");return 0;}
    renderer=(SDL_Renderer*)calloc(1,sizeof(*renderer));
    if(!renderer)return 0;
    renderer->window=window; renderer->color=0xFF000000U; window->renderer=renderer;
    renderer->flags=(flags&SDL_RENDERER_PRESENTVSYNC)|SDL_RENDERER_SOFTWARE;
    return renderer;
}
void SDL_DestroyRenderer(SDL_Renderer *renderer){if(renderer){if(renderer->window&&renderer->window->renderer==renderer)renderer->window->renderer=0;free(renderer);}}
SDL_Renderer *SDL_GetRenderer(SDL_Window*w){return w?w->renderer:0;}
int SDL_GetRendererOutputSize(SDL_Renderer*r,int*w,int*h){if(!r)return-1;if(w)*w=r->window->width;if(h)*h=r->window->height;return 0;}
int SDL_SetRenderDrawColor(SDL_Renderer*r,Uint8 red,Uint8 green,Uint8 blue,Uint8 alpha){if(!r)return-1;r->color=SDL_MapRGBA(0,red,green,blue,alpha);return 0;}
int SDL_RenderClear(SDL_Renderer*r){return r?SDL_FillRect(&r->window->surface,0,r->color):-1;}
int SDL_RenderDrawPoint(SDL_Renderer*r,int x,int y){SDL_Surface*s;if(!r)return-1;s=&r->window->surface;if(x<0||y<0||x>=s->w||y>=s->h)return 0;*(Uint32*)((Uint8*)s->pixels+y*s->pitch+x*4)=r->color;return 0;}
int SDL_RenderDrawLine(SDL_Renderer*r,int x0,int y0,int x1,int y1){int dx=x1>x0?x1-x0:x0-x1,sx=x0<x1?1:-1,dy=-(y1>y0?y1-y0:y0-y1),sy=y0<y1?1:-1,e=dx+dy;for(;;){SDL_RenderDrawPoint(r,x0,y0);if(x0==x1&&y0==y1)break;int e2=2*e;if(e2>=dy){e+=dy;x0+=sx;}if(e2<=dx){e+=dx;y0+=sy;}}return r?0:-1;}
int SDL_RenderFillRect(SDL_Renderer*r,const SDL_Rect*rect){return r?SDL_FillRect(&r->window->surface,rect,r->color):-1;}
int SDL_RenderDrawRect(SDL_Renderer*r,const SDL_Rect*q){SDL_Rect a;if(!r)return-1;a=q?*q:(SDL_Rect){0,0,r->window->width,r->window->height};if(a.w<=0||a.h<=0)return 0;SDL_RenderDrawLine(r,a.x,a.y,a.x+a.w-1,a.y);SDL_RenderDrawLine(r,a.x,a.y+a.h-1,a.x+a.w-1,a.y+a.h-1);SDL_RenderDrawLine(r,a.x,a.y,a.x,a.y+a.h-1);return SDL_RenderDrawLine(r,a.x+a.w-1,a.y,a.x+a.w-1,a.y+a.h-1);}

SDL_Texture *SDL_CreateTexture(SDL_Renderer*r,Uint32 format,int access,int width,int height){SDL_Texture*t;if(!r||width<=0||height<=0||format!=SDL_PIXELFORMAT_ARGB8888)return 0;t=(SDL_Texture*)calloc(1,sizeof(*t));if(!t)return 0;t->pixels=(Uint32*)calloc((size_t)width*(size_t)height,4U);if(!t->pixels){free(t);return 0;}t->renderer=r;t->format=format;t->access=access;t->width=width;t->height=height;t->pitch=width*4;return t;}
SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer*r,SDL_Surface*s){SDL_Texture*t;if(!r||!s)return 0;t=SDL_CreateTexture(r,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STATIC,s->w,s->h);if(t)for(int y=0;y<s->h;y++)memcpy((Uint8*)t->pixels+y*t->pitch,(Uint8*)s->pixels+y*s->pitch,(size_t)s->w*4U);return t;}
void SDL_DestroyTexture(SDL_Texture*t){if(t){free(t->pixels);free(t);}}
int SDL_QueryTexture(SDL_Texture*t,Uint32*f,int*a,int*w,int*h){if(!t)return-1;if(f)*f=t->format;if(a)*a=t->access;if(w)*w=t->width;if(h)*h=t->height;return 0;}
int SDL_UpdateTexture(SDL_Texture*t,const SDL_Rect*q,const void*p,int pitch){SDL_Rect r;if(!t||!p||pitch<=0)return-1;r=q?*q:(SDL_Rect){0,0,t->width,t->height};if(r.x<0||r.y<0||r.w<0||r.h<0||r.x+r.w>t->width||r.y+r.h>t->height)return-1;for(int y=0;y<r.h;y++)memcpy((Uint8*)t->pixels+(r.y+y)*t->pitch+r.x*4,(const Uint8*)p+y*pitch,(size_t)r.w*4U);return 0;}
int SDL_LockTexture(SDL_Texture*t,const SDL_Rect*q,void**p,int*pitch){SDL_Rect r;if(!t||!p||!pitch||t->locked)return-1;r=q?*q:(SDL_Rect){0,0,t->width,t->height};if(r.x<0||r.y<0||r.x+r.w>t->width||r.y+r.h>t->height)return-1;t->locked=1;*p=(Uint8*)t->pixels+r.y*t->pitch+r.x*4;*pitch=t->pitch;return 0;}
void SDL_UnlockTexture(SDL_Texture*t){if(t)t->locked=0;}
int SDL_RenderCopy(SDL_Renderer*r,SDL_Texture*t,const SDL_Rect*s,const SDL_Rect*d){SDL_Surface source;SDL_Rect destination;if(!r||!t)return-1;memset(&source,0,sizeof(source));source.pixels=t->pixels;source.w=t->width;source.h=t->height;source.pitch=t->pitch;destination=d?*d:(SDL_Rect){0,0,r->window->width,r->window->height};return SDL_BlitScaled(&source,s,&r->window->surface,&destination);}
void SDL_RenderPresent(SDL_Renderer*r){if(r)SDL_UpdateWindowSurface(r->window);}
int SDL_SetTextureBlendMode(SDL_Texture*t,SDL_BlendMode mode){(void)mode;return t?0:-1;}
int SDL_SetRenderDrawBlendMode(SDL_Renderer*r,SDL_BlendMode mode){(void)mode;return r?0:-1;}

int SDL_CreateWindowAndRenderer(int width,int height,Uint32 flags,SDL_Window**w,SDL_Renderer**r){if(!w||!r)return-1;*w=SDL_CreateWindow("SDL application",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,width,height,flags);if(!*w)return-1;*r=SDL_CreateRenderer(*w,-1,SDL_RENDERER_SOFTWARE);if(!*r){SDL_DestroyWindow(*w);*w=0;return-1;}return 0;}
void SDL_GetWindowSize(SDL_Window*w,int*width,int*height){if(width)*width=w?w->width:0;if(height)*height=w?w->height:0;}
void SDL_SetWindowTitle(SDL_Window*w,const char*t){(void)w;(void)t;}
void SDL_ShowWindow(SDL_Window*w){if(w)bk_gui_window_invalidate(w->native);}void SDL_HideWindow(SDL_Window*w){(void)w;}void SDL_RaiseWindow(SDL_Window*w){if(w)bk_gui_window_invalidate(w->native);}

int SDL_PushEvent(SDL_Event *event){SDL_Window*w=event_window();if(!event||!w)return-1;event_push(w,event);return 1;}
void SDL_FlushEvents(Uint32 minimum,Uint32 maximum){SDL_Event kept[SDL_EVENT_CAPACITY];Uint32 count=0;SDL_Event e;while(event_pop(&e))if((e.type<minimum||e.type>maximum)&&count<SDL_EVENT_CAPACITY)kept[count++]=e;for(Uint32 i=0;i<count;i++)SDL_PushEvent(&kept[i]);}
int SDL_HasEvent(Uint32 type){for(SDL_Window*w=windows;w;w=w->next){Uint32 i=w->head;while(i!=w->tail){if(w->events[i].type==type)return 1;i=(i+1U)%SDL_EVENT_CAPACITY;}}return 0;}
SDL_Surface *SDL_SetVideoMode(int width,int height,int bits,Uint32 flags){if(bits&&bits!=32){error_set("SDL 1.2 mode requires 32 bpp");return 0;}if(legacy_window)SDL_DestroyWindow(legacy_window);legacy_window=SDL_CreateWindow("SDL application",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,width,height,flags&SDL_FULLSCREEN?SDL_WINDOW_SHOWN:SDL_WINDOW_SHOWN);return legacy_window?SDL_GetWindowSurface(legacy_window):0;}
SDL_Surface *SDL_GetVideoSurface(void){return legacy_window?SDL_GetWindowSurface(legacy_window):0;}
int SDL_VideoModeOK(int width,int height,int bits,Uint32 flags){(void)flags;return width>0&&height>0&&(bits==0||bits==32)?32:0;}
int SDL_Flip(SDL_Surface*s){return s&&s->window?SDL_UpdateWindowSurface(s->window):-1;}
void SDL_WM_SetCaption(const char*t,const char*i){(void)i;if(legacy_window)SDL_SetWindowTitle(legacy_window,t);}
int SDL_EnableKeyRepeat(int delay,int interval){(void)delay;(void)interval;return 0;}
void SDL_WarpMouse(Uint16 x,Uint16 y){if(legacy_window){legacy_window->last_mouse_x=x;legacy_window->last_mouse_y=y;}}

/* Native presentation interoperability; ownership stays with SDL. */
bk_gui_window_t *SDL_BleskGetWindow(SDL_Window *window) {
    return window ? window->native : NULL;
}
