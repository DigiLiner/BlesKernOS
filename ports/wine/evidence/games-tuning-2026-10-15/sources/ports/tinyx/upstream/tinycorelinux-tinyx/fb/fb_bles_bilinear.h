/* SPDX-License-Identifier: GPL-3.0-or-later
 * Interior affine ARGB/XRGB scanline; fbFetchTransformed checks the clip.
 * Keep the generic fetcher's precision, including alpha for ARGB pictures.
 */
/* A full scaled row normally touches a border at each end. Keep those
 * pixels on the general path instead of rejecting the entire fast row. */
static int
fbBlesBilinearSpan(int width, xFixed start, xFixed step, int left, int right,
                    int *skip, int *count)
{
    int first = 0, end = width;
    long long last;
    if (width <= 0 || right <= left) return 0;
    last = (long long)start + (long long)(width - 1) * step;
    if (last < (-2147483647LL - 1) || last > 2147483647LL) return 0;
    if (((start >> 16) < left && (last >> 16) < left) ||
        ((start >> 16) >= right && (last >> 16) >= right)) return 0;
    while (first < end) {
        int x = ((long long)start + (long long)first * step) >> 16;
        if (x >= left && x < right) break;
        first++;
    }
    while (end > first) {
        int x = ((long long)start + (long long)(end - 1) * step) >> 16;
        if (x >= left && x < right) break;
        end--;
    }
    if (first == end) return 0;
    *skip = first;
    *count = end - first;
    return 1;
}

static void
fbBlesBilinear32Scalar(const CARD32 *top, const CARD32 *bottom, CARD32 *out,
                 int width, xFixed start, xFixed step, int dx,
                 unsigned wy, int opaque)
{
    int i;
    unsigned iy = 256 - wy;
    for (i = 0; i < width; ++i) {
        int x = (start >> 16) + dx;
        unsigned wx = (start >> 8) & 255, ix = 256 - wx;
        CARD32 tl = top[x], tr = top[x + 1];
        CARD32 bl = bottom[x], br = bottom[x + 1], r;
        unsigned t, b;
#define BLES_CHANNEL(shift) \
        t = ((tl >> (shift)) & 255) * ix + ((tr >> (shift)) & 255) * wx; \
        b = ((bl >> (shift)) & 255) * ix + ((br >> (shift)) & 255) * wx; \
        r |= ((t * iy + b * wy) >> 16) << (shift)
        r = 0;
        BLES_CHANNEL(0);
        BLES_CHANNEL(8);
        BLES_CHANNEL(16);
        if (opaque)
            r |= 0xff000000;
        else {
            BLES_CHANNEL(24);
        }
#undef BLES_CHANNEL
        out[i] = r;
        /* The caller bounds the first and last fixed-point coordinates.
         * Do not step beyond the final pixel (which could overflow). */
        if (i + 1 < width)
            start += step;
    }
}

#if defined(__i386__) || defined(__x86_64__)
#include <emmintrin.h>
/* The horizontal sums are unsigned 16-bit (0..65280). Bias them into
 * signed range before PMADDWD, then restore the exact 24-bit vertical sum.
 * This keeps all eight fractional bits of both interpolation stages. */
__attribute__((target("sse2"))) static void
fbBlesBilinear32SSE2(const CARD32 *top, const CARD32 *bottom, CARD32 *out,
                     int width, xFixed start, xFixed step, int dx,
                     unsigned wy, int opaque)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i bias16 = _mm_set1_epi16((short)0x8000);
    const __m128i bias32 = _mm_set1_epi32(1 << 23);
    const __m128i vertical = _mm_set_epi16(wy,256-wy,wy,256-wy,wy,256-wy,wy,256-wy);
    for (int i=0;i<width;i++) {
        int x=(start >> 16)+dx;
        unsigned wx=(start >> 8)&255;
        __m128i horizontal=_mm_set_epi16(wx,wx,wx,wx,256-wx,256-wx,256-wx,256-wx);
        __m128i t=_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(top+x)),zero);
        __m128i b=_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(bottom+x)),zero);
        t=_mm_mullo_epi16(t,horizontal);b=_mm_mullo_epi16(b,horizontal);
        t=_mm_add_epi16(t,_mm_srli_si128(t,8));b=_mm_add_epi16(b,_mm_srli_si128(b,8));
        __m128i pairs=_mm_xor_si128(_mm_unpacklo_epi16(t,b),bias16);
        __m128i rgba=_mm_srli_epi32(_mm_add_epi32(_mm_madd_epi16(pairs,vertical),bias32),16);
        rgba=_mm_packs_epi32(rgba,zero);rgba=_mm_packus_epi16(rgba,zero);
        out[i]=(CARD32)_mm_cvtsi128_si32(rgba) | (opaque?0xff000000U:0);
        if(i+1<width)start+=step;
    }
}
#endif
static void
fbBlesBilinear32(const CARD32 *top, const CARD32 *bottom, CARD32 *out,
                 int width, xFixed start, xFixed step, int dx,
                 unsigned wy, int opaque)
{
#if defined(__i386__) || defined(__x86_64__)
    if (__builtin_cpu_supports("sse2")) {
        fbBlesBilinear32SSE2(top,bottom,out,width,start,step,dx,wy,opaque);
        return;
    }
#endif
    fbBlesBilinear32Scalar(top,bottom,out,width,start,step,dx,wy,opaque);
}
