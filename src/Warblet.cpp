// See Warblet.h.
//
// The decisions about bytes on the wire are all made by the portable core
// (src/warblet/core); this file moves those bytes through the board's Wi-Fi
// and TLS client. Its logic is tested on a PC against stand-in headers
// (test/sim).
#include "Warblet.h"

#include <stdlib.h>
#include <string.h>

#include "WarbletBoard.h"
#include "warblet/core/sha256.h"
#include "warblet/core/sign.h"
#include "warblet/core/text.h"

using namespace warblet;

namespace {

const char* const kDefaultHost = "https.warbletiot.com";  // contract §Hosts
const uint16_t kPort = 443;
const unsigned long kCommandCheckMs = 10000UL;  // how often to ask for commands
const unsigned long kCommandCheckMinMs = 5000UL;  // the fastest a sketch may ask for
// restartWhenStuck(): shorter would restart at the first unanswered request,
// over and over through an outage that ends by itself.
const unsigned long kStuckMinMs = 300000UL;
const unsigned long kCommandCheckMaxMs = 600000UL;  // the longest back-off, after refusals
const unsigned long kUnansweredMaxMs = 60000UL;     // ...and when nothing answers (an outage)
const unsigned long kSetupQuietMs = 10000UL;    // after a CHIRP? answer: USB only
const unsigned long kJoinWaitMs = 12000UL;      // per network, as the demos do
const unsigned long kJoinRetryMs = 15000UL;     // between rounds of the list
const unsigned long kClockWaitMs = 10000UL;
const unsigned long kClockResyncMs = 6UL * 3600UL * 1000UL;  // a crystal drifts
const unsigned long kClockAfterRefusalMs = 15UL * 60UL * 1000UL;
const unsigned long kAnswerWaitMs = 20000UL;
const size_t kStoreMax = 2048;
const size_t kCommandMax = 8192;  // the queue's own cap on a command (contract v0.40)
const int kTakeAtOnce = 10;       // commands taken one after another before the normal pace

// Static, not on the stack: small boards have small stacks.
char storeText[kStoreMax];
uint8_t commandBody[kCommandMax + 1];

// Copy a typed-in value into a settings field; false when it does not fit or
// holds a control character.
bool copyField(char* field, size_t cap, const char* value) {
  if (value == nullptr) value = "";
  size_t n = strlen(value);
  if (n + 1 > cap || hasControlChar(value)) return false;
  memcpy(field, value, n + 1);
  return true;
}

}  // namespace

Warblet::Warblet()
    : settings_(), scratch_(), setup_(scratch_), response_(nullptr, 0),
      commandChecks_(kCommandCheckMs, kCommandCheckMaxMs, kUnansweredMaxMs),
      reports_(kCommandCheckMs, kCommandCheckMaxMs, kUnansweredMaxMs),
      mqttRefusals_(kCommandCheckMs, kCommandCheckMaxMs, kUnansweredMaxMs) {}

void Warblet::say(const char* text) {
  Serial.print("warblet: ");
  Serial.println(text);
}

const char* Warblet::host() const { return settings_.host[0] ? settings_.host : kDefaultHost; }

// For a while after answering the setup page's probe, the board does nothing
// but listen on USB, so the push that follows is read as it arrives.
bool Warblet::setupQuiet() const { return quiet_ && millis() - quietSince_ < kSetupQuietMs; }

void Warblet::demo(const char* slug) {
  badSlug_ = !validSlug(slug);
  setup_.setSlug(slug);
}

void Warblet::begin() {
  board::serialBegin(115200);
  if (badSlug_) say("demo(): that is not a demo slug (3 to 40 of a-z, 0-9 and -), so the announce leaves it out");
  started_ = true;
  typed_ = false;
  size_t len = 0;
  scratch_.clear();
  if (board::loadSettings(storeText, sizeof storeText, len) &&
      applyPush(storeText, len, scratch_, settings_) == ProvResult::Ok) {
    say("settings loaded from this board");
  } else {
    settings_.clear();
  }
  hasKey_ = settings_.key[0] && decodeKey(settings_.key, strlen(settings_.key), key_);
  state_ = settings_.ready() ? JOINING_WIFI : NOT_SET_UP;
  lastAnswerMs_ = millis();  // the ten minutes count from the start
  checkRefused();
  if (stateHandler_ != nullptr) stateHandler_(state_);
}

