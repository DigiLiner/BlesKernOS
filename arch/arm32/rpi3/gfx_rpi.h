#ifndef BLES_ARCH_ARM32_RPI3_GFX_RPI_H
#define BLES_ARCH_ARM32_RPI3_GFX_RPI_H

#include "../../../kernel/include/types.h"

bool rpi3_gfx_register(void);

/* Convert between BlesKernOS canonical 0x00RRGGBB and the native
 * 32-bit word layout selected by the VideoCore framebuffer.  Early/direct
 * gfx.c primitives use these too, so the bootsplash and the compositor
 * share exactly the same colour path. */
uint32_t rpi3_gfx_encode_rgb(uint32_t rgb);
uint32_t rpi3_gfx_decode_rgb(uint32_t native);

#endif
