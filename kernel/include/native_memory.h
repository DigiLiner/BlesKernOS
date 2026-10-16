#ifndef NATIVE_MEMORY_H
#define NATIVE_MEMORY_H
#include "types.h"
/* Return true for a handled primitive, including rejection. */
bool native_memory_dispatch(uint32_t target,const uint32_t *arguments,uint64_t *result);
#endif