void Warblet::onState(StateHandler handler) { stateHandler_ = handler; }

void Warblet::setState(State state, int answer) {
  if (state == state_ && answer == answer_) return;
  state_ = state;
  answer_ = answer;
  if (stateHandler_ != nullptr) stateHandler_(state_);
}

void Warblet::answered(int status) {
  if (stopped_) return;  // NOT_ON_ACCOUNT stays, and asks nothing
  if (status >= 0) {
    lastAnswerMs_ = millis();  // any answer, a refusal or a fault too: the network works
    // A 5xx is Warblet's own fault: for the board, as good as no answer. The
    // code stays readable in answer().
    setState(status >= 200 && status < 300 ? ANSWERED : status >= 500 ? NO_ANSWER : REFUSED, status);
    return;
  }
  setState(NO_ANSWER, answer_);
  // No answer at all. With Wi-Fi saying it is connected and nothing heard
  // for stuckMs_, the radio or the network stack may be stuck: restart.
  if (stuckMs_ != 0 && settings_.ready() && board::wifiConnected() && millis() - lastAnswerMs_ >= stuckMs_) {
    Serial.print("warblet: nothing has answered for ");
    Serial.print((unsigned)(stuckMs_ / 60000UL));
    Serial.println(" minutes -- restarting");
    board::restart();
    lastAnswerMs_ = millis();  // only if the board could not restart
  }
}

void Warblet::restartWhenStuck(unsigned long ms) {
  if (ms != 0 && ms < kStuckMinMs) {
    say("restartWhenStuck(): 5 min at least, so 5 min");
    ms = kStuckMinMs;
  }
  stuckMs_ = ms;
}

// A setup Warblet answered "unclaimed" stays refused across a restart: the
// board keeps its fingerprint (never the code) and compares it at start.
void Warblet::checkRefused() {
  char mark[33], fp[17];
  if (!settings_.ready() || !board::loadRefusedMark(mark, sizeof mark)) return;
  setupFingerprint(settings_, fp);
  if (strcmp(mark, fp) != 0) return;
  stopped_ = true;
  stoppedSaid_ = true;
  state_ = NOT_ON_ACCOUNT;
  sayStopped();
}

void Warblet::sayStopped() {
  say("not on an account: Warblet does not know this board (its device was deleted, or its setup code was never "
      "used). It has stopped: no reports, no command checks, off its Wi-Fi network.");
  say(typed_ ? "to start it again, type new settings (a token from the device page) into begin() and upload the sketch"
             : "to start it again, set it up over USB at warbletiot.com/flash");
}

void Warblet::stopUnclaimed() {
  char fp[17];
  setupFingerprint(settings_, fp);
  board::saveRefusedMark(fp);
  stopped_ = true;
  if (mqtt_ != nullptr) (this->*mqtt_->restart)();
  board::wifiStop();
  setState(NOT_ON_ACCOUNT, answer_);
  if (!stoppedSaid_) sayStopped();
  stoppedSaid_ = true;
}

