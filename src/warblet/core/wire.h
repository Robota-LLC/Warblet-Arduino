// Framing for HTTPS, the one transport the library speaks first: request
// heads, the response reader, nonces, when to ask again after a refusal,
// downlink frames, and the two-byte reading the example decoders read.
// Portable C++, no Arduino headers, no heap. Host-tested (test/).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace warblet {

// ---- names ------------------------------------------------------------------

// A tag is [A-Za-z0-9_.-], 1 to 32 characters (contract §Device ingest → Tags).
bool validTag(const char* tag);

// A device id: letters, digits and : - _ . only, 1 to 128 characters (the
// rule the public MicroPython demos apply to a pushed hwid).
static const size_t kHwIdMax = 128;
bool validHwId(const char* hwId);

// A claim code is "CHIRP-" + 8 characters; it rides where the token goes
// until it is redeemed, and then it IS the token (contract §Claim-code-first).
bool isClaimCode(const char* credential);

// True when s holds a control character (below 0x20, or 0x7f). Such a byte in
// a host or a token would end up inside a request header.
bool hasControlChar(const char* s);

// ---- MQTT -------------------------------------------------------------------

// "chirp/{hwId}/up", or "chirp/{hwId}/up/{tag}" for a tagged message: exactly
// one extra level. Returns the length, or 0 if it does not fit or the tag is
// invalid (a tag can hold neither "/" nor a wildcard).
size_t topicUp(const char* hwId, const char* tag, char* out, size_t cap);
// "chirp/{hwId}/down", where the board's commands arrive.
size_t topicDown(const char* hwId, char* out, size_t cap);

// ---- nonces -----------------------------------------------------------------

// Unix milliseconds, strictly increasing: the platform refuses a nonce that is
// not greater than the last one it accepted, for uplinks and polls alike.
class NonceClock {
 public:
  // 2025-01-01T00:00:00Z. A clock below this was never set, so no nonce.
  static const uint64_t kClockSaneMs = 1735689600000ULL;
  // Returns 0 when nowMs is below kClockSaneMs (no clock: send no nonce).
  uint64_t next(uint64_t nowMs);
  uint64_t last() const { return last_; }

 private:
  uint64_t last_ = 0;
};

// ---- HTTP requests ----------------------------------------------------------

// Room for the longest request head the library can build: a 128-character
// host, id and token, a claim header, a 32-character tag, a 20-digit nonce and
// a signature (host-tested to fit).
static const size_t kHttpHeadMax = 1024;

struct HttpIngest {
  const char* host;        // e.g. "https.warbletiot.com"
  const char* hwId;
  const char* credential;  // token or claim code; nullptr or "" at Open
  const char* tag;         // nullptr or "" for none
  uint64_t nonce;          // 0 for none
  const char* signature;   // 64 hex, nullptr when not signing
  size_t bodyLen;
};

// "POST /ingest/{hwId} HTTP/1.1" and headers, ending in the blank line. The
// header order matches the public MicroPython HTTPS demo. Returns the length,
// or 0 if it does not fit in cap.
size_t httpIngestHead(const HttpIngest& r, char* out, size_t cap);

struct HttpPoll {
  const char* host;
  const char* hwId;
  const char* credential;  // nullptr or "" at Open
  uint64_t nonce;          // 0 when not signing
  const char* signature;   // signPoll() hex, nullptr when not signing
};

// "POST /ingest/{hwId}/down HTTP/1.1" with an empty body (contract v0.43:
// collecting a command is a POST). Returns the length or 0.
size_t httpPollHead(const HttpPoll& r, char* out, size_t cap);

// ---- HTTP responses ---------------------------------------------------------

// Reads one HTTP/1.1 response fed in pieces: status, Content-Length or
// chunked body, X-Chirp-Down and Retry-After. The body goes into the caller's
// buffer; a body longer than that buffer is reported as tooLong(), never
// truncated silently. Interim 1xx answers are skipped. A body with neither a
// length nor chunking is never complete: a reply cut short could not be told
// from a whole one (the platform always sends one or the other).
class HttpResponse {
 public:
  HttpResponse(uint8_t* body, size_t cap) { reset(body, cap); }
  void reset(uint8_t* body, size_t cap);
  // Feed received bytes. Returns false once the response is malformed.
  bool feed(const uint8_t* data, size_t n);
  // Call when the connection closed. Returns true if the response is whole.
  bool finishAtClose();
  bool complete() const { return state_ == kDone; }
  bool malformed() const { return state_ == kBad; }
  bool tooLong() const { return tooLong_; }
  int status() const { return status_; }
  size_t bodyLen() const { return bodyLen_; }
  // The X-Chirp-Down header value ("data", "key"), or "" when absent.
  const char* downHeader() const { return down_; }
  // Retry-After in seconds (the delay form), or 0 when absent or a date.
  unsigned long retryAfter() const { return retryAfter_; }
  // True when X-Chirp-Budget came: on a 429 it is the day's storage budget,
  // not the rate (the platform sends it on a 202 and a budget 429 only).
  bool hasBudget() const { return budget_; }

