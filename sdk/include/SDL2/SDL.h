#ifndef BLESKERNOS_SDL2_H
#define BLESKERNOS_SDL2_H

#include <stdint.h>

typedef uint8_t Uint8; typedef uint16_t Uint16; typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef int64_t Sint64; typedef uint64_t Uint64;
typedef enum { SDL_FALSE=0, SDL_TRUE=1 } SDL_bool;
typedef enum { SDL_BLENDMODE_NONE=0, SDL_BLENDMODE_BLEND=1,
               SDL_BLENDMODE_ADD=2, SDL_BLENDMODE_MOD=4 } SDL_BlendMode;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef Uint32 SDL_AudioDeviceID;
typedef Uint16 SDL_AudioFormat;
typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int length);
typedef struct SDL_AudioSpec {
    int freq; SDL_AudioFormat format; Uint8 channels, silence;
    Uint16 samples, padding; Uint32 size;
    SDL_AudioCallback callback; void *userdata;
} SDL_AudioSpec;
typedef struct SDL_PixelFormat {
    Uint32 format; Uint8 BitsPerPixel, BytesPerPixel; Uint16 padding;
    Uint32 Rmask, Gmask, Bmask, Amask;
} SDL_PixelFormat;
typedef struct SDL_Surface {
    Uint32 flags; SDL_PixelFormat *format; int w,h,pitch; void *pixels;
    void *userdata; int locked; SDL_Window *window;
} SDL_Surface;
typedef struct SDL_Rect { int x,y,w,h; } SDL_Rect;

#define SDL_INIT_TIMER 0x1U
#define SDL_INIT_AUDIO 0x10U
#define SDL_INIT_VIDEO 0x20U
#define SDL_INIT_EVENTS 0x4000U
#define SDL_WINDOWPOS_UNDEFINED 0
#define SDL_WINDOW_SHOWN 0x4U
#define SDL_WINDOW_RESIZABLE 0x20U
#define SDL_RENDERER_SOFTWARE 0x1U
#define SDL_RENDERER_ACCELERATED 0x2U
#define SDL_RENDERER_PRESENTVSYNC 0x4U
#define SDL_TEXTUREACCESS_STATIC 0
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_TEXTUREACCESS_TARGET 2
#define AUDIO_U8 0x0008U
#define AUDIO_S16LSB 0x8010U
#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x1U
#define SDL_AUDIO_ALLOW_FORMAT_CHANGE 0x2U
#define SDL_AUDIO_ALLOW_CHANNELS_CHANGE 0x4U
#define SDL_PIXELFORMAT_ARGB8888 0x16362004U
#define SDL_QUIT 0x100U
#define SDL_WINDOWEVENT 0x200U
#define SDL_KEYDOWN 0x300U
#define SDL_KEYUP 0x301U
#define SDL_MOUSEMOTION 0x400U
#define SDL_MOUSEBUTTONDOWN 0x401U
#define SDL_MOUSEBUTTONUP 0x402U
#define SDL_MOUSEWHEEL 0x403U
#define SDL_WINDOWEVENT_RESIZED 5
#define SDL_WINDOWEVENT_CLOSE 14
#define SDL_SWSURFACE 0U
#define SDL_HWSURFACE 0x1U
#define SDL_DOUBLEBUF 0x40000000U
#define SDL_FULLSCREEN 0x80000000U
#define SDL_NUM_SCANCODES 512
#define SDL_BUTTON(X) (1U << ((X)-1))

typedef struct { Uint32 type,timestamp,windowID; Uint8 event,padding1,padding2,padding3; Sint32 data1,data2; } SDL_WindowEvent;
typedef struct { Uint32 type,timestamp,windowID; Uint8 state,repeat,padding2,padding3; Sint32 sym; Uint16 mod; } SDL_KeyboardEvent;
typedef struct { Uint32 type,timestamp,windowID,which,state; Sint32 x,y,xrel,yrel; } SDL_MouseMotionEvent;
typedef struct { Uint32 type,timestamp,windowID,which; Uint8 button,state,clicks,padding1; Sint32 x,y; } SDL_MouseButtonEvent;
typedef struct { Uint32 type,timestamp,windowID,which; Sint32 x,y; Uint32 direction; } SDL_MouseWheelEvent;
typedef union SDL_Event {
    Uint32 type; SDL_WindowEvent window; SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion; SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel; Uint8 padding[56];
} SDL_Event;

