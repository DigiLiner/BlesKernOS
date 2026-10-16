#include "include/stdlib.h"
#include "include/stdint.h"

/* Minimal GCC runtime support for freestanding 32-bit builds.
 * Provides the helper symbols required by the Doom port when linking
 * without libgcc. */

typedef union { uint64_t value; struct { uint32_t low, high; } word; } u64_words_t;

uint64_t __ashldi3(uint64_t value, int shift) {
    u64_words_t in, out; in.value = value;
    if (shift <= 0) return value;
    if (shift >= 64) return 0;
    if (shift >= 32) { out.word.low=0; out.word.high=in.word.low<<(shift-32); }
    else { out.word.low=in.word.low<<shift; out.word.high=(in.word.high<<shift)|(in.word.low>>(32-shift)); }
    return out.value;
}

uint64_t __lshrdi3(uint64_t value, int shift) {
    u64_words_t in, out; in.value = value;
    if (shift <= 0) return value;
    if (shift >= 64) return 0;
    if (shift >= 32) { out.word.high=0; out.word.low=in.word.high>>(shift-32); }
    else { out.word.high=in.word.high>>shift; out.word.low=(in.word.low>>shift)|(in.word.high<<(32-shift)); }
    return out.value;
}

int64_t __ashrdi3(int64_t value, int shift) {
    u64_words_t in, out; in.value = (uint64_t)value;
    if (shift <= 0) return value;
    if (shift >= 64) return (int32_t)in.word.high < 0 ? -1 : 0;
    if (shift >= 32) { out.word.high=(int32_t)in.word.high<0?0xFFFFFFFFU:0U;out.word.low=(uint32_t)((int32_t)in.word.high>>(shift-32)); }
    else { out.word.high=(uint32_t)((int32_t)in.word.high>>shift);out.word.low=(in.word.low>>shift)|(in.word.high<<(32-shift)); }
    return (int64_t)out.value;
}

int64_t __fixdfdi(double value) {
    u64_words_t out; bool negative = value < 0.0;
    if (negative) value = -value;
    out.word.high = (uint32_t)(value / 4294967296.0);
    out.word.low = (uint32_t)(value - (double)out.word.high * 4294967296.0);
    if (negative) { out.word.low=~out.word.low+1U;out.word.high=~out.word.high+(out.word.low==0U); }
    return (int64_t)out.value;
}

uint64_t __udivdi3(uint64_t a, uint64_t b) {
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    int bits;

    if (b == 0) {
        return 0;
    }

    for (bits = 63; bits >= 0; --bits) {
        remainder = (remainder << 1) | ((a >> bits) & 1);
        if (remainder >= b) {
            remainder -= b;
            quotient |= (uint64_t)1 << bits;
        }
    }

    return quotient;
}

/* GCC combines quotient/remainder conversions (e.g. nanoseconds) into this
 * libgcc ABI helper on i386. Keep freestanding builds independent of libgcc. */
uint64_t __udivmoddi4(uint64_t a, uint64_t b, uint64_t *remainder) {
    uint64_t quotient = __udivdi3(a, b);
    if (remainder) *remainder = a - quotient * b;
    return quotient;
}

uint64_t __umoddi3(uint64_t a, uint64_t b) {
    uint64_t quotient = __udivdi3(a, b);
    return a - (quotient * b);
}

int64_t __divdi3(int64_t a, int64_t b) {
    int neg = 0;
    uint64_t ua;
    uint64_t ub;
    uint64_t result;

    if (a < 0) {
        neg ^= 1;
        ua = (uint64_t)(-a);
    } else {
        ua = (uint64_t)a;
    }

    if (b < 0) {
        neg ^= 1;
        ub = (uint64_t)(-b);
    } else {
        ub = (uint64_t)b;
    }

    result = __udivdi3(ua, ub);

    if (neg) {
        result = (uint64_t)(-(int64_t)result);
    }

    return (int64_t)result;
}

int64_t __moddi3(int64_t a, int64_t b) {
    int neg = 0;
    uint64_t ua;
    uint64_t ub;
    uint64_t result;

    if (a < 0) {
        neg ^= 1;
        ua = (uint64_t)(-a);
    } else {
        ua = (uint64_t)a;
    }

    if (b < 0) {
        neg ^= 1;
        ub = (uint64_t)(-b);
    } else {
        ub = (uint64_t)b;
    }

    result = __umoddi3(ua, ub);

    if (neg) {
        result = (uint64_t)(-(int64_t)result);
    }

    return (int64_t)result;
}

int64_t __divmoddi4(int64_t a, int64_t b, int64_t *remainder) {
    int64_t quotient = __divdi3(a, b);
    if (remainder) {
        *remainder = a - quotient * b;
    }
    return quotient;
}
