// Runs one example sketch, as it is, on the simulated board against a fake
// Warblet (or broker), and reports what it did. run_tests.sh builds it once
// per example: -DEXAMPLE_INO='"examples/X/X.ino"' and -DEX_<NAME>.
//
//   example_run record FILE   the example's inputs and commands; every message
//                             it sends is written to FILE as "tag<TAB>hex<TAB>
//                             value the decoder must give", for run_decoders.py
//   example_run flood STATUS  every request answered STATUS (401, 403 or 500;
//                             for MQTT a refused or unreachable broker) for ten
//                             minutes, 600 ms per request: requests and console
//                             lines are counted, and must stay few
//   example_run corrected MIN every request refused 401 (for MQTT, the broker
//                             refuses the session) for MIN minutes; then a
//                             setup push with a token Warblet accepts. The
//                             hello must be stored at once, not when the
//                             report hold or the back-off would have ended
//
// What this proves: what the examples send and how often, with the library's
// logic, on stand-in headers. Not a board compile, and no real decoder runs here.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <string>

#include "Arduino.h"
#include "fake_board.h"

#include EXAMPLE_INO

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) failures++;
}

std::string answer(int status, const std::string& body = "", const std::string& extra = "") {
  return "HTTP/1.1 " + std::to_string(status) + " X\r\n" + extra + "Content-Length: " + std::to_string(body.size()) +
         "\r\n\r\n" + body;
}

bool isPoll(const SimRequest& r) { return r.head.find("/down HTTP/1.1\r\n") != std::string::npos; }
bool isUplink(const SimRequest& r) { return r.head.rfind("POST", 0) == 0 && !isPoll(r); }

int occurrences(const std::string& text) {
  int n = 0;
  for (size_t p = Serial.out.find(text); p != std::string::npos; p = Serial.out.find(text, p + 1)) n++;
  return n;
}

// Run the sketch's loop() for ms of simulated time.
void runFor(unsigned long ms) {
  unsigned long end = millis() + ms;
  while ((long)(end - millis()) > 0) {
    loop();
    delay(1);
  }
}

void pushSettings() {
  Serial.type("CHIRP+ {\"ssid\":\"lab\",\"pass\":\"pw\",\"hwid\":\"sim-board-1\",\"token\":\"TOKEN-1\"}\r\n");
}

std::string hex(const std::string& s) {
  std::string out;
  char b[3];
  for (unsigned char c : s) {
    snprintf(b, sizeof b, "%02x", c);
    out += b;
  }
  return out;
}

#if defined(EX_LIGHT_MQTT)
const bool kMqtt = true;
#else
const bool kMqtt = false;
#endif

// Everything the example sent, in order: tag and payload.
std::vector<std::pair<std::string, std::string>> sent() {
  std::vector<std::pair<std::string, std::string>> out;
  if (kMqtt) {
    for (auto& p : broker.published) {
      size_t up = p.first.find("/up");
      std::string tag = p.first.size() > up + 4 ? p.first.substr(up + 4) : "";
      out.push_back(std::make_pair(tag, p.second));
    }
  } else {
    for (auto& r : sim.requests) {
      if (isUplink(r)) out.push_back(std::make_pair(r.header("X-Chirp-Tag"), r.body));
    }
  }
  return out;
}

