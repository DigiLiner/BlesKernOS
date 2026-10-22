#include <bleskernos_archive_lbs.h>
int bk_archive_info(const void *data, uint32_t size, uint32_t *unpacked_size) {
    const uint8_t *p = data;
    if (!p || !unpacked_size || size < 8 || p[0]!='B' || p[1]!='K' || p[2]!='Z' || p[3]!='1') return BK_LBS_INVALID;
    uint32_t n = p[4] | ((uint32_t)p[5]<<8) | ((uint32_t)p[6]<<16) | ((uint32_t)p[7]<<24);
    if (n > BK_ARCHIVE_MAX_BYTES) return BK_LBS_INVALID;
    *unpacked_size = n;
    return 0;
}
int bk_archive_pack(const void *data, uint32_t size, void **output, uint32_t *output_size) {
    if (!output || !output_size) return BK_LBS_INVALID;
    *output = 0; *output_size = 0;
    if ((!data && size) || size > BK_ARCHIVE_MAX_BYTES) return BK_LBS_INVALID;
    const uint8_t *p = data;
    uint8_t *out = bk_sys_alloc(size*2U+8U);
    if (!out) return BK_LBS_NOMEM;
    out[0]='B';out[1]='K';out[2]='Z';out[3]='1';
    for (unsigned i=0;i<4;i++) out[4+i]=(uint8_t)(size>>(i*8));
    uint32_t n=8;
    for (uint32_t i=0;i<size;) {
        uint8_t v=p[i], count=1;
        while (i+count<size && count<255 && p[i+count]==v) count++;
        out[n++]=count;out[n++]=v;i+=count;
    }
    *output=out;*output_size=n;return 0;
}
int bk_archive_unpack(const void *data, uint32_t size, void **output, uint32_t *output_size) {
    if (!output || !output_size) return BK_LBS_INVALID;
    *output=0;*output_size=0;
    uint32_t expected;
    if (bk_archive_info(data,size,&expected)) return BK_LBS_INVALID;
    const uint8_t *p=data;
    /* Validate before allocating, including exact length and zero-length runs. */
    uint32_t n=0;
    if ((size-8)&1U) return BK_LBS_INVALID;
    for (uint32_t i=8;i<size;i+=2) {
        if (!p[i] || p[i]>expected-n) return BK_LBS_INVALID;
        n+=p[i];
    }
    if (n!=expected) return BK_LBS_INVALID;
    uint8_t *out=bk_sys_alloc(expected?expected:1);
    if (!out) return BK_LBS_NOMEM;
    n=0;
    for (uint32_t i=8;i<size;i+=2)
        for (unsigned j=0;j<p[i];j++)out[n++]=p[i+1];
    *output=out;*output_size=n;return 0;
}
