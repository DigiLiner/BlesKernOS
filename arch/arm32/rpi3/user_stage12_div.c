#include "../../../sdk/include/bleskernos_api.h"

uint32_t rpi3_user_udivmod32(uint32_t numerator, uint32_t denominator,
                             uint32_t *remainder) {
    uint32_t quotient = 0U;
    uint32_t rem = 0U;
    int bit;
    if (!denominator) {
        if (remainder) *remainder = numerator;
        return 0U;
    }
    for (bit = 31; bit >= 0; --bit) {
        uint32_t high = rem >> 31;
        rem = (rem << 1) | ((numerator >> (uint32_t)bit) & 1U);
        if (high || rem >= denominator) {
            rem -= denominator;
            quotient |= 1U << (uint32_t)bit;
        }
    }
    if (remainder) *remainder = rem;
    return quotient;
}

int32_t rpi3_user_sdivmod32(int32_t numerator, int32_t denominator,
                            int32_t *remainder) {
    bool neg_q, neg_r;
    uint32_t a, b, ur = 0U, uq;
    if (!denominator) {
        if (remainder) *remainder = numerator;
        return 0;
    }
    neg_q = (numerator < 0) != (denominator < 0);
    neg_r = numerator < 0;
    a = numerator < 0 ? (uint32_t)(-(numerator + 1)) + 1U : (uint32_t)numerator;
    b = denominator < 0 ? (uint32_t)(-(denominator + 1)) + 1U : (uint32_t)denominator;
    uq = rpi3_user_udivmod32(a, b, &ur);
    if (remainder) *remainder = neg_r ? -(int32_t)ur : (int32_t)ur;
    return neg_q ? -(int32_t)uq : (int32_t)uq;
}

void rpi3_user_udivmod64_words(uint32_t nlo, uint32_t nhi,
                               uint32_t dlo, uint32_t dhi,
                               uint32_t out[4]) {
    uint32_t qlo = 0U, qhi = 0U, rlo = 0U, rhi = 0U;
    int bit;
    if (!out) return;
    if (!(dlo | dhi)) {
        out[0] = out[1] = 0U; out[2] = nlo; out[3] = nhi; return;
    }
    for (bit = 63; bit >= 0; --bit) {
        uint32_t incoming = bit >= 32
            ? ((nhi >> (uint32_t)(bit - 32)) & 1U)
            : ((nlo >> (uint32_t)bit) & 1U);
        uint32_t carry = rlo >> 31;
        bool ge;
        rlo = (rlo << 1) | incoming; rhi = (rhi << 1) | carry;
        ge = rhi > dhi || (rhi == dhi && rlo >= dlo);
        if (ge) {
            uint32_t old = rlo; rlo -= dlo; rhi -= dhi + (old < dlo ? 1U : 0U);
            if (bit >= 32) qhi |= 1U << (uint32_t)(bit - 32);
            else qlo |= 1U << (uint32_t)bit;
        }
    }
    out[0] = qlo; out[1] = qhi; out[2] = rlo; out[3] = rhi;
}