int record(const char* file) {
#if defined(EXPECT_LED_ON)
  // Built as for a given board: the sketch must light its LED at the level
  // that board needs, whatever the sketch's own #if says.
  check(LED_ON == EXPECT_LED_ON, std::string("built for this board, the sketch lights the LED on ") +
                                     (EXPECT_LED_ON == LOW ? "LOW" : "HIGH"));
#endif
  std::deque<std::string> commands;
  sim.server = [&](const SimRequest& r) -> std::string {
    if (isUplink(r)) return answer(202);
    if (commands.empty()) return "HTTP/1.1 204 No Content\r\n\r\n";
    std::string body = commands.front();
    commands.pop_front();
    return answer(200, body, "X-Chirp-Down: data\r\n");
  };
  auto command = [&](const char* text) {
    if (kMqtt) {
      broker.inbox.push_back(text);
    } else {
      commands.push_back(text);
    }
    runFor(25000);
  };
  (void)command;  // the examples that take no commands
  // What each message should decode to, in the order they are sent.
  std::vector<std::pair<std::string, double>> expected;
  simPin[4] = HIGH;  // the button (pin 4, a pull-up) is not pressed
  pushSettings();
  setup();
#if defined(EX_SENSOR)
  simAnalog = 512;
  runFor(150000);  // the first reading goes before Wi-Fi is up; two more, a minute apart
  for (int i = 0; i < 2; i++) expected.push_back(std::make_pair("", 50.0));
  simAnalog = 1023;
  runFor(60000);
  expected.push_back(std::make_pair("", 100.0));
  simAnalog = 0;
  runFor(60000);
  expected.push_back(std::make_pair("", 0.0));
#elif defined(EX_BUTTON)
  runFor(20000);
  for (int i = 1; i <= 3; i++) {
    simPin[4] = LOW;  // pressed
    runFor(200);
    simPin[4] = HIGH;
    runFor(2000);
    expected.push_back(std::make_pair("button", i));
  }
#elif defined(EX_LIGHT) || defined(EX_LIGHT_MQTT)
  // The LED lights at the sketch's LED_ON level (LOW on the XIAO ESP32-S3),
  // and every report is what the sketch set, not the pin's level.
  auto lit = []() { return digitalRead(LED_BUILTIN) == LED_ON; };
  runFor(20000);
  check(!lit(), "the LED starts off");
  expected.push_back(std::make_pair("led", 0));  // the hello
  command("led:on");
  check(lit(), "led:on lights the LED (its pin at LED_ON)");
  expected.push_back(std::make_pair("led", 1));
  command("led:off");
  check(!lit(), "led:off puts it out");
  expected.push_back(std::make_pair("led", 0));
  command("led:blink");
  check(!lit(), "after led:blink an unlit LED is out again");
  expected.push_back(std::make_pair("led", 0));
  command("led:on");
  expected.push_back(std::make_pair("led", 1));
  command("led:blink");
  check(lit(), "after led:blink a lit LED is lit again");
  expected.push_back(std::make_pair("led", 1));
#elif defined(EX_TWO)
  runFor(20000);
  expected.push_back(std::make_pair("hello", 0));
  simPin[4] = LOW;
  runFor(200);
  simPin[4] = HIGH;
  runFor(2000);
  expected.push_back(std::make_pair("button", 1));
  check(digitalRead(LED_BUILTIN) != LED_ON, "the LED starts off");
  command("led:blink");  // blinks; sends nothing
  check(digitalRead(LED_BUILTIN) != LED_ON, "after led:blink the LED is out again");
#endif
  auto got = sent();
  check(got.size() == expected.size(), "the example sent " + std::to_string(got.size()) + " messages, expected " +
                                           std::to_string(expected.size()));
  FILE* f = fopen(file, "w");
  if (f == nullptr) return 2;
  for (size_t i = 0; i < got.size() && i < expected.size(); i++) {
    check(got[i].first == expected[i].first, "message " + std::to_string(i + 1) + " is tagged \"" + expected[i].first +
                                                 "\" (it is \"" + got[i].first + "\")");
    fprintf(f, "%s\t%s\t%.6f\n", got[i].first.c_str(), hex(got[i].second).c_str(), expected[i].second);
  }
  fclose(f);
  return failures ? 1 : 0;
}

