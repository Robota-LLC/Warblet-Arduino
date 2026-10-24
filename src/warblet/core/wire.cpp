// See wire.h.
#include "wire.h"

#include <stdio.h>
#include <string.h>

#include "text.h"

namespace warblet {

namespace {

bool tagChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '_' || c == '.' || c == '-';
}

bool hwIdChar(char c) { return tagChar(c) || c == ':'; }

// Appends text to a bounded buffer and remembers whether anything was cut.
class Out {
 public:
  Out(char* buf, size_t cap) : buf_(buf), cap_(cap) {}
  Out& add(const char* s) { return add(s, strlen(s)); }
  Out& add(const char* s, size_t n) {
    if (len_ + n + 1 > cap_) {
      ok_ = false;
      return *this;
    }
    memcpy(buf_ + len_, s, n);
    len_ += n;
    buf_[len_] = '\0';
    return *this;
  }
  Out& num(uint64_t v) {
    char d[21];
    size_t n = toDecimal(v, d);
    return add(d, n);
  }
  Out& header(const char* name, const char* value) {
    return add(name).add(": ").add(value).add("\r\n");
  }
  size_t done() const { return ok_ ? len_ : 0; }

 private:
  char* buf_;
  size_t cap_;
  size_t len_ = 0;
  bool ok_ = true;
};

bool present(const char* s) { return s != nullptr && s[0] != '\0'; }

bool equalsNoCase(const char* a, size_t n, const char* b) {
  if (strlen(b) != n) return false;
  for (size_t i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

}  // namespace

bool validTag(const char* tag) {
  if (tag == nullptr) return false;
  size_t n = strlen(tag);
  if (n < 1 || n > 32) return false;
  for (size_t i = 0; i < n; i++) {
    if (!tagChar(tag[i])) return false;
  }
  return true;
}

bool validHwId(const char* hwId) {
  if (hwId == nullptr) return false;
  size_t n = strlen(hwId);
  if (n < 1 || n > kHwIdMax) return false;
  for (size_t i = 0; i < n; i++) {
    if (!hwIdChar(hwId[i])) return false;
  }
  return true;
}

bool isClaimCode(const char* credential) {
  return credential != nullptr && strncmp(credential, "CHIRP-", 6) == 0;
}

size_t topicUp(const char* hwId, const char* tag, char* out, size_t cap) {
  Out o(out, cap);
  o.add("chirp/").add(hwId).add("/up");
  if (present(tag)) {
    if (!validTag(tag)) return 0;
    o.add("/").add(tag);
  }
  return o.done();
}

size_t topicDown(const char* hwId, char* out, size_t cap) {
  Out o(out, cap);
  return o.add("chirp/").add(hwId).add("/down").done();
}

bool hasControlChar(const char* s) {
  for (; s != nullptr && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x20 || c == 0x7f) return true;
  }
  return false;
}

uint64_t NonceClock::next(uint64_t nowMs) {
  if (nowMs < kClockSaneMs) return 0;
  // Never two signed requests on one millisecond, and never backwards.
  last_ = nowMs > last_ ? nowMs : last_ + 1;
  return last_;
}

size_t httpIngestHead(const HttpIngest& r, char* out, size_t cap) {
  Out o(out, cap);
  o.add("POST /ingest/").add(r.hwId).add(" HTTP/1.1\r\n");
  o.header("Host", r.host);
  o.add("Content-Length: ").num(r.bodyLen).add("\r\n");
  o.header("Connection", "close");
  o.header("Content-Type", "application/octet-stream");
  if (present(r.credential)) {
    o.header("X-Chirp-Token", r.credential);
    // A claim code rides in both headers until it binds and becomes the token.
    if (isClaimCode(r.credential)) o.header("X-Chirp-Claim", r.credential);
  }
  if (present(r.tag)) o.header("X-Chirp-Tag", r.tag);
  if (r.nonce != 0) o.add("X-Chirp-Nonce: ").num(r.nonce).add("\r\n");
  if (present(r.signature)) o.header("X-Chirp-Signature", r.signature);
  o.add("\r\n");
  return o.done();
}

size_t httpPollHead(const HttpPoll& r, char* out, size_t cap) {
  Out o(out, cap);
  // A POST with an empty body collects (contract v0.43); a GET is answered
  // 405 unless it carries a real credential, and a claim code is not one.
  o.add("POST /ingest/").add(r.hwId).add("/down HTTP/1.1\r\n");
  o.header("Host", r.host);
  o.header("Content-Length", "0");
  o.header("Connection", "close");
  if (present(r.credential)) o.header("X-Chirp-Token", r.credential);
  if (r.nonce != 0 && present(r.signature)) {
    o.add("X-Chirp-Nonce: ").num(r.nonce).add("\r\n");
    o.header("X-Chirp-Signature", r.signature);
  }
  o.add("\r\n");
  return o.done();
}

// ---- HttpResponse -----------------------------------------------------------

void HttpResponse::reset(uint8_t* body, size_t cap) {
  body_ = body;
  cap_ = cap;
  state_ = kStatus;
  lineLen_ = 0;
  status_ = 0;
  contentLength_ = -1;
  chunked_ = false;
  chunkLeft_ = 0;
  bodyLen_ = 0;
  bodySeen_ = 0;
  tooLong_ = false;
  down_[0] = '\0';
  retryAfter_ = 0;
  budget_ = false;
}

void HttpResponse::store(uint8_t c) {
  bodySeen_++;
  if (bodyLen_ < cap_) {
    body_[bodyLen_++] = c;
  } else {
    tooLong_ = true;
  }
}

bool HttpResponse::line(const char* s, size_t n) {
  switch (state_) {
    case kStatus: {
      // "HTTP/1.1 202 Accepted": exactly three digits, then a space or the end.
      if (n < 12 || strncmp(s, "HTTP/1.", 7) != 0 || s[8] != ' ') return false;
      if (n > 12 && s[12] != ' ') return false;
      int code = 0;
      for (size_t i = 9; i < 12; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        code = code * 10 + (s[i] - '0');
      }
      status_ = code;
      state_ = kHeaders;
      return true;
    }
    case kHeaders: {
      if (n == 0) {
        if (status_ >= 100 && status_ < 200) {
          // An interim answer (100 Continue): the real one follows.
          contentLength_ = -1;
          chunked_ = false;
          down_[0] = '\0';
          retryAfter_ = 0;
          budget_ = false;
          state_ = kStatus;
        } else if (status_ == 204 || status_ == 304) {
          state_ = kDone;
        } else if (chunked_) {
          state_ = kChunkSize;
        } else if (contentLength_ == 0) {
          state_ = kDone;
        } else {
          state_ = kBody;
        }
        return true;
      }
      const char* colon = static_cast<const char*>(memchr(s, ':', n));
      if (colon == nullptr) return false;
      size_t nameLen = (size_t)(colon - s);
      const char* v = colon + 1;
      size_t vlen = n - nameLen - 1;
      while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
      while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) vlen--;
      if (equalsNoCase(s, nameLen, "content-length")) {
        if (vlen == 0) return false;
        long long cl = 0;
        for (size_t i = 0; i < vlen; i++) {
          if (v[i] < '0' || v[i] > '9' || cl > 100000000LL) return false;
          cl = cl * 10 + (v[i] - '0');
        }
        contentLength_ = cl;
      } else if (equalsNoCase(s, nameLen, "transfer-encoding")) {
        chunked_ = equalsNoCase(v, vlen, "chunked");
        if (!chunked_) return false;  // no other coding is expected from the platform
      } else if (equalsNoCase(s, nameLen, "x-chirp-down")) {
        size_t m = vlen < sizeof down_ - 1 ? vlen : sizeof down_ - 1;
        memcpy(down_, v, m);
        down_[m] = '\0';
      } else if (equalsNoCase(s, nameLen, "x-chirp-budget")) {
        budget_ = true;
      } else if (equalsNoCase(s, nameLen, "retry-after")) {
        // Seconds; the HTTP-date form is left at 0 (the back-off still applies).
        unsigned long secs = 0;
        size_t i = 0;
        for (; i < vlen && v[i] >= '0' && v[i] <= '9' && secs < 86400UL; i++) secs = secs * 10 + (unsigned long)(v[i] - '0');
        retryAfter_ = (i == vlen && vlen > 0) ? secs : 0;
      }
      return true;
    }
    case kChunkSize: {
      size_t size = 0;
      size_t i = 0;
      for (; i < n && s[i] != ';'; i++) {
        char c = s[i];
        int d = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0 || size > 0x0fffffff) return false;
        size = size * 16 + (size_t)d;
      }
      if (i == 0) return false;
      chunkLeft_ = size;
      state_ = size == 0 ? kTrailers : kChunkData;
      return true;
    }
    case kChunkEnd:
      if (n != 0) return false;
      state_ = kChunkSize;
      return true;
    case kTrailers:
      if (n == 0) state_ = kDone;
      return true;
    default:
      return false;
  }
}

