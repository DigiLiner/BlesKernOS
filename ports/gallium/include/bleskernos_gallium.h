/* SPDX-License-Identifier: MIT */
#ifndef BLESKERNOS_GALLIUM_H
#define BLESKERNOS_GALLIUM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* User-space ELF32/POSIX ABI. This is not the freestanding .LBS ABI: Mesa
 * and its C++/TLS runtime are loaded by the OS's supported ELF interpreter. */
typedef struct bkg_context bkg_context;
bkg_context *bkg_create(unsigned width, unsigned height);
void bkg_destroy(bkg_context *);
int bkg_current(bkg_context *);
int bkg_present(bkg_context *);
int bkg_should_quit(bkg_context *);
/* Poll one native input event. 1=move, 2=button down, 3=button up,
 * 5=key down, 6=key up, 10=close. Keys are OS character codes. */
typedef struct {
    uint32_t type;
    int32_t x,y,dx,dy;
    uint8_t buttons,button,key,shift,ctrl,alt,reserved[2];
} bkg_event;
int bkg_poll_event(bkg_context *, void *event /* bkg_event * */);
void *bkg_get_proc(const char *name);
#ifdef __cplusplus
}
#endif
#endif