int flood(int status) {
  sim.exchangeMs = 600;
  sim.server = [&](const SimRequest&) { return answer(status); };
  if (kMqtt) {
    if (status == 500) {
      broker.unreachable = true;
    } else {
      broker.refuse = true;
    }
  }
  simPin[4] = HIGH;
  pushSettings();
  setup();
  runFor(600000);
  int uplinks = 0, polls = 0;
  for (auto& r : sim.requests) {
    uplinks += isUplink(r);
    polls += isPoll(r);
  }
  int lines = occurrences("warblet: not stored");
  printf("      %d answers for ten minutes: %d messages sent, %d command checks, %d MQTT sessions, "
         "%d \"not stored\" lines\n",
         status, uplinks + (int)broker.published.size(), polls, broker.starts, lines);
  check(uplinks + (int)broker.published.size() <= 12, "a few messages in ten minutes, not one per pass of loop()");
  check(lines <= 2, "the refusal is said once or twice, not every time");
  check(polls <= 12 && broker.starts <= 12, "command checks and sessions back off too");
  return failures ? 1 : 0;
}

int corrected(int minutes) {
  sim.exchangeMs = 600;
  sim.server = [&](const SimRequest& r) -> std::string {
    if (r.header("X-Chirp-Token") != "TOKEN-2") return answer(401);
    return isUplink(r) ? answer(202) : "HTTP/1.1 204 No Content\r\n\r\n";
  };
  if (kMqtt) broker.refuse = true;
  simPin[4] = HIGH;
  pushSettings();
  setup();
  runFor(minutes * 60000UL);
  if (!kMqtt) {
    // A push with another token Warblet also refuses: new settings, so the
    // refusal is said again and the wait starts again at a minute.
    size_t said = Serial.out.size();
    Serial.type("CHIRP+ {\"token\":\"TOKEN-WRONG\"}\r\n");
    runFor(5000);
    std::string after = Serial.out.substr(said);
    check(after.find("not stored (401)") != std::string::npos && after.find("messages wait 60 s") != std::string::npos,
          "a push still refused is said again, and messages wait a minute, not where the old back-off was");
  }
  size_t requestsBefore = sim.requests.size(), publishedBefore = broker.published.size();
  unsigned long pushAt = millis();
  Serial.type("CHIRP+ {\"token\":\"TOKEN-2\"}\r\n");
  if (kMqtt) broker.refuse = false;  // the broker takes the new token
  runFor(660000);  // long enough to see the old ten-minute hold run out
  // The first message sent with the new token, when it went and its tag.
  long storedAfter = -1;
  std::string tag;
  if (kMqtt) {
    if (broker.published.size() > publishedBefore) {
      storedAfter = (long)(broker.publishedAt[publishedBefore] - pushAt);
      const std::string& topic = broker.published[publishedBefore].first;
      size_t up = topic.find("/up");
      tag = topic.size() > up + 4 ? topic.substr(up + 4) : "";
    }
  } else {
    for (size_t i = requestsBefore; i < sim.requests.size(); i++) {
      const SimRequest& r = sim.requests[i];
      if (isUplink(r) && r.header("X-Chirp-Token") == "TOKEN-2") {
        storedAfter = (long)(r.at - pushAt);
        tag = r.header("X-Chirp-Tag");
        break;
      }
    }
  }
#if defined(EX_TWO)
  const char* hello = "hello";
#else
  const char* hello = "led";
#endif
  printf("      after %d minutes of 401s, a corrected token: the hello stored %ld ms after the push\n", minutes,
         storedAfter);
  check(storedAfter >= 0 && tag == hello, "the first message stored after the push is the hello");
  check(storedAfter >= 0 && storedAfter < 10000,
        "a corrected push ends the hold: the hello is stored at once, not when the back-off would have ended");
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 3 && strcmp(argv[1], "record") == 0) return record(argv[2]);
  if (argc == 3 && strcmp(argv[1], "flood") == 0) return flood(atoi(argv[2]));
  if (argc == 3 && strcmp(argv[1], "corrected") == 0) return corrected(atoi(argv[2]));
  fprintf(stderr, "usage: example_run record FILE | flood STATUS | corrected MINUTES\n");
  return 2;
}
