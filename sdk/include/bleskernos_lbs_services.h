#ifndef BLESKERNOS_LBS_SERVICES_H
#define BLESKERNOS_LBS_SERVICES_H
#include <bleskernos_lbs.h>
#include <bleskernos_api.h>
int bk_crypto_sha256(const void *, uint32_t, uint8_t[32]);
void bk_crypto_wipe(void *, uint32_t);
int bk_crypto_random(void *, uint32_t);
int bk_crypto_random_available(void);
#define BK_CRYPTO_SHA256 1U
#define BK_CRYPTO_SHA224 8U
#define BK_CRYPTO_HMAC_SHA256 16U
int bk_crypto_sha224(const void *, uint32_t, uint8_t[28]);
int bk_crypto_hmac_sha256(const void *, uint32_t, const void *, uint32_t, uint8_t[32]);
#define BK_CRYPTO_HARDWARE_RANDOM 2U
#define BK_CRYPTO_TLS_SERVICE 4U
uint32_t bk_crypto_capabilities(void);
/* HTTPS service backed by the existing BearSSL provider and trust store.
   Its clock/entropy limitations are documented with NETWORK/TLS. */
int32_t bk_crypto_https_exchange(const char *, const void *, uint32_t, void *, uint32_t, uint32_t);
typedef struct {
    uint32_t size, pid, syscall_abi, flags;
} bk_security_context_t;
#define BK_SECURITY_RING3 1U
/* No synthetic UID or permission decision is represented as enforcement. */
int bk_security_context(bk_security_context_t *);
int bk_security_authorize(uint32_t operation, const char *resource);
int bk_security_set_privileges(uint32_t privileges);
#endif
