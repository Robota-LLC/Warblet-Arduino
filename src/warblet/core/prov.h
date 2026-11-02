// CHIRP-PROV, the USB setup protocol the Warblet setup page speaks (contract
// §Serial provisioning, v1.8): answer "CHIRP?" with an announce that says what
// this firmware can do, and apply a "CHIRP+ <json>" push as a merge onto the
// stored settings, refusing any field the library does not act on. The rules follow
// the public MicroPython demos' chirp_prov.py; where this file is stricter it
// says so. Portable C++, no Arduino headers, no heap. Host-tested (test/).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace warblet {

static const size_t kMaxNetworks = 8;  // CHIRP-PROV v1.1: at most 8 networks
// The longest setup line a legal push makes is 3,169 bytes: eight networks
// of two-byte characters sent as \u escapes (six bytes each), the first one
// at the top as well, a 128-character id, a 63-character token of
// backslashes, a host and a key. 4096 is the limit every family uses.
static const size_t kProvLineMax = 4096;
// The longest record it can leave is 2,325 bytes of JSON: the same with
// names and passwords of backslashes, which are stored as two bytes each.
static const size_t kSettingsTextMax = 3072;
static const size_t kProvReplyMax = 512;  // CHIRP-PROV v1.8: the longest line a board prints
// A line whose bytes stop for this long is given up (CHIRP-PROV v1.8): longer
// than the 1 s a browser tab out of view waits between pieces of a push.
static const unsigned long kProvLineIdleMs = 1500;
// A host finishes a line within this of its first byte (CHIRP-PROV v1.8): a
// 4,096-byte line in 64-byte pieces a second apart takes about 64 s.
static const unsigned long kProvLineLateMs = 70000;
static const size_t kProvCapsMax = 256;   // ... and the longest capability object
static const size_t kFirmwareMax = 64;    // fw=: 1 to 64 of A-Z a-z 0-9 . _ -
// The version a sketch announces when it names none (board.firmware()):
// "warblet-arduino-" and this library's version, as library.properties has it.
static const char kLibraryFirmware[] = "warblet-arduino-0.2.0";

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
  char credential[64];   // the device token, or a claim code ("CHIRP-..."), 1 to 63
  char hwId[129];
  char host[64];         // an override, 0 to 63 of A-Z a-z 0-9 . -; empty means the built-in default
  char key[45];          // the signing key as base64, or empty

  void clear();
  // Complete enough to report with: a network, a credential and an id.
  bool ready() const;
  // The networks to try, first the top-level one, then the list, without
  // repeating a name, one at a time and without copying: the i-th network
  // (from 0). Returns false past the end.
  bool network(size_t i, const char** ssidOut, const char** passOut) const;
};

enum class ProvResult { Ok, BadJson, MissingField, StoreFailed, Unsupported };

// "CHIRP= ok", "CHIRP= err bad-json", ... ("CHIRP= err unsupported" without the
// field name, which ProvResponder adds.)
const char* provResultLine(ProvResult r);

// Merge one push onto stored, writing the result to merged (which may not be
// the same object as stored). Only Ok means merged is complete and valid.
// CHIRP-PROV v1.8: the library acts on ssid, pass, aps, claim, token, hwid,
// host and key, and nothing else. Any other field makes the push Unsupported,
// and the name of the one that sorts first (byte by byte) is written to
// refused; a name that is not 1 to 32 of A-Z a-z 0-9 _ . - cannot be named and
// is bad-json. The answers come in the contract's order: not one JSON object,
// then a field it does not act on, then a bad value (an empty claim, token or
// key among them), then missing-field.
// A push that carries a setup code ("claim"), or an id other than the stored
// one, starts a new device: the stored signing key is dropped unless the same
// push carries "key". Stored settings load as a push onto empty settings, and
// carry their own key.
// Stricter than chirp_prov.py, on purpose, because these values go into
// fixed buffers and a key that cannot decode would sign nothing useful:
//   - a string longer than its field above is bad-json;
//   - a non-empty key must be 44 base64 characters that decode to 32 bytes
//     (the rule the bare-metal C demo's own tests apply);
//   - more than 8 networks is bad-json (chirp_prov.py also refuses it).
ProvResult applyPush(const char* json, size_t len, const Settings& stored, Settings& merged,
                     char* refused = nullptr, size_t refusedCap = 0);

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

// A firmware version as fw= takes it: 1 to 64 of A-Z a-z 0-9 . _ -, starting
// with a letter or a digit.
bool validFirmware(const char* fw);

// The capability object (CHIRP-PROV v1.8), compact, keys in the contract's
// order: Token and Signed; the key over USB, and also a first key on an HTTPS
// command check when bootstrap is set; no over-the-air rotation or update;
// the transport, https or mqtts. Returns the length, or 0 if it does not fit.
size_t provCaps(bool mqtts, bool bootstrap, char* out, size_t cap);

// "CHIRP! v=1 hw=<id> fw=<fw> key_set=<0|1> radios=wifi t=<slug> caps=<caps>"
// (CHIRP-PROV v1.8): hw= left out when there is no id, t= when there is no
// slug or it breaks the grammar; caps= always last. Returns the length, or 0
// if it does not fit cap or the contract's 512.
size_t provAnnounce(const char* hwId, const char* fw, bool keySet, const char* slug, const char* caps,
                    char* out, size_t cap);

// Reads the serial line one character at a time and decides what to answer.
// Lines that do not start with CHIRP are the sketch's own and are ignored.
class ProvResponder {
 public:
  enum Event {
    kNone,   // nothing to do
    kReply,  // print reply() as one line
    kStore,  // store pending(); then print stored(true or false) as one line
  };
  explicit ProvResponder(Settings& scratch);
  Event feed(char c, const Settings& current, unsigned long nowMs);
  const char* reply() const { return reply_; }
  const Settings& pending() const { return pending_; }
  // The demo slug the announce carries; "" (the default) for none.
  void setSlug(const char* slug);
  // The version the announce carries (kLibraryFirmware unless set); false,
  // and no change, for one that breaks the fw= grammar.
  bool setFirmware(const char* fw);
  // The capability object the announce carries (provCaps(false, false) unless set).
  void setCaps(const char* caps);
  const char* stored(bool ok) const { return provResultLine(ok ? ProvResult::Ok : ProvResult::StoreFailed); }
  // True while a setup line (one that starts CHIRP) is arriving: begun, not
  // over the limit, its last byte under kProvLineIdleMs ago and its first
  // under kProvLineLateMs ago.
  bool lineArriving(unsigned long nowMs) const;

 private:
  Event line(const Settings& current);
  Settings& pending_;
  char buf_[kProvLineMax + 1];
  size_t len_ = 0;
  bool overflow_ = false;
  unsigned long lastMs_ = 0;   // when the line's last byte came
  unsigned long firstMs_ = 0;  // when its first byte came
  // The longest reply is the announce, at most 512 bytes (CHIRP-PROV v1.8):
  // a 128-byte id, a 64-byte version, a 40-byte slug and the capability
  // object take about 450.
  char reply_[kProvReplyMax + 1];
  char slug_[41] = "";
  char fw_[kFirmwareMax + 1];
  char caps_[kProvCapsMax + 1];
};

}  // namespace warblet
