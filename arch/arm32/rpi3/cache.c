#include "cache.h"

#define CACHE_LINE_SIZE 64U

static void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uintptr_t line_down(uintptr_t p) {
    return p & ~(uintptr_t)(CACHE_LINE_SIZE - 1U);
}

static uintptr_t line_end(const void *addr, uint32_t bytes) {
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end;
    if (bytes == 0U) return start;
    end = start + (uintptr_t)bytes;
    if (end < start) return 0xFFFFFFFFU;
    return (end + CACHE_LINE_SIZE - 1U) & ~(uintptr_t)(CACHE_LINE_SIZE - 1U);
}

void rpi3_cache_clean_range(const void *addr, uint32_t bytes) {
    uintptr_t p = line_down((uintptr_t)addr);
    uintptr_t end = line_end(addr, bytes);
    while (p < end) {
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"((uint32_t)p) : "memory");
        p += CACHE_LINE_SIZE;
    }
    dsb();
}

void rpi3_cache_invalidate_range(const void *addr, uint32_t bytes) {
    uintptr_t p = line_down((uintptr_t)addr);
    uintptr_t end = line_end(addr, bytes);
    while (p < end) {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"((uint32_t)p) : "memory");
        p += CACHE_LINE_SIZE;
    }
    dsb();
}

void rpi3_cache_clean_invalidate_range(const void *addr, uint32_t bytes) {
    uintptr_t p = line_down((uintptr_t)addr);
    uintptr_t end = line_end(addr, bytes);
    while (p < end) {
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"((uint32_t)p) : "memory");
        p += CACHE_LINE_SIZE;
    }
    dsb();
}

void rpi3_cache_sync_code(const void *addr, uint32_t bytes) {
    /* ARMv7 requires modified instructions to reach PoU before the I-cache is
       invalidated. The whole-I-cache operation is acceptable at Stage 11 and
       avoids relying on implementation-specific I-line geometry. */
    rpi3_cache_clean_range(addr, bytes);
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0U) : "memory");
    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
}
