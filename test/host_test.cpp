// Host tests for the portable core (src/warblet/core). No board, no network.
//
// Groups:
//   sha256     FIPS 180-4 examples (the same four the bare-metal demo checks)
//   hmac       RFC 4231 cases 1-4, 6, 7 (the demo checks 1, 2, 4, 6)
//   demo-c     differential: the bare-metal demo's own chirp_sign() (compiled
//              from the reference clone) and this library sign the same
//              inputs; must agree byte for byte
//   vectors    test/vectors.txt, written by test/oracle.py from the public
//              MicroPython chirp_sign.py and Python's own hmac module
//   wire       signed-input bytes, request heads, topics, nonces, tags, ids,
//              the README's example readings, base64, the response reader
//   prov       CHIRP-PROV: announce, pushes, merges, refusals, storage
//
// Build and run: test/run_tests.sh (or make -C test). Exit code 0 = all pass.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../src/warblet/core/json.h"
#include "../src/warblet/core/prov.h"
#include "../src/warblet/core/sha256.h"
#include "../src/warblet/core/sign.h"
#include "../src/warblet/core/text.h"
#include "../src/warblet/core/wire.h"

#ifdef WITH_DEMO_C
extern "C" void chirp_sign(const unsigned char* key, unsigned klen, const void* msg,
                           unsigned mlen, char out_hex[65]);
#endif

using namespace warblet;

namespace {

struct Group {
  const char* name;
  int pass = 0, fail = 0;
};
std::vector<Group> groups;
Group* cur = nullptr;

void group(const char* name) {
  groups.push_back(Group());
  groups.back().name = name;
  cur = &groups.back();
  printf("\n--- %s ---\n", name);
}

void check(bool ok, const char* what, const std::string& detail = "") {
  if (ok) {
    cur->pass++;
  } else {
    cur->fail++;
    printf("FAIL  %s %s\n", what, detail.c_str());
  }
}

void checkStr(const std::string& got, const std::string& want, const char* what) {
  check(got == want, what, "\n      want " + want + "\n      got  " + got);
}

std::vector<uint8_t> unhex(const std::string& h) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back((uint8_t)strtoul(h.substr(i, 2).c_str(), nullptr, 16));
  return out;
}

std::string hexOf(const uint8_t* p, size_t n) {
  std::string s(n * 2, '0');
  toHex(p, n, &s[0]);
  return s;
}

// ---------------------------------------------------------------------------

void testSha256() {
  group("sha256");
  struct { const char* msg; const char* want; } v[] = {
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
       "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
  };
  for (auto& t : v) {
    Sha256 s;
    s.update(t.msg, strlen(t.msg));
    uint8_t d[32];
    s.finish(d);
    checkStr(hexOf(d, 32), t.want, "sha256 FIPS example");
  }
  // One million 'a', fed in uneven pieces (FIPS 180-4 long example).
  Sha256 s;
  std::string chunk(997, 'a');
  size_t left = 1000000;
  while (left) {
    size_t n = left < chunk.size() ? left : chunk.size();
    s.update(chunk.data(), n);
    left -= n;
  }
  uint8_t d[32];
  s.finish(d);
  checkStr(hexOf(d, 32), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
           "sha256 of a million 'a' in 997-byte pieces");
}

