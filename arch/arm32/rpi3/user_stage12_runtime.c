#include "../../../sdk/include/bleskernos_api.h"

void *memcpy(void *destination, const void *source, size_t bytes) {
    uint8_t *d = (uint8_t *)destination;
    const uint8_t *s = (const uint8_t *)source;
    while (bytes--) *d++ = *s++;
    return destination;
}

void *memmove(void *destination, const void *source, size_t bytes) {
    uint8_t *d = (uint8_t *)destination;
    const uint8_t *s = (const uint8_t *)source;
    if (d < s) while (bytes--) *d++ = *s++;
    else {
        d += bytes; s += bytes;
        while (bytes--) *--d = *--s;
    }
    return destination;
}

void *memset(void *destination, int value, size_t bytes) {
    uint8_t *d = (uint8_t *)destination;
    while (bytes--) *d++ = (uint8_t)value;
    return destination;
}

int memcmp(const void *left, const void *right, size_t bytes) {
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;
    while (bytes--) {
        if (*a != *b) return *a < *b ? -1 : 1;
        ++a; ++b;
    }
    return 0;
}

void __aeabi_memcpy(void *d, const void *s, size_t n) { (void)memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { (void)memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { (void)memcpy(d, s, n); }
void __aeabi_memmove(void *d, const void *s, size_t n) { (void)memmove(d, s, n); }
void __aeabi_memset(void *d, size_t n, int c) { (void)memset(d, c, n); }
void __aeabi_memset4(void *d, size_t n, int c) { (void)memset(d, c, n); }
void __aeabi_memset8(void *d, size_t n, int c) { (void)memset(d, c, n); }
void __aeabi_memclr(void *d, size_t n) { (void)memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { (void)memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { (void)memset(d, 0, n); }
