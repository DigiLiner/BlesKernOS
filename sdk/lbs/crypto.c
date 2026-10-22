#include <bleskernos_lbs_services.h>
#include <bearssl_hash.h>
#include <bearssl_hmac.h>
/* BearSSL's existing implementation, compiled PIC without kernel state. */
int bk_crypto_sha256(const void *data, uint32_t size, uint8_t output[32]) {
    br_sha256_context c;
    if ((!data && size) || !output)
        return BK_LBS_INVALID;
    br_sha256_init(&c);
    br_sha256_update(&c, data, size);
    br_sha256_out(&c, output);
    bk_crypto_wipe(&c, sizeof(c));
    return 0;
}
void bk_crypto_wipe(void *data, uint32_t size) {
    volatile uint8_t *p = data;
    if (!p)
        return;
    while (size--)
        *p++ = 0;
}
/* Use only the CPU's advertised hardware CSPRNG. Never fall back to time,
   process IDs, addresses or the kernel's legacy getrandom seed. */
int bk_crypto_random(void *output, uint32_t size) {
    uint8_t *p = output;
    uint32_t done = 0;
    if (!p && size)
        return BK_LBS_INVALID;
    if (!size)
        return 0;
    if (!bk_crypto_random_available())
        return BK_LBS_UNSUPPORTED;
    while (done < size) {
        uint32_t value = 0, attempt;
        uint8_t ready = 0;
        for (attempt = 0; attempt < 10; attempt++) {
            __asm__ volatile(".byte 0x0f,0xc7,0xf0; setc %1" : "=a"(value), "=qm"(ready));
            if (ready)
                break;
        }
        if (!ready) {
            bk_crypto_wipe(output, size);
            return -5;
        }
        for (attempt = 0; attempt < 4 && done < size; attempt++, done++) {
            p[done] = value;
            value >>= 8;
        }
    }
    return 0;
}

int bk_crypto_random_available(void) {
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (c & (1U << 30)) != 0;
}

/* The existing TLS provider owns its certificate store and validation policy.
   This facade neither bypasses X.509 checks nor reimplements TCP or TLS. */
uint32_t bk_crypto_capabilities(void) {
    bk_net_info_t info;
    uint32_t result = BK_CRYPTO_SHA256 | BK_CRYPTO_SHA224 | BK_CRYPTO_HMAC_SHA256;
    if (bk_crypto_random_available())
        result |= BK_CRYPTO_HARDWARE_RANDOM;
    if (bk_net_get_info(&info) && info.tls_ready)
        result |= BK_CRYPTO_TLS_SERVICE;
    return result;
}
int32_t bk_crypto_https_exchange(const char *url, const void *request, uint32_t request_length,
                                 void *data, uint32_t capacity, uint32_t timeout_ms) {
    static const char prefix[] = "https://";
    uint32_t i;
    if (!url || (!request && request_length) || (!data && capacity))
        return BK_LBS_INVALID;
    for (i = 0; i < sizeof(prefix) - 1; i++)
        if (url[i] != prefix[i])
            return BK_LBS_INVALID;
    if (!(bk_crypto_capabilities() & BK_CRYPTO_TLS_SERVICE))
        return BK_LBS_UNSUPPORTED;
    return bk_net_http_exchange_quiet(url, request, request_length, data, capacity, timeout_ms);
}

int bk_crypto_sha224(const void *data, uint32_t size, uint8_t output[28]) {
    br_sha224_context context;
    if ((!data && size) || !output)
        return BK_LBS_INVALID;
    br_sha224_init(&context);
    br_sha224_update(&context, data, size);
    br_sha224_out(&context, output);
    bk_crypto_wipe(&context, sizeof(context));
    return 0;
}
int bk_crypto_hmac_sha256(const void *key, uint32_t key_size, const void *data, uint32_t size,
                          uint8_t output[32]) {
    br_hmac_key_context kc;
    br_hmac_context context;
    if ((!key && key_size) || (!data && size) || !output)
        return BK_LBS_INVALID;
    br_hmac_key_init(&kc, &br_sha256_vtable, key, key_size);
    br_hmac_init(&context, &kc, 0);
    br_hmac_update(&context, data, size);
    br_hmac_out(&context, output);
    bk_crypto_wipe(&context, sizeof(context));
    bk_crypto_wipe(&kc, sizeof(kc));
    return 0;
}