int SDL_Init(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
SDL_Window *SDL_CreateWindow(const char *title,int x,int y,int w,int h,Uint32 flags);
int SDL_CreateWindowAndRenderer(int width,int height,Uint32 flags,
                                SDL_Window **window,SDL_Renderer **renderer);
void SDL_DestroyWindow(SDL_Window *window);
void SDL_GetWindowSize(SDL_Window *window,int *width,int *height);
void SDL_SetWindowTitle(SDL_Window *window,const char *title);
void SDL_ShowWindow(SDL_Window *window);
void SDL_HideWindow(SDL_Window *window);
void SDL_RaiseWindow(SDL_Window *window);
SDL_Surface *SDL_GetWindowSurface(SDL_Window *window);
int SDL_UpdateWindowSurface(SDL_Window *window);
int SDL_UpdateWindowSurfaceRects(SDL_Window *window,const SDL_Rect *rects,int count);
int SDL_PollEvent(SDL_Event *event);
int SDL_WaitEvent(SDL_Event *event);
int SDL_WaitEventTimeout(SDL_Event *event,int timeout_ms);
void SDL_PumpEvents(void);
Uint32 SDL_GetTicks(void);
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);
void SDL_Delay(Uint32 milliseconds);
const Uint8 *SDL_GetKeyboardState(int *count);
Uint32 SDL_GetMouseState(int *x,int *y);
Uint32 SDL_MapRGB(const SDL_PixelFormat *format,Uint8 r,Uint8 g,Uint8 b);
Uint32 SDL_MapRGBA(const SDL_PixelFormat *format,Uint8 r,Uint8 g,Uint8 b,Uint8 a);
int SDL_FillRect(SDL_Surface *surface,const SDL_Rect *rect,Uint32 color);
int SDL_BlitSurface(SDL_Surface *source,const SDL_Rect *source_rect,
                    SDL_Surface *destination,SDL_Rect *destination_rect);
int SDL_BlitScaled(SDL_Surface *source,const SDL_Rect *source_rect,
                   SDL_Surface *destination,SDL_Rect *destination_rect);
int SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);
SDL_Surface *SDL_LoadBMP(const char *path);
SDL_Surface *SDL_SetVideoMode(int width,int height,int bits,Uint32 flags);
SDL_Surface *SDL_GetVideoSurface(void);
int SDL_VideoModeOK(int width,int height,int bits,Uint32 flags);
int SDL_Flip(SDL_Surface *surface);
void SDL_WM_SetCaption(const char *title,const char *icon);
int SDL_EnableKeyRepeat(int delay,int interval);
void SDL_WarpMouse(Uint16 x,Uint16 y);
SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags,int width,int height,
                                            int depth,Uint32 format);
void SDL_FreeSurface(SDL_Surface *surface);

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window,int index,Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *renderer);
SDL_Renderer *SDL_GetRenderer(SDL_Window *window);
int SDL_GetRendererOutputSize(SDL_Renderer *renderer,int *width,int *height);
int SDL_SetRenderDrawColor(SDL_Renderer *renderer,Uint8 r,Uint8 g,Uint8 b,Uint8 a);
int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_RenderDrawPoint(SDL_Renderer *renderer,int x,int y);
int SDL_RenderDrawLine(SDL_Renderer *renderer,int x1,int y1,int x2,int y2);
int SDL_RenderDrawRect(SDL_Renderer *renderer,const SDL_Rect *rect);
int SDL_RenderFillRect(SDL_Renderer *renderer,const SDL_Rect *rect);
SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer,Uint32 format,int access,
                               int width,int height);
SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer,
                                          SDL_Surface *surface);
void SDL_DestroyTexture(SDL_Texture *texture);
int SDL_QueryTexture(SDL_Texture *texture,Uint32 *format,int *access,int *width,
                     int *height);
int SDL_UpdateTexture(SDL_Texture *texture,const SDL_Rect *rect,
                      const void *pixels,int pitch);
int SDL_LockTexture(SDL_Texture *texture,const SDL_Rect *rect,void **pixels,
                    int *pitch);
void SDL_UnlockTexture(SDL_Texture *texture);
int SDL_RenderCopy(SDL_Renderer *renderer,SDL_Texture *texture,
                   const SDL_Rect *source,const SDL_Rect *destination);
void SDL_RenderPresent(SDL_Renderer *renderer);
int SDL_SetTextureBlendMode(SDL_Texture *texture,SDL_BlendMode mode);
int SDL_SetRenderDrawBlendMode(SDL_Renderer *renderer,SDL_BlendMode mode);

int SDL_PushEvent(SDL_Event *event);
void SDL_FlushEvents(Uint32 minimum,Uint32 maximum);
int SDL_HasEvent(Uint32 type);
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device,int capture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained,int changes);
void SDL_CloseAudioDevice(SDL_AudioDeviceID device);
void SDL_PauseAudioDevice(SDL_AudioDeviceID device,int paused);
int SDL_QueueAudio(SDL_AudioDeviceID device,const void *data,Uint32 length);
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID device);
void SDL_ClearQueuedAudio(SDL_AudioDeviceID device);
int SDL_OpenAudio(SDL_AudioSpec *desired,SDL_AudioSpec *obtained);
void SDL_CloseAudio(void);
void SDL_PauseAudio(int paused);

#define SDL_MUSTLOCK(surface) 0

#endif