bool HttpResponse::feed(const uint8_t* data, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = data[i];
    switch (state_) {
      case kBad:
        return false;
      case kDone:
        return true;  // bytes after a complete response are ignored
      case kBody:
        store(c);
        if (contentLength_ >= 0 && (long long)bodySeen_ >= contentLength_) {
          state_ = kDone;
        }
        break;
      case kChunkData:
        store(c);
        if (--chunkLeft_ == 0) state_ = kChunkEnd;
        break;
      default:  // a line-oriented state
        if (c == '\n') {
          size_t len = lineLen_;
          if (len > 0 && line_[len - 1] == '\r') len--;
          lineLen_ = 0;
          if (!line(line_, len)) {
            state_ = kBad;
            return false;
          }
        } else if (lineLen_ < sizeof line_) {
          line_[lineLen_++] = (char)c;
        }
        // A header line longer than line_ keeps only its start; the headers
        // that matter here are all short.
        break;
    }
  }
  return state_ != kBad;
}

bool HttpResponse::finishAtClose() {
  // A body with neither a length nor chunking stays incomplete: a reply cut
  // short at that point looks exactly like a whole one.
  return state_ == kDone;
}

bool answerIsUnclaimed(const uint8_t* body, size_t len) {
  static const char kWord[] = "\"unclaimed\"";
  const size_t n = sizeof kWord - 1;
  for (size_t i = 0; i + n <= len; i++) {
    if (memcmp(body + i, kWord, n) == 0) return true;
  }
  return false;
}

