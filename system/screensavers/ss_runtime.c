/* Private, in-process memory primitives for the statically linked Mesa savers.
 * Mesa copies short vertex attributes and scanlines frequently. Resolving each
 * copy/clear through a native kernel thunk dominates the software renderer. */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *destination, const void *source, size_t size) {
    void *result=destination;
    uint8_t *dst=destination;
    const uint8_t *src=source;
#ifndef BLES_ARCH_ARM32_RPI3
    size_t words=size/4U;
    __asm__ volatile("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(words) : : "memory");
    size &= 3U;
#endif
    while(size--) *dst++=*src++;
    return result;
}

void *memset(void *destination, int value, size_t size) {
    void *result=destination;
    uint8_t *dst=destination;
#ifndef BLES_ARCH_ARM32_RPI3
    size_t words=size/4U;
    uint32_t color=(uint8_t)value * 0x01010101U;
    __asm__ volatile("cld; rep stosl" : "+D"(dst), "+c"(words) : "a"(color) : "memory");
    size &= 3U;
#endif
    while(size--) *dst++=(uint8_t)value;
    return result;
}

void *memmove(void *destination, const void *source, size_t size) {
    uint8_t *dst=destination;
    const uint8_t *src=source;
    if ((uintptr_t)dst <= (uintptr_t)src || (uintptr_t)dst-(uintptr_t)src >= size)
        return memcpy(destination,source,size);
    while(size){size--;dst[size]=src[size];}
    return destination;
}
