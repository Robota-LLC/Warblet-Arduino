// SHA-256 (FIPS 180-4) and HMAC (RFC 2104). See sha256.h.
#include "sha256.h"

#include <string.h>

namespace warblet {

namespace {

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

}  // namespace

void wipe(void* p, size_t n) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  while (n--) *v++ = 0;
}

void Sha256::reset() {
  static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(h_, init, sizeof h_);
  bytes_ = 0;
  used_ = 0;
}

void Sha256::compress(const uint8_t b[kSha256Block]) {
  // The message schedule kept as a 16-word ring (64 bytes, not 256): small
  // boards have small stacks.
  uint32_t w[16];
  for (unsigned i = 0; i < 16; i++) {
    w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 |
           (uint32_t)b[i * 4 + 2] << 8 | (uint32_t)b[i * 4 + 3];
  }
  uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3];
  uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (unsigned i = 0; i < 64; i++) {
    if (i >= 16) {
      uint32_t w15 = w[(i - 15) & 15], w2 = w[(i - 2) & 15];
      uint32_t s0 = rotr(w15, 7) ^ rotr(w15, 18) ^ (w15 >> 3);
      uint32_t s1 = rotr(w2, 17) ^ rotr(w2, 19) ^ (w2 >> 10);
      w[i & 15] = w[i & 15] + s0 + w[(i - 7) & 15] + s1;
    }
    uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i & 15];
    uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
    h = g; g = f; f = e; e = d + t1;
    d = c; c = bb; bb = a; a = t1 + t2;
  }
  h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d;
  h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
  wipe(w, sizeof w);
}

void Sha256::update(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  bytes_ += len;
  while (len > 0) {
    size_t take = kSha256Block - used_;
    if (take > len) take = len;
    memcpy(buf_ + used_, p, take);
    used_ += take;
    p += take;
    len -= take;
    if (used_ == kSha256Block) {
      compress(buf_);
      used_ = 0;
    }
  }
}

void Sha256::finish(uint8_t out[kSha256Digest]) {
  uint64_t bits = bytes_ * 8u;
  uint8_t pad = 0x80;
  update(&pad, 1);
  pad = 0;
  while (used_ != 56) update(&pad, 1);
  uint8_t len[8];
  for (unsigned i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
  update(len, 8);
  for (unsigned i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(h_[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(h_[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(h_[i] >> 8);
    out[i * 4 + 3] = (uint8_t)h_[i];
  }
  wipe(buf_, sizeof buf_);
}

HmacSha256::HmacSha256(const uint8_t* key, size_t keyLen) {
  memset(k_, 0, sizeof k_);
  if (keyLen > kSha256Block) {  // RFC 2104: a long key is hashed first
    Sha256 s;
    s.update(key, keyLen);
    s.finish(k_);
  } else if (keyLen > 0) {
    memcpy(k_, key, keyLen);
  }
  uint8_t pad[kSha256Block];
  for (size_t i = 0; i < kSha256Block; i++) pad[i] = k_[i] ^ 0x36;
  inner_.update(pad, sizeof pad);
  wipe(pad, sizeof pad);
}

HmacSha256::~HmacSha256() { wipe(k_, sizeof k_); }

void HmacSha256::update(const void* data, size_t len) { inner_.update(data, len); }

void HmacSha256::finish(uint8_t out[kSha256Digest]) {
  uint8_t innerDigest[kSha256Digest];
  inner_.finish(innerDigest);
  // The outer hash reuses the inner one's state, one fewer object on the stack.
  uint8_t pad[kSha256Block];
  for (size_t i = 0; i < kSha256Block; i++) pad[i] = k_[i] ^ 0x5c;
  inner_.reset();
  inner_.update(pad, sizeof pad);
  inner_.update(innerDigest, sizeof innerDigest);
  inner_.finish(out);
  wipe(pad, sizeof pad);
  wipe(innerDigest, sizeof innerDigest);
}

}  // namespace warblet
