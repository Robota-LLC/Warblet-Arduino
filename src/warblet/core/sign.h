// What a Signed (L2) device signs, exactly as the Warblet API contract v0.31
// defines it (§Auth → The signed input). Portable C++, no Arduino headers.
// Host-tested against the contract's three worked examples and an
// independent Python transcription of the rule (test/).
//
//   One shape on every transport, both lines ALWAYS present, empty fields
//   left empty (there is no compatibility mode with the v0.29 shape):
//     signed input = <nonce> "\n" "tag=" <tag> "\n" <body>
//   so a message with neither field signs "\ntag=\n" + body.
//
//   Header transports (HTTP/HTTPS):
//     X-Chirp-Signature = hex hmac_sha256(key, signed input)
//
//   Envelope transports (MQTT/MQTTS, raw TCP), which carry no nonce:
//     payload on the wire = 32 raw bytes of hmac_sha256(key, "\ntag=<tag>\n" + body) + body
//
//   Downlink poll at Signed (§Auth → Downlink poll), unchanged by v0.31:
//     X-Chirp-Signature = hex hmac_sha256(key, "chirp-down\n<hwId>\n<nonce>")
//
// The key is the 32 bytes the base64 string decodes to, as every public
// demo uses it (chirp_sign.py decodes with a2b_base64 before signing).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

namespace warblet {

// A nonce of 0 means "no nonce"; a tag of nullptr or "" means "no tag".
// Nonces are Unix milliseconds, so 0 is never a real one.

// Write the signed input into out. Returns the full length it needs; the
// bytes are written only when that length fits in cap. For tests and logs:
// signing itself streams the parts and never builds this buffer.
size_t signedInput(uint64_t nonce, const char* tag, const uint8_t* body, size_t len,
                   uint8_t* out, size_t cap);

// X-Chirp-Signature for an uplink on HTTP/HTTPS: 64 lowercase hex + NUL.
void signHeader(const uint8_t* key, size_t keyLen, uint64_t nonce, const char* tag,
                const uint8_t* body, size_t len, char hex[65]);

// The 32 raw bytes that go in front of the payload on MQTT/MQTTS and TCP
// (the MQTT transport sends them at Signed). They carry no nonce.
void signEnvelope(const uint8_t* key, size_t keyLen, const char* tag,
                  const uint8_t* body, size_t len, uint8_t mac[kSha256Digest]);

// X-Chirp-Signature for POST /ingest/{hwId}/down (collecting a command) at Signed.
void signPoll(const uint8_t* key, size_t keyLen, const char* hwId, uint64_t nonce,
              char hex[65]);

}  // namespace warblet
