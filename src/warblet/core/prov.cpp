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

// The push fields the library acts on (CHIRP-PROV v1.8); any other is refused.
const char* const kAccepted[] = {"ssid", "pass", "aps", "claim", "token", "hwid", "host", "key"};

bool accepted(const JsonValue& key) {
  for (const char* name : kAccepted) {
    if (jsonKeyIs(key, name)) return true;
  }
  return false;
}

// A pushed host: 0 to 63 (the field's size) of A-Z a-z 0-9 . and -.
bool validHost(const char* host) {
  for (const char* p = host; *p; p++) {
    char ch = *p;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' ||
          ch == '-'))
      return false;
  }
  return true;
}

// A claim code or token: printable ASCII only (CHIRP-PROV v1.8), so it is the
// same number of characters and bytes on every responder.
bool printableAscii(const char* text) {
  for (const char* p = text; *p; p++) {
    if (*p < 0x20 || *p > 0x7E) return false;
  }
  return true;
}

// A field name the refusal line can carry: 1 to 32 of A-Z a-z 0-9 _ . -.
bool nameable(const JsonValue& key) {
  if (key.n == 0 || key.n > 32) return false;
  for (size_t i = 0; i < key.n; i++) {
    char c = key.p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
          c == '-'))
      return false;
  }
  return true;
}

// a before b, byte by byte, a prefix first.
bool sortsBefore(const char* a, size_t an, const char* b, size_t bn) {
  int c = memcmp(a, b, an < bn ? an : bn);
  return c < 0 || (c == 0 && an < bn);
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
    // Exactly an ssid and a pass, nothing else (CHIRP-PROV v1.8).
    bool hasSsid = false, hasPass = false;
    JsonObject entry(p, (size_t)(q + 1 - p));
    JsonValue k, v;
    while (entry.next(k, v)) {
      if (jsonKeyIs(k, "ssid")) {
        if (!takeString(v, n.ssid, sizeof n.ssid)) return false;
        hasSsid = true;
      } else if (jsonKeyIs(k, "pass")) {
        if (!takeString(v, n.pass, sizeof n.pass)) return false;
        hasPass = true;
      } else {
        return false;
      }
    }
    if (!entry.ok() || !hasSsid || !hasPass || n.ssid[0] == '\0') return false;
    out.apsCount++;
    p = q + 1;
  }
  if (out.apsCount == 0) return false;  // the whole list, so at least one network
  out.hasAps = true;
  return true;
}

}  // namespace

void Settings::clear() { memset(this, 0, sizeof *this); }

bool Settings::ready() const { return ssid[0] && credential[0] && hwId[0]; }

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
    case ProvResult::Unsupported: return "CHIRP= err unsupported";
  }
  return "CHIRP= err bad-json";
}