void Warblet::begin(const char* wifiName, const char* wifiPassword, const char* deviceId,
                    const char* deviceToken, const char* signingKey) {
  board::serialBegin(115200);
  started_ = true;
  typed_ = true;
  settings_.clear();
  bool ok = copyField(settings_.ssid, sizeof settings_.ssid, wifiName) &&
            copyField(settings_.pass, sizeof settings_.pass, wifiPassword) &&
            copyField(settings_.hwId, sizeof settings_.hwId, deviceId) &&
            copyField(settings_.credential, sizeof settings_.credential, deviceToken) &&
            copyField(settings_.key, sizeof settings_.key, signingKey);
  if (!ok) {
    say("a value typed into begin() is too long or holds a control character");
    settings_.clear();
    return;
  }
  if (!validHwId(settings_.hwId)) {
    say("the device id may use only letters, digits and : - _ .");
    settings_.clear();
    return;
  }
  hasKey_ = false;
  if (settings_.key[0]) {
    hasKey_ = decodeKey(settings_.key, strlen(settings_.key), key_);
    if (!hasKey_) {
      say("the signing key should be the 44-character key from the device page");
      settings_.clear();
    }
  }
  state_ = settings_.ready() ? JOINING_WIFI : NOT_SET_UP;
  lastAnswerMs_ = millis();
  checkRefused();
  if (stateHandler_ != nullptr) stateHandler_(state_);
}

void Warblet::onCommand(CommandHandler handler) { handler_ = handler; }

void Warblet::checkCommandsEvery(unsigned long ms) {
  if (ms < kCommandCheckMinMs) {
    say("checkCommandsEvery(): 5 s at least, so every 5 s");
    ms = kCommandCheckMinMs;
  }
  commandChecks_.pace(ms);
}

bool Warblet::connected() {
  return !stopped_ && settings_.ready() && board::wifiConnected() &&
         (mqtt_ == nullptr || mqtt_->connected());
}

void Warblet::loop() {
  if (!started_) return;
  if (inLoop_) {
    // Re-entered from the sketch's command function (board.wait() there):
    // keep answering the setup page, start nothing else.
    serviceSetup();
    return;
  }
  inLoop_ = true;
  serviceSetup();
  if (stopped_) {  // not on an account: only USB setup can bring it back
    inLoop_ = false;
    return;
  }
  if (!setupQuiet() && ensureOnline()) {
    if (mqtt_ != nullptr) {
      (this->*mqtt_->service)();  // commands arrive on the session: nothing to ask for
    } else if (handler_ != nullptr && (long)(millis() - nextPoll_) >= 0) {
      // Only a sketch with a command function asks for commands: one without
      // is spared a TLS connection, and a pause in its loop, every 10 s.
      // After a command there may be more waiting: ask again at once, up to
      // kTakeAtOnce in a row, then go back to the normal pace.
      int status = poll();
      taken_ = status == 200 ? taken_ + 1 : 0;
      if (taken_ > 0 && taken_ < kTakeAtOnce) {
        nextPoll_ = millis();
      } else {
        taken_ = 0;
        nextPoll_ = millis() + commandChecks_.wait();
      }
    }
  }
  inLoop_ = false;
}

void Warblet::wait(unsigned long ms) {
  unsigned long start = millis();
  for (;;) {
    loop();
    unsigned long spent = millis() - start;
    if (spent >= ms) return;
    unsigned long left = ms - spent;
    delay(left < 10 ? left : 10);
  }
}

// ---- USB setup ----------------------------------------------------------------

