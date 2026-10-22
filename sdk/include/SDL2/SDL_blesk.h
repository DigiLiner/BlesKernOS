#ifndef BLESKERNOS_SDL2_NATIVE_H
#define BLESKERNOS_SDL2_NATIVE_H
#include <SDL2/SDL.h>
#include <bleskernos_api.h>
/* Borrowed opaque native window, valid until SDL_DestroyWindow().
 * Lets native graphics libraries use native presentation APIs directly.
 * SDL keeps ownership and continues delivering keyboard/mouse events. */
bk_gui_window_t *SDL_BleskGetWindow(SDL_Window *window);
#endif
