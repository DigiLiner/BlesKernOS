/* NetSurf historically mixes its C allocations with bk_sys_free. Keep that
 * allocator domain separate from the POSIX heap used by the LBS loader. */
#include <bleskernos_api.h>
extern void *bk_sys_alloc_zero(size_t);
void *nsbk_lbs_calloc(size_t count, size_t size) {
    if (size && count > 0xffffffffU / size) return 0;
    return bk_sys_alloc_zero(count * size);
}
char *nsbk_lbs_strdup(const char *text) {
    if (!text) return 0;
    size_t n=0;while(text[n])n++;
    char *copy=bk_sys_alloc(n+1);
    if (copy) for(size_t i=0;i<=n;i++)copy[i]=text[i];
    return copy;
}
