// SHA-256 and HMAC-SHA256, written from FIPS 180-4 and RFC 2104.
// Portable C++: no Arduino headers, no heap. Host-tested (see test/).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace warblet {

static const size_t kSha256Block = 64;
static const size_t kSha256Digest = 32;

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const void* data, size_t len);
  void finish(uint8_t out[kSha256Digest]);  // leaves the object needing reset()

 private:
  void compress(const uint8_t block[kSha256Block]);
  uint32_t h_[8];
  uint64_t bytes_;
  uint8_t buf_[kSha256Block];
  size_t used_;
};

// HMAC-SHA256, fed in pieces so a signed message never has to be copied
// into one buffer first.
class HmacSha256 {
 public:
  HmacSha256(const uint8_t* key, size_t keyLen);
  ~HmacSha256();
  void update(const void* data, size_t len);
  void finish(uint8_t out[kSha256Digest]);

 private:
  uint8_t k_[kSha256Block];
  Sha256 inner_;
};

// Overwrite secret bytes in a way the compiler will not drop.
void wipe(void* p, size_t n);

}  // namespace warblet
