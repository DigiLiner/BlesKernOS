#ifndef BLESKERNOS_POSIX_STRINGS_H
#define BLESKERNOS_POSIX_STRINGS_H
#include <string.h>
#define bzero(pointer, size) memset((pointer), 0, (size))
#define bcopy(source, destination, size) memmove((destination), (source), (size))
#endif