// Reads what has arrived on Serial. Returns true when new settings were
// stored and are now in use.
bool Warblet::serviceSetup() {
  if (typed_) return false;  // the settings live in the sketch
  bool changed = false;
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    ProvResponder::Event e = setup_.feed((char)c, settings_);
    if (e == ProvResponder::kReply) {
      Serial.println(setup_.reply());
      if (strncmp(setup_.reply(), "CHIRP!", 6) == 0) {
        quiet_ = true;
        quietSince_ = millis();
      }
    } else if (e == ProvResponder::kStore) {
      size_t len = settingsToJson(setup_.pending(), storeText, sizeof storeText);
      bool ok = len > 0 && board::saveSettings(storeText, len);
      Serial.println(setup_.stored(ok));
      quiet_ = false;
      if (ok) {
        // A new id or token is a new start (below); anything else is used now:
        // drop Wi-Fi and join with it.
        bool newSetup = strcmp(settings_.hwId, setup_.pending().hwId) != 0 ||
                        strcmp(settings_.credential, setup_.pending().credential) != 0;
        settings_ = setup_.pending();
        hasKey_ = settings_.key[0] && decodeKey(settings_.key, strlen(settings_.key), key_);
        if (mqtt_ != nullptr) (this->*mqtt_->restart)();  // a session uses the credential it started with
        board::wifiStop();
        lastJoin_ = 0;
        joinFailSaid_ = false;  // new networks: say the first round again
        waitingSaid_ = false;
        headSaid_ = false;
        nextPoll_ = millis();
        holdMs_ = 0;  // reports refused with the old settings wait no longer
        reports_.reset();
        changed = true;
        if (stopped_) {
          char mark[33], fp[17];
          setupFingerprint(settings_, fp);
          if (board::loadRefusedMark(mark, sizeof mark) && strcmp(mark, fp) == 0) {
            say("settings saved; still the setup Warblet refused, so still stopped (push a new setup code)");
            continue;
          }
          stopped_ = false;
          stoppedSaid_ = false;
          board::saveRefusedMark("");
        }
        if (newSetup) {
          // As the MicroPython demos do: the sketch starts afresh under the new
          // setup, so it says hello again and shows nothing the old one had.
          say("settings saved; a new setup, so the board restarts");
          board::restart();
        }
        say("settings saved; connecting with them");
      }
    }
  }
  return changed;
}

// ---- staying online -------------------------------------------------------------

bool Warblet::ensureOnline() {
  if (!settings_.ready()) {
    setState(NOT_SET_UP, answer_);
    if (!waitingSaid_) {
      say(typed_ ? "the settings typed into begin() are incomplete"
                 : "waiting for settings from the Warblet setup page over USB");
      waitingSaid_ = true;
    }
    return false;
  }
  if (!board::wifiConnected()) {
    if (lastJoin_ != 0 && millis() - lastJoin_ < kJoinRetryMs) return false;
    joinWifi();
    if (lastJoin_ == 0) return false;  // new settings arrived during the join: start over
    if (!board::wifiConnected()) return false;
  }
  // Certificates are checked against the date, and nonces are the time, so
  // nothing is sent before the clock is set.
  if (!clockSet_ || millis() - lastClock_ >= kClockResyncMs) {
    lastClock_ = millis();
    if (board::syncClock(kClockWaitMs)) {
      clockSet_ = true;
    } else if (!clockSet_) {
      say("no clock yet: this network must allow NTP, because secure connections need the date");
      answered(-1);  // nothing can be sent: no answer, and the restartWhenStuck() rule
      return false;
    }
  }
  return true;
}

void Warblet::joinWifi() {
  const char* ssid = nullptr;
  const char* pass = nullptr;
  for (size_t i = 0; settings_.network(i, &ssid, &pass); i++) {
    setState(JOINING_WIFI, answer_);
    if (!joinFailSaid_) {  // the first round is said; the ones after a failure are not
      Serial.print("warblet: joining Wi-Fi network ");
      Serial.println((unsigned)(i + 1));
    }
    board::wifiBegin(ssid, pass);
    unsigned long start = millis();
    while (!board::wifiConnected() && millis() - start < kJoinWaitMs) {
      // The setup page can still reach a board that cannot join, and a
      // corrected push takes effect at once.
      if (serviceSetup()) return;  // lastJoin_ is 0: the next loop() joins with it
      delay(50);
    }
    if (board::wifiConnected()) {
      lastJoin_ = millis();
      joinFailSaid_ = false;
      say("Wi-Fi joined");
      setState(REACHING_WARBLET, answer_);
      return;
    }
    board::wifiStop();
  }
  lastJoin_ = millis();
  setState(NO_WIFI, answer_);
  if (!joinFailSaid_) say("no stored Wi-Fi network joined; trying them again every 15 s until one does");
  joinFailSaid_ = true;
}

// ---- sending --------------------------------------------------------------------

bool Warblet::sendNumberText(const char* text, size_t length, const char* tag) {
  if (length == 0) {
    say("sendNumber: not a number (NaN or infinity); not sent");
    return false;
  }
  return send(reinterpret_cast<const uint8_t*>(text), length, tag);
}

