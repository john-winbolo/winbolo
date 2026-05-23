/*
 * Minimal MD5 implementation for in-tree integrity checks (e.g. the
 * lobby map-upload "use local copy if MD5 matches" handshake). MD5 is
 * cryptographically broken — do not use this for security purposes.
 * For integrity of locally-trusted files it's fine, and the algorithm
 * is small enough to vendor without pulling in a dep.
 *
 * Public-domain reference based on RFC 1321 / Colin Plumb's pseudocode.
 */

#ifndef WINBOLO_MD5_H
#define WINBOLO_MD5_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[4];   /* a, b, c, d */
    uint32_t count[2];   /* bit count: count[0]=low, count[1]=high */
    uint8_t  buffer[64]; /* unprocessed bytes */
} Md5Ctx;

void md5Init(Md5Ctx *ctx);
void md5Update(Md5Ctx *ctx, const void *data, size_t len);
void md5Final(uint8_t digest[16], Md5Ctx *ctx);

/* One-shot helper. digest must point to 16 bytes. */
void md5Compute(const void *data, size_t len, uint8_t digest[16]);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_MD5_H */