void testHmac() {
  group("hmac");
  auto hm = [](const std::vector<uint8_t>& k, const std::string& m) {
    HmacSha256 h(k.data(), k.size());
    h.update(m.data(), m.size());
    uint8_t d[32];
    h.finish(d);
    return hexOf(d, 32);
  };
  checkStr(hm(std::vector<uint8_t>(20, 0x0b), "Hi There"),
           "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "RFC 4231 case 1");
  checkStr(hm(std::vector<uint8_t>{'J', 'e', 'f', 'e'}, "what do ya want for nothing?"),
           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "RFC 4231 case 2");
  checkStr(hm(std::vector<uint8_t>(20, 0xaa), std::string(50, (char)0xdd)),
           "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", "RFC 4231 case 3");
  std::vector<uint8_t> k4;
  for (int i = 1; i <= 25; i++) k4.push_back((uint8_t)i);
  checkStr(hm(k4, std::string(50, (char)0xcd)),
           "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", "RFC 4231 case 4");
  checkStr(hm(std::vector<uint8_t>(131, 0xaa), "Test Using Larger Than Block-Size Key - Hash Key First"),
           "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "RFC 4231 case 6");
  checkStr(hm(std::vector<uint8_t>(131, 0xaa),
              "This is a test using a larger than block-size key and a larger than block-size data. "
              "The key needs to be hashed before being used by the HMAC algorithm."),
           "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", "RFC 4231 case 7");
}

// Deterministic pseudo-random bytes, so a failure reproduces.
uint32_t rng = 0x5eed1234;
inline uint8_t rnd() {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return (uint8_t)rng;
}

void testDemoC() {
  group("demo-c");
#ifdef WITH_DEMO_C
  // The bare-metal demo signs whatever bytes it is handed; the library builds
  // the contract's signed input and signs it in pieces. Feed the demo the
  // library's own signedInput() bytes and compare, over many shapes: with and
  // without nonce and tag, empty bodies, bodies across block boundaries.
  const char* tags[] = {"", "snap", "heartbeat", "a", "0123456789abcdefghijklmnopqrstuv"};
  uint64_t nonces[] = {0, 1735689600000ULL, 1759363200123ULL, 18446744073709551615ULL};
  int n = 0;
  for (int round = 0; round < 50; round++) {
    uint8_t key[32];
    for (auto& b : key) b = rnd();
    size_t len = (size_t)(round * 7 % 300);
    std::vector<uint8_t> body(len);
    for (auto& b : body) b = rnd();
    for (const char* tag : tags) {
      for (uint64_t nonce : nonces) {
        std::vector<uint8_t> input(signedInput(nonce, tag, body.data(), len, nullptr, 0));
        signedInput(nonce, tag, body.data(), len, input.data(), input.size());
        char demo[65], lib[65];
        chirp_sign(key, 32, input.data(), (unsigned)input.size(), demo);
        signHeader(key, 32, nonce, tag, body.data(), len, lib);
        check(strcmp(demo, lib) == 0, "header signature agrees with the demo's chirp_sign()");
        if (nonce == 0) {
          uint8_t mac[32];
          signEnvelope(key, 32, tag, body.data(), len, mac);
          check(hexOf(mac, 32) == demo, "envelope prefix agrees with the demo's chirp_sign()");
        }
        n++;
      }
    }
  }
  // The poll challenge, built by hand from the contract's words.
  for (int round = 0; round < 20; round++) {
    uint8_t key[32];
    for (auto& b : key) b = rnd();
    std::string hw = "board-" + std::to_string(round);
    uint64_t nonce = 1759363200000ULL + (uint64_t)round * 1013;
    std::string msg = "chirp-down\n" + hw + "\n" + std::to_string(nonce);
    char demo[65], lib[65];
    chirp_sign(key, 32, msg.data(), (unsigned)msg.size(), demo);
    signPoll(key, 32, hw.c_str(), nonce, lib);
    check(strcmp(demo, lib) == 0, "poll challenge agrees with the demo's chirp_sign()");
  }
  // The bare-metal demo's own example frame (valtest: key 00..1f, body 00 c9).
  uint8_t key[32];
  for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
  // Under v0.31 the frame signs "\ntag=\n" + 00 c9, no longer the body alone.
  uint8_t body[2] = {0x00, 0xc9};
  uint8_t input[16];
  size_t inLen = signedInput(0, nullptr, body, 2, input, sizeof input);
  char demo[65], old[65], lib[65];
  chirp_sign(key, 32, input, (unsigned)inLen, demo);
  chirp_sign(key, 32, body, 2, old);
  signHeader(key, 32, 0, nullptr, body, 2, lib);
  check(strcmp(demo, lib) == 0, "the demo's example frame (key 00..1f, body 00c9), signed over the v0.31 input");
  check(strcmp(old, lib) != 0, "...and no longer equal to the v1.0.2 body-only signature");
  printf("      demo frame X-Chirp-Signature (v0.31) = %s\n", lib);
  printf("      %d signed-input shapes compared\n", n);
#else
  printf("SKIP  the reference clone was not found; set REF= to the Warblet-IoT v1.0.2 clone\n");
#endif
}

#ifdef WITH_DEMO_CAM
extern "C" int cam_oracle(int post, const uint8_t key[32], const char* hwid, const char* tag, const uint8_t* body,
                          size_t len);
extern "C" const char* stub_header(const char* name);
#endif

// The shipped HTTPS camera demo (v1.0.2) builds its own signed input and its
// own downlink challenge, in C, hashed by the bare-metal demo's SHA-256:
// nothing of this library is in that path. It signs the v0.29 shape, which
// v0.31 says agrees with the new one only when BOTH a nonce and a tag are
// present. So: with a tag, the signatures must be equal (an independent check
// of the nonce-line/tag-line layout and the HMAC); without one they must
// DIFFER (the cut is real); the poll challenge, which v0.31 does not move,
// must be equal.
void testDemoCam() {
  group("demo-cam");
#ifdef WITH_DEMO_CAM
  const char* tags[] = {nullptr, "snap", "heartbeat", "0123456789abcdefghijklmnopqrstuv"};
  int n = 0, differ = 0;
  for (int round = 0; round < 40; round++) {
    uint8_t key[32];
    for (auto& b : key) b = rnd();
    size_t len = (size_t)(round * 13 % 301);
    std::vector<uint8_t> body(len);
    for (auto& b : body) b = rnd();
    if (round == 1) {
      const char* tricky = "1759363200123\ntag=snap\n\x00\xc9";  // looks like a prefix, is a body
      body.assign(tricky, tricky + 25);
    }
    for (const char* tag : tags) {
      check(cam_oracle(1, key, "cam-0a1b2c", tag, body.data(), body.size()) == 0, "camera demo signs");
      std::string nonceText = stub_header("X-Chirp-Nonce");
      uint64_t nonce = strtoull(nonceText.c_str(), nullptr, 10);
      char lib[65];
      signHeader(key, 32, nonce, tag, body.data(), body.size(), lib);
      if (tag != nullptr) {
        check(nonce != 0 && strcmp(stub_header("X-Chirp-Signature"), lib) == 0,
              "nonce + tag: signature equals the camera demo's (both shapes agree here)",
              std::string("tag=") + tag + " nonce=" + nonceText);
        n++;
      } else {
        check(nonce != 0 && strcmp(stub_header("X-Chirp-Signature"), lib) != 0,
              "no tag: signature differs from the v1.0.2 camera demo's (v0.31 keeps the empty tag line)");
        differ++;
      }
    }
    std::string hw = "cam-" + std::to_string(round * 7919);
    check(cam_oracle(0, key, hw.c_str(), nullptr, nullptr, 0) == 0, "camera demo signs a poll");
    uint64_t nonce = strtoull(stub_header("X-Chirp-Nonce"), nullptr, 10);
    char lib[65];
    signPoll(key, 32, hw.c_str(), nonce, lib);
    check(nonce != 0 && strcmp(stub_header("X-Chirp-Signature"), lib) == 0,
          "poll challenge equals the camera demo's chirp-down signature");
    n++;
  }
  printf("      %d signatures equal to the camera demo's own builder; %d untagged ones differ, as v0.31 says\n", n,
         differ);
#else
  printf("SKIP  the reference clone was not found; set REF= to the Warblet-IoT v1.0.2 clone\n");
#endif
}

// test/vectors.txt: one vector per line, fields separated by single spaces:
//   header   <keyb64> <nonce|-> <tag|-> <bodyhex|-> <hex>
//   envelope <keyb64> <tag|-> <bodyhex|-> <hex>
//   poll     <keyb64> <hwid> <nonce> <hex>
//   hmac     <keyb64> <msghex|-> <hex>         (the demos' hmac_sha256 primitive)
void testVectors(const char* path) {
  group("vectors");
  FILE* f = fopen(path, "r");
  if (!f) {
    check(false, "open vectors file", path);
    return;
  }
  char buf[4096];
  int n = 0;
  while (fgets(buf, sizeof buf, f)) {
    if (buf[0] == '#' || buf[0] == '\n') continue;
    std::vector<std::string> fld;
    char* save = nullptr;
    for (char* t = strtok_r(buf, " \r\n", &save); t; t = strtok_r(nullptr, " \r\n", &save)) fld.push_back(t);
    auto dash = [](const std::string& s) { return s == "-" ? std::string() : s; };
    uint8_t key[32];
    if (fld.size() < 2 || !decodeKey(fld[1].c_str(), fld[1].size(), key)) {
      check(false, "vector key decodes", fld.size() > 1 ? fld[1] : "");
      continue;
    }
    char hex[65];
    if (fld[0] == "header" && fld.size() == 6) {
      std::vector<uint8_t> body = unhex(dash(fld[4]));
      uint64_t nonce = fld[2] == "-" ? 0 : strtoull(fld[2].c_str(), nullptr, 10);
      signHeader(key, 32, nonce, dash(fld[3]).c_str(), body.data(), body.size(), hex);
      checkStr(hex, fld[5], "header vector");
    } else if (fld[0] == "envelope" && fld.size() == 5) {
      std::vector<uint8_t> body = unhex(dash(fld[3]));
      uint8_t mac[32];
      signEnvelope(key, 32, dash(fld[2]).c_str(), body.data(), body.size(), mac);
      checkStr(hexOf(mac, 32), fld[4], "envelope vector");
    } else if (fld[0] == "hmac" && fld.size() == 4) {
      std::vector<uint8_t> msg = unhex(dash(fld[2]));
      HmacSha256 h(key, 32);
      h.update(msg.data(), msg.size());
      uint8_t mac[32];
      h.finish(mac);
      checkStr(hexOf(mac, 32), fld[3], "hmac vector (the demos' own hmac_sha256)");
    } else if (fld[0] == "poll" && fld.size() == 5) {
      signPoll(key, 32, fld[2].c_str(), strtoull(fld[3].c_str(), nullptr, 10), hex);
      checkStr(hex, fld[4], "poll vector");
    } else {
      check(false, "vector line shape", fld[0]);
      continue;
    }
    n++;
  }
  fclose(f);
  check(n >= 20, "the vectors file holds the expected cases");
  printf("      %d vectors checked\n", n);
}

void testWire() {
  group("wire");
  // Signed input (contract v0.31 §Auth): <nonce> "\n" "tag=" <tag> "\n" <body>,
  // both lines always present, empty fields left empty.
  const uint8_t body[2] = {0x00, 0xc9};
  uint8_t out[64];
  size_t n = signedInput(0, nullptr, body, 2, out, sizeof out);
  check(n == 8 && memcmp(out, "\ntag=\n\x00\xc9", 8) == 0, "neither field: \"\\ntag=\\n\" + body");
  n = signedInput(1759363200123ULL, nullptr, body, 2, out, sizeof out);
  check(n == 21 && memcmp(out, "1759363200123\ntag=\n\x00\xc9", 21) == 0, "nonce only: the tag line is still there");
  n = signedInput(0, "snap", body, 2, out, sizeof out);
  check(n == 12 && memcmp(out, "\ntag=snap\n\x00\xc9", 12) == 0, "tag only: the empty nonce field is still there");
  n = signedInput(1759363200123ULL, "snap", body, 2, out, sizeof out);
  check(n == 25 && memcmp(out, "1759363200123\ntag=snap\n\x00\xc9", 25) == 0,
        "both: nonce line, then tag line, then body");
  check(signedInput(1759363200123ULL, "snap", body, 2, out, 24) == 25, "too small a buffer reports the need");
  check(signedInput(0, "", body, 2, out, sizeof out) == 8, "an empty tag is an empty tag field");
  n = signedInput(0, nullptr, nullptr, 0, out, sizeof out);
  check(n == 6 && memcmp(out, "\ntag=\n", 6) == 0, "an empty message signs \"\\ntag=\\n\"");

  // The contract's three worked examples (v0.31 §Auth), byte for byte: key =
  // the 32 ASCII bytes 0123456789abcdef0123456789abcdef, payload temp=21.4.
  {
    const uint8_t* key = reinterpret_cast<const uint8_t*>("0123456789abcdef0123456789abcdef");
    const uint8_t* p = reinterpret_cast<const uint8_t*>("temp=21.4");
    uint8_t in[64];
    char hex[65];
    size_t m = signedInput(1757332800123ULL, "snap", p, 9, in, sizeof in);
    checkStr(hexOf(in, m), "313735373333323830303132330a7461673d736e61700a74656d703d32312e34",
             "worked example 1 (HTTP, nonce + tag): signed input");
    signHeader(key, 32, 1757332800123ULL, "snap", p, 9, hex);
    checkStr(hex, "1239148f97a361adfb7173445a84a68a4f0b3eb915d626b98e1080f7e6f78e81",
             "worked example 1 (HTTP, nonce + tag): hmac");
    m = signedInput(0, nullptr, p, 9, in, sizeof in);
    checkStr(hexOf(in, m), "0a7461673d0a74656d703d32312e34", "worked example 2 (HTTP, neither field): signed input");
    signHeader(key, 32, 0, nullptr, p, 9, hex);
    checkStr(hex, "fd071ea6328d4ca04bf766d76c4298359a07db6b161fbc8446b336511954bda2",
             "worked example 2 (HTTP, neither field): hmac");
    m = signedInput(0, "snap", p, 9, in, sizeof in);
    checkStr(hexOf(in, m), "0a7461673d736e61700a74656d703d32312e34",
             "worked example 3 (envelope, tag snap): signed input");
    uint8_t mac[32];
    signEnvelope(key, 32, "snap", p, 9, mac);
    checkStr(hexOf(mac, 32), "aaa23e9e54a70f6c76ff1bfdc282831c3c62c0d574f592fdd831b554bef3e946",
             "worked example 3 (envelope, tag snap): the 32-byte prefix");
  }

  // The forgery v0.31 closes: (nonce N, tag T, body B) and (nothing, body
  // "N\ntag=T\nB") must sign different bytes.
  {
    uint8_t key[32] = {1};
    const char* forged = "1759363200123\ntag=snap\n\x00\xc9";
    char honest[65], moved[65];
    signHeader(key, 32, 1759363200123ULL, "snap", body, 2, honest);
    signHeader(key, 32, 0, nullptr, reinterpret_cast<const uint8_t*>(forged), 25, moved);
    check(strcmp(honest, moved) != 0, "moving the nonce and tag lines into the body breaks the signature");
  }

  // Tags and ids.
  check(validTag("snap") && validTag("a.b_c-9") && validTag(std::string(32, 'x').c_str()), "valid tags");
  check(!validTag("") && !validTag(std::string(33, 'x').c_str()) && !validTag("a b") &&
            !validTag("a/b") && !validTag("a+b") && !validTag("a#"),
        "invalid tags: empty, 33 long, space, slash, + and #");
  check(validHwId("sunny-otter-7c2f") && validHwId("A.b_c:9-z"), "valid ids");
  check(!validHwId("") && !validHwId("two words") && !validHwId("a/b") &&
            !validHwId(std::string(129, 'a').c_str()) && validHwId(std::string(128, 'a').c_str()),
        "invalid ids: empty, space, slash, 129 long; 128 fits");
  check(isClaimCode("CHIRP-ABCDEFGH") && !isClaimCode("tok-123"), "claim code recognised by its prefix");

  // MQTT topics (contract §Device ingest → MQTT, Tags).
  {
    char t[96];
    topicUp("sunny-otter-7c2f", nullptr, t, sizeof t);
    checkStr(t, "chirp/sunny-otter-7c2f/up", "uplink topic");
    topicUp("sunny-otter-7c2f", "button", t, sizeof t);
    checkStr(t, "chirp/sunny-otter-7c2f/up/button", "tagged uplink topic: exactly one extra level");
    check(topicUp("x", "a/b", t, sizeof t) == 0 && topicUp("x", "+", t, sizeof t) == 0 &&
              topicUp("x", "#", t, sizeof t) == 0,
          "a tag that would add a level, or a wildcard, is refused");
    topicDown("sunny-otter-7c2f", t, sizeof t);
    checkStr(t, "chirp/sunny-otter-7c2f/down", "downlink topic");
    check(topicUp(std::string(100, 'a').c_str(), nullptr, t, sizeof t) == 0, "a topic that does not fit is refused");
  }

  // Control characters.
  check(hasControlChar("evil.example\r\nX-Injected: 1") && hasControlChar("a\tb") && hasControlChar("\x7f") &&
            !hasControlChar("caf\xc3\xa9 \"guest\"") && !hasControlChar(""),
        "control characters found; UTF-8 and quotes are fine");

  // The longest head fits kHttpHeadMax: 128-character host, id and
  // token (a claim code, so it is sent twice), a 32-character tag, a 20-digit
  // nonce and a signature.
  {
    std::string h128(128, 'h'), id128(128, 'i'), tok128 = "CHIRP-" + std::string(122, 't');
    std::string tag32(32, 'g'), sig(64, 'a');
    static char big[kHttpHeadMax];
    HttpIngest worst = {h128.c_str(), id128.c_str(), tok128.c_str(), tag32.c_str(),
                        18446744073709551615ULL, sig.c_str(), 4294967295u};
    size_t wl = httpIngestHead(worst, big, sizeof big);
    check(wl > 0, "the longest ingest head fits kHttpHeadMax");
    printf("      longest ingest head: %u of %u bytes\n", (unsigned)wl, (unsigned)kHttpHeadMax);
    HttpPoll worstPoll = {h128.c_str(), id128.c_str(), tok128.c_str(), 18446744073709551615ULL, sig.c_str()};
    size_t pl = httpPollHead(worstPoll, big, sizeof big);
    check(pl > 0, "the longest poll head fits kHttpHeadMax");
    printf("      longest poll head:   %u of %u bytes\n", (unsigned)pl, (unsigned)kHttpHeadMax);
  }

  // Nonces.
  NonceClock c;
  check(c.next(5000) == 0, "no clock: no nonce");
  check(c.next(1759363200000ULL) == 1759363200000ULL, "a set clock is the nonce");
  check(c.next(1759363200000ULL) == 1759363200001ULL, "same millisecond: bumped by one");
  check(c.next(1759363199000ULL) == 1759363200002ULL, "clock stepped back: still increasing");
  check(c.next(1759363300000ULL) == 1759363300000ULL, "clock moves on: follows it");

  // Request heads. The ingest head matches the MicroPython HTTPS demo's order.
  HttpIngest in = {"https.warbletiot.com", "sunny-otter-7c2f", "YOUR_DEVICE_TOKEN", nullptr, 0, nullptr, 2};
  char head[600];
  httpIngestHead(in, head, sizeof head);
  checkStr(head,
           "POST /ingest/sunny-otter-7c2f HTTP/1.1\r\nHost: https.warbletiot.com\r\nContent-Length: 2\r\n"
           "Connection: close\r\nContent-Type: application/octet-stream\r\nX-Chirp-Token: YOUR_DEVICE_TOKEN\r\n\r\n",
           "ingest head at Token");
  in.credential = "CHIRP-ABCDEFGH";
  in.tag = "button";
  in.nonce = 1759363200123ULL;
  in.signature = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
  httpIngestHead(in, head, sizeof head);
  checkStr(head,
           "POST /ingest/sunny-otter-7c2f HTTP/1.1\r\nHost: https.warbletiot.com\r\nContent-Length: 2\r\n"
           "Connection: close\r\nContent-Type: application/octet-stream\r\nX-Chirp-Token: CHIRP-ABCDEFGH\r\n"
           "X-Chirp-Claim: CHIRP-ABCDEFGH\r\nX-Chirp-Tag: button\r\nX-Chirp-Nonce: 1759363200123\r\n"
           "X-Chirp-Signature: 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\r\n\r\n",
           "ingest head with claim code, tag, nonce and signature");
  in.credential = nullptr;
  in.tag = nullptr;
  in.nonce = 0;
  in.signature = nullptr;
  httpIngestHead(in, head, sizeof head);
  check(strstr(head, "X-Chirp-Token") == nullptr, "Open: no token header");
  check(httpIngestHead(in, head, 40) == 0, "a head that does not fit is refused, not cut");

  // Collecting a command is a POST with an empty body (contract v0.43): a GET
  // with a claim code, or with no credential at all, is answered 405.
  HttpPoll pl = {"https.warbletiot.com", "sunny-otter-7c2f", "YOUR_DEVICE_TOKEN", 0, nullptr};
  httpPollHead(pl, head, sizeof head);
  checkStr(head,
           "POST /ingest/sunny-otter-7c2f/down HTTP/1.1\r\nHost: https.warbletiot.com\r\nContent-Length: 0\r\n"
           "Connection: close\r\nX-Chirp-Token: YOUR_DEVICE_TOKEN\r\n\r\n",
           "poll head at Token: POST, empty body (v0.43)");
  pl.credential = "CHIRP-ABCDEFGH";
  httpPollHead(pl, head, sizeof head);
  check(strncmp(head, "POST /ingest/sunny-otter-7c2f/down HTTP/1.1\r\n", 45) == 0,
        "poll head with a claim code is a POST too (a GET with one is 405)");
  pl.credential = nullptr;
  httpPollHead(pl, head, sizeof head);
  check(strncmp(head, "POST ", 5) == 0 && strstr(head, "X-Chirp-Token") == nullptr,
        "poll head at Open: POST, no token header");
  pl.credential = "YOUR_DEVICE_TOKEN";
  pl.nonce = 1759363200124ULL;
  pl.signature = "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";
  httpPollHead(pl, head, sizeof head);
  check(strstr(head, "X-Chirp-Nonce: 1759363200124\r\nX-Chirp-Signature: ffeedd") != nullptr,
        "poll head at Signed carries nonce and signature");

  // Readings: the README examples of the public temperature demos.
  struct { double v; uint8_t a, b; const char* where; } r[] = {
      {20.1, 0x00, 0xc9, "nucleo-wba65-baremetal README: 00 c9 = 20.1"},
      {28.7, 0x01, 0x1f, "firebeetle-c5 README: 01 1f = 28.7"},
      {21.5, 0x00, 0xd7, "esp32-generic README: 00 d7 = 21.5"},
      {-5.0, 0xff, 0xce, "esp32-generic README: ff ce = -5.0"},
  };
  for (auto& e : r) {
    uint8_t o[2] = {0, 0};
    check(encodeTenths(e.v, o) && o[0] == e.a && o[1] == e.b, e.where);
  }
  uint8_t o[2] = {0x12, 0x34};
  check(encodeTenths(3276.7, o) && o[0] == 0x7f && o[1] == 0xff, "3276.7 is the top");
  check(encodeTenths(-3276.8, o) && o[0] == 0x80 && o[1] == 0x00, "-3276.8 is the bottom");
  o[0] = 0x12;
  check(!encodeTenths(3276.8, o) && !encodeTenths(-3277.0, o) && !encodeTenths(NAN, o) && o[0] == 0x12,
        "out of range and NaN are refused, nothing written");

  // Numbers as text (sendNumber). On the platform, a dry run decoded
  // "21.53", "-5.0" and "101325" with float(str(payload)).
  {
    char t[kNumberTextMax];
    formatReal(21.53, 15, t);
    checkStr(t, "21.53", "double 21.53 -> \"21.53\"");
    formatReal(21.53f, 7, t);
    checkStr(t, "21.53", "float 21.53f -> \"21.53\", not 21.5300006866455");
    formatReal(-5.0, 15, t);
    checkStr(t, "-5.0", "double -5.0 -> \"-5.0\" (still a real number)");
    formatInteger(101325, t);
    checkStr(t, "101325", "pressure in pascals, past sendTenths' range, is exact");
    formatInteger(-5, t);
    checkStr(t, "-5", "int -5 -> \"-5\"");
    formatInteger(-9223372036854775807LL - 1, t);
    checkStr(t, "-9223372036854775808", "the most negative 64-bit value");
    formatUnsigned(4294967295UL, t);
    checkStr(t, "4294967295", "a 32-bit unsigned value, e.g. millis()");
    formatReal(51.5007292, 15, t);
    checkStr(t, "51.5007292", "a GPS coordinate keeps its digits");
    formatReal(1759363200123.0, 15, t);
    checkStr(t, "1759363200123.0", "a large whole double");
    formatReal(1e20, 15, t);
    checkStr(t, "1e+20", "beyond 15 digits: exponent form (not yet decoded on the platform)");
    formatReal(0.1 + 0.2, 15, t);
    checkStr(t, "0.3", "0.1 + 0.2 at 15 digits is 0.3");
    check(formatReal(NAN, 15, t) == 0 && formatReal(INFINITY, 15, t) == 0 && formatReal(-INFINITY, 7, t) == 0,
          "NaN and infinity have no text");
  }

  // Base64: the firebeetle README's downlink example, and keys.
  uint8_t dec[32];
  int dn = base64Decode("bGVkOm9u", 8, dec, sizeof dec);
  check(dn == 6 && memcmp(dec, "led:on", 6) == 0, "firebeetle-c5 README: bGVkOm9u = led:on");
  check(decodeKey("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=", 44, dec) && dec[0] == 0 && dec[31] == 31,
        "the demos' example key decodes to bytes 00..1f");
  check(!decodeKey("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8", 43, dec), "key without its '=' pad");
  check(!decodeKey("c2hvcnQ=", 8, dec), "key that is not 32 bytes");
  check(!decodeKey("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdH!8=", 44, dec), "key with a character outside base64");
  check(base64Decode("ab=c", 4, dec, sizeof dec) < 0 && base64Decode("abc", 3, dec, sizeof dec) < 0,
        "padding in the middle, or a length not a multiple of 4");

  // Downlink frames.
  const char* keyFrame = "CHIRPKEY1 AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\n";
  check(classifyDownlink((const uint8_t*)keyFrame, strlen(keyFrame)) == Downlink::Key, "CHIRPKEY1 frame is a key");
  check(classifyDownlink((const uint8_t*)"led:on", 6) == Downlink::Command, "led:on is a command");
  check(classifyDownlink((const uint8_t*)"CHIRPKEY1", 9) == Downlink::Command, "the magic needs its space");
  check(classifyDownlink(nullptr, 0) == Downlink::Empty, "nothing is empty");
  char kb[45];
  check(parseKeyFrame((const uint8_t*)keyFrame, strlen(keyFrame), kb) &&
            strcmp(kb, "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=") == 0,
        "key frame: the base64 key comes out as sent");
  const char* noNewline = "CHIRPKEY1 AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
  const char* badKey = "CHIRPKEY1 AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdH!8=\n";
  check(!parseKeyFrame((const uint8_t*)noNewline, strlen(noNewline), kb) &&
            !parseKeyFrame((const uint8_t*)badKey, strlen(badKey), kb) &&
            !parseKeyFrame((const uint8_t*)"led:on", 6, kb),
        "key frame: no newline, a bad character, or not a key frame is refused");

  // The response reader.
  auto parse = [](const std::string& raw, size_t cap, size_t step, HttpResponse*& keep, uint8_t* buf) {
    keep = new HttpResponse(buf, cap);
    for (size_t i = 0; i < raw.size(); i += step) {
      size_t m = raw.size() - i < step ? raw.size() - i : step;
      keep->feed((const uint8_t*)raw.data() + i, m);
    }
    return keep;
  };
  uint8_t bodyBuf[64];
  HttpResponse* rsp = nullptr;
  for (size_t step : {1, 3, 1000}) {
    parse("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nX-Chirp-Down: data\r\n"
          "Content-Length: 6\r\n\r\nled:on",
          sizeof bodyBuf, step, rsp, bodyBuf);
    check(rsp->complete() && rsp->status() == 200 && rsp->bodyLen() == 6 &&
              memcmp(bodyBuf, "led:on", 6) == 0 && strcmp(rsp->downHeader(), "data") == 0,
          "200 with Content-Length, fed in pieces");
    delete rsp;
  }
  parse("HTTP/1.1 204 No Content\r\nDate: x\r\n\r\n", sizeof bodyBuf, 1, rsp, bodyBuf);
  check(rsp->complete() && rsp->status() == 204 && rsp->bodyLen() == 0, "204: nothing queued");
  delete rsp;
  parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nx-chirp-down: key\r\n\r\n"
        "4\r\nCHIR\r\n6;ext=1\r\nPKEY1 \r\n0\r\n\r\n",
        sizeof bodyBuf, 2, rsp, bodyBuf);
  check(rsp->complete() && rsp->bodyLen() == 10 && memcmp(bodyBuf, "CHIRPKEY1 ", 10) == 0 &&
            strcmp(rsp->downHeader(), "key") == 0,
        "chunked body, lowercase header names");
  delete rsp;
  parse("HTTP/1.1 202 Accepted\r\nX-Chirp-Budget: 1000\r\n\r\nstored", sizeof bodyBuf, 5, rsp, bodyBuf);
  check(!rsp->complete() && !rsp->finishAtClose(),
        "no length and no chunking: never complete, since a cut-short reply looks the same");
  delete rsp;
  parse("HTTP/1.1 2000 OK\r\nContent-Length: 0\r\n\r\n", sizeof bodyBuf, 1, rsp, bodyBuf);
  check(rsp->malformed(), "HTTP/1.1 2000 is malformed, not 200");
  delete rsp;
  parse("HTTP/1.1 200\r\nContent-Length: 2\r\n\r\nok", sizeof bodyBuf, 1, rsp, bodyBuf);
  check(rsp->complete() && rsp->status() == 200, "a status line with no reason phrase");
  delete rsp;
  parse("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nX-Chirp-Down: data\r\nContent-Length: 6\r\n\r\nled:on",
        sizeof bodyBuf, 3, rsp, bodyBuf);
  check(rsp->complete() && rsp->status() == 200 && rsp->bodyLen() == 6 && strcmp(rsp->downHeader(), "data") == 0,
        "100 Continue is skipped and the real answer read");
  delete rsp;
  parse("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 120\r\nContent-Length: 0\r\n\r\n", sizeof bodyBuf, 4, rsp,
        bodyBuf);
  check(rsp->complete() && rsp->status() == 429 && rsp->retryAfter() == 120, "Retry-After in seconds is read");
  check(!rsp->hasBudget(), "a rate 429 carries no X-Chirp-Budget");
  delete rsp;
  // The budget 429 carries X-Chirp-Budget (the bytes left, 0); a rate 429
  // never does (contract §Budgets), so the board can say which it was.
  parse("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 3600\r\nx-chirp-budget: 0\r\nContent-Length: 0\r\n\r\n",
        sizeof bodyBuf, 3, rsp, bodyBuf);
  check(rsp->complete() && rsp->hasBudget(), "a budget 429 is told apart by X-Chirp-Budget");
  delete rsp;
  parse("HTTP/1.1 503 Busy\r\nRetry-After: Wed, 21 Oct 2026 07:28:00 GMT\r\nContent-Length: 0\r\n\r\n", sizeof bodyBuf,
        4, rsp, bodyBuf);
  check(rsp->complete() && rsp->retryAfter() == 0, "Retry-After as a date is left at 0");
  delete rsp;

  // The unclaimed answer.
  const char* unclaimed = "{\"status\":\"unclaimed\"}";
  check(answerIsUnclaimed((const uint8_t*)unclaimed, strlen(unclaimed)) &&
            !answerIsUnclaimed((const uint8_t*)"", 0) && !answerIsUnclaimed((const uint8_t*)"{}", 2),
        "202 {\"status\":\"unclaimed\"} is told apart from a stored message");

  // Asking again: 10 s normally; 10 min at most after refusals, 1 min at most
  // when nothing answers, so a board is back soon after an outage.
  Backoff b(10000, 600000, 60000);
  check(b.after(204, 0) == 10000 && !b.failing() && !b.changed(), "success: the normal pace, nothing to say");
  check(b.after(401, 0) == 60000 && b.failing() && b.changed(), "first 401: a minute, and say so");
  check(b.after(401, 0) == 120000 && !b.changed(), "second 401: doubles, says nothing new");
  b.after(401, 0);
  b.after(401, 0);
  b.after(401, 0);
  check(b.after(401, 0) == 600000, "401s level off at the maximum");
  check(b.after(429, 0) == 600000 && b.changed(), "a different refusal is said once");
  check(b.after(200, 0) == 10000 && !b.failing() && b.changed(), "first success after failures: back to pace, say so");
  check(b.after(-1, 0) == 20000 && b.after(503, 0) == 40000, "no answer, then 5xx: doubling from the pace");
  check(b.after(-1, 0) == 60000 && b.after(-1, 0) == 60000 && b.after(502, 0) == 60000,
        "no answer and server errors level off at a minute, not ten");
  for (int i = 0; i < 6; i++) b.after(401, 0);
  check(b.wait() == 600000 && b.after(-1, 0) == 60000, "an outage after refusals waits a minute at most");
  check(b.after(503, 120) == 120000, "a server error's Retry-After is still honoured");
  check(b.after(429, 1800) == 1800000, "Retry-After longer than the maximum is honoured");
  check(b.after(429, 99999) == 3600000, "...up to an hour");

  parse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789", 4, 1, rsp, bodyBuf);
  check(rsp->complete() && rsp->tooLong() && rsp->bodyLen() == 4, "a body past the buffer is reported, not hidden");
  delete rsp;
  parse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123", sizeof bodyBuf, 1, rsp, bodyBuf);
  check(!rsp->complete() && !rsp->finishAtClose(), "a body cut short is not complete");
  delete rsp;
  parse("SSH-2.0-nope\r\n\r\n", sizeof bodyBuf, 1, rsp, bodyBuf);
  check(rsp->malformed(), "not HTTP is malformed");
  delete rsp;
  parse("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n", sizeof bodyBuf, 7, rsp, bodyBuf);
  check(rsp->complete() && rsp->status() == 401, "401 with an empty body");
  delete rsp;
}

// ---- CHIRP-PROV --------------------------------------------------------------

Settings stored, merged;

ProvResult push(const char* json, const Settings& from) { return applyPush(json, strlen(json), from, merged); }

void expect(const char* json, ProvResult want, const char* why, const Settings* from = nullptr) {
  Settings empty;
  empty.clear();
  ProvResult got = push(json, from ? *from : empty);
  check(got == want, why, std::string("\n      want ") + provResultLine(want) + "\n      got  " + provResultLine(got) +
                              "\n      " + json);
}

void testProv() {
  group("prov");
  const char* key = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
  Settings empty;
  empty.clear();

  // The refused-setup fingerprint: what a board keeps after Warblet answered
  // "unclaimed", so it stays stopped across a restart until a new setup.
  {
    Settings a, b;
    a.clear();
    strcpy(a.hwId, "sunny-otter-7c2f");
    strcpy(a.credential, "CHIRP-ABCDEFGH");
    strcpy(a.ssid, "workshop");
    b = a;
    char fa[17], fb[17];
    setupFingerprint(a, fa);
    setupFingerprint(b, fb);
    check(strlen(fa) == 16 && strspn(fa, "0123456789abcdef") == 16 && strcmp(fa, fb) == 0,
          "fingerprint: 16 hex characters, the same for the same setup");
    check(strstr(fa, "CHIRP") == nullptr && strstr(fa, "ABCDEFGH") == nullptr, "fingerprint: not the code itself");
    strcpy(b.ssid, "another-network");
    setupFingerprint(b, fb);
    check(strcmp(fa, fb) == 0, "fingerprint: a Wi-Fi-only change keeps it (the board is still unknown)");
    strcpy(b.credential, "CHIRP-NEWCODE1");
    setupFingerprint(b, fb);
    check(strcmp(fa, fb) != 0, "fingerprint: a new setup code changes it");
    b = a;
    strcpy(b.hwId, "another-id");
    setupFingerprint(b, fb);
    check(strcmp(fa, fb) != 0, "fingerprint: a new device id changes it");
    b = a;
    strcpy(b.key, "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=");
    setupFingerprint(b, fb);
    check(strcmp(fa, fb) != 0, "fingerprint: a new signing key changes it");
  }

  // Announce.
  char line[200];
  provAnnounce("sunny-otter-7c2f", nullptr, line, sizeof line);
  checkStr(line, "CHIRP! v=1 hw=sunny-otter-7c2f radios=wifi sig=1", "announce with an id");
  provAnnounce("", nullptr, line, sizeof line);
  checkStr(line, "CHIRP! v=1 radios=wifi sig=1", "announce without an id leaves hw= out");
  // CHIRP-PROV v1.6: a demo's firmware names its demo, last, as t=<slug>.
  provAnnounce("sunny-otter-7c2f", "demo-cyd-desk-display-arduino", line, sizeof line);
  checkStr(line, "CHIRP! v=1 hw=sunny-otter-7c2f radios=wifi sig=1 t=demo-cyd-desk-display-arduino",
           "announce with the demo's slug, last");
  check(validSlug("demo-xiao-c6") && validSlug("abc") && !validSlug("ab") && !validSlug("Demo-x") &&
            !validSlug("demo x") && !validSlug(std::string(41, 'a').c_str()) && validSlug(std::string(40, 'a').c_str()),
        "a slug is 3 to 40 of a-z, 0-9 and -");
  provAnnounce("", "Not A Slug", line, sizeof line);
  checkStr(line, "CHIRP! v=1 radios=wifi sig=1", "a slug that breaks the grammar is left out");

  // Accepted.
  expect("{\"ssid\":\"workshop\",\"pass\":\"hunter2\",\"claim\":\"CHIRP-ABCDEFGH\",\"hwid\":\"sunny-otter-7c2f\"}",
         ProvResult::Ok, "what the setup page pushes at a fresh board");
  check(strcmp(merged.credential, "CHIRP-ABCDEFGH") == 0, "a claim code is stored where the token goes");
  expect("{\"ssid\":\"s\",\"pass\":\"\",\"token\":\"T\",\"hwid\":\"h\",\"level\":2,\"thread\":{\"name\":\"n\"},"
         "\"unknown\":{\"deep\":[1,{\"a\":\"}\"},3]},\"tls\":false,\"t\":\"demo-x\"}",
         ProvResult::Ok, "unknown keys, nested values and braces inside strings are ignored");
  expect("{\"ssid\":\"caf\\u00e9 \\\"guest\\\"\",\"pass\":\"p\",\"token\":\"T\",\"hwid\":\"h\"}", ProvResult::Ok,
         "escapes and \\u in strings");
  checkStr(merged.ssid, "caf\xc3\xa9 \"guest\"", "...decoded to UTF-8");
  expect("{\"ssid\":\"\\ud83d\\ude00\",\"pass\":\"\",\"token\":\"T\",\"hwid\":\"h\"}", ProvResult::Ok, "surrogate pair");
  checkStr(merged.ssid, "\xf0\x9f\x98\x80", "...decoded to one 4-byte character");
  expect("  {\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"h\"}  ", ProvResult::Ok, "whitespace around the object");
  std::string withKey = std::string("{\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"h\",\"key\":\"") + key + "\"}";
  expect(withKey.c_str(), ProvResult::Ok, "a 44-character key");
  checkStr(merged.key, key, "...stored as sent");

  // Refused.
  expect("{\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"h\"", ProvResult::BadJson, "truncated object");
  expect("[1,2,3]", ProvResult::BadJson, "not an object");
  expect("{\"ssid\":\"s\",}", ProvResult::BadJson, "trailing comma");
  expect("{'ssid':'s'}", ProvResult::BadJson, "single quotes");
  expect("{\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"h\"} trailing", ProvResult::BadJson, "junk after the object");
  expect("{\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"h\"} {}", ProvResult::BadJson, "two objects on one line");
  expect("{\"ssid\":\"a\x01\",\"token\":\"T\",\"hwid\":\"h\"}", ProvResult::BadJson, "a raw control character");
  expect("{\"ssid\":\"s\",\"claim\":\"C\",\"token\":\"T\",\"hwid\":\"h\"}", ProvResult::BadJson, "both claim and token");
  expect("{\"ssid\":5,\"token\":\"T\",\"hwid\":\"h\"}", ProvResult::BadJson, "ssid must be a string");
  expect("{\"ssid\":\"s\",\"claim\":123,\"hwid\":\"h\"}", ProvResult::BadJson, "claim must be a string");
  expect("{\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"two words\"}", ProvResult::BadJson, "hwid with a space");
  expect("{\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"id\\r\\nCHIRP! v=2 hw=other\"}", ProvResult::BadJson,
         "a line break in hwid cannot forge an announce");
  expect("{\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"h\",\"key\":\"c2hvcnQ=\"}", ProvResult::BadJson,
         "key that is not 32 bytes");
  expect("{\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"h\",\"key\":42}", ProvResult::BadJson, "key must be a string");
  expect("{\"ssid\":\"0123456789abcdef0123456789abcdefX\",\"token\":\"T\",\"hwid\":\"h\"}", ProvResult::BadJson,
         "an ssid of 33 bytes does not fit 802.11's 32");
  expect((std::string("{\"ssid\":\"") + std::string(32, 's') + "\",\"pass\":\"" + std::string(64, 'p') +
          "\",\"token\":\"" + std::string(128, 't') + "\",\"hwid\":\"" + std::string(128, 'h') + "\"}")
             .c_str(),
         ProvResult::Ok, "fields exactly at their limits fit (ssid 32, pass 64, token 128, hwid 128)");
  std::string seventeen = "{\"token\":\"T\",\"hwid\":\"h\",\"ssid\":\"";
  for (int i = 0; i < 17; i++) seventeen += "\\u00e9";
  seventeen += "\"}";
  expect(seventeen.c_str(), ProvResult::BadJson, "17 two-byte characters (34 bytes) do not fit an ssid");
  expect("{\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"h\",\"ssid\":\"\\u0000\"}", ProvResult::BadJson,
         "a NUL inside a string");

  // Control characters in any pushed string.
  expect("{\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"h\",\"host\":\"evil.example\\r\\nX-Injected: 1\"}",
         ProvResult::BadJson, "a line break in host cannot add a request header");
  expect("{\"ssid\":\"s\",\"token\":\"T\\r\\nX-Injected: 1\",\"hwid\":\"h\"}", ProvResult::BadJson,
         "a line break in the token cannot add a request header");
  expect("{\"ssid\":\"s\\t\",\"token\":\"T\",\"hwid\":\"h\"}", ProvResult::BadJson, "a tab in the Wi-Fi name");
  expect("{\"ssid\":\"s\",\"pass\":\"p\\u007f\",\"token\":\"T\",\"hwid\":\"h\"}", ProvResult::BadJson,
         "a DEL in the password");
  expect("{\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"h\",\"aps\":[{\"ssid\":\"a\\nb\"}]}", ProvResult::BadJson,
         "a line break in a listed network");

  // Missing fields: completeness is judged on the merge.
  expect("{\"host\":\"h\"}", ProvResult::MissingField, "no credential, no network, no id");
  expect("{\"ssid\":\"s\",\"pass\":\"p\",\"hwid\":\"h\"}", ProvResult::MissingField,
         "a network without a credential is refused");
  expect((std::string("{\"key\":\"") + key + "\"}").c_str(), ProvResult::MissingField,
         "first-ever key-only push is refused");
  expect("{\"ssid\":\"s\",\"token\":\"T\"}", ProvResult::MissingField, "no device id");
  expect("{\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"\"}", ProvResult::MissingField, "an empty device id");

  // Merges (v1.3): a push carries only what changed.
  check(push("{\"ssid\":\"workshop\",\"pass\":\"hunter2\",\"token\":\"TOK-ONE\",\"hwid\":\"h\"}", empty) ==
            ProvResult::Ok,
        "provision");
  stored = merged;
  expect((std::string("{\"key\":\"") + key + "\"}").c_str(), ProvResult::Ok, "key-only push onto a provisioned board",
         &stored);
  check(strcmp(merged.credential, "TOK-ONE") == 0 && strcmp(merged.ssid, "workshop") == 0 &&
            strcmp(merged.key, key) == 0,
        "...keeps token and network, lands the key");
  expect("{\"token\":\"TOK-TWO\"}", ProvResult::Ok, "token-only push (rotation)", &stored);
  check(strcmp(merged.credential, "TOK-TWO") == 0 && strcmp(merged.pass, "hunter2") == 0, "...replaces the credential");
  Settings claimed;
  push("{\"ssid\":\"s\",\"claim\":\"CHIRP-OLD\",\"hwid\":\"h\"}", empty);
  claimed = merged;
  push("{\"token\":\"TOK-NEW\"}", claimed);
  check(strcmp(merged.credential, "TOK-NEW") == 0, "a token push evicts the stored claim code");

  // Networks.
  expect("{\"ssid\":\"old\",\"pass\":\"p\",\"aps\":[{\"ssid\":\"old\",\"pass\":\"p\"},{\"ssid\":\"van\",\"pass\":\"\"},"
         "{\"ssid\":\"phone\"}],\"token\":\"T\",\"hwid\":\"h\"}",
         ProvResult::Ok, "a list of networks");
  Network nets[kMaxNetworks + 1];
  size_t nn = merged.networks(nets, kMaxNetworks + 1);
  check(nn == 3 && strcmp(nets[0].ssid, "old") == 0 && strcmp(nets[1].ssid, "van") == 0 &&
            strcmp(nets[2].ssid, "phone") == 0 && nets[2].pass[0] == '\0',
        "...tried top-level first, then the list, without repeats");
  {
    const char *ns = nullptr, *np = nullptr;
    bool same = true;
    for (size_t i = 0; i < nn; i++) {
      same = same && merged.network(i, &ns, &np) && strcmp(ns, nets[i].ssid) == 0 && strcmp(np, nets[i].pass) == 0;
    }
    check(same && !merged.network(nn, &ns, &np), "network(i) gives the same order without copying");
  }
  stored = merged;
  push("{\"ssid\":\"new\",\"pass\":\"n\"}", stored);
  nn = merged.networks(nets, kMaxNetworks + 1);
  check(!merged.hasAps && nn == 1 && strcmp(nets[0].ssid, "new") == 0,
        "a single-ssid push replaces the whole network list");
  push("{\"token\":\"T2\"}", stored);
  check(merged.hasAps && merged.apsCount == 3, "a push without networks keeps the list");
  std::string nine = "{\"token\":\"T\",\"hwid\":\"h\",\"ssid\":\"a\",\"aps\":[";
  for (int i = 0; i < 9; i++) nine += std::string(i ? "," : "") + "{\"ssid\":\"n" + std::to_string(i) + "\"}";
  nine += "]}";
  expect(nine.c_str(), ProvResult::BadJson, "nine networks are refused, not truncated");
  expect("{\"token\":\"T\",\"hwid\":\"h\",\"ssid\":\"a\",\"aps\":[{\"pass\":\"x\"}]}", ProvResult::BadJson,
         "a network without a name");
  expect("{\"token\":\"T\",\"hwid\":\"h\",\"ssid\":\"a\",\"aps\":[\"a\"]}", ProvResult::BadJson,
         "a network that is not an object");
  expect("{\"token\":\"T\",\"hwid\":\"h\",\"ssid\":\"a\",\"aps\":{}}", ProvResult::BadJson, "aps that is not a list");

  // Storage round trip.
  push("{\"ssid\":\"caf\\u00e9\",\"pass\":\"p\\\"q\",\"aps\":[{\"ssid\":\"caf\\u00e9\",\"pass\":\"p\\\"q\"},"
       "{\"ssid\":\"back\\\\slash\",\"pass\":\"\"}],\"claim\":\"CHIRP-ABCDEFGH\",\"hwid\":\"sunny-otter-7c2f\","
       "\"host\":\"my-core.example.com\"}",
       empty);
  Settings before = merged;
  char text[1600];
  size_t tl = settingsToJson(before, text, sizeof text);
  Settings after;
  check(tl > 0 && applyPush(text, tl, empty, after) == ProvResult::Ok && memcmp(&before, &after, sizeof before) == 0,
        "settings survive storage byte for byte");
  check(settingsToJson(before, text, 20) == 0, "storage text that does not fit is refused");

  // The responder, one character at a time, as the serial line delivers it.
  Settings scratch, current;
  current.clear();
  ProvResponder r(scratch);
  auto feedLine = [&](const std::string& s) {
    ProvResponder::Event last = ProvResponder::kNone;
    for (char ch : s) {
      ProvResponder::Event e = r.feed(ch, current);
      if (e != ProvResponder::kNone) last = e;
    }
    return last;
  };
  check(feedLine("CHIRP?\r\n") == ProvResponder::kReply && strcmp(r.reply(), "CHIRP! v=1 radios=wifi sig=1") == 0,
        "probe with CRLF");
  check(feedLine("  CHIRP?  \n") == ProvResponder::kReply, "probe with spaces and a bare LF");
  check(feedLine("temperature 21.5\r\n") == ProvResponder::kNone, "the sketch's own output is ignored");
  check(feedLine("CHIRP! v=1 hw=echo\r\n") == ProvResponder::kNone, "an announce echoed back is ignored");
  check(feedLine("CHIRP+ {\"ssid\":\"s\"}\r\n") == ProvResponder::kReply &&
            strcmp(r.reply(), "CHIRP= err missing-field") == 0,
        "an incomplete push gets missing-field");
  check(feedLine("CHIRP+ {\"ssid\":\"s\",\"token\":\"T\",\"hwid\":\"board-1\"}\r\n") == ProvResponder::kStore &&
            strcmp(r.pending().hwId, "board-1") == 0,
        "a complete push asks to be stored");
  checkStr(r.stored(true), "CHIRP= ok", "stored: ok");
  checkStr(r.stored(false), "CHIRP= err store-failed", "not stored: store-failed");
  current = r.pending();
  check(feedLine("CHIRP?\r\n") == ProvResponder::kReply &&
            strcmp(r.reply(), "CHIRP! v=1 hw=board-1 radios=wifi sig=1") == 0,
        "after provisioning the announce carries the id");
  check(feedLine("CHIRP+ " + std::string(kProvLineMax, ' ') + "{}\r\n") == ProvResponder::kReply &&
            strcmp(r.reply(), "CHIRP= err bad-json") == 0,
        "a push longer than the line buffer is bad-json, not half-applied");
  check(feedLine("CHIRP?\r\n") == ProvResponder::kReply, "and the next line is read normally");
  r.setSlug("demo-cyd-desk-display-arduino");
  check(feedLine("CHIRP?\r\n") == ProvResponder::kReply &&
            strcmp(r.reply(), "CHIRP! v=1 hw=board-1 radios=wifi sig=1 t=demo-cyd-desk-display-arduino") == 0,
        "the responder announces the slug it was given");
  {
    // The longest of both: a 128-character id and a 40-character slug.
    std::string longId(128, 'b'), longSlug(40, 'c');
    strcpy(current.hwId, longId.c_str());
    r.setSlug(longSlug.c_str());
    std::string want = "CHIRP! v=1 hw=" + longId + " radios=wifi sig=1 t=" + longSlug;
    check(feedLine("CHIRP?\r\n") == ProvResponder::kReply && want == r.reply(),
          "a 128-character id and a 40-character slug still fit the announce");
  }
}

// test/prov_vectors.txt, written by test/prov_oracle.py from the demos'
// chirp_prov.py run on CPython: stored<TAB>push<TAB>answer<TAB>saved<TAB>kind.
std::string canonical(const Settings& s) {
  static char buf[4096];
  JsonWriter w(buf, sizeof buf);
  w.beginArray();
  const char* fields[] = {s.ssid, s.pass, s.credential, s.hwId, s.host, s.key};
  for (const char* f : fields) {
    w.comma();
    w.string(f);
  }
  w.comma();
  if (s.hasAps) {
    w.beginArray();
    for (size_t i = 0; i < s.apsCount; i++) {
      w.comma();
      w.beginArray();
      w.comma();
      w.string(s.aps[i].ssid);
      w.comma();
      w.string(s.aps[i].pass);
      w.endArray();
    }
    w.endArray();
  } else {
    w.raw("null");
  }
  w.endArray();
  return std::string(buf, w.done());
}

void testProvDemo(const char* path) {
  group("prov-demo");
  FILE* f = fopen(path, "r");
  if (!f) {
    check(false, "open the chirp_prov.py vectors", path);
    return;
  }
  static char line[8192];
  int same = 0, stricter = 0;
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    std::vector<std::string> fld;
    std::string l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    size_t start = 0;
    for (size_t tab; (tab = l.find('\t', start)) != std::string::npos; start = tab + 1) fld.push_back(l.substr(start, tab - start));
    fld.push_back(l.substr(start));
    if (fld.size() != 5) {
      check(false, "vector line has five fields", l);
      continue;
    }
    Settings stored, merged;
    stored.clear();
    if (fld[0] != "{}") {
      Settings empty;
      empty.clear();
      check(applyPush(fld[0].data(), fld[0].size(), empty, stored) == ProvResult::Ok, "stored config loads", fld[0]);
    }
    ProvResult r = applyPush(fld[1].data(), fld[1].size(), stored, merged);
    std::string got = provResultLine(r);
    if (fld[4] == "stricter") {
      stricter++;
      checkStr(got, "CHIRP= err bad-json", ("documented stricter: " + fld[1]).c_str());
      continue;
    }
    same++;
    checkStr(got, fld[2], ("answer equals chirp_prov.py: " + fld[1]).c_str());
    if (r == ProvResult::Ok && fld[3] != "-") {
      checkStr(canonical(merged), fld[3], ("saved config equals chirp_prov.py's: " + fld[1]).c_str());
    }
  }
  fclose(f);
  check(same >= 30 && stricter >= 8, "the vectors file holds the expected cases");
  printf("      %d pushes answered as chirp_prov.py answers them; %d documented stricter cases refused\n", same,
         stricter);
}

}  // namespace

int main(int argc, char** argv) {
  const char* vectors = argc > 1 ? argv[1] : "test/vectors.txt";
  const char* provVectors = argc > 2 ? argv[2] : "test/prov_vectors.txt";
  groups.reserve(16);  // cur points into this vector
  testSha256();
  testHmac();
  testDemoC();
  testDemoCam();
  testVectors(vectors);
  testWire();
  testProv();
  testProvDemo(provVectors);
  int pass = 0, fail = 0;
  printf("\n%-10s %5s %5s\n", "group", "pass", "fail");
  for (auto& g : groups) {
    printf("%-10s %5d %5d\n", g.name, g.pass, g.fail);
    pass += g.pass;
    fail += g.fail;
  }
  printf("%-10s %5d %5d\n", "total", pass, fail);
  printf("%s\n", fail ? "FAILED" : "PASSED");
  return fail ? 1 : 0;
}
