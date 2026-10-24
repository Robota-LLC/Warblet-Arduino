// CHIRP-PROV, the USB setup protocol the Warblet setup page speaks (contract
// §Serial provisioning, v1.6): answer "CHIRP?" with an announce, and apply a
// "CHIRP+ <json>" push as a merge onto the stored settings. The rules follow
// the public MicroPython demos' chirp_prov.py; where this file is stricter it
// says so. Portable C++, no Arduino headers, no heap. Host-tested (test/).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace warblet {

static const size_t kMaxNetworks = 8;  // CHIRP-PROV v1.1: at most 8 networks
static const size_t kProvLineMax = 2048;

struct Network {
  char ssid[33];  // 802.11 allows 32 bytes
  char pass[65];  // WPA2 allows 63 characters, or 64 hex digits
};

// Everything the board keeps between restarts. Every string is NUL-ended;
// an empty string means "not set".
struct Settings {
  char ssid[33];
  char pass[65];
  Network aps[kMaxNetworks];  // the full list when the setup page sent one
  uint8_t apsCount;
  bool hasAps;
  char credential[129];  // the device token, or a claim code ("CHIRP-...")
  char hwId[129];
  char host[129];        // an override; empty means the built-in default
  char key[45];          // the signing key as base64, or empty

  void clear();
  // Complete enough to report with: a network, a credential and an id.
  bool ready() const;
  // The networks to try, first the top-level one, then the list, without
  // repeating a name. Returns how many were written.
  size_t networks(Network* out, size_t max) const;
  // The same order, one at a time and without copying: the i-th network
  // (from 0). Returns false past the end.
  bool network(size_t i, const char** ssidOut, const char** passOut) const;
};

enum class ProvResult { Ok, BadJson, MissingField, StoreFailed };

// "CHIRP= ok", "CHIRP= err bad-json", ...
const char* provResultLine(ProvResult r);

// Merge one push onto stored, writing the result to merged (which may not be
// the same object as stored). Only Ok means merged is complete and valid.
// Stricter than chirp_prov.py, on purpose, because these values go into
// fixed buffers and a key that cannot decode would sign nothing useful:
//   - a string longer than its field above is bad-json;
//   - a non-empty key must be 44 base64 characters that decode to 32 bytes
//     (the rule the bare-metal C demo's own tests apply);
//   - more than 8 networks is bad-json (chirp_prov.py also refuses it).
ProvResult applyPush(const char* json, size_t len, const Settings& stored, Settings& merged);

// Settings as one JSON object, the same shape as a push, for storage. Loading
// is applyPush() of that text onto empty settings. Returns the length, or 0
// if it does not fit.
size_t settingsToJson(const Settings& s, char* out, size_t cap);

// A fingerprint of a setup: the first 8 bytes of SHA-256 over the device id,
// the credential and the signing key, as 16 hex characters. A board keeps it
// after Warblet answered "unclaimed", never the code itself, so it can tell
// after a restart that it still holds the setup Warblet refused, and that a
// new USB setup is a different one. Wi-Fi changes do not change it.
void setupFingerprint(const Settings& s, char out[17]);

// A demo's slug, as the setup page names demos: 3 to 40 of a-z, 0-9 and -.
bool validSlug(const char* slug);

// "CHIRP! v=1 hw=<id> radios=wifi sig=1 t=<slug>": hw= left out when there is
// no id, t= when there is no slug or it breaks the grammar (CHIRP-PROV v1.6:
// the demo this firmware is, which the setup page waits for).
size_t provAnnounce(const char* hwId, const char* slug, char* out, size_t cap);

// Reads the serial line one character at a time and decides what to answer.
// Lines that do not start with CHIRP are the sketch's own and are ignored.
class ProvResponder {
 public:
  enum Event {
    kNone,   // nothing to do
    kReply,  // print reply() as one line
    kStore,  // store pending(); then print stored(true or false) as one line
  };
  explicit ProvResponder(Settings& scratch) : pending_(scratch) {}
  Event feed(char c, const Settings& current);
  const char* reply() const { return reply_; }
  const Settings& pending() const { return pending_; }
  // The demo slug the announce carries; "" (the default) for none.
  void setSlug(const char* slug);
  const char* stored(bool ok) const { return provResultLine(ok ? ProvResult::Ok : ProvResult::StoreFailed); }

 private:
  Event line(const Settings& current);
  Settings& pending_;
  char buf_[kProvLineMax + 1];
  size_t len_ = 0;
  bool overflow_ = false;
  // The longest reply is the announce: "CHIRP! v=1", " hw=" and a 128-byte
  // id, " radios=wifi sig=1", and " t=" with a 40-byte slug.
  char reply_[10 + 4 + 128 + 18 + 3 + 40 + 1];
  char slug_[41] = "";
};

}  // namespace warblet