ProvResult applyPush(const char* json, size_t len, const Settings& stored, Settings& merged, char* refused,
                     size_t refusedCap) {
  // First pass: is it one valid object, does it name a field the library does
  // not act on, and does it carry both credentials?
  bool claim = false;
  {
    JsonObject o(json, len);
    JsonValue k, v;
    bool token = false, unnameable = false;
    const char* first = nullptr;
    size_t firstLen = 0;
    while (o.next(k, v)) {
      if (jsonKeyIs(k, "claim")) claim = true;
      if (jsonKeyIs(k, "token")) token = true;
      if (accepted(k)) continue;
      if (!nameable(k)) {
        unnameable = true;
      } else if (first == nullptr || sortsBefore(k.p, k.n, first, firstLen)) {
        first = k.p;
        firstLen = k.n;
      }
    }
    if (!o.ok() || unnameable) return ProvResult::BadJson;
    if (first != nullptr) {
      if (refused != nullptr && refusedCap > firstLen) {
        memcpy(refused, first, firstLen);
        refused[firstLen] = '\0';
      }
      return ProvResult::Unsupported;
    }
    if (claim && token) return ProvResult::BadJson;  // one credential slot
  }

  merged = stored;
  bool pushedSsid = false, pushedPass = false, pushedAps = false, pushedKey = false;
  JsonObject o(json, len);
  JsonValue k, v;
  while (o.next(k, v)) {
    bool good = true;
    if (jsonKeyIs(k, "ssid")) {
      // 1 to 32 bytes: 802.11's limit, and an empty one names no network.
      good = takeString(v, merged.ssid, sizeof merged.ssid) && merged.ssid[0] != '\0';
      pushedSsid = true;
    } else if (jsonKeyIs(k, "pass")) {
      good = takeString(v, merged.pass, sizeof merged.pass);
      pushedPass = true;
    } else if (jsonKeyIs(k, "aps")) {
      good = validNetworks(v, merged);
      pushedAps = true;
    } else if (jsonKeyIs(k, "claim") || jsonKeyIs(k, "token")) {
      // A claim code is stored where the token goes: once redeemed it IS
      // the token (contract §Claim-code-first onboarding). An empty one is
      // refused (CHIRP-PROV v1.8).
      good = takeString(v, merged.credential, sizeof merged.credential) && merged.credential[0] != '\0' &&
             printableAscii(merged.credential);
    } else if (jsonKeyIs(k, "hwid")) {
      // It goes into the announce line, a topic and a URL.
      good = takeString(v, merged.hwId, sizeof merged.hwId) &&
             (merged.hwId[0] == '\0' || validHwId(merged.hwId));
    } else if (jsonKeyIs(k, "host")) {
      good = takeString(v, merged.host, sizeof merged.host) && validHost(merged.host);
    } else if (jsonKeyIs(k, "key")) {
      uint8_t raw[kKeyBytes];
      // A key that will sign, never "" (CHIRP-PROV v1.8: a key goes only with
      // a push that starts a new device).
      good = takeString(v, merged.key, sizeof merged.key) && merged.key[0] != '\0' &&
             decodeKey(merged.key, strlen(merged.key), raw);
      wipe(raw, sizeof raw);
      pushedKey = true;
    }
    if (!good) return ProvResult::BadJson;
  }
  if (pushedAps) {
    // aps is the whole list, its first entry tried first. A top-level ssid or
    // pass sent with it must be that entry's (CHIRP-PROV v1.8).
    if ((pushedSsid && strcmp(merged.ssid, merged.aps[0].ssid) != 0) ||
        (pushedPass && strcmp(merged.pass, merged.aps[0].pass) != 0))
      return ProvResult::BadJson;
    strcpy(merged.ssid, merged.aps[0].ssid);
    strcpy(merged.pass, merged.aps[0].pass);
  } else if (pushedSsid) {
    // A top-level ssid without aps names exactly one network (v1.3).
    merged.hasAps = false;
    merged.apsCount = 0;
  }
  // A setup code, or an id other than the stored one, starts a new device.
  // Ids are compared byte for byte, as Warblet looks a device up: sensor and
  // SENSOR can be two devices. The stored signing key was the old device's, so
  // it goes unless this push brings one. A token, networks or a key alone keep
  // it (v1.3's merge).
  if ((claim || strcmp(merged.hwId, stored.hwId) != 0) && !pushedKey) merged.key[0] = '\0';
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

bool validFirmware(const char* fw) {
  if (fw == nullptr || fw[0] == '\0') return false;
  size_t n = 0;
  for (; fw[n]; n++) {
    char c = fw[n];
    bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (n >= kFirmwareMax || !(alnum || (n > 0 && (c == '.' || c == '_' || c == '-')))) return false;
  }
  return true;
}

size_t provCaps(bool mqtts, bool bootstrap, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.raw("{\"v\":1,\"auth\":[1,2],\"keyProvisioning\":[\"usb\"");
  if (bootstrap && !mqtts) w.raw(",\"https-bootstrap\"");
  w.raw("],\"otaKeyRotation\":false,\"otaFirmwareUpdate\":false,\"transports\":[");
  w.raw(mqtts ? "\"mqtts\"" : "\"https\"");
  w.raw("]}");
  return w.done();
}

size_t provAnnounce(const char* hwId, const char* fw, bool keySet, const char* slug, const char* caps,
                    char* out, size_t cap) {
  bool id = hwId != nullptr && hwId[0] != '\0';
  bool named = validSlug(slug);
  size_t need = 10 + (id ? 4 + strlen(hwId) : 0) + 4 + strlen(fw) + 10 + 12 + (named ? 3 + strlen(slug) : 0) +
                6 + strlen(caps);
  if (need + 1 > cap || need > kProvReplyMax) return 0;
  strcpy(out, "CHIRP! v=1");
  if (id) {
    strcat(out, " hw=");
    strcat(out, hwId);
  }
  strcat(out, " fw=");
  strcat(out, fw);
  strcat(out, keySet ? " key_set=1" : " key_set=0");
  strcat(out, " radios=wifi");
  if (named) {
    strcat(out, " t=");
    strcat(out, slug);
  }
  strcat(out, " caps=");  // last: the one value that holds JSON
  strcat(out, caps);
  return need;
}

ProvResponder::ProvResponder(Settings& scratch) : pending_(scratch) {
  strcpy(fw_, kLibraryFirmware);
  provCaps(false, false, caps_, sizeof caps_);
}

void ProvResponder::setSlug(const char* slug) {
  slug_[0] = '\0';
  if (validSlug(slug)) strcpy(slug_, slug);
}

bool ProvResponder::setFirmware(const char* fw) {
  if (!validFirmware(fw)) return false;
  strcpy(fw_, fw);
  return true;
}

void ProvResponder::setCaps(const char* caps) {
  if (caps != nullptr && strlen(caps) <= kProvCapsMax) strcpy(caps_, caps);
}

ProvResponder::Event ProvResponder::feed(char c, const Settings& current) {
  if (c == '\r' || c == '\n') {  // the portal sends CRLF; a bare LF is fine too
    // A line over the limit is thrown away whole, through its line ending:
    // no answer, and no part of it is read as a command (CHIRP-PROV v1.8).
    Event e = kNone;
    if (len_ > 0 && !overflow_) {
      buf_[len_] = '\0';
      e = line(current);
    }
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

  if (n == 6 && strncmp(p, "CHIRP?", 6) == 0) {
    // key_set=1: a key that will sign is held (stored settings and a key
    // from a command check are both checked before they are kept).
    if (provAnnounce(current.hwId, fw_, current.key[0] != '\0', slug_, caps_, reply_, sizeof reply_) == 0)
      return kNone;
    return kReply;
  }
  if (n >= 6 && strncmp(p, "CHIRP+", 6) == 0) {
    char field[33] = "";
    ProvResult r = applyPush(p + 6, n - 6, current, pending_, field, sizeof field);
    if (r == ProvResult::Ok) return kStore;
    strcpy(reply_, provResultLine(r));
    if (r == ProvResult::Unsupported) {  // "CHIRP= err unsupported <field>": the name, never a value
      strcat(reply_, " ");
      strcat(reply_, field);
    }
    return kReply;
  }
  return kNone;  // the sketch's own output, boot chatter, or our own lines
}

}  // namespace warblet
