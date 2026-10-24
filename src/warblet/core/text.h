// Small text helpers the wire needs: base64 decoding, lowercase hex, and
// unsigned decimal. Portable C++, no Arduino headers. Host-tested.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace warblet {

// A signing key is 32 bytes, carried as standard base64: 44 characters
// ending in one '=' (contract §Fleet keys → OTA wire format; CHIRP-PROV v1.5).
static const size_t kKeyBytes = 32;
static const size_t kKeyBase64Chars = 44;

// Decode standard base64 (with '=' padding, length a multiple of 4).
// Returns the decoded length, or -1 for a bad character, a bad length, or
// an output that does not fit in cap.
int base64Decode(const char* src, size_t len, uint8_t* out, size_t cap);

// True only for exactly 44 base64 characters that decode to 32 bytes.
bool decodeKey(const char* b64, size_t len, uint8_t out[kKeyBytes]);

// Lowercase hex of n bytes into out (2n characters plus a NUL).
void toHex(const uint8_t* bytes, size_t n, char* out);

// Unsigned decimal, no leading zeros, plus a NUL. Returns the length.
// out needs room for 21 characters.
size_t toDecimal(uint64_t v, char out[21]);

}  // namespace warblet