bool Warblet::sendNumber(int value, const char* tag) { return sendNumber((long long)value, tag); }
bool Warblet::sendNumber(long value, const char* tag) { return sendNumber((long long)value, tag); }
bool Warblet::sendNumber(unsigned int value, const char* tag) { return sendNumber((unsigned long long)value, tag); }
bool Warblet::sendNumber(unsigned long value, const char* tag) { return sendNumber((unsigned long long)value, tag); }

bool Warblet::sendNumber(long long value, const char* tag) {
  char text[kNumberTextMax];
  return sendNumberText(text, formatInteger(value, text), tag);
}

bool Warblet::sendNumber(unsigned long long value, const char* tag) {
  char text[kNumberTextMax];
  return sendNumberText(text, formatUnsigned(value, text), tag);
}

bool Warblet::sendNumber(float value, const char* tag) {
  char text[kNumberTextMax];
  return sendNumberText(text, formatReal(value, 7, text), tag);
}

bool Warblet::sendNumber(double value, const char* tag) {
  char text[kNumberTextMax];
  return sendNumberText(text, formatReal(value, 15, text), tag);
}

bool Warblet::sendTenths(double value, const char* tag) {
  uint8_t two[2];
  if (!encodeTenths(value, two)) {
    say("sendTenths: the value must be between -3276.8 and 3276.7 (sendNumber takes any size); not sent");
    return false;
  }
  return send(two, sizeof two, tag);
}

bool Warblet::sendText(const char* text, const char* tag) {
  return send(reinterpret_cast<const uint8_t*>(text), strlen(text), tag);
}

bool Warblet::send(const uint8_t* bytes, size_t length, const char* tag) {
  if (tag != nullptr && tag[0] != '\0' && !validTag(tag)) {
    say("a tag is 1 to 32 letters, digits, _ . or -; not sent");
    return false;
  }
  if (!started_) {
    say("call board.begin() first; not sent");
    return false;
  }
  serviceSetup();
  if (stopped_) return false;  // said once, when it stopped
  if (setupQuiet()) {
    say("the setup page is talking to the board; not sent");
    return false;
  }
  if (holdMs_ != 0 && millis() - holdSince_ < holdMs_) {
    return false;  // held after a refused or unanswered report; said when the hold began
  }
  if (!ensureOnline()) {
    say("not connected yet; not sent");
    return false;
  }
  return mqtt_ != nullptr ? (this->*mqtt_->publish)(bytes, length, tag) : post(bytes, length, tag);
}

