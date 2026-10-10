#ifndef CB_MD5_H
#define CB_MD5_H
#include <stdint.h>
#include <stddef.h>

#define MD5_LEN 16

/* Per-sector hash of the manifest and the plan. Equality and transport
 * integrity only -- never a trust boundary (DESIGN.md section 10). */
void md5(const void *data, size_t len, uint8_t out[MD5_LEN]);

#endif