 private:
  enum State { kStatus, kHeaders, kBody, kChunkSize, kChunkData, kChunkEnd, kTrailers, kDone, kBad };
  bool line(const char* s, size_t n);
  void store(uint8_t c);
  uint8_t* body_;
  size_t cap_;
  State state_;
  char line_[160];
  size_t lineLen_;
  int status_;
  long long contentLength_;
  bool chunked_;
  size_t chunkLeft_;
  size_t bodyLen_;   // bytes kept
  size_t bodySeen_;  // bytes received, kept or not
  bool tooLong_;
  char down_[8];
  unsigned long retryAfter_;
  bool budget_;
};

// True for the 202 that records only a sighting: {"status":"unclaimed"}
// (contract §HTTP). The message was NOT stored on any device.
bool answerIsUnclaimed(const uint8_t* body, size_t len);

// ---- asking again -----------------------------------------------------------

// How long to wait before the next request after an answer. A success goes
// back to the normal pace. A refusal (401, 403, 404) waits a minute and
// doubles, and a 429 doubles from the normal pace, up to maxMs: a refusal does
// not fix itself soon. No answer, or a server error, doubles from the normal
// pace up to unansweredMaxMs, so a board is back soon after an outage.
// Retry-After is honoured when it asks for longer, up to an hour.
class Backoff {
 public:
  Backoff(unsigned long baseMs, unsigned long maxMs, unsigned long unansweredMaxMs)
      : base_(baseMs), max_(maxMs), unanswered_(unansweredMaxMs), wait_(baseMs) {}
  // status: the HTTP status, or -1 when no answer came.
  unsigned long after(int status, unsigned long retryAfterS);
  unsigned long wait() const { return wait_; }
  // A new normal pace; a wait under way after failures keeps its length.
  void pace(unsigned long baseMs) {
    base_ = baseMs;
    if (!failing_) wait_ = baseMs;
  }
  // New settings: what was refused before says nothing about them.
  void reset() {
    wait_ = base_;
    failing_ = false;
    changed_ = false;
    lastStatus_ = 0;
  }
  bool failing() const { return failing_; }
  // True when this answer changed things: the first failure, a different
  // failure, or the first success after failures. Say something only then.
  bool changed() const { return changed_; }

 private:
  unsigned long base_, max_, unanswered_, wait_;
  bool failing_ = false;
  bool changed_ = false;
  int lastStatus_ = 0;
};

// ---- downlink frames --------------------------------------------------------

// A downlink is either a command for the sketch or the platform handing the
// board its signing key ("CHIRPKEY1 " + 44 base64 + "\n", contract §Fleet
// keys → OTA wire format). The body magic is what decides, not the header.
enum class Downlink { Command, Key, Empty };
Downlink classifyDownlink(const uint8_t* body, size_t len);

// Take the base64 key out of a key frame: exactly "CHIRPKEY1 " + 44
// characters that decode to 32 bytes + "\n". Returns false otherwise.
bool parseKeyFrame(const uint8_t* body, size_t len, char keyBase64[45]);

// ---- readings ---------------------------------------------------------------

// A number as text, the payload sendNumber() sends and a one-line decoder
// reads back with float(str(payload)). Whole numbers are written exactly
// ("101325", "-5"). A fractional value is written with as many significant
// digits as its type holds (7 for a float, 15 for a double) and always shows
// a point or an exponent ("21.53", "-5.0", "1e+20"). Each returns the length,
// or 0 for NaN or infinity, which have no text a decoder should accept.
static const size_t kNumberTextMax = 32;
size_t formatInteger(long long value, char out[kNumberTextMax]);
size_t formatUnsigned(unsigned long long value, char out[kNumberTextMax]);
size_t formatReal(double value, int significantDigits, char out[kNumberTextMax]);

// Two bytes, high byte first: the value times ten, rounded, as a signed
// 16-bit number. This is what the public temperature demos send and what
// their decoder.star reads (00 c9 = 20.1; ff ce = -5.0). Returns false,
// writing nothing, when the value does not fit (-3276.8 to 3276.7) or is
// not a number.
bool encodeTenths(double value, uint8_t out[2]);

}  // namespace warblet
