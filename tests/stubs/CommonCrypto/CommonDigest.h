/*
 * Minimal CommonCrypto, enough to syntax check the sources that name it.
 * Nothing here does anything. See the README beside this file.
 */
#ifndef T150_STUB_CC_DIGEST_H
#define T150_STUB_CC_DIGEST_H
#include <stdint.h>
typedef uint32_t CC_LONG;
#define CC_SHA256_DIGEST_LENGTH 32
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md);

#endif /* T150_STUB_CC_DIGEST_H */
