/* Stand-in for mbedtls/md.h: HMAC-SHA256 only, computed in ../stub_mbedtls.c
 * with the public bare-metal demo's own sha256.c, so no byte of this oracle
 * comes from the library under test. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum { MBEDTLS_MD_SHA256 = 6 } mbedtls_md_type_t;
typedef struct mbedtls_md_info_t mbedtls_md_info_t;
typedef struct {
  unsigned char state[256];  /* a sha256_ctx and the key pad, kept opaque here */
} mbedtls_md_context_t;

const mbedtls_md_info_t *mbedtls_md_info_from_type(mbedtls_md_type_t type);
void mbedtls_md_init(mbedtls_md_context_t *ctx);
int mbedtls_md_setup(mbedtls_md_context_t *ctx, const mbedtls_md_info_t *info, int hmac);
int mbedtls_md_hmac_starts(mbedtls_md_context_t *ctx, const unsigned char *key, size_t keylen);
int mbedtls_md_hmac_update(mbedtls_md_context_t *ctx, const unsigned char *input, size_t ilen);
int mbedtls_md_hmac_finish(mbedtls_md_context_t *ctx, unsigned char *output);
void mbedtls_md_free(mbedtls_md_context_t *ctx);
