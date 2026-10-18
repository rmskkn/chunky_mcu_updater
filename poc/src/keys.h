#ifndef CB_KEYS_H
#define CB_KEYS_H
#include <stdint.h>

/* PoC key material. The device holds these in its identity page; the server
 * holds the same bytes. Production replaces both with ECDSA P-256 keypairs
 * (DESIGN.md section 10.2) -- a shared secret is a PoC shortcut, not a design. */
static inline void poc_dev_key(uint8_t k[32])
{
    for (int i = 0; i < 32; i++) k[i] = (uint8_t)(0xA0 + i);
}
static inline void poc_root_key(uint8_t k[32])
{
    for (int i = 0; i < 32; i++) k[i] = (uint8_t)(0x50 + i);
}

#endif
