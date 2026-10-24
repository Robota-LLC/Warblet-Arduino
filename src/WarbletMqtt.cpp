// The MQTT transport: Warblet board(Warblet::MQTT). See Warblet.h.
//
// A file of its own, reached only through the constructor below, so a sketch
// that uses HTTPS carries none of it, nor Espressif's MQTT client (the linker
// drops what nothing names). Its logic is tested on a PC against a fake
// broker (test/sim).
#include <stdlib.h>
#include <string.h>

#include "Warblet.h"
#include "WarbletBoard.h"
#include "warblet/core/sha256.h"
#include "warblet/core/sign.h"

using namespace warblet;

namespace {

const char* const kMqttHost = "mqtt.warbletiot.com";  // MQTT over TLS on 8883
const uint16_t kMqttPort = 8883;
const unsigned long kMqttWaitMs = 15000UL;  // for a session before a first report
const unsigned long kMqttAckMs = 10000UL;   // for the broker's acknowledgement

}  // namespace

const Warblet::MqttHooks Warblet::kMqttHooks = {&Warblet::serviceMqtt, &Warblet::publishMqtt, &Warblet::restartMqtt,
                                                &board::mqttConnected};

Warblet::Warblet(Transport transport) : Warblet() {
  if (transport == MQTT) mqtt_ = &kMqttHooks;
}

// The pushed host is an override for a self-hosted core, for either transport.
const char* Warblet::mqttHost() const { return settings_.host[0] ? settings_.host : kMqttHost; }

// New settings start a new session at once, even in the middle of a back-off.
void Warblet::restartMqtt() {
  if (mqttStarted_) board::mqttStop();
  mqttStarted_ = false;
  mqttRetryAt_ = 0;
  mqttLastNews_ = 0;
}

// Start the session if it is not running, say what changed, and hand over
// the commands that arrived (also while a refused session backs off: the
// broker counted them delivered).
void Warblet::serviceMqtt() {
  bool backingOff = !mqttStarted_ && mqttRetryAt_ != 0 && (long)(millis() - mqttRetryAt_) < 0;
  char down[160];
  if (!mqttStarted_ && !backingOff && topicDown(settings_.hwId, down, sizeof down) != 0) {
    const char* pass = settings_.credential[0] ? settings_.credential : nullptr;  // none at Open
    if (board::mqttStart(mqttHost(), kMqttPort, settings_.hwId, settings_.hwId, pass, down)) {
      mqttStarted_ = true;
    } else if (!mqttMissingSaid_) {
      say("MQTT is not available on this board; use Warblet board; (HTTPS) instead");
      mqttMissingSaid_ = true;
    }
  }
  char detail[96];
  for (board::MqttNews n; mqttStarted_ && (n = board::mqttNews(detail, sizeof detail)) != board::kMqttNone;) {
    if (n == board::kMqttNote) {  // said as it is; the session's state is unchanged
      Serial.print("warblet: MQTT: ");
      Serial.println(detail);
      continue;
    }
    // The client ends each failed attempt with a disconnect: only a session
    // that was up can be lost.
    if (n == board::kMqttLost && mqttLastNews_ != board::kMqttConnected) continue;
    bool repeat = n == mqttLastNews_ && (n == board::kMqttUnreachable || n == board::kMqttRefused);
    mqttLastNews_ = n;
    if (n == board::kMqttConnected) {
      mqttRefusals_.after(200, 0);
      answered(200);
      say("MQTT connected; commands arrive as soon as they are sent");
    } else if (n == board::kMqttLost) {
      answered(-1);
      say("MQTT session lost; the client reconnects by itself");
    } else if (n == board::kMqttUnreachable && repeat) {
      answered(-1);
    } else if (n == board::kMqttUnreachable) {
      answered(-1);
      Serial.print("warblet: MQTT has no session (");
      Serial.print(detail);
      Serial.println("); the client keeps trying by itself - check the network, NTP and the host");
    } else if (n == board::kMqttNoDevice) {
      // Not on an account (its device was deleted, or its setup code was
      // spent): stop, as an "unclaimed" answer does over HTTPS.
      stopUnclaimed();
    } else if (n == board::kMqttRefused) {
      answered(401);
      mqttRefusals_.after(401, 0);
      if (mqttRefusals_.changed() || !repeat) {
        Serial.print("warblet: MQTT refused (");
        Serial.print(detail);
        Serial.println("): the device token was refused, or the board's first message is not stored yet;"
                       " asking less often until it works (the setup page can push new settings)");
      }
      board::mqttStop();  // the client would retry every 10 s; back off instead
      mqttStarted_ = false;
      mqttRetryAt_ = millis() + mqttRefusals_.wait();
    }
  }
  size_t cap;
  uint8_t* body = commandBuffer(&cap);
  for (;;) {
    int len = board::mqttTake(body, cap);
    if (len == 0) break;
    if (len < 0) {
      say("a command longer than 8192 bytes arrived and was dropped");
      continue;
    }
    deliverCommand((size_t)len);
    if (stopped_) break;  // the command function changed things
  }
}

// Publish one report at QoS 1 and wait for the broker's acknowledgement. MQTT
// has no answer that says the message was stored: true means the broker took it.
bool Warblet::publishMqtt(const uint8_t* bytes, size_t length, const char* tag) {
  char topic[200];
  if (topicUp(settings_.hwId, tag, topic, sizeof topic) == 0) {
    say("the MQTT topic does not fit; not sent");
    return false;
  }
  if (!mqttStarted_) serviceMqtt();
  // Wait for the session only while it is coming up (nothing heard since it
  // started): once the client says it has none, a report returns at once.
  unsigned long start = millis();
  while (mqttLastNews_ == 0 && !board::mqttConnected() && millis() - start < kMqttWaitMs) {
    serviceSetup();
    if (stopped_ || !mqttStarted_) break;
    delay(20);
  }
  if (!board::mqttConnected()) {
    if (!mqttNoSessionSaid_) say("not sent: MQTT has no session; reports go again once it is back");
    mqttNoSessionSaid_ = true;
    return false;
  }
  mqttNoSessionSaid_ = false;
  const uint8_t* data = bytes;
  size_t n = length;
  uint8_t* envelope = nullptr;
  if (hasKey_) {
    // Signed: 32 raw HMAC bytes over "\ntag=<tag>\n" + payload (contract v0.31;
    // the envelope carries no nonce), then the payload.
    envelope = static_cast<uint8_t*>(malloc(kSha256Digest + length));
    if (envelope == nullptr) {
      say("not sent: no memory for the signed envelope");
      return false;
    }
    signEnvelope(key_, sizeof key_, tag, bytes, length, envelope);
    memcpy(envelope + kSha256Digest, bytes, length);
    data = envelope;
    n = kSha256Digest + length;
  }
  unsigned long began = millis();
  int id = board::mqttPublish(topic, data, n);
  free(envelope);
  if (id < 0) {
    say("not sent: MQTT has no session");
    return false;
  }
  while (!board::mqttAcked(id) && millis() - began < kMqttAckMs) {
    serviceSetup();
    delay(5);
  }
  bool acked = board::mqttAcked(id);
#if defined(WARBLET_VERBOSE)
  Serial.print(acked ? "warblet: [verbose] MQTT publish -> acknowledged in " : "warblet: [verbose] MQTT publish -> no acknowledgement in ");
  Serial.print((unsigned)(millis() - began));
  Serial.print(" ms, free heap ");
  Serial.println((unsigned)board::freeHeap());
#endif
  if (!acked) say("published, but the broker did not acknowledge it within 10 s");
  return acked;
}
