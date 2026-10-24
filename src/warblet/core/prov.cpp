// See prov.h.
#include "prov.h"

#include <string.h>

#include "json.h"
#include "sha256.h"
#include "text.h"
#include "wire.h"

namespace warblet {

namespace {

// Copy a string value into a fixed field. False when it is not a string,
// does not fit, or holds a control character: these values end up in
// request headers, the announce line and the board's own console.
bool takeString(const JsonValue& v, char* field, size_t cap) {
  return v.type == JsonValue::kString && jsonString(v, field, cap) >= 0 && !hasControlChar(field);
}

bool validNetworks(const JsonValue& list, Settings& out) {
  if (list.type != JsonValue::kArray) return false;
  // Walk the array by wrapping each element: the array text was already
  // validated, so a tiny scanner over it is enough.
  const char* p = list.p + 1;
  const char* end = list.p + list.n - 1;  // the closing bracket
  out.apsCount = 0;
  for (;;) {
    while (p < end && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= end) break;
    if (*p != '{') return false;  // every entry must be an object
    // Find the end of this object (strings may hold braces).
    const char* q = p;
    int depth = 0;
    bool inString = false;
    for (; q < end; q++) {
      if (inString) {
        if (*q == '\\') q++;
        else if (*q == '"') inString = false;
      } else if (*q == '"') {
        inString = true;
      } else if (*q == '{' || *q == '[') {
        depth++;
      } else if (*q == '}' || *q == ']') {
        if (--depth == 0) break;
      }
    }
    if (out.apsCount >= kMaxNetworks) return false;  // refused, not truncated
    Network& n = out.aps[out.apsCount];
    n.ssid[0] = n.pass[0] = '\0';
    JsonObject entry(p, (size_t)(q + 1 - p));
    JsonValue k, v;
    while (entry.next(k, v)) {
      if (jsonKeyIs(k, "ssid")) {
        if (!takeString(v, n.ssid, sizeof n.ssid)) return false;
      } else if (jsonKeyIs(k, "pass")) {
        if (!takeString(v, n.pass, sizeof n.pass)) return false;
      }
    }
    if (!entry.ok() || n.ssid[0] == '\0') return false;
    out.apsCount++;
    p = q + 1;
  }
  out.hasAps = true;
  return true;
}

}  // namespace

void Settings::clear() { memset(this, 0, sizeof *this); }

bool Settings::ready() const { return ssid[0] && credential[0] && hwId[0]; }

size_t Settings::networks(Network* out, size_t max) const {
  size_t n = 0;
  if (ssid[0] && n < max) {
    memcpy(out[n].ssid, ssid, sizeof ssid);
    memcpy(out[n].pass, pass, sizeof pass);
    n++;
  }
  for (size_t i = 0; i < apsCount && n < max; i++) {
    bool seen = false;
    for (size_t j = 0; j < n; j++) {
      if (strcmp(out[j].ssid, aps[i].ssid) == 0) seen = true;
    }
    if (!seen) out[n++] = aps[i];
  }
  return n;
}

bool Settings::network(size_t i, const char** ssidOut, const char** passOut) const {
  size_t seen = 0;
  if (ssid[0]) {
    if (i == 0) {
      *ssidOut = ssid;
      *passOut = pass;
      return true;
    }
    seen = 1;
  }
  for (size_t a = 0; a < apsCount; a++) {
    bool repeat = ssid[0] && strcmp(aps[a].ssid, ssid) == 0;
    for (size_t b = 0; b < a && !repeat; b++) {
      if (strcmp(aps[b].ssid, aps[a].ssid) == 0) repeat = true;
    }
    if (repeat) continue;
    if (seen == i) {
      *ssidOut = aps[a].ssid;
      *passOut = aps[a].pass;
      return true;
    }
    seen++;
  }
  return false;
}

const char* provResultLine(ProvResult r) {
  switch (r) {
    case ProvResult::Ok: return "CHIRP= ok";
    case ProvResult::BadJson: return "CHIRP= err bad-json";
    case ProvResult::MissingField: return "CHIRP= err missing-field";
    case ProvResult::StoreFailed: return "CHIRP= err store-failed";
  }
  return "CHIRP= err bad-json";
}

ProvResult applyPush(const char* json, size_t len, const Settings& stored, Settings& merged) {
  // First pass: is it one valid object, and does it carry both credentials?
  {
    JsonObject o(json, len);
    JsonValue k, v;
    bool claim = false, token = false;
    while (o.next(k, v)) {
      if (jsonKeyIs(k, "claim")) claim = true;
      if (jsonKeyIs(k, "token")) token = true;
    }
    if (!o.ok()) return ProvResult::BadJson;
    if (claim && token) return ProvResult::BadJson;  // one credential slot
  }

  merged = stored;
  bool pushedSsid = false, pushedAps = false;
  JsonObject o(json, len);
  JsonValue k, v;
  while (o.next(k, v)) {
    bool good = true;
    if (jsonKeyIs(k, "ssid")) {
      good = takeString(v, merged.ssid, sizeof merged.ssid);
      pushedSsid = true;
    } else if (jsonKeyIs(k, "pass")) {
      good = takeString(v, merged.pass, sizeof merged.pass);
    } else if (jsonKeyIs(k, "aps")) {
      good = validNetworks(v, merged);
      pushedAps = true;
    } else if (jsonKeyIs(k, "claim") || jsonKeyIs(k, "token")) {
      // A claim code is stored where the token goes: once redeemed it IS
      // the token (contract §Claim-code-first onboarding).
      good = takeString(v, merged.credential, sizeof merged.credential);
    } else if (jsonKeyIs(k, "hwid")) {
      // It goes into the announce line, a topic and a URL.
      good = takeString(v, merged.hwId, sizeof merged.hwId) &&
             (merged.hwId[0] == '\0' || validHwId(merged.hwId));
    } else if (jsonKeyIs(k, "host")) {
      good = takeString(v, merged.host, sizeof merged.host);
    } else if (jsonKeyIs(k, "key")) {
      uint8_t raw[kKeyBytes];
      good = takeString(v, merged.key, sizeof merged.key) &&
             (merged.key[0] == '\0' || decodeKey(merged.key, strlen(merged.key), raw));
      wipe(raw, sizeof raw);
    }
    // Every other key (level, thread, tls, t, ...) is ignored, as v1 requires.
    if (!good) return ProvResult::BadJson;
  }
  // A top-level ssid without aps names exactly one network (v1.3).
  if (pushedSsid && !pushedAps) {
    merged.hasAps = false;
    merged.apsCount = 0;
  }
  return merged.ready() ? ProvResult::Ok : ProvResult::MissingField;
}

size_t settingsToJson(const Settings& s, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  if (s.ssid[0]) { w.member("ssid"); w.string(s.ssid); w.member("pass"); w.string(s.pass); }
  if (s.hasAps) {
    w.member("aps");
    w.beginArray();
    for (size_t i = 0; i < s.apsCount; i++) {
      w.comma();
      w.beginObject();
      w.member("ssid"); w.string(s.aps[i].ssid);
      w.member("pass"); w.string(s.aps[i].pass);
      w.endObject();
    }
    w.endArray();
  }
  if (s.credential[0]) { w.member("token"); w.string(s.credential); }
  if (s.hwId[0]) { w.member("hwid"); w.string(s.hwId); }
  if (s.host[0]) { w.member("host"); w.string(s.host); }
  if (s.key[0]) { w.member("key"); w.string(s.key); }
  w.endObject();
  return w.done();
}

void setupFingerprint(const Settings& s, char out[17]) {
  Sha256 h;
  h.update("warblet-setup\n", 14);
  h.update(s.hwId, strlen(s.hwId));
  h.update("\n", 1);
  h.update(s.credential, strlen(s.credential));
  h.update("\n", 1);
  h.update(s.key, strlen(s.key));
  uint8_t d[kSha256Digest];
  h.finish(d);
  toHex(d, 8, out);
  wipe(d, sizeof d);
}

bool validSlug(const char* slug) {
  if (slug == nullptr) return false;
  size_t n = 0;
  for (; slug[n]; n++) {
    char c = slug[n];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') || n >= 40) return false;
  }
  return n >= 3;
}

size_t provAnnounce(const char* hwId, const char* slug, char* out, size_t cap) {
  const char* head = "CHIRP! v=1";
  const char* tail = " radios=wifi sig=1";
  bool named = validSlug(slug);
  size_t need = strlen(head) + strlen(tail) + (hwId && hwId[0] ? 4 + strlen(hwId) : 0) +
                (named ? 3 + strlen(slug) : 0);
  if (need + 1 > cap) return 0;
  strcpy(out, head);
  if (hwId && hwId[0]) {
    strcat(out, " hw=");
    strcat(out, hwId);
  }
  strcat(out, tail);
  if (named) {
    strcat(out, " t=");
    strcat(out, slug);
  }
  return need;
}

void ProvResponder::setSlug(const char* slug) {
  slug_[0] = '\0';
  if (validSlug(slug)) strcpy(slug_, slug);
}

ProvResponder::Event ProvResponder::feed(char c, const Settings& current) {
  if (c == '\r' || c == '\n') {  // the portal sends CRLF; a bare LF is fine too
    if (len_ == 0 && !overflow_) return kNone;
    buf_[len_] = '\0';
    Event e = line(current);
    len_ = 0;
    overflow_ = false;
    return e;
  }
  if (len_ < kProvLineMax) {
    buf_[len_++] = c;
  } else {
    overflow_ = true;
  }
  return kNone;
}

ProvResponder::Event ProvResponder::line(const Settings& current) {
  const char* p = buf_;
  const char* end = buf_ + len_;
  while (p < end && (*p == ' ' || *p == '\t')) p++;
  while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
  size_t n = (size_t)(end - p);

  if (n == 6 && strncmp(p, "CHIRP?", 6) == 0 && !overflow_) {
    if (provAnnounce(current.hwId, slug_, reply_, sizeof reply_) == 0) return kNone;
    return kReply;
  }
  if (n >= 6 && strncmp(p, "CHIRP+", 6) == 0) {
    ProvResult r = overflow_ ? ProvResult::BadJson
                             : applyPush(p + 6, n - 6, current, pending_);
    if (r == ProvResult::Ok) return kStore;
    strcpy(reply_, provResultLine(r));
    return kReply;
  }
  return kNone;  // the sketch's own output, boot chatter, or our own lines
}

}  // namespace warblet
