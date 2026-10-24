/* SHA accelerator (TRM 18): SHA-1, SHA-224/256, SHA-384/512, SHA-512/224, SHA-512/256. Streaming contexts may be
 * interleaved (the engine state is saved per context); safe from both cores. */
#pragma once
#include "ndf.h"

typedef enum { SHA1 = 0, SHA224 = 1, SHA256 = 2, SHA384 = 3, SHA512 = 4, SHA512_224 = 5, SHA512_256 = 6 } sha_type_t;
typedef struct {
    uint8_t  type;
    bool     first;                          /* no block processed yet */
    uint32_t fill;                           /* bytes waiting in buf */
    uint64_t total;                          /* message bytes so far */
    uint32_t h[16];                          /* engine state after the last block */
    uint8_t  buf[128] __attribute__((aligned(4)));
} sha_ctx_t;

void   sha_start(sha_ctx_t *c, sha_type_t type);
void   sha_update(sha_ctx_t *c, const void *data, size_t len);
void   sha_finish(sha_ctx_t *c, uint8_t *digest);              /* sha_size(type) bytes */
size_t sha_size(sha_type_t type);
void   sha(sha_type_t type, const void *data, size_t len, uint8_t *digest);   /* one shot */
