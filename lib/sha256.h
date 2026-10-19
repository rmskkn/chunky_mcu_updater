#ifndef CB_SHA256_H
#define CB_SHA256_H
#include <stdint.h>
#include <stddef.h>

#define SHA256_LEN 32

typedef struct {
    uint32_t h[8];
    uint8_t  buf[64];
    size_t   buflen;
    uint64_t total;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t len);
void sha256_final(sha256_t *s, uint8_t out[SHA256_LEN]);
void sha256(const void *data, size_t len, uint8_t out[SHA256_LEN]);

void hmac_sha256(const uint8_t *key, size_t keylen,
                 const void *data, size_t len, uint8_t out[SHA256_LEN]);

#endif
