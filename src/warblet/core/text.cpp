// See text.h.
#include "text.h"

namespace warblet {

namespace {

int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

int base64Decode(const char* src, size_t len, uint8_t* out, size_t cap) {
  if (len % 4 != 0) return -1;
  size_t n = 0;
  for (size_t i = 0; i < len; i += 4) {
    bool last = (i + 4 == len);
    int v[4];
    int pads = 0;
    for (int j = 0; j < 4; j++) {
      char c = src[i + j];
      if (c == '=' && last && j >= 2) {
        v[j] = 0;
        pads++;
      } else {
        if (pads > 0) return -1;  // data after padding
        v[j] = b64Value(c);
        if (v[j] < 0) return -1;
      }
    }
    uint32_t triple = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
    int bytes = 3 - pads;
    if (n + (size_t)bytes > cap) return -1;
    out[n++] = (uint8_t)(triple >> 16);
    if (bytes > 1) out[n++] = (uint8_t)(triple >> 8);
    if (bytes > 2) out[n++] = (uint8_t)triple;
  }
  return (int)n;
}

bool decodeKey(const char* b64, size_t len, uint8_t out[kKeyBytes]) {
  if (len != kKeyBase64Chars || b64[len - 1] != '=' || b64[len - 2] == '=') return false;
  return base64Decode(b64, len, out, kKeyBytes) == (int)kKeyBytes;
}

void toHex(const uint8_t* bytes, size_t n, char* out) {
  static const char H[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = H[bytes[i] >> 4];
    out[i * 2 + 1] = H[bytes[i] & 0x0f];
  }
  out[n * 2] = '\0';
}

size_t toDecimal(uint64_t v, char out[21]) {
  char tmp[21];
  size_t n = 0;
  do {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  } while (v > 0);
  for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  out[n] = '\0';
  return n;
}

}  // namespace warblet
