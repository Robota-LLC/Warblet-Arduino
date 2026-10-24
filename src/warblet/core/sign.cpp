// See sign.h for the byte strings, quoted from the contract (v0.31).
#include "sign.h"

#include <string.h>

#include "text.h"

namespace warblet {

namespace {

bool hasTag(const char* tag) { return tag != nullptr && tag[0] != '\0'; }

// The nonce line and the tag line, in contract order, BOTH ALWAYS PRESENT
// (v0.31): "<nonce>\n" with an empty nonce when there is none, then
// "tag=<tag>\n" with an empty tag when there is none. This is the only place
// the shape is written; signedInput() below spells the same bytes for tests.
void feedPrefix(HmacSha256& h, uint64_t nonce, const char* tag) {
  if (nonce != 0) {
    char digits[21];
    size_t n = toDecimal(nonce, digits);
    h.update(digits, n);
  }
  h.update("\ntag=", 5);
  if (hasTag(tag)) h.update(tag, strlen(tag));
  h.update("\n", 1);
}

}  // namespace

size_t signedInput(uint64_t nonce, const char* tag, const uint8_t* body, size_t len,
                   uint8_t* out, size_t cap) {
  char digits[21];
  size_t nlen = nonce != 0 ? toDecimal(nonce, digits) : 0;
  size_t tlen = hasTag(tag) ? strlen(tag) : 0;
  size_t need = nlen + 5 + tlen + 1 + len;  // <nonce> "\ntag=" <tag> "\n" <body>
  if (out == nullptr || need > cap) return need;
  size_t at = 0;
  memcpy(out + at, digits, nlen);
  at += nlen;
  memcpy(out + at, "\ntag=", 5);
  at += 5;
  if (tlen > 0) memcpy(out + at, tag, tlen);
  at += tlen;
  out[at++] = '\n';
  if (len > 0) memcpy(out + at, body, len);
  return need;
}

void signHeader(const uint8_t* key, size_t keyLen, uint64_t nonce, const char* tag,
                const uint8_t* body, size_t len, char hex[65]) {
  HmacSha256 h(key, keyLen);
  feedPrefix(h, nonce, tag);
  h.update(body, len);
  uint8_t mac[kSha256Digest];
  h.finish(mac);
  toHex(mac, sizeof mac, hex);
}

void signEnvelope(const uint8_t* key, size_t keyLen, const char* tag,
                  const uint8_t* body, size_t len, uint8_t mac[kSha256Digest]) {
  HmacSha256 h(key, keyLen);
  feedPrefix(h, 0, tag);  // the envelope carries no nonce: the field is empty
  h.update(body, len);
  h.finish(mac);
}

void signPoll(const uint8_t* key, size_t keyLen, const char* hwId, uint64_t nonce,
              char hex[65]) {
  // Unchanged by v0.31: "chirp-down\n<hwId>\n<nonce>".
  HmacSha256 h(key, keyLen);
  h.update("chirp-down\n", 11);
  h.update(hwId, strlen(hwId));
  h.update("\n", 1);
  char digits[21];
  size_t n = toDecimal(nonce, digits);
  h.update(digits, n);
  uint8_t mac[kSha256Digest];
  h.finish(mac);
  toHex(mac, sizeof mac, hex);
}

}  // namespace warblet
