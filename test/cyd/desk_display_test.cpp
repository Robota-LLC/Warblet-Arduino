// Runs the DeskDisplay example, as it is, on the simulated board (test/sim)
// with a fake screen and touch (fake_screen.cpp) against a fake Warblet. It
// checks what a person sees and what Warblet gets: the screens' words, the
// line per computer, one "seen" per message, the not-on-an-account stop, the
// self-restart, the pace; and that the routine and decoder are the Warblet
// MicroPython desk display's files, unchanged.
//
// What this proves: the sketch's logic and words. It does NOT prove the
// picture on a real screen (screen_test.cpp checks the driver's bytes).

#include <stdio.h>
#include <string.h>

#include <deque>
#include <string>

#include "WarbletBoard.h"
#include "fake_board.h"
#include "fake_screen.h"
#include "warblet/core/sha256.h"

// The sketch, four times: each restart of the board (board::restart(), which
// the simulator records) boots the next copy, with its globals fresh and the
// settings it stored still there, as a real restart does. Its headers come
// first, so the copies share them.
#include <WiFi.h>
#include <Warblet.h>
#include <ctype.h>

#include <vector>

#include "screen.h"
namespace boot1 {
#include "../../examples/DeskDisplay/DeskDisplay.ino"
}
namespace boot2 {
#include "../../examples/DeskDisplay/DeskDisplay.ino"
}
namespace boot3 {
#include "../../examples/DeskDisplay/DeskDisplay.ino"
}
namespace boot4 {
#include "../../examples/DeskDisplay/DeskDisplay.ino"
}
using namespace boot1;  // the sketch's constants, the same in every copy

SimSignal WiFi;
SimEsp ESP;

namespace {

int pass = 0, fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    pass++;
  } else {
    fail++;
    printf("FAIL  %s\n", what.c_str());
  }
}

bool said(const std::string& text) { return Serial.out.find(text) != std::string::npos; }

struct Boot {
  void (*setup)();
  void (*loop)();
};
const Boot boots[] = {{boot1::setup, boot1::loop}, {boot2::setup, boot2::loop}, {boot3::setup, boot3::loop},
                      {boot4::setup, boot4::loop}};
int booted = 0;             // which copy is running
size_t restartsSeen = 0;    // sim.restarts already acted on
bool rebootFailed = false;  // more restarts than copies

// The board restarted: Wi-Fi goes down with it, and the next copy starts.
void reboot() {
  warblet::board::wifiStop();
  if (booted + 1 >= (int)(sizeof boots / sizeof boots[0])) {
    rebootFailed = true;
    return;
  }
  booted++;
  boots[booted].setup();
}

void runFor(unsigned long ms) {
  unsigned long end = millis() + ms;
  while ((long)(end - millis()) > 0) {
    boots[booted].loop();
    if (sim.restarts.size() < restartsSeen) restartsSeen = sim.restarts.size();  // the test cleared them
    if (sim.restarts.size() > restartsSeen) {
      restartsSeen = sim.restarts.size();
      reboot();
    }
  }
}

bool isPoll(const SimRequest& r) { return r.head.find("/down HTTP/1.1\r\n") != std::string::npos; }
bool isUplink(const SimRequest& r) { return r.head.rfind("POST", 0) == 0 && !isPoll(r); }

std::string answer(int status, const std::string& body = "", const std::string& extra = "") {
  return "HTTP/1.1 " + std::to_string(status) + " X\r\n" + extra + "Content-Length: " + std::to_string(body.size()) +
         "\r\n\r\n" + body;
}

// The messages the board sent from request i on: tag and body.
std::vector<std::pair<std::string, std::string>> sentFrom(size_t i) {
  std::vector<std::pair<std::string, std::string>> out;
  for (; i < sim.requests.size(); i++) {
    if (isUplink(sim.requests[i])) out.push_back(std::make_pair(sim.requests[i].header("X-Chirp-Tag"), sim.requests[i].body));
  }
  return out;
}

int count(const std::vector<std::pair<std::string, std::string>>& sent, const std::string& tag) {
  int n = 0;
  for (auto& s : sent) n += s.first == tag;
  return n;
}

std::string strip() { return simWordsAt(1); }
uint16_t stripColour() { return simTextAt(1) ? simTextAt(1)->colour : 0; }
std::string middle() { return simMiddle(MESSAGE_TOP, MESSAGE_TOP + MESSAGE_HEIGHT); }
int middleSize() {
  const SimText* t = simTextAt(MESSAGE_TOP);
  return t ? t->size : 0;
}

