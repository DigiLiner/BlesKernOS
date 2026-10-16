#ifndef BOOTSPLASH_H
#define BOOTSPLASH_H

#include "types.h"

void bootsplash_show(const char *status, uint8_t progress);
void bootsplash_pulse(void);
void bootsplash_disable(void);
bool bootsplash_active(void);
void bootsplash_debug(const char *message);

#endif
