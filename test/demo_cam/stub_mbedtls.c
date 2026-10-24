/* HMAC-SHA256 behind the mbedtls/md.h stand-in, built on the public
 * bare-metal demo's sha256.c (nucleo-wba65-baremetal), not on this library. */
#include <string.h>

#include "mbedtls/md.h"
#include "sha256.h"

struct mbedtls_md_info_t {
  int unused;
};
static const struct mbedtls_md_info_t kSha256Info = {0};

typedef struct {
  sha256_ctx inner;
  unsigned char key[SHA256_BLOCK];
} hmac_state;

typedef char state_fits[sizeof(hmac_state) <= sizeof(((mbedtls_md_context_t *)0)->state) ? 1 : -1];

const mbedtls_md_info_t *mbedtls_md_info_from_type(mbedtls_md_type_t type) {
  return type == MBEDTLS_MD_SHA256 ? &kSha256Info : NULL;
}

void mbedtls_md_init(mbedtls_md_context_t *ctx) { memset(ctx, 0, sizeof *ctx); }

int mbedtls_md_setup(mbedtls_md_context_t *ctx, const mbedtls_md_info_t *info, int hmac) {
  (void)ctx;
  return (info == &kSha256Info && hmac == 1) ? 0 : -1;
}

int mbedtls_md_hmac_starts(mbedtls_md_context_t *ctx, const unsigned char *key, size_t keylen) {
  hmac_state *s = (hmac_state *)ctx->state;
  unsigned char pad[SHA256_BLOCK];
  unsigned i;
  memset(s->key, 0, sizeof s->key);
  if (keylen > SHA256_BLOCK) {
    sha256(key, (unsigned)keylen, s->key);
  } else {
    memcpy(s->key, key, keylen);
  }
  for (i = 0; i < SHA256_BLOCK; i++) pad[i] = s->key[i] ^ 0x36;
  sha256_init(&s->inner);
  sha256_update(&s->inner, pad, SHA256_BLOCK);
  return 0;
}

int mbedtls_md_hmac_update(mbedtls_md_context_t *ctx, const unsigned char *input, size_t ilen) {
  hmac_state *s = (hmac_state *)ctx->state;
  sha256_update(&s->inner, input, (unsigned)ilen);
  return 0;
}

int mbedtls_md_hmac_finish(mbedtls_md_context_t *ctx, unsigned char *output) {
  hmac_state *s = (hmac_state *)ctx->state;
  unsigned char inner[SHA256_DIGEST], pad[SHA256_BLOCK];
  sha256_ctx outer;
  unsigned i;
  sha256_final(&s->inner, inner);
  for (i = 0; i < SHA256_BLOCK; i++) pad[i] = s->key[i] ^ 0x5c;
  sha256_init(&outer);
  sha256_update(&outer, pad, SHA256_BLOCK);
  sha256_update(&outer, inner, SHA256_DIGEST);
  sha256_final(&outer, output);
  return 0;
}

void mbedtls_md_free(mbedtls_md_context_t *ctx) { memset(ctx, 0, sizeof *ctx); }
