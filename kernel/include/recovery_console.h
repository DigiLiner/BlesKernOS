#ifndef BLESKERNOS_RECOVERY_CONSOLE_H
#define BLESKERNOS_RECOVERY_CONSOLE_H

#include "types.h"

/* Conserva el modo VESA/LFB activo y dibuja una terminal sobre él.
 * Sólo restaura VGA texto cuando no existe ningún framebuffer válido. */
void recovery_console_enter(void);
bool recovery_console_graphics_active(void);
void recovery_console_clear(void);
void recovery_console_putchar(char character, uint8_t fg);
void recovery_console_set_cursor(int x, int y);
void recovery_console_get_cursor(int *x, int *y);

#endif