bool Warblet::post(const uint8_t* bytes, size_t length, const char* tag) {
  uint64_t nonce = 0;
  char signature[65];
  if (hasKey_) {
    nonce = nonces_.next(board::clockMs());  // 0 without a clock: body-only signature
    signHeader(key_, sizeof key_, nonce, tag, bytes, length, signature);
  }
  HttpIngest request = {host(), settings_.hwId, settings_.credential, tag, nonce,
                        hasKey_ ? signature : nullptr, length};
  size_t headLen = httpIngestHead(request, head_, sizeof head_);
  if (headLen == 0) {
    say("the request does not fit; not sent");
    return false;
  }
  int status = exchange(headLen, bytes, length, answerBody_, sizeof answerBody_);
  if (status == 202 && answerIsUnclaimed(answerBody_, response_.bodyLen())) {
    // Warblet only noted a board it does not know (contract §HTTP): the device
    // was deleted, or its code was never used. Nothing it sends would reach an
    // account, so the board stops taking part until a new USB setup.
    stopUnclaimed();
    return false;
  }
  answered(status);
  if (status == 202) {
    holdMs_ = 0;
    reports_.after(202, 0);
    if (reports_.changed()) say("messages are stored again");
    // A stored message is what puts a board on its account, so command checks
    // refused before it may be answered now: ask at once, not after the back-off.
    if (commandChecks_.failing()) nextPoll_ = millis();
    return true;
  }

  // A refusal or no answer will not change on the next pass of loop(): reports
  // wait, longer while it lasts (as command checks do), so a sketch that says
  // hello until it is stored does not flood Warblet. Said once, not each time.
  bool hold = status < 0 || status == 401 || status == 403 || status == 404 || status == 429 || status >= 500;
  if (hold) {
    reports_.after(status, response_.retryAfter());
    holdSince_ = millis();
    holdMs_ = reports_.wait();
    if (!reports_.changed()) return false;
  }
  if (status == 401) {
    Serial.print("warblet: not stored (401): the device id, token or signing key was refused, or the board's first"
                 " message is not stored yet");
    // A refused signed message may be a clock outside the window: re-sync soon.
    if (hasKey_ && millis() - lastRefused_ >= kClockAfterRefusalMs) {
      lastRefused_ = millis();
      lastClock_ = millis() - kClockResyncMs;
    }
  } else if (status == 429) {
    Serial.print(response_.hasBudget() ? "warblet: not stored (429): today's storage budget for this board is used up"
                                       : "warblet: not stored (429): too many messages in a short time");
  } else if (status < 0) {
    Serial.print("warblet: not stored: no complete answer from Warblet (check the network, and that NTP is allowed)");
  } else {
    Serial.print("warblet: not stored, answer ");
    Serial.print(status);
  }
  if (hold) {
    Serial.print("; messages wait ");
    Serial.print((unsigned)(holdMs_ / 1000UL));
    Serial.println(" s, then longer while it lasts");
  } else {
    Serial.println("");
  }
  return false;
}

// ---- commands -------------------------------------------------------------------

int Warblet::poll() {
  uint64_t nonce = 0;
  char signature[65];
  if (hasKey_) {
    // At Signed, a poll must prove the key (contract §Auth → Downlink poll).
    nonce = nonces_.next(board::clockMs());
    if (nonce == 0) {
      if (!noClockSaid_) say("no clock, so a signed board cannot ask for commands yet");
      noClockSaid_ = true;
      return -1;
    }
    signPoll(key_, sizeof key_, settings_.hwId, nonce, signature);
  }
  HttpPoll request = {host(), settings_.hwId, settings_.credential, nonce, hasKey_ ? signature : nullptr};
  size_t headLen = httpPollHead(request, head_, sizeof head_);
  if (headLen == 0) {
    // Cannot happen with settings the setup accepts (host-tested); said once if it does.
    if (!headSaid_) say("the command check does not fit its buffer; commands cannot be collected");
    headSaid_ = true;
    return -1;
  }
  int status = exchange(headLen, nullptr, 0, commandBody, kCommandMax);
  if (status == 202 && answerIsUnclaimed(commandBody, response_.bodyLen())) {
    // The ingest's answer for a board Warblet does not know, on a command
    // check: the same stop as a report that hears it.
    stopUnclaimed();
    return status;
  }
  answered(status);
  commandChecks_.after(status, response_.retryAfter());
  if (commandChecks_.changed()) {
    if (!commandChecks_.failing()) {
      say("command checks are answered again");
    } else if (status == 401) {
      // The board cannot tell these apart, so the line names both.
      say("command check refused (401): the device token or signing key was refused, or the board's first "
          "message is not stored yet; asking less often until it works (the setup page can push new settings)");
    } else {
      Serial.print("warblet: command check failed (");
      Serial.print(status);
      Serial.println("); asking less often until it works");
    }
  }
  if (status != 200) return status;
  if (response_.tooLong()) {
    say("a command longer than 8192 bytes arrived and was dropped");
    return status;
  }
  deliverCommand(response_.bodyLen());
  return status;
}