unsigned long Backoff::after(int status, unsigned long retryAfterS) {
  bool ok = status >= 200 && status < 300;
  bool wasFailing = failing_;
  if (ok) {
    wait_ = base_;
  } else if (status == 401 || status == 403 || status == 404) {
    // A credential or an id the platform refuses: it will not fix itself soon.
    unsigned long minute = 60000UL;
    wait_ = (!wasFailing || wait_ < minute) ? minute : wait_ * 2;
  } else {
    wait_ = wasFailing ? wait_ * 2 : base_ * 2;  // 429, 5xx, or no answer
  }
  unsigned long ceiling = (status < 0 || status >= 500) ? unanswered_ : max_;
  if (!ok && wait_ > ceiling) wait_ = ceiling;
  if (!ok && retryAfterS > 0) {
    unsigned long asked = (retryAfterS > 3600UL ? 3600UL : retryAfterS) * 1000UL;
    if (asked > wait_) wait_ = asked;
  }
  failing_ = !ok;
  changed_ = (failing_ != wasFailing) || (failing_ && status != lastStatus_);
  lastStatus_ = status;
  return wait_;
}

// ---- downlink, readings -----------------------------------------------------

Downlink classifyDownlink(const uint8_t* body, size_t len) {
  if (len == 0) return Downlink::Empty;
  if (len >= 10 && memcmp(body, "CHIRPKEY1 ", 10) == 0) return Downlink::Key;
  return Downlink::Command;
}

bool parseKeyFrame(const uint8_t* body, size_t len, char keyBase64[45]) {
  if (len != 10 + kKeyBase64Chars + 1 || classifyDownlink(body, len) != Downlink::Key ||
      body[len - 1] != '\n') {
    return false;
  }
  memcpy(keyBase64, body + 10, kKeyBase64Chars);
  keyBase64[kKeyBase64Chars] = '\0';
  uint8_t raw[kKeyBytes];
  bool ok = decodeKey(keyBase64, kKeyBase64Chars, raw);
  memset(raw, 0, sizeof raw);
  if (!ok) keyBase64[0] = '\0';
  return ok;
}

size_t formatUnsigned(unsigned long long value, char out[kNumberTextMax]) {
  return toDecimal((uint64_t)value, out);
}

size_t formatInteger(long long value, char out[kNumberTextMax]) {
  if (value >= 0) return toDecimal((uint64_t)value, out);
  out[0] = '-';
  // The magnitude, computed so the most negative value does not overflow.
  uint64_t magnitude = (uint64_t)(-(value + 1)) + 1u;
  return 1 + toDecimal(magnitude, out + 1);
}

size_t formatReal(double value, int significantDigits, char out[kNumberTextMax]) {
  if (value != value || value > 1.7976931348623157e308 || value < -1.7976931348623157e308) {
    out[0] = '\0';
    return 0;  // NaN or infinity
  }
  if (significantDigits < 1) significantDigits = 1;
  if (significantDigits > 17) significantDigits = 17;
  int n = snprintf(out, kNumberTextMax, "%.*g", significantDigits, value);
  if (n <= 0 || (size_t)n >= kNumberTextMax - 2) {
    out[0] = '\0';
    return 0;
  }
  // "-5" from -5.0 would read as a whole number; say it is a real one.
  if (strchr(out, '.') == nullptr && strchr(out, 'e') == nullptr) {
    out[n++] = '.';
    out[n++] = '0';
    out[n] = '\0';
  }
  return (size_t)n;
}

bool encodeTenths(double value, uint8_t out[2]) {
  double r = value * 10.0;
  if (!(r > -32768.5 && r < 32767.5)) return false;  // also refuses NaN
  long t = (long)(r >= 0 ? r + 0.5 : r - 0.5);       // half away from zero, like lroundf
  uint16_t u = (uint16_t)(int16_t)t;
  out[0] = (uint8_t)(u >> 8);
  out[1] = (uint8_t)(u & 0xff);
  return true;
}

}  // namespace warblet
