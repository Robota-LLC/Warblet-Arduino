// Runs the LightByCommand example sketch and src/Warblet.cpp on a PC, against
// a simulated board (fake_board.cpp) and a fake Warblet server. Checks what
// the sketch-facing layer puts on the wire and what it does with answers.
//
// What this proves: the logic of src/Warblet.cpp and the example, run with
// stand-in Arduino headers. What it does NOT prove: that anything compiles
// with a real board core, or that Wi-Fi, TLS, NTP or storage work on a board.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "Warblet.h"
#include "WarbletBoard.h"
#include "fake_board.h"
#include "warblet/core/sign.h"
#include "warblet/core/text.h"

extern Warblet board;  // the sketch's
extern const int LED_ON;  // the sketch's: the level that lights its LED
void setup();
void loop();

namespace {

int pass = 0, fail = 0;

// Since contract v0.43 both a command check and an uplink are POSTs; the
// path tells them apart.
bool isPoll(const SimRequest& r) { return r.head.find("/down HTTP/1.1\r\n") != std::string::npos; }
bool isUplink(const SimRequest& r) { return r.head.rfind("POST", 0) == 0 && !isPoll(r); }

uint32_t rngState = 7;
uint8_t rnd8() {  // junk bytes for the USB tests
  rngState = rngState * 1103515245u + 12345u;
  return (uint8_t)(rngState >> 16);
}

std::string recorded;  // the last command recordCommand() was handed
void recordCommand(String text) { recorded = text.c_str(); }

void check(bool ok, const std::string& what) {
  if (ok) {
    pass++;
  } else {
    fail++;
    printf("FAIL  %s\n", what.c_str());
  }
}

bool said(const std::string& text) { return Serial.out.find(text) != std::string::npos; }
int occurrences(const std::string& text) {
  int n = 0;
  for (size_t p = Serial.out.find(text); p != std::string::npos; p = Serial.out.find(text, p + 1)) n++;
  return n;
}

void run(int loops, unsigned long stepMs = 100) {
  for (int i = 0; i < loops; i++) {
    loop();
    delay(stepMs);
  }
}

std::string answer(int status, const std::string& reason, const std::string& body = "",
                   const std::string& extra = "") {
  return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" + extra +
         "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

const char* kKey1 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";  // bytes 00..1f, the demos' example
const char* kKey2 = "ICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8=";  // bytes 20..3f

bool signatureOk(const SimRequest& r, const char* keyB64) {
  uint8_t key[32];
  warblet::decodeKey(keyB64, 44, key);
  uint64_t nonce = strtoull(r.header("X-Chirp-Nonce").c_str(), nullptr, 10);
  char hex[65];
  if (isPoll(r)) {
    warblet::signPoll(key, 32, "sim-board-1", nonce, hex);
  } else {
    std::string tag = r.header("X-Chirp-Tag");
    warblet::signHeader(key, 32, nonce, tag.c_str(), reinterpret_cast<const uint8_t*>(r.body.data()),
                        r.body.size(), hex);
  }
  return nonce != 0 && r.header("X-Chirp-Signature") == hex;
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);  // a crash must not swallow what was already printed
  std::string queued;  // the next command the fake server hands out
  bool claimed = false;
  sim.server = [&](const SimRequest& r) -> std::string {
    if (r.head.rfind("POST /ingest/sim-board-1 HTTP/1.1\r\n", 0) == 0) {
      claimed = true;
      return answer(202, "Accepted", "", "X-Chirp-Budget: 1000\r\n");
    }
    if (r.head.rfind("GET ", 0) == 0) return answer(405, "Method Not Allowed", "", "Allow: POST\r\n");
    if (r.head.rfind("POST /ingest/sim-board-1/down HTTP/1.1\r\n", 0) == 0) {
      if (!claimed) return answer(401, "Unauthorized");
      if (queued.empty()) return "HTTP/1.1 204 No Content\r\n\r\n";
      std::string body = queued;
      queued.clear();
      bool isKey = body.rfind("CHIRPKEY1 ", 0) == 0;
      // Chunked, as a Go server sends a body of unknown length.
      char size[16];
      snprintf(size, sizeof size, "%zx", body.size());
      return std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-Chirp-Down: ") +
             (isKey ? "key" : "data") + "\r\n\r\n" + size + "\r\n" + body + "\r\n0\r\n\r\n";
    }
    return answer(404, "Not Found");
  };

  // 0. A demo's sketch names its demo (CHIRP-PROV v1.6): the setup page
  // finishes a demo's setup only when the board announces t=<its slug>.
  {
    Warblet named;
    named.demo("demo-cyd-desk-display-arduino");
    named.begin();
    Serial.out.clear();
    Serial.type("CHIRP?\r\n");
    named.wait(100);
    check(said("CHIRP! v=1 radios=wifi sig=1 t=demo-cyd-desk-display-arduino\r\n"),
          "board.demo(): the announce carries t=<slug>");
    Warblet unnamed;
    unnamed.demo("Not A Slug");
    Serial.out.clear();
    unnamed.begin();
    Serial.type("CHIRP?\r\n");
    unnamed.wait(100);
    check(said("CHIRP! v=1 radios=wifi sig=1\r\n") && said("not a demo slug"),
          "board.demo(): a slug that breaks the grammar is left out, and said");
    Serial.out.clear();
  }

  // 0b. The device id, for a sketch to show: "" until the setup page gives one.
  {
    std::string kept = sim.stored;
    sim.stored.clear();
    Warblet w;
    w.begin();
    std::string before = w.deviceId();
    Serial.type("CHIRP+ {\"ssid\":\"lab\",\"pass\":\"pw\",\"hwid\":\"sim-id-7\",\"token\":\"T\"}\r\n");
    w.wait(100);
    check(before.empty() && std::string(w.deviceId()) == "sim-id-7", "deviceId(): empty, then the id the setup page gave");
    sim.stored = kept;
    warblet::board::wifiStop();  // the scenarios below start from a board that never joined
    sim.wifiBegins = 0;
    sim.joins.clear();
    sim.requests.clear();
    Serial.out.clear();
  }

  // 1. Nothing stored: the board waits for the setup page, and says so once.
  setup();
  run(5);
  check(said("warblet: waiting for settings from the Warblet setup page over USB"), "says it is waiting for settings");
  check(sim.requests.empty() && sim.wifiBegins == 0, "nothing is sent before there are settings");

  // 2. The setup page probes and pushes, as the contract's CHIRP-PROV does.
  Serial.type("CHIRP?\r\n");
  run(1);
  check(said("CHIRP! v=1 radios=wifi sig=1\r\n"), "announce answered, no id yet");
  Serial.type("CHIRP+ {\"ssid\":\"lab\",\"pass\":\"pw\",\"claim\":\"CHIRP-TESTCODE\",\"hwid\":\"sim-board-1\",\"level\":1}\r\n");
  run(3);  // the line arrives over a few milliseconds
  check(said("CHIRP= ok\r\n"), "push answered ok");
  check(sim.stored.find("\"hwid\":\"sim-board-1\"") != std::string::npos &&
            sim.stored.find("\"token\":\"CHIRP-TESTCODE\"") != std::string::npos,
        "settings stored, the claim code where the token goes");
  run(3);
  check(sim.wifiBegins >= 1, "joins Wi-Fi with the new settings");

  // 3. The sketch's hello: the first stored message, carrying the claim code.
  const SimRequest* hello = nullptr;
  for (auto& r : sim.requests) {
    if (isUplink(r)) { hello = &r; break; }
  }
  check(hello != nullptr, "the sketch's hello was sent");
  if (hello) {
    check(hello->host == "https.warbletiot.com" && hello->port == 443, "to https.warbletiot.com:443");
    check(hello->header("X-Chirp-Token") == "CHIRP-TESTCODE" && hello->header("X-Chirp-Claim") == "CHIRP-TESTCODE",
          "claim code in X-Chirp-Token and X-Chirp-Claim");
    check(hello->header("X-Chirp-Tag") == "led" && hello->body == "0", "tag led, body \"0\" (sendNumber sends text)");
    check(hello->header("X-Chirp-Signature").empty(), "no key yet: not signed");
  }

  // 4. A command: led:on reaches the sketch, which reports the LED setting.
  queued = "led:on";
  size_t before = sim.requests.size();
  run(120);  // 12 s of loop
  check(simPin[LED_BUILTIN] == HIGH, "led:on reached the sketch's command()");
  bool reported = false;
  for (size_t i = before; i < sim.requests.size(); i++) {
    if (isUplink(sim.requests[i]) && sim.requests[i].body == "1") reported = true;
  }
  check(reported, "the sketch reported LED 1 (\"1\")");

  // A command's words are the sender's: the console says one arrived and
  // its size, never what it says (a build with WARBLET_VERBOSE adds them).
  check(said("warblet: command received (6 bytes)") && !said("received (6 bytes): led:on"),
        "the console says a command arrived and its size, not its words");

  // 4b. led:blink: the sketch's command function blinks with board.wait(200),
  // which re-enters loop(); that must not start a second command check.
  queued = "led:blink";
  before = sim.requests.size();
  run(80);
  int checksDuringBlink = 0;
  bool blinkReported = false;
  // Count the command checks up to the report the blink ends with. The LED
  // was lit (led:on above), so the blink ends lit and reports 1.
  for (size_t i = before; i < sim.requests.size() && !blinkReported; i++) {
    if (isUplink(sim.requests[i]) && sim.requests[i].body == "1") {
      blinkReported = true;
    } else {
      checksDuringBlink += isPoll(sim.requests[i]);
    }
  }
  check(simPin[LED_BUILTIN] == LED_ON && blinkReported,
        "led:blink ran (wait() inside the command function), ended lit as it was, and reported 1");
  check(checksDuringBlink == 1, "no second command check started from inside the command function");

  // 5. A signing key over USB: polls and messages are signed from now on.
  Serial.type(std::string("CHIRP+ {\"key\":\"") + kKey1 + "\"}\r\n");
  queued = "led:off";
  before = sim.requests.size();
  run(120);
  check(said("CHIRP= ok\r\n") && sim.stored.find(kKey1) != std::string::npos, "key push stored");
  check(simPin[LED_BUILTIN] == LOW, "led:off reached the sketch");
  int signedPolls = 0, signedPosts = 0, badSignatures = 0;
  uint64_t lastNonce = 0;
  bool increasing = true;
  for (size_t i = before; i < sim.requests.size(); i++) {
    const SimRequest& r = sim.requests[i];
    if (r.header("X-Chirp-Signature").empty()) continue;
    (isPoll(r) ? signedPolls : signedPosts)++;
    if (!signatureOk(r, kKey1)) badSignatures++;
    uint64_t nonce = strtoull(r.header("X-Chirp-Nonce").c_str(), nullptr, 10);
    if (nonce <= lastNonce) increasing = false;
    lastNonce = nonce;
  }
  check(signedPolls >= 1 && signedPosts >= 1, "both the command check and the report are signed");
  check(badSignatures == 0, "every signature verifies against the contract's signed input");
  check(increasing, "nonces strictly increase across uplinks and polls");

  // 6. A key frame over the air replaces the key; the next report uses it.
  queued = std::string("CHIRPKEY1 ") + kKey2 + "\n";
  run(110);
  check(sim.stored.find(kKey2) != std::string::npos, "key frame stored");
  check(said("a signing key arrived over the air and was stored"), "says so");
  queued = "led:on";
  before = sim.requests.size();
  run(110);
  bool newKeyUsed = false;
  for (size_t i = before; i < sim.requests.size(); i++) {
    if (isUplink(sim.requests[i]) && signatureOk(sim.requests[i], kKey2)) newKeyUsed = true;
  }
  check(newKeyUsed, "the report after the key frame is signed with the new key");

  // 7. Refusals are said aloud.
  check(!board.sendTenths(5000), "sendTenths(5000) is refused");
  check(said("the value must be between -3276.8 and 3276.7"), "...and says why");
  check(!board.sendNumber(NAN), "sendNumber(NaN) is refused");
  check(said("not a number"), "...and says why");
  size_t beforeBig = sim.requests.size();
  check(board.sendNumber(101325) && sim.requests.size() > beforeBig && sim.requests.back().body == "101325",
        "sendNumber(101325) is sent, as the text 101325");
  check(board.sendNumber(21.53f) && sim.requests.back().body == "21.53", "sendNumber(21.53f) is sent as 21.53");
  check(board.sendTenths(21.5) && sim.requests.back().body == std::string("\x00\xd7", 2),
        "sendTenths(21.5) is sent as 00 d7, the public temperature demos' format");
  check(!board.sendText("x", "two words"), "a tag with a space is refused");
  sim.server = [&](const SimRequest&) { return answer(401, "Unauthorized"); };
  check(!board.sendText("x"), "a 401 is not stored");
  check(said("not stored (401): the device id, token or signing key was refused"), "...and says what to check");

  // (The "202 unclaimed" answer is scenario 15: it stops a
  // board until a new USB setup, so it runs on its own instance at the end.)

  // 8. A board that restarts loads its settings and keeps its key.
  Warblet again;
  Serial.out.clear();
  again.begin();
  check(said("settings loaded from this board"), "settings load after a restart");
  sim.server = [&](const SimRequest& r) { return isUplink(r) ? answer(202, "Accepted") : answer(204, "No Content"); };
  before = sim.requests.size();
  check(again.sendText("hi", "boot"), "a fresh instance sends");
  check(sim.requests.size() > before && signatureOk(sim.requests.back(), kKey2), "...signed with the stored key");

  // 9. The typed-in alternative never answers the setup page.
  Warblet typed;
  typed.begin("lab", "pw", "sim-board-1", "TOKEN", kKey2);
  Serial.out.clear();
  Serial.type("CHIRP?\r\n");
  typed.loop();
  check(!said("CHIRP!"), "typed settings: the setup page is not answered");
  Warblet badKey;
  Serial.out.clear();
  badKey.begin("lab", "pw", "sim-board-1", "TOKEN", "short");
  check(said("the signing key should be the 44-character key"), "a bad typed key is refused and named");

  // 10. A setup push that arrives while the board is busy.
  // The page probes; for 10 s after the answer the board only listens.
  sim.server = [&](const SimRequest& r) {
    return isUplink(r) ? answer(202, "Accepted") : answer(204, "No Content");
  };
  sim.exchangeMs = 1500;  // each request blocks the sketch for 1.5 s
  {
    Warblet w;
    w.begin();
    for (int i = 0; i < 50; i++) { w.loop(); delay(20); }  // online, checking for commands
    Serial.out.clear();
    Serial.type("CHIRP?\r\n");
    w.loop();
    check(said("CHIRP! v=1 hw=sim-board-1 radios=wifi sig=1"), "probe answered");
    size_t at = sim.requests.size();
    for (int i = 0; i < 450; i++) { w.loop(); delay(20); }  // 9 s
    check(sim.requests.size() == at, "no request runs in the 10 s after the probe is answered");
    check(!w.sendText("x"), "a send during that window is refused, not run");
    check(said("the setup page is talking to the board"), "...and says why");
    for (int i = 0; i < 100; i++) { w.loop(); delay(20); }  // the window ends

    // A push of two networks and a key (over 256 bytes), arriving 100 ms into
    // a 1.5 s request: it waits in the 2 KB receive buffer.
    std::string push = std::string("CHIRP+ {\"ssid\":\"lab\",\"pass\":\"a-long-wifi-password-0123456789\",") +
                       "\"aps\":[{\"ssid\":\"lab\",\"pass\":\"a-long-wifi-password-0123456789\"}," +
                       "{\"ssid\":\"van\",\"pass\":\"another-long-password-9876543210\"}]," +
                       "\"token\":\"TOKEN-AFTER-ROTATION-0123456789\",\"hwid\":\"sim-board-1\",\"key\":\"" + kKey2 +
                       "\"}\r\n";
    check(push.size() > 256, "the push is longer than the 256-byte default buffer");
    Serial.out.clear();
    Serial.dropped = 0;
    Serial.type(push, 100);
    w.sendText("busy");  // 1.5 s, during which the push arrives
    for (int i = 0; i < 5; i++) { w.loop(); delay(20); }
    check(Serial.dropped == 0 && said("CHIRP= ok"), "with the 2 KB buffer the push survives a busy board");

    // The same with the buffer left at the 256-byte default: the push is
    // lost, which is why the buffer is raised.
    Serial.rxCap = 256;
    Serial.out.clear();
    Serial.dropped = 0;
    Serial.type(push, 100);
    w.sendText("busy");
    for (int i = 0; i < 5; i++) { w.loop(); delay(20); }
    check(Serial.dropped > 0 && !said("CHIRP= ok"), "at 256 bytes the same push is lost (why the buffer is raised)");
    Serial.rxCap = 2048;
    Serial.type("\r\n");  // end the broken line
    w.loop();
  }
  sim.exchangeMs = 0;

  // 11. A corrected push during a failing Wi-Fi join takes effect at once.
  {
    Warblet w;
    w.begin();
    warblet::board::wifiStop();
    sim.joinable = "right-network";
    sim.joins.clear();
    Serial.out.clear();
    unsigned long pushAt = millis() + 4000;
    Serial.type("CHIRP+ {\"ssid\":\"right-network\",\"pass\":\"pw\"}\r\n", 4000);
    for (int i = 0; i < 1500 && !w.connected(); i++) { w.loop(); delay(20); }
    unsigned long joinedAt = 0;
    for (auto& j : sim.joins) {
      if (j.second == "right-network") { joinedAt = j.first; break; }
    }
    check(said("CHIRP= ok") && joinedAt != 0, "the corrected network is tried");
    check(joinedAt != 0 && joinedAt - pushAt < 1000, "...within a second of the push, not after the old list");
    printf("      corrected network tried %lu ms after the push\n", joinedAt - pushAt);
    sim.joinable.clear();
  }

  // 12. Refused command checks back off and say so once.
  {
    Warblet w;
    w.begin();
    w.onCommand(recordCommand);  // only a sketch with a command function asks for commands
    sim.server = [&](const SimRequest& r) {
      return isUplink(r) ? answer(202, "Accepted") : answer(401, "Unauthorized");
    };
    Serial.out.clear();
    size_t at = sim.requests.size();
    for (int i = 0; i < 6000; i++) { w.loop(); delay(100); }  // ten minutes
    int gets = 0;
    for (size_t i = at; i < sim.requests.size(); i++) gets += isPoll(sim.requests[i]);
    size_t lines = 0;
    for (size_t p = Serial.out.find("command check refused"); p != std::string::npos;
         p = Serial.out.find("command check refused", p + 1)) {
      lines++;
    }
    check(gets >= 3 && gets <= 6, "ten minutes of 401s: a few command checks, not 60");
    check(lines == 1, "...and one console line, not two every 10 s");
    // Seen on a board after a token rotation: the line must name every
    // cause the board cannot tell apart, not only the unclaimed board.
    check(said("the device token or signing key was refused, or the board's first message is not stored yet"),
          "the 401 line names both causes (a rotated credential, or not yet on the account)");
    printf("      %d command checks and %u console lines in ten minutes of 401s\n", gets, (unsigned)lines);
    sim.server = [&](const SimRequest& r) {
      return isUplink(r) ? answer(429, "Too Many Requests", "", "Retry-After: 120\r\n")
                                          : answer(204, "No Content");
    };
    for (int i = 0; i < 6000; i++) { w.loop(); delay(100); }
    check(said("command checks are answered again"), "recovery is said once");
    at = sim.requests.size();
    check(!w.sendText("a") && !w.sendText("b"), "a 429 is not stored");
    int posts = 0;
    for (size_t i = at; i < sim.requests.size(); i++) posts += isUplink(sim.requests[i]);
    check(posts == 1, "after a 429 with Retry-After the next send waits instead of asking again");
  }

  // 12a. A 429 says whether it was the rate or today's storage budget.
  {
    for (int budget = 0; budget < 2; budget++) {
      sim.server = [&](const SimRequest& r) {
        if (!isUplink(r)) return answer(204, "No Content");
        return answer(429, "Too Many Requests", "", budget ? "Retry-After: 3600\r\nX-Chirp-Budget: 0\r\n"
                                                            : "Retry-After: 30\r\n");
      };
      Warblet w;
      w.begin();
      w.wait(15000);
      Serial.out.clear();
      w.sendNumber(1);
      if (budget) {
        check(said("today's storage budget") && !said("too many messages"),
              "a 429 with X-Chirp-Budget is said as the day's storage budget");
      } else {
        check(said("too many messages") && !said("storage budget"), "a 429 without it is said as the rate");
      }
    }
  }

  // 12b. A long outage with Wi-Fi up (the server does not answer for 45
  // minutes): command checks slow down, but to a minute, not ten, so a
  // command queued meanwhile is collected soon after the server is back.
  {
    std::string queuedHere;
    sim.server = [&](const SimRequest& r) -> std::string {
      if (isUplink(r)) return answer(202, "Accepted");
      if (queuedHere.empty()) return "HTTP/1.1 204 No Content\r\n\r\n";
      std::string body = queuedHere;
      queuedHere.clear();
      return answer(200, "OK", body, "X-Chirp-Down: data\r\n");
    };
    Warblet w;
    w.begin();
    w.onCommand(recordCommand);
    w.wait(30000);
    sim.silent = true;
    w.wait(45UL * 60UL * 1000UL);
    sim.silent = false;
    queuedHere = "led:on";
    recorded.clear();
    unsigned long back = millis();
    while (recorded.empty() && millis() - back < 15UL * 60UL * 1000UL) w.wait(1000);
    unsigned long took = millis() - back;
    check(recorded == "led:on" && took <= 90000, "after a 45-minute outage a queued command arrives within 90 s");
    printf("      45-minute outage: queued command collected %lu s after the server was back\n", took / 1000);
  }

  // 12h. restartWhenStuck(): after a request gets no answer at all while
  // Wi-Fi says it is connected, the board restarts if nothing has answered
  // for that long. Any HTTP answer counts, and so does the start; it is
  // never used with no settings, or once stopped; it is the sketch's choice.
  {
    const unsigned long tenMin = 10UL * 60UL * 1000UL;
    int status = 204;
    sim.server = [&](const SimRequest& r) {
      return answer(status, "X", "", isUplink(r) ? "" : "");
    };
    auto run20 = [&](bool stuck, std::function<void(bool)> fault, unsigned long ms) {
      sim.restarts.clear();
      Warblet w;
      w.begin();
      w.onCommand(recordCommand);
      if (stuck) w.restartWhenStuck(tenMin);
      w.wait(20000);
      unsigned long from = millis();
      fault(true);
      w.wait(ms);
      fault(false);
      return sim.restarts.empty() ? -1L : (long)(sim.restarts.front() - from);
    };
    auto failing = [&](bool on) { sim.connectFails = on; };
    long at = run20(true, failing, 12UL * 60UL * 1000UL);
    check(at >= (long)tenMin && at <= (long)tenMin + 70000,
          "restartWhenStuck: no answer at all for 10 minutes restarts the board, once the 10 minutes are up");
    printf("      restartWhenStuck: restarted %ld s after the last answer\n", at / 1000);
    check(run20(false, failing, 20UL * 60UL * 1000UL) < 0, "restartWhenStuck: not asked for, never");
    {
      // restartWhenStuck(0) turns it off again, with no floor applied.
      sim.restarts.clear();
      Serial.out.clear();
      Warblet w;
      w.begin();
      w.onCommand(recordCommand);
      w.restartWhenStuck(tenMin);
      w.restartWhenStuck(0);
      w.wait(20000);
      sim.connectFails = true;
      w.wait(20UL * 60UL * 1000UL);
      sim.connectFails = false;
      check(sim.restarts.empty() && !said("5 min at least"), "restartWhenStuck(0): off, never restarts");
    }
    check(run20(true, [&](bool on) { status = on ? 500 : 204; }, 20UL * 60UL * 1000UL) < 0,
          "restartWhenStuck: 500s are answers: never");
    check(run20(true, [&](bool on) { status = on ? 401 : 204; }, 20UL * 60UL * 1000UL) < 0,
          "restartWhenStuck: 401s are answers: never");
    {
      // Nine minutes of 500s, then nothing at all: the 500s were answers, so
      // the ten minutes count from the last of them, not from the start.
      sim.restarts.clear();
      Warblet w;
      w.begin();
      w.onCommand(recordCommand);
      w.restartWhenStuck(tenMin);
      w.wait(20000);
      status = 500;
      w.wait(9UL * 60UL * 1000UL);
      sim.connectFails = true;
      w.wait(5UL * 60UL * 1000UL);
      bool early = !sim.restarts.empty();
      w.wait(10UL * 60UL * 1000UL);
      bool later = !sim.restarts.empty();
      sim.connectFails = false;
      status = 204;
      check(!early && later,
            "restartWhenStuck: a refusal is an answer, so the ten minutes count from the last 500, not the start");
    }
    check(run20(true,
                [&](bool on) {
                  sim.wifiJoins = !on;
                  if (on) warblet::board::wifiStop();
                },
                20UL * 60UL * 1000UL) < 0,
          "restartWhenStuck: with Wi-Fi gone, never (an outage ends by itself)");
    // A floor: restartWhenStuck(1) would restart at the first request that got
    // no answer, over and over through any outage. It means 5 minutes, and says so.
    {
      sim.restarts.clear();
      Serial.out.clear();
      Warblet w;
      w.begin();
      w.onCommand(recordCommand);
      w.restartWhenStuck(1);
      w.wait(20000);
      unsigned long from = millis();
      sim.connectFails = true;
      w.wait(4UL * 60UL * 1000UL);
      bool early = !sim.restarts.empty();
      w.wait(3UL * 60UL * 1000UL);
      sim.connectFails = false;
      long at = sim.restarts.empty() ? -1L : (long)(sim.restarts.front() - from);
      check(!early && at >= 5L * 60L * 1000L && said("restartWhenStuck(): 5 min at least"),
            "restartWhenStuck: under 5 minutes means 5 minutes, and is said");
    }
    // Nine minutes of nothing, one answer, nine more: the ten minutes start again.
    sim.restarts.clear();
    {
      Warblet w;
      w.begin();
      w.onCommand(recordCommand);
      w.restartWhenStuck(tenMin);
      w.wait(20000);
      sim.connectFails = true;
      w.wait(9UL * 60UL * 1000UL);
      sim.connectFails = false;
      w.wait(70000);
      sim.connectFails = true;
      w.wait(9UL * 60UL * 1000UL);
      sim.connectFails = false;
      check(sim.restarts.empty(), "restartWhenStuck: an answer starts the ten minutes again");
    }
    // Not on an account: stopped, asks nothing, never restarts.
    sim.restarts.clear();
    {
      sim.refusedMark.clear();
      status = 0;
      sim.server = [&](const SimRequest&) {
        return std::string("HTTP/1.1 202 Accepted\r\nContent-Length: 22\r\n\r\n{\"status\":\"unclaimed\"}");
      };
      Warblet w;
      w.begin();
      w.onCommand(recordCommand);
      w.restartWhenStuck(tenMin);
      w.wait(20000);
      w.sendNumber(1);
      sim.connectFails = true;
      w.wait(25UL * 60UL * 1000UL);
      sim.connectFails = false;
      check(sim.restarts.empty() && w.state() == Warblet::NOT_ON_ACCOUNT,
            "restartWhenStuck: never once stopped (not on an account)");
      sim.refusedMark.clear();
    }
    sim.restarts.clear();
  }

  // 12g. The board's state, for a screen: every change is handed to the
  // sketch's function, the join before it blocks, each answer as it comes.
  {
    static std::vector<std::string> seenStates;
    static Warblet* watched = nullptr;
    struct Names {
      static std::string of(Warblet::State s) {
        switch (s) {
          case Warblet::NOT_SET_UP: return "not set up";
          case Warblet::JOINING_WIFI: return "joining";
          case Warblet::NO_WIFI: return "no wifi";
          case Warblet::REACHING_WARBLET: return "reaching";
          case Warblet::ANSWERED: return "answered " + std::to_string(watched->answer());
          case Warblet::REFUSED: return "refused " + std::to_string(watched->answer());
          case Warblet::NO_ANSWER: return "no answer";
          case Warblet::NOT_ON_ACCOUNT: return "not on an account";
        }
        return "?";
      }
      static void record(Warblet::State s) {
        std::string n = of(s);
        if (seenStates.empty() || seenStates.back() != n) seenStates.push_back(n);
      }
    };
    int status = 204;
    sim.server = [&](const SimRequest& r) {
      if (status == 0) return std::string("HTTP/1.1 202 Accepted\r\nContent-Length: 22\r\n\r\n{\"status\":\"unclaimed\"}");
      return isUplink(r) ? answer(status == 204 ? 202 : status, "X") : answer(status, "X");
    };
    std::string kept = sim.stored;
    sim.stored.clear();
    sim.refusedMark.clear();
    Warblet w;
    watched = &w;
    w.onState(Names::record);
    w.onCommand(recordCommand);
    w.begin();
    w.wait(1000);
    check(w.state() == Warblet::NOT_SET_UP, "state: no settings is NOT_SET_UP");
    Serial.type("CHIRP+ {\"ssid\":\"lab\",\"pass\":\"pw\",\"hwid\":\"sim-board-1\",\"token\":\"T\"}\r\n");
    w.wait(15000);
    status = 401;
    w.wait(15000);
    status = 429;
    w.sendNumber(1);
    w.wait(1000);
    sim.silent = true;
    w.wait(130000);
    sim.silent = false;
    status = 204;
    w.wait(70000);
    sim.wifiJoins = false;
    warblet::board::wifiStop();
    w.wait(40000);
    sim.wifiJoins = true;
    w.wait(70000);
    status = 0;  // unclaimed
    w.sendNumber(1);
    w.wait(1000);
    std::string seq;
    for (auto& s : seenStates) seq += (seq.empty() ? "" : ", ") + s;
    check(seq == "not set up, joining, reaching, answered 204, refused 401, refused 429, no answer, answered 204, "
                 "joining, no wifi, joining, no wifi, joining, reaching, answered 204, not on an account",
          "state: each change reaches the sketch's function, in order");
    check(seq.find("joining, no wifi, joining, reaching") != std::string::npos,
          "state: a lost network is joining, then no wifi, then joining again");
    check(seq.find("refused 401") != std::string::npos && seq.find("refused 429") != std::string::npos &&
              seq.find("no answer") != std::string::npos,
          "state: refusals are REFUSED and carry their status; a request with no answer is NO_ANSWER");
    check(w.state() == Warblet::NOT_ON_ACCOUNT && seenStates.back() == "not on an account",
          "state: unclaimed is NOT_ON_ACCOUNT");
    printf("      states: %s\n", seq.c_str());
    sim.stored = kept;
    sim.refusedMark.clear();
    watched = nullptr;
  }

  // 12g4. A setup push that changes the board's id or its token is a new
  // start: the board restarts, as the MicroPython demos do, so the sketch
  // starts afresh under the new setup (a hello, nothing the old one showed).
  // Wi-Fi alone, a key alone, or the same id and token again: used at once,
  // no restart.
  {
    sim.server = [&](const SimRequest& r) { return answer(isUplink(r) ? 202 : 204, "X"); };
    std::string kept = sim.stored;
    sim.stored.clear();
    sim.refusedMark.clear();
    sim.restarts.clear();
    Warblet w;
    w.onCommand(recordCommand);
    w.begin();
    w.wait(1000);
    Serial.out.clear();
    Serial.type("CHIRP+ {\"ssid\":\"lab\",\"pass\":\"pw\",\"hwid\":\"sim-board-1\",\"token\":\"T\"}\r\n");
    w.wait(15000);
    bool first = sim.restarts.size() == 1 && said("CHIRP= ok") &&
                 Serial.out.find("CHIRP= ok") < Serial.out.find("a new setup, so the board restarts");
    sim.restarts.clear();
    Serial.type("CHIRP+ {\"ssid\":\"lab-2\",\"pass\":\"pw2\"}\r\n");
    w.wait(15000);
    bool wifiOnly = sim.restarts.empty();
    Serial.type(std::string("CHIRP+ {\"key\":\"") + kKey1 + "\"}\r\n");
    w.wait(15000);
    bool keyOnly = sim.restarts.empty();
    Serial.type("CHIRP+ {\"hwid\":\"sim-board-1\",\"token\":\"T\"}\r\n");
    w.wait(15000);
    bool same = sim.restarts.empty();
    Serial.type("CHIRP+ {\"token\":\"T-NEW\"}\r\n");
    w.wait(15000);
    bool newToken = sim.restarts.size() == 1;
    Serial.type("CHIRP+ {\"hwid\":\"sim-board-9\"}\r\n");
    w.wait(15000);
    bool newId = sim.restarts.size() == 2;
    check(first, "setup: the first setup is stored, answered ok, then the board restarts");
    check(wifiOnly && keyOnly && same, "setup: Wi-Fi alone, a key alone, or the same id and token: no restart");
    check(newToken && newId, "setup: a new token, or a new id, restarts the board");
    sim.stored = kept;
    sim.refusedMark.clear();
    sim.restarts.clear();
  }

  // 12g5. Wi-Fi up but no clock (NTP blocked): nothing can be sent, which is
  // no answer, as the MicroPython desk display shows it; and with
  // restartWhenStuck() the board restarts once that has lasted its time.
  {
    sim.server = [&](const SimRequest& r) { return answer(isUplink(r) ? 202 : 204, "X"); };
    sim.restarts.clear();
    sim.clockAnswers = false;
    warblet::board::simClockLost();  // a power-on: no clock yet
    Warblet w;
    w.onCommand(recordCommand);
    w.restartWhenStuck(10UL * 60UL * 1000UL);
    w.begin();
    unsigned long from = millis();
    w.wait(60000);
    bool noAnswer = w.state() == Warblet::NO_ANSWER && sim.restarts.empty();
    w.wait(11UL * 60UL * 1000UL);
    long at = sim.restarts.empty() ? -1L : (long)(sim.restarts.front() - from);
    sim.clockAnswers = true;
    check(noAnswer, "no clock: the state is NO_ANSWER, not Reaching Warblet for ever");
    check(at >= 10L * 60L * 1000L && at <= 11L * 60L * 1000L,
          "no clock: with restartWhenStuck(10 min) the board restarts after 10 minutes");
    sim.restarts.clear();
  }

  // 12g2. ANSWERED is Warblet taking the request (2xx). A 4xx is REFUSED: the
  // board turned away. A 5xx is Warblet's own fault, so NO_ANSWER, as when
  // nothing answers. Each keeps its status in answer().
  {
    int status = 202;
    sim.server = [&](const SimRequest& r) { return answer(isUplink(r) ? status : 204, "X"); };
    std::string kept = sim.stored;
    Warblet w;
    w.begin();
    w.wait(20000);
    std::string wrong;
    for (int code : {200, 202, 204, 400, 401, 403, 404, 413, 429, 500, 502, 503}) {
      status = code;
      Warblet fresh;  // no hold left from the last refusal
      fresh.begin();
      fresh.wait(15000);
      fresh.sendNumber(1);
      Warblet::State want = code < 300 ? Warblet::ANSWERED : code < 500 ? Warblet::REFUSED : Warblet::NO_ANSWER;
      bool ok = fresh.answer() == code && fresh.state() == want;
      if (!ok) wrong += " " + std::to_string(code);
    }
    check(wrong.empty(), "state: 2xx is ANSWERED; 400, 401, 403, 404, 413 and 429 are REFUSED; 500, 502 and 503 are"
                         " NO_ANSWER; each with its status in answer()" +
                             (wrong.empty() ? std::string() : " (wrong:" + wrong + ")"));
    sim.stored = kept;
  }

  // 12g3. A command check answered "unclaimed", the ingest's answer for a
  // board Warblet does not know, stops the board as a report would. A
  // command whose words happen to be the same JSON is still a command.
  {
    int pollStatus = 202;
    sim.server = [&](const SimRequest& r) {
      if (isUplink(r)) return answer(202, "X");
      return pollStatus == 202
                 ? std::string("HTTP/1.1 202 Accepted\r\nContent-Length: 22\r\n\r\n{\"status\":\"unclaimed\"}")
                 : std::string("HTTP/1.1 200 OK\r\nX-Chirp-Down: data\r\nContent-Length: 22\r\n\r\n"
                               "{\"status\":\"unclaimed\"}");
    };
    std::string kept = sim.stored;
    pollStatus = 200;
    {
      recorded.clear();
      Warblet w;
      w.onCommand(recordCommand);
      w.begin();
      w.wait(25000);
      check(recorded == "{\"status\":\"unclaimed\"}" && w.state() != Warblet::NOT_ON_ACCOUNT,
            "poll: a 200 command is a command, whatever its words");
    }
    sim.refusedMark.clear();
    pollStatus = 202;
    {
      Warblet w;
      w.onCommand(recordCommand);
      w.begin();
      w.wait(25000);
      size_t before = sim.requests.size();
      w.wait(60000);
      check(w.state() == Warblet::NOT_ON_ACCOUNT && sim.requests.size() == before && !sim.refusedMark.empty(),
            "poll: a command check answered 202 unclaimed stops the board, as a report would");
    }
    sim.stored = kept;
    sim.refusedMark.clear();
  }

  // 12f. The pace of command checks: 10 s unless the sketch asks for another,
  // and never under 5 s.
  {
    sim.server = [&](const SimRequest& r) {
      return isUplink(r) ? answer(202, "Accepted") : answer(204, "No Content");
    };
    auto checksIn = [&](Warblet& w, unsigned long ms) {
      size_t at = sim.requests.size();
      w.wait(ms);
      int n = 0;
      for (size_t i = at; i < sim.requests.size(); i++) n += isPoll(sim.requests[i]);
      return n;
    };
    Warblet w;
    w.begin();
    w.onCommand(recordCommand);
    w.wait(15000);
    int normal = checksIn(w, 60000);
    w.checkCommandsEvery(5000);
    w.wait(10000);
    int fast = checksIn(w, 60000);
    Serial.out.clear();
    w.checkCommandsEvery(1000);
    w.wait(10000);
    int floor = checksIn(w, 60000);
    check(normal == 6 && fast == 12 && floor == 12 && said("5 s at least"),
          "checkCommandsEvery(): 6 checks a minute by default, 12 at 5 s, and not faster than 5 s");
    printf("      command checks a minute: %d by default, %d at 5 s, %d when asked for 1 s\n", normal, fast, floor);
  }

  // 12e. Several commands waiting: after one arrives the board asks again at
  // once, so all of them come in one round (up to 10), not one per 10 s.
  {
    std::vector<std::string> waiting = {"one", "two", "three"};
    std::vector<std::string> got;
    sim.server = [&](const SimRequest& r) -> std::string {
      if (isUplink(r)) return answer(202, "Accepted");
      if (waiting.empty()) return answer(204, "No Content");
      std::string body = waiting.front();
      waiting.erase(waiting.begin());
      return answer(200, "OK", body, "X-Chirp-Down: data\r\n");
    };
    Warblet w;
    w.begin();
    w.onCommand(recordCommand);
    w.wait(15000);
    waiting = {"one", "two", "three"};
    for (int i = 0; i < 12; i++) waiting.push_back("more-" + std::to_string(i));  // 15 in all
    unsigned long start = millis();
    while (millis() - start < 11000) w.wait(100);
    check(waiting.size() == 5, "three and more waiting: ten come in one round, the rest at the next check");
    printf("      15 commands waiting: %d taken in the first 11 s\n", (int)(15 - waiting.size()));
  }

  // 12c. A sketch with no command function (SensorReading, Button) asks for no
  // commands: no TLS connection every 10 s, and no pause in its loop for one.
  {
    sim.server = [&](const SimRequest& r) {
      return isUplink(r) ? answer(202, "Accepted") : answer(204, "No Content");
    };
    Warblet w;
    w.begin();
    size_t at = sim.requests.size();
    w.wait(600000);
    int checks = 0;
    for (size_t i = at; i < sim.requests.size(); i++) checks += isPoll(sim.requests[i]);
    check(checks == 0, "no onCommand(): no command checks in ten minutes");
    int without = checks;
    w.onCommand(recordCommand);
    at = sim.requests.size();
    w.wait(30000);
    checks = 0;
    for (size_t i = at; i < sim.requests.size(); i++) checks += isPoll(sim.requests[i]);
    check(checks >= 2, "...and they start once a command function is set");
    printf("      no onCommand(): %d command checks in ten minutes; %d in 30 s once one is set\n", without, checks);
  }

  // 12d. A reply cut short: the connection closes before the body its
  // Content-Length promised. A cut command is not handed over as a shorter
  // one, and a cut 202 is not counted as stored.
  {
    bool cut = true;
    sim.server = [&](const SimRequest& r) -> std::string {
      if (isUplink(r)) {
        return cut ? "HTTP/1.1 202 Accepted\r\nContent-Length: 22\r\n\r\n{\"status\":\"uncl" : answer(202, "Accepted");
      }
      return cut ? "HTTP/1.1 200 OK\r\nX-Chirp-Down: data\r\nContent-Length: 6\r\n\r\nled:o" : answer(204, "No Content");
    };
    Warblet w;
    w.begin();
    w.onCommand(recordCommand);
    recorded.clear();
    w.wait(30000);
    check(recorded.empty(), "a command cut short at close is not handed over");
    check(!w.sendNumber(1), "a 202 cut short at close is not counted as stored");
    cut = false;
    w.wait(70000);
    check(w.sendNumber(2), "...and the next whole answer is");
  }

  // 13. board.wait(): a setup push and a command both arrive during a long
  // wait and are handled before it returns.
  {
    std::string queuedHere = "led:on";
    sim.server = [&](const SimRequest& r) -> std::string {
      if (isUplink(r)) return answer(202, "Accepted");
      if (queuedHere.empty()) return "HTTP/1.1 204 No Content\r\n\r\n";
      std::string body = queuedHere;
      queuedHere.clear();
      return answer(200, "OK", body, "X-Chirp-Down: data\r\n");
    };
    Warblet w;
    w.begin();
    w.onCommand(recordCommand);
    recorded.clear();
    Serial.out.clear();
    std::string storedBefore = sim.stored;
    Serial.type("CHIRP+ {\"token\":\"TOKEN-ROTATED-DURING-WAIT\"}\r\n", 5000);
    unsigned long start = millis();
    w.wait(60000);
    unsigned long took = millis() - start;
    check(said("CHIRP= ok") && sim.stored.find("TOKEN-ROTATED-DURING-WAIT") != std::string::npos,
          "wait(): a setup push 5 s in is answered and stored");
    check(recorded == "led:on", "wait(): a command queued before the wait reaches the command function");

    // The dashboard's text box sends what a person types, up to the queue's
    // 8 KiB cap (contract v0.40): a 2,000-character message arrives whole.
    std::string typed(2000, 'x');
    typed.replace(0, 6, "hello ");
    queuedHere = typed;
    recorded.clear();
    w.wait(15000);
    check(recorded == typed, "a 2,000-character typed command reaches the command function whole (v0.40)");
    queuedHere = std::string(8193, 'y');
    recorded.clear();
    Serial.out.clear();
    w.wait(15000);
    check(recorded.empty() && said("longer than 8192 bytes"), "a command past the 8 KiB cap is dropped, and said");
    check(took >= 60000 && took < 61000, "wait(60000) returns after about 60 s");
    printf("      wait(60000) took %lu ms; command \"%s\" handled; push stored\n", took, recorded.c_str());
    bool newToken = false;
    for (auto& r : sim.requests) newToken = newToken || r.header("X-Chirp-Token") == "TOKEN-ROTATED-DURING-WAIT";
    check(newToken, "wait(): command checks after the push use the new token");
  }

  // 14. Stays connected - SIMULATED faults: a
  // board that reports once a minute (sendNumber, then wait(60000)) stores
  // messages again by itself within one report interval plus one minute after
  // each fault ends. Requests take 1 s here.
  {
    std::vector<unsigned long> stored;
    sim.server = [&](const SimRequest& r) {
      if (isUplink(r)) {
        stored.push_back(simNowMs);
        return answer(202, "Accepted");
      }
      return answer(204, "No Content");
    };
    sim.exchangeMs = 1000;
    Warblet w;
    w.begin();
    auto report = [&](unsigned long ms) {
      unsigned long start = millis();
      while (millis() - start < ms) {
        w.sendNumber(21.5);
        w.wait(60000);
      }
    };
    auto firstStoredAfter = [&](unsigned long t) -> long {
      for (unsigned long s : stored) {
        if (s >= t) return (long)(s - t);
      }
      return -1;
    };
    auto fault = [&](const char* name, unsigned long lastsMs, std::function<void(bool)> set) {
      report(130000);  // healthy first
      set(true);
      report(lastsMs);
      set(false);
      unsigned long ended = millis();
      report(180000);
      long back = firstStoredAfter(ended);
      check(back >= 0 && back <= 120000, std::string("stays connected (simulated): ") + name);
      printf("      stays connected (simulated): %-34s stored again %ld s after it ended\n", name,
             back < 0 ? -1L : back / 1000);
    };
    auto wifiDown = [&](bool on) {
      sim.wifiJoins = !on;
      if (on) warblet::board::wifiStop();
    };
    fault("Wi-Fi gone for 3 minutes", 180000, wifiDown);
    fault("Wi-Fi gone for 20 minutes", 1200000, wifiDown);
    fault("TLS refused or DNS failing, 5 minutes", 300000, [&](bool on) { sim.connectFails = on; });
    fault("server not answering, 5 minutes", 300000, [&](bool on) { sim.silent = on; });
    fault("junk on the USB port while reporting", 1000, [&](bool on) {
      if (!on) return;
      std::string junk = "\x5a\xa6";
      for (int i = 0; i < 1024; i++) junk += (char)rnd8();
      junk += "\xe2\x9c";  // a lone partial character
      Serial.type(junk);
    });
    // A restart: a new instance, as after a reset, loads its settings and stores again.
    unsigned long restartAt = millis();
    Warblet again2;
    again2.begin();
    for (int i = 0; i < 3; i++) {
      again2.sendNumber(21.5);
      again2.wait(60000);
    }
    long back = firstStoredAfter(restartAt);
    check(back >= 0 && back <= 120000, "stays connected (simulated): a restart");
    printf("      stays connected (simulated): %-34s stored again %ld s after it\n", "a restart",
           back < 0 ? -1L : back / 1000);
  }
  sim.exchangeMs = 0;

  // 14b. Wi-Fi gone for 20 minutes: the board keeps trying its networks, and
  // says so once, not two lines every round; it says when it is back.
  {
    sim.server = [&](const SimRequest& r) {
      return isUplink(r) ? answer(202, "Accepted") : answer(204, "No Content");
    };
    Warblet w;
    w.begin();
    w.wait(15000);
    sim.wifiJoins = false;
    warblet::board::wifiStop();
    Serial.out.clear();
    int beginsBefore = sim.wifiBegins;
    w.wait(20UL * 60UL * 1000UL);
    int tries = sim.wifiBegins - beginsBefore;
    check(tries >= 20, "Wi-Fi gone: the board keeps trying (" + std::to_string(tries) + " tries in 20 minutes)");
    check(occurrences("no stored Wi-Fi network joined") == 1 && occurrences("joining Wi-Fi network") <= 1,
          "Wi-Fi gone: said once, not every round");
    sim.wifiJoins = true;
    w.wait(60000);
    check(said("Wi-Fi joined"), "Wi-Fi back: joined again, and said");
    printf("      Wi-Fi gone 20 minutes: %d tries, %d console lines\n", tries,
           occurrences("warblet:"));
  }

  // 15. Unclaimed stop: once a
  // report is answered "unclaimed" - the device was deleted, or its code was
  // never redeemed - the board stops reporting and stops asking for commands,
  // turns Wi-Fi off, and stays so across a restart, until USB setup stores a
  // new setup. It keeps a fingerprint of the refused setup, never the code.
  {
    bool unclaimed = true;
    sim.exchangeMs = 0;
    sim.refusedMark.clear();
    sim.server = [&](const SimRequest& r) {
      if (isUplink(r)) {
        return unclaimed ? answer(202, "Accepted", "{\"status\":\"unclaimed\"}", "Content-Type: application/json\r\n")
                         : answer(202, "Accepted");
      }
      return answer(204, "No Content");
    };
    std::string credBefore;
    {
      size_t at = sim.stored.find("\"token\":\"");
      if (at != std::string::npos) credBefore = sim.stored.substr(at + 9, sim.stored.find('"', at + 9) - at - 9);
    }
    Warblet w;
    w.begin();
    w.wait(3000);
    Serial.out.clear();
    check(!w.sendNumber(1), "unclaimed: the report is not counted as stored");
    size_t afterFirst = sim.requests.size();
    for (int i = 0; i < 20; i++) {  // twenty minutes of a sketch that reports once a minute
      w.sendNumber(1);
      w.wait(60000);
    }
    check(sim.requests.size() == afterFirst, "unclaimed: for 20 minutes nothing more is sent, no reports, no command checks");
    check(!warblet::board::wifiConnected() && !w.connected(), "unclaimed: Wi-Fi is turned off");
    size_t lines = 0;
    for (size_t p = Serial.out.find("not on an account"); p != std::string::npos; p = Serial.out.find("not on an account", p + 1)) {
      lines++;
    }
    check(lines == 1, "unclaimed: the console says so once, not every minute");
    check(sim.refusedMark.size() == 16 && strspn(sim.refusedMark.c_str(), "0123456789abcdef") == 16 &&
              (credBefore.empty() || sim.refusedMark.find(credBefore) == std::string::npos),
          "unclaimed: the board keeps a 16-hex fingerprint, never the code");

    // A restart with the same setup sends nothing.
    Warblet restarted;
    Serial.out.clear();
    restarted.begin();
    size_t beforeRestart = sim.requests.size();
    for (int i = 0; i < 5; i++) {
      restarted.sendNumber(1);
      restarted.wait(60000);
    }
    check(sim.requests.size() == beforeRestart && said("not on an account"),
          "unclaimed: after a restart with the same setup it sends nothing, and says why");

    // A Wi-Fi-only push is not a new setup: still stopped.
    Serial.type("CHIRP+ {\"ssid\":\"lab-2\",\"pass\":\"pw2\"}\r\n");
    for (int i = 0; i < 3; i++) {
      restarted.sendNumber(1);
      restarted.wait(60000);
    }
    check(said("CHIRP= ok") && sim.requests.size() == beforeRestart,
          "unclaimed: a Wi-Fi-only push is stored, and the board stays stopped");

    // A new setup over USB brings it back.
    unclaimed = false;
    Serial.out.clear();
    Serial.type("CHIRP+ {\"token\":\"TOKEN-AFTER-A-NEW-SETUP\"}\r\n");
    restarted.wait(15000);
    bool sentAgain = restarted.sendNumber(2);
    check(said("CHIRP= ok") && sentAgain && sim.requests.size() > beforeRestart,
          "unclaimed: a new setup over USB brings it back, and its report is stored");
    check(sim.refusedMark.empty(), "unclaimed: the fingerprint is cleared once a new setup is stored");
  }

  // 15b. A board with typed settings ignores the setup page, so when it stops
  // on "unclaimed" it is told to change the settings in the sketch.
  {
    sim.server = [&](const SimRequest& r) {
      return isUplink(r) ? answer(202, "Accepted", "{\"status\":\"unclaimed\"}") : answer(204, "No Content");
    };
    sim.refusedMark.clear();
    Serial.out.clear();
    Warblet typed;
    typed.begin("lab", "pw", "sim-typed-1", "TOKEN-TYPED");
    typed.wait(15000);
    typed.sendNumber(1);
    check(said("not on an account") && said("begin()") && !said("over USB"),
          "typed settings: the stop says to change them in the sketch, not over USB");
    sim.refusedMark.clear();
  }

  // 16. MQTT over TLS: the
  // same class, Warblet board(Warblet::MQTT), on Espressif's own client. The
  // simulated broker stands in for mqtt.warbletiot.com:8883.
  {
    sim.exchangeMs = 0;
    broker = SimBroker();
    size_t httpBefore = sim.requests.size();
    Warblet m(Warblet::MQTT);
    m.begin();
    m.onCommand(recordCommand);
    recorded.clear();
    Serial.out.clear();
    m.wait(3000);
    check(broker.starts == 1 && broker.host == "mqtt.warbletiot.com" && broker.port == 8883,
          "MQTT: one session to mqtt.warbletiot.com:8883 (TLS)");
    check(broker.clientId == "sim-board-1" && broker.user == "sim-board-1" && !broker.passNull &&
              broker.pass == "TOKEN-AFTER-A-NEW-SETUP",
          "MQTT: client id and username are the device id, the password is the device's credential");
    check(broker.subscribe == "chirp/sim-board-1/down", "MQTT: subscribes to the board's command topic");
    check(said("MQTT connected"), "MQTT: the console says it connected");

    // At Signed: the 32-byte v0.31 envelope in front of the payload, tag in the topic.
    check(m.sendNumber(21.5, "led"), "MQTT: a report is published and the broker acknowledges it");
    uint8_t key[32], mac[32];
    warblet::decodeKey(kKey2, 44, key);
    warblet::signEnvelope(key, 32, "led", reinterpret_cast<const uint8_t*>("21.5"), 4, mac);
    check(!broker.published.empty() && broker.published.back().first == "chirp/sim-board-1/up/led",
          "MQTT: a tagged report goes to chirp/<id>/up/<tag>");
    check(!broker.published.empty() && broker.published.back().second.size() == 36 &&
              memcmp(broker.published.back().second.data(), mac, 32) == 0 &&
              broker.published.back().second.substr(32) == "21.5",
          "MQTT at Signed: 32 raw HMAC bytes over \"\\ntag=led\\n21.5\" (v0.31), then the payload");

    // Commands arrive at once over the subscription; no HTTPS at all.
    broker.inbox.push_back("led:on");
    unsigned long t0 = millis();
    m.wait(100);
    check(recorded == "led:on" && millis() - t0 <= 200, "MQTT: a command reaches the command function at once");
    check(sim.requests.size() == httpBefore, "MQTT: no HTTPS request at all, so no polling");

    // The broker drops the session: the client comes back by itself.
    Serial.out.clear();
    warblet::board::simNote("a command was cut off on the way and was dropped");
    m.wait(100);
    warblet::board::simDropSession();
    m.wait(3000);
    broker.inbox.push_back("led:off");
    m.wait(100);
    check(said("warblet: MQTT: a command was cut off on the way and was dropped"),
          "MQTT: what the board layer reports (a dropped command) is said");
    check(said("MQTT session lost") && said("MQTT connected") && recorded == "led:off",
          "MQTT: after the broker drops the session it reconnects, says so, and commands flow again");

    // The broker refuses the credential: the board backs off and says so once.
    Serial.out.clear();
    broker.refuse = true;
    warblet::board::simDropSession();
    int startsBefore = broker.starts;
    for (int i = 0; i < 600; i++) m.wait(1000);  // ten minutes
    int restarts = broker.starts - startsBefore;
    size_t refusedLines = 0;
    for (size_t p = Serial.out.find("MQTT refused"); p != std::string::npos; p = Serial.out.find("MQTT refused", p + 1)) {
      refusedLines++;
    }
    check(restarts >= 3 && restarts <= 6 && refusedLines == 1,
          "MQTT: a refused CONNECT backs off (a few sessions in ten minutes, not sixty) and is said once");
    check(m.answer() == 401 && m.state() == Warblet::REFUSED,
          "MQTT: a refused CONNECT is answer() 401 and state() REFUSED");
    printf("      MQTT: %d sessions started in ten minutes of refusals, %u console lines\n", restarts,
           (unsigned)refusedLines);
    broker.refuse = false;
    for (int i = 0; i < 900 && !broker.up; i++) m.wait(1000);
    check(broker.up && m.connected(), "MQTT: once the credential is accepted again, the session comes back");

    // Seen on a board: new credentials pushed during a refusal's back-off
    // waited out the back-off (2 min 40 s). A corrected setup acts at once.
    broker.refuse = true;
    warblet::board::simDropSession();
    for (int i = 0; i < 200; i++) m.wait(1000);  // into the back-off
    broker.refuse = false;
    Serial.type("CHIRP+ {\"token\":\"TOKEN-PUSHED-DURING-BACKOFF\"}\r\n");
    unsigned long pushedAt = millis();
    for (int i = 0; i < 100 && !m.connected(); i++) m.wait(100);
    check(m.connected() && broker.pass == "TOKEN-PUSHED-DURING-BACKOFF" && millis() - pushedAt <= 5000,
          "MQTT: new credentials pushed during a back-off start a session at once");
    printf("      MQTT: session up %lu ms after a push made during the back-off\n", millis() - pushedAt);

    // A wrong host pushed over USB: no session, said; the right one: back.
    Serial.out.clear();
    Serial.type("CHIRP+ {\"host\":\"mqtt.wrong.example\"}\r\n");
    m.wait(3000);
    check(broker.host == "mqtt.wrong.example" && !m.connected() && said("MQTT has no session"),
          "MQTT: a wrong host pushed over USB is used, fails, and is said");
    // The client tries again and again, and each failure ends in a disconnect
    // (found on the board: a pair of lines every 15 s). Said once; nothing was lost.
    for (int i = 0; i < 60; i++) m.wait(1000);
    check(occurrences("MQTT has no session") == 1 && !said("session lost"),
          "MQTT: a host that keeps failing is said once, and no session is called lost");
    Serial.type("CHIRP+ {\"host\":\"\"}\r\n");
    m.wait(3000);
    check(broker.host == "mqtt.warbletiot.com" && m.connected(), "MQTT: pushing no host goes back to the built-in one");

    // At Token (no key): the payload alone, untagged topic.
    Serial.type("CHIRP+ {\"key\":\"\"}\r\n");
    m.wait(3000);
    check(m.sendNumber(21.5) && broker.published.back().first == "chirp/sim-board-1/up" &&
              broker.published.back().second == "21.5",
          "MQTT at Token: the payload alone, on chirp/<id>/up");
  }

  // 16b. While the broker cannot be reached, a report returns at once and the
  // board says so once: a sketch that sends on every pass is not held up
  // 15 s a time, and its console is not filled. Back once the session is.
  {
    broker = SimBroker();
    broker.unreachable = true;
    sim.refusedMark.clear();
    Warblet m(Warblet::MQTT);
    m.begin();
    m.wait(30000);
    Serial.out.clear();
    unsigned long t0 = millis();
    int ok = 0;
    for (int i = 0; i < 100; i++) ok += m.sendNumber(i);
    unsigned long took = millis() - t0;
    check(ok == 0 && took < 2000 && occurrences("not sent") == 1,
          "MQTT, no session: 100 reports return at once, said once");
    printf("      MQTT, no session: 100 reports took %lu ms, %d \"not sent\" lines\n", took, occurrences("not sent"));
    broker.unreachable = false;
    m.wait(15000);
    check(m.sendNumber(1) && broker.published.size() == 1, "MQTT: once the session is back, reports go again");
  }

  // 17. Not on an account, over MQTT (contract v0.47): the broker refuses a
  // CONNECT for an id that is no device with code 4 (its device was deleted,
  // or its setup code was spent). The board stops as it does on HTTPS: no
  // sessions, Wi-Fi off, said once, kept across a restart, until a new setup.
  {
    broker = SimBroker();
    broker.noDevice = true;
    sim.refusedMark.clear();
    Serial.out.clear();
    Warblet m(Warblet::MQTT);
    m.begin();
    for (int i = 0; i < 20; i++) {
      m.sendNumber(1);
      m.wait(60000);
    }
    check(broker.starts == 1 && broker.published.empty(), "MQTT code 4: one session tried, none in the next 20 minutes");
    check(occurrences("not on an account") == 1, "MQTT code 4: the board says once that it is not on an account");
    check(!warblet::board::wifiConnected(), "MQTT code 4: Wi-Fi is turned off");
    check(sim.refusedMark.size() == 16 && sim.stored.find(sim.refusedMark) == std::string::npos,
          "MQTT code 4: the board keeps a fingerprint, never the code");
    Warblet again(Warblet::MQTT);
    again.begin();
    again.wait(120000);
    check(broker.starts == 1 && occurrences("not on an account") == 2,
          "MQTT code 4: after a restart it stays stopped, and says why");
    broker.noDevice = false;
    Serial.type("CHIRP+ {\"token\":\"TOKEN-AFTER-A-NEW-MQTT-SETUP\"}\r\n");
    again.wait(15000);
    check(again.connected() && broker.pass == "TOKEN-AFTER-A-NEW-MQTT-SETUP" && sim.refusedMark.empty(),
          "MQTT code 4: a new setup over USB brings it back");
  }

  printf("\nsim: %d passed, %d failed\n%s\n", pass, fail, fail ? "FAILED" : "PASSED");
  return fail ? 1 : 0;
}