// One command from either transport, in commandBody: a signing key from the
// platform is stored; anything else is said and handed to the sketch.
void Warblet::deliverCommand(size_t len) {
  switch (classifyDownlink(commandBody, len)) {
    case Downlink::Empty:
      return;
    case Downlink::Key: {
      // The platform hands a board its signing key this way, only over TLS.
      char b64[45];
      if (!parseKeyFrame(commandBody, len, b64)) {
        say("a signing-key frame arrived malformed and was ignored");
        return;
      }
      scratch_ = settings_;
      memcpy(scratch_.key, b64, sizeof b64);
      size_t textLen = settingsToJson(scratch_, storeText, sizeof storeText);
      if (textLen == 0 || !board::saveSettings(storeText, textLen)) {
        say("a signing key arrived but could not be stored; it was not used");
        return;
      }
      settings_ = scratch_;
      hasKey_ = decodeKey(settings_.key, strlen(settings_.key), key_);
      say("a signing key arrived over the air and was stored; messages are now signed");
      return;
    }
    case Downlink::Command:
      break;
  }
  commandBody[len] = '\0';
  // Say on the console that one arrived and its size, never its words: they
  // are the sender's (a message for a screen, say). A build with
  // -DWARBLET_VERBOSE shows the first 60 characters, for debugging.
  Serial.print("warblet: command received (");
  Serial.print((unsigned)len);
#if defined(WARBLET_VERBOSE)
  char shown[64];
  size_t n = len < 60 ? len : 60;
  for (size_t i = 0; i < n; i++) {
    char c = (char)commandBody[i];
    shown[i] = ((unsigned char)c < 0x20 || c == 0x7f) ? '?' : c;
  }
  shown[n] = '\0';
  Serial.print(" bytes): ");
  Serial.print(shown);
  Serial.println(len > n ? "..." : "");
#else
  Serial.println(" bytes)");
#endif
  if (strlen(reinterpret_cast<const char*>(commandBody)) != len) {
    say("a command with a zero byte in it arrived; it was handed over cut at that byte");
  }
  if (handler_ == nullptr) {
    say("a command arrived, but no onCommand() function is set");
    return;
  }
  handler_(String(reinterpret_cast<const char*>(commandBody)));
}

// The command buffer, for the MQTT code in WarbletMqtt.cpp.
uint8_t* Warblet::commandBuffer(size_t* cap) {
  *cap = kCommandMax;
  return commandBody;
}

// ---- one HTTPS request ----------------------------------------------------------

// Sends head_ (headLen bytes) and the body; reads the answer into response_.
// Returns the status, or -1 when no complete answer came.
int Warblet::exchange(size_t headLen, const uint8_t* body, size_t bodyLen, uint8_t* answer, size_t answerCap) {
  response_.reset(answer, answerCap);
  Client& client = board::tlsClient();
#if defined(WARBLET_VERBOSE)
  // Build with -DWARBLET_VERBOSE for one console line per request: what it
  // was, the answer, and how long it took from connect to the last byte.
  unsigned long began = millis();
  struct Report {
    const char* head;
    const HttpResponse& r;
    unsigned long began;
    ~Report() {
      Serial.print(strstr(head, "/down HTTP/1.1") ? "warblet: [verbose] command check -> " : "warblet: [verbose] message -> ");
      Serial.print(r.complete() ? r.status() : -1);
      Serial.print(" in ");
      Serial.print((unsigned)(millis() - began));
      Serial.print(" ms, free heap ");
      Serial.println((unsigned)board::freeHeap());
    }
  } report = {head_, response_, began};
#endif
  if (!client.connect(host(), kPort)) return -1;  // the caller says so, once
  client.write(reinterpret_cast<const uint8_t*>(head_), headLen);
  if (bodyLen > 0) client.write(body, bodyLen);
  unsigned long start = millis();
  while (!response_.complete() && !response_.malformed() && millis() - start < kAnswerWaitMs) {
    int ready = client.available();
    if (ready > 0) {
      int n = client.read(io_, ready < (int)sizeof io_ ? (size_t)ready : sizeof io_);
      if (n > 0) response_.feed(io_, (size_t)n);
      continue;
    }
    if (!client.connected()) {
      response_.finishAtClose();
      break;
    }
    delay(5);
  }
  client.stop();
  return response_.complete() ? response_.status() : -1;
}