std::string fileSha(const char* path) {
  FILE* f = fopen(path, "rb");
  if (f == nullptr) return "missing";
  warblet::Sha256 h;
  uint8_t buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) h.update(buf, n);
  fclose(f);
  uint8_t d[32];
  h.finish(d);
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
  return hex;
}

void tap() {
  simTouch = true;
  runFor(200);
  simTouch = false;
  runFor(200);
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::deque<std::string> waiting;  // commands for the board
  int status = 0;                   // 0: answer normally
  bool unclaimed = false;
  sim.server = [&](const SimRequest& r) -> std::string {
    if (status != 0) return answer(status);
    if (isUplink(r)) return unclaimed ? answer(202, "{\"status\":\"unclaimed\"}") : answer(202);
    if (waiting.empty()) return answer(204);
    std::string body = waiting.front();
    waiting.pop_front();
    return answer(200, body, "X-Chirp-Down: data\r\n");
  };
  auto command = [&](const std::string& text) {
    waiting.push_back(text);
    runFor(7000);  // one check (every 5 s), and the drawing
  };

  // ---- not set up -------------------------------------------------------------
  boots[0].setup();
  runFor(2000);
  check(strip() == "Not set up" && stripColour() == RED, "not set up: the strip says Not set up, in red");
  check(middle().find("Set me up at") != std::string::npos && middle().find("warbletiot.com/flash") != std::string::npos,
        "not set up: the middle says Set me up at warbletiot.com/flash");
  check(sim.requests.empty(), "not set up: nothing is sent");
  Serial.type("CHIRP?\r\n");
  runFor(500);
  check(said("CHIRP! v=1 radios=wifi sig=1 t=demo-cyd-desk-display-arduino"), "the announce names the demo (t=)");

  // ---- set up over USB: stored, then a restart --------------------------------------
  Serial.type("CHIRP+ {\"ssid\":\"lab\",\"pass\":\"pw\",\"hwid\":\"sim-desk-1\",\"claim\":\"CHIRP-DESK1234\"}\r\n", 10500);
  runFor(30000);
  check(booted == 1 && sim.restarts.size() == 1 && said("CHIRP= ok"),
        "set up: the push is stored and answered ok, then the board restarts into the new setup");
  std::string seq;
  for (auto& s : simStrip) seq += (seq.empty() ? "" : " > ") + s;
  check(seq.find("Not set up > Joining Wi-Fi > Reaching Warblet > Connected") == 0,
        "set up: the strip goes Joining Wi-Fi, Reaching Warblet, Connected");
  check(stripColour() == GREEN, "Connected is green");
  check(simTextAt(1)->x == 4 && simTextAt(1)->size == 2, "the strip's words start 4 pixels in, so it holds 19 letters");
  check(middle() == "Type a message for|sim-desk-1 on your|Warblet dashboard." && simTextAt(MESSAGE_TOP)->colour == GREY,
        "the waiting text names the board, in grey");
  auto sent = sentFrom(0);
  check(!sent.empty() && sent[0].first == "hello" && sent[0].second == "{\"wifi_dbm\": -52}",
        "the first report is tagged hello and carries the Wi-Fi signal");
  check(said("desk: hello stored, 172000 bytes free, largest block 110000"),
        "each message it sends is said with the free memory, as the MicroPython desk display prints it");
  printf("      strip: %s\n", seq.c_str());

  // ---- a message --------------------------------------------------------------
  Serial.out.clear();
  command("Dinner is ready, come down when you can");
  check(middle() == "Dinner is ready,|come down when you|can" && middleSize() == 2,
        "a 39-letter message: medium letters over three lines");
  check(simWordsAt(FOOTER_TOP + 1) == "Message just now" && simTextAt(FOOTER_TOP + 1)->colour == GREY,
        "footer: Message just now, in grey");
  check(simWordsAt(FOOTER_TOP + 21) == "Tap: I've seen it", "footer: Tap: I've seen it");
  check(!said("Dinner") && said("command received ("), "the console never shows the message's words");
  command("Hi");
  check(middle() == "Hi" && middleSize() == 4, "a short message: the biggest letters");
  std::string longest(256, 'x');
  for (size_t i = 9; i < longest.size(); i += 10) longest[i] = ' ';
  command(longest);
  int lines = 0;
  for (char c : middle()) lines += c == '|';
  check(middleSize() == 1 && lines + 1 == 7, "256 letters: all of them, in the smallest letters (seven lines)");
  command("Caf\xc3\xa9 \xf0\x9f\x8e\x89");
  check(middle() == "Caf? ?", "letters outside plain English show as '?'");
  command("Hello\nWorld");
  check(middle() == "Hello|World" && middleSize() == 4,
        "a newline separates words, as in MicroPython: Hello and World on two lines at the biggest size");
  command("Tea\tnow");
  check(middle() == "Tea now", "a tab separates words too");

  // ---- seen -------------------------------------------------------------------
  command("Dinner is ready");
  size_t before = sim.requests.size();
  tap();
  runFor(1000);
  sent = sentFrom(before);
  check(count(sent, "seen") == 1 && sent.back().second == "{\"seen\": \"Dinner is ready\"}",
        "a tap sends one seen with the message");
  check(simWordsAt(FOOTER_TOP + 21) == "Seen: sent" && simTextAt(FOOTER_TOP + 21)->colour == GREEN, "footer: Seen: sent, in green");
  tap();
  tap();
  runFor(6000);
  check(count(sentFrom(before), "seen") == 1, "more taps on the same message send nothing more");
  check(said("desk: seen stored, 172000 bytes free") && said("desk: report stored, 172000 bytes free"),
        "a seen and a report are said with the free memory too");
  command("Second one");
  check(simWordsAt(FOOTER_TOP + 21) == "Tap: I've seen it", "a new message can be seen again");
  tap();
  runFor(1000);
  check(count(sentFrom(before), "seen") == 2, "...and its tap sends its own seen");

  // ---- the routine's lines ------------------------------------------------------
  command("info:61% full: kitchen-pi /");
  check(simWordsAt(INFO_TOP + 1) == "61% kitchen-pi /" && simTextAt(INFO_TOP + 1)->colour == YELLOW,
        "a disk line: '61% kitchen-pi /', in yellow");
  command("info:40% full: nas /data");
  command("info:62% full: kitchen-pi /");
  check(simWordsAt(INFO_TOP + 1) == "62% kitchen-pi /" && simWordsAt(INFO_TOP + 19) == "40% nas /data",
        "each computer keeps its own line, updated in place");
  command("info:12% full: laptop C:");
  command("info:90% full: desk /");
  check(simWordsAt(INFO_TOP + 1) == "62% kitchen-pi /" && simWordsAt(INFO_TOP + 19) == "12% laptop C:" &&
            simWordsAt(INFO_TOP + 37) == "90% desk /",
        "a fourth computer replaces the one heard from longest ago");
  command("clear");
  check(middle() == "Type a message for|sim-desk-1 on your|Warblet dashboard." && simWordsAt(FOOTER_TOP + 1).empty() &&
            simWordsAt(INFO_TOP + 1) == "62% kitchen-pi /",
        "clear: the waiting text again, no footer, the disk lines stay");

  // ---- the pace -----------------------------------------------------------------
  before = sim.requests.size();
  runFor(60000);
  int polls = 0;
  for (size_t i = before; i < sim.requests.size(); i++) polls += isPoll(sim.requests[i]);
  sent = sentFrom(before);
  check(polls == 12, "a check for messages every 5 s (12 a minute)");
  check(sent.size() == 2 && sent[0].second == "{\"wifi_dbm\": -52}" && sent[0].first.empty(),
        "a report with the Wi-Fi signal every 30 s, untagged after the hello");
  printf("      a minute: %d checks, %u reports\n", polls, (unsigned)sent.size());

  // ---- offline: a tap waits -----------------------------------------------------
  command("Back soon");
  sim.wifiJoins = false;
  warblet::board::wifiStop();
  runFor(20000);
  check(strip() == "No Wi-Fi, retrying" || strip() == "Joining Wi-Fi", "Wi-Fi gone: the strip says so");
  before = sim.requests.size();
  tap();
  check(simWordsAt(FOOTER_TOP + 21) == "Seen: not sent yet", "a tap while offline: Seen: not sent yet");
  check(middle() == "Back soon", "the message stays while Wi-Fi is gone");
  sim.wifiJoins = true;
  runFor(60000);
  sent = sentFrom(before);
  check(count(sent, "seen") == 1 && simWordsAt(FOOTER_TOP + 21) == "Seen: sent", "back online: the waiting seen is sent");

  // ---- a Wi-Fi-only push: no restart, the message stays ----------------------------
  {
    int bootNow = booted;
    size_t restartsNow = sim.restarts.size();
    Serial.type("CHIRP+ {\"ssid\":\"lab\",\"pass\":\"pw\"}\r\n");
    runFor(30000);
    check(booted == bootNow && sim.restarts.size() == restartsNow && middle() == "Back soon" &&
              strip() == "Connected",
          "a Wi-Fi-only push: no restart, and the message stays");
  }

  // ---- Warblet's answers on the strip -------------------------------------------
  status = 401;
  runFor(10000);
  check(strip() == "Warblet refused 401" && stripColour() == RED, "a 401: Warblet refused 401, in red");
  status = 429;
  runFor(70000);
  check(strip() == "Warblet: slow down" && stripColour() == AMBER, "a 429: Warblet: slow down");
  status = 503;
  runFor(150000);  // the 429 has slowed the checks: the next one comes within two minutes
  check(strip() == "Warblet: no answer" && stripColour() == AMBER,
        "a 503 is Warblet's own fault: Warblet: no answer, as the MicroPython desk display says");
  status = 403;
  runFor(150000);
  check(strip() == "Warblet refused 403" && stripColour() == RED, "a 403 turns the board away: Warblet refused 403");
  status = 0;
  runFor(660000);  // the refusals slowed the checks; back to normal within ten minutes
  status = 0;
  sim.silent = true;
  runFor(130000);
  check(strip() == "Warblet: no answer" && stripColour() == AMBER, "no answer: Warblet: no answer");
  sim.silent = false;
  runFor(70000);
  check(strip() == "Connected", "and Connected again");

  // ---- the self-restart -----------------------------------------------------------
  sim.restarts.clear();
  sim.connectFails = true;
  runFor(12UL * 60UL * 1000UL);
  sim.connectFails = false;
  check(sim.restarts.size() >= 1, "Wi-Fi up, nothing answering for 10 minutes: the board restarts itself");
  check(booted == 2 && middle().find("Type a message for") == 0,
        "after that restart the sketch starts afresh: the waiting text, no message");
  sim.restarts.clear();
  runFor(70000);

  // ---- not on an account ----------------------------------------------------------
  unclaimed = true;
  runFor(40000);  // the next report is answered "unclaimed"
  check(strip() == "Not on an account" && stripColour() == AMBER, "unclaimed: the strip says Not on an account");
  check(middle().find("Set me up at") != std::string::npos, "unclaimed: Set me up at warbletiot.com/flash");
  check(!simShows("sim-desk-1") && !simShows("kitchen-pi") && !simShows("Back soon"),
        "unclaimed: neither the id nor anything the account sent stays on the screen");
  before = sim.requests.size();
  runFor(20UL * 60UL * 1000UL);
  check(sim.requests.size() == before && !warblet::board::wifiConnected() && sim.restarts.empty(),
        "unclaimed: nothing sent for 20 minutes, Wi-Fi off, no self-restart");

  // ---- not on an account, then set up again: a new start, and a hello ---------------
  unclaimed = false;
  before = sim.requests.size();
  size_t restartsBefore = sim.restarts.size();
  Serial.type("CHIRP+ {\"ssid\":\"lab\",\"pass\":\"pw\",\"hwid\":\"sim-desk-2\",\"claim\":\"CHIRP-DESK5678\"}\r\n");
  runFor(40000);
  sent = sentFrom(before);
  check(sim.restarts.size() == restartsBefore + 1 && booted == 3,
        "set up again after the stop: the push is a new start (a restart)");
  check(!sent.empty() && sent[0].first == "hello", "set up again: the first report is tagged hello");
  check(strip() == "Connected" && middle() == "Type a message for|sim-desk-2 on your|Warblet dashboard.",
        "set up again: Connected, and the waiting text names the new id");
  check(!rebootFailed, "no more restarts than the test expects");

  // ---- the routine and the decoder are the MicroPython demo's ------------------
  check(fileSha("examples/DeskDisplay/desk-display-disk.star") ==
            "6da2fc7ecf7c0e8684a79930cc240002f9f92f08d0a1e10fa7559ed65dea328f",
        "desk-display-disk.star is the MicroPython desk display's, byte for byte");
  check(fileSha("examples/DeskDisplay/decoder.star") == "c20e657d267c3c30c220f4564e766408eda40b3315f53c02680cda4f8a6595b0",
        "decoder.star is the MicroPython desk display's, byte for byte");

  printf("\ndesk display (the DeskDisplay example on the PC): %d passed, %d failed\n%s\n", pass, fail,
         fail ? "FAILED" : "PASSED");
  return fail ? 1 : 0;
}
