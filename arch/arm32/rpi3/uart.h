#ifndef BLES_ARCH_ARM32_RPI3_UART_H
#define BLES_ARCH_ARM32_RPI3_UART_H

#include "../../../kernel/include/types.h"

void rpi3_uart_init(void);
void rpi3_uart_putc(char c);
void rpi3_uart_puts(const char *s);
void rpi3_uart_puthex(unsigned int value);
bool rpi3_uart_try_getc(char *out);

#endif
