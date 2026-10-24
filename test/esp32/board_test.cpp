// Compiles the ESP32 section of src/WarbletBoard.cpp, unmodified, against
// stand-ins of the ESP32 core and of Espressif's MQTT client (this folder),
// and drives it with the events the real client produces.
//
// What this proves: what the board layer asks of the core and the client
// (the serial buffer, the MQTT configuration, the log tags it touches) and
// what it makes of the client's events (sessions, refusals, commands in
// pieces, acknowledgements). What it does NOT prove: that the real core or
// client behave as the stand-ins do; the stand-ins follow ESP-IDF v5.5.5's
// source where it matters, as esp32_stubs.cpp says.

#include <stdio.h>
#include <string.h>

#include <string>

#include "WarbletBoard.h"
#include "WarbletRoots.h"
#include "WiFiClientSecure.h"
#include "esp_tls_errors.h"
#include "stub_control.h"

using namespace warblet::board;
using warblet::kRootCertificates;

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

const char* kDown = "chirp/esp-board-1/down";

// Start a session as the library does, and bring it up.
void startSession(const char* pass = "TOKEN-1") {
  check(mqttStart("mqtt.warbletiot.com", 8883, "esp-board-1", "esp-board-1", pass, kDown), "mqttStart");
  stub::connected();
}

// Every piece of news waiting, in order.
std::vector<std::pair<MqttNews, std::string>> news() {
  std::vector<std::pair<MqttNews, std::string>> out;
  char detail[128];
  for (MqttNews n; (n = mqttNews(detail, sizeof detail)) != kMqttNone;) out.push_back(std::make_pair(n, detail));
  return out;
}

// Every command waiting, in order; a dropped one (too long) is "<dropped>".
std::vector<std::string> takeAll() {
  std::vector<std::string> out;
  static uint8_t buf[8193];
  for (int n; (n = mqttTake(buf, 8192)) != 0;) out.push_back(n < 0 ? "<dropped>" : std::string((char*)buf, (size_t)n));
  return out;
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // ---- the USB serial line ---------------------------------------------------
  // A setup push with two networks or a key is over the core's 256-byte
  // default, and arrives while the sketch may be busy: the buffer must be
  // 2 KB, and it must be set before begin() (the core keeps it only then).
  serialBegin(115200);
  check(Serial.rxCapAtBegin == 2048, "serial: the receive buffer is 2 KB when begin() runs");

  // ---- HTTPS client limits ---------------------------------------------------
  Client& c = tlsClient();
  WiFiClientSecure& tls = static_cast<WiFiClientSecure&>(c);
  // The roots are a static array in a header: each file has its own copy, so compare the text.
  check(tls.caCert != nullptr && strcmp(tls.caCert, kRootCertificates) == 0, "TLS: the pinned roots are loaded");
  check(tls.connectionTimeoutMs == 10000 && tls.handshakeTimeoutS == 15, "TLS: a 10 s connect and 15 s handshake limit");

  // ---- the MQTT session's configuration --------------------------------------
  startSession();
  const esp_mqtt_client_config_t& cfg = stub::config.raw;
  check(stub::config.host == "mqtt.warbletiot.com" && cfg.broker.address.port == 8883 &&
            cfg.broker.address.transport == MQTT_TRANSPORT_OVER_SSL,
        "MQTT: mqtt.warbletiot.com:8883 over TLS");
  check(stub::config.certificate == kRootCertificates, "MQTT: the same pinned roots");
  check(stub::config.username == "esp-board-1" && stub::config.clientId == "esp-board-1" &&
            !stub::config.passwordNull && stub::config.password == "TOKEN-1",
        "MQTT: client id and username are the device id; the password is the credential");
  check(cfg.session.keepalive == 60 && cfg.network.timeout_ms == 10000 && cfg.network.reconnect_timeout_ms == 10000,
        "MQTT: keepalive 60 s, every network wait 10 s, reconnect after 10 s");
  check(!stub::subscribes.empty() && stub::subscribes.back() == kDown, "MQTT: subscribes to the command topic on connect");
  // The library quiets only the MQTT client's own log tag: esp-tls and
  // transport_base are shared, and quieting them would hide a sketch's own
  // TLS errors too.
  bool onlyOwn = !stub::logTags.empty();
  for (auto& t : stub::logTags) onlyOwn = onlyOwn && t == "mqtt_client";
  check(onlyOwn, "MQTT: only the MQTT client's own log tag is quieted");
  auto n = news();
  check(n.size() == 1 && n[0].first == kMqttConnected && mqttConnected(), "MQTT: the session is up, said once");
  mqttStop();
  check(mqttStart("mqtt.warbletiot.com", 8883, "esp-board-1", "esp-board-1", nullptr, kDown) &&
            stub::config.passwordNull,
        "MQTT: no password flag when there is no credential");
  mqttStop();
  check(stub::clientsAlive == 0, "MQTT: stopping destroys the client");

  // ---- what the client's events mean -----------------------------------------
  mqttStart("mqtt.warbletiot.com", 8883, "esp-board-1", "esp-board-1", "TOKEN-1", kDown);
  stub::transportError(ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME);
  n = news();
  check(n.size() == 1 && n[0].first == kMqttUnreachable && n[0].second == "the host name did not resolve",
        "MQTT: a failed attempt is said in words, and its disconnect is not a lost session");
  stub::refused(MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED);
  n = news();
  check(n.size() == 1 && n[0].first == kMqttRefused && n[0].second == "CONNECT refused (code 5)",
        "MQTT: a refused CONNECT (code 5) is a refusal");
  // Contract v0.47: a credential for an id that is no device is refused
  // with code 4; a wrong token for a device that exists stays code 5.
  stub::refused(MQTT_CONNECTION_REFUSE_BAD_USERNAME);
  n = news();
  check(n.size() == 1 && n[0].first == kMqttNoDevice && n[0].second == "CONNECT refused (code 4)",
        "MQTT: a CONNECT refused with code 4 says no device has this id");
  stub::connected();
  stub::disconnected();
  n = news();
  check(n.size() == 2 && n[1].first == kMqttLost && !mqttConnected(), "MQTT: an open session that drops is lost");
  stub::connected();
  news();

  // ---- reports and their acknowledgement -------------------------------------
  int id = mqttPublish("chirp/esp-board-1/up", (const uint8_t*)"21.5", 4);
  check(id > 0 && !mqttAcked(id), "MQTT: a report is handed over, not yet acknowledged");
  stub::outbox.back().ackAt = millis() + 300;
  stub::run(1000);
  check(mqttAcked(id), "MQTT: the PUBACK is seen");

  // ESP-IDF's client sends an unacknowledged QoS 1 report again after
  // session.message_retransmit_timeout (1 s when left unset), and the broker
  // stores every copy (MQTT has no nonce). The library waits 10 s for the
  // acknowledgement, so the client must not resend inside that wait, nor
  // before the report leaves its outbox (30 s).
  check(cfg.session.message_retransmit_timeout > 10000,
        "MQTT: the client's retransmit timeout is longer than the library's 10 s acknowledgement wait");
  int slow = mqttPublish("chirp/esp-board-1/up", (const uint8_t*)"21.6", 4);
  stub::outbox.back().ackAt = millis() + 2500;  // a slow broker: the PUBACK after 2.5 s
  stub::run(12000);
  check(mqttAcked(slow) && stub::outbox.back().sends == 1, "MQTT: a report acknowledged after 2.5 s went out once");
  int never = mqttPublish("chirp/esp-board-1/up", (const uint8_t*)"21.7", 4);
  stub::run(40000);  // no PUBACK at all: the library gives up at 10 s, the client drops it at 30 s
  check(!mqttAcked(never) && stub::outbox.back().sends == 1, "MQTT: a report never acknowledged went out once");
  printf("      MQTT: copies on the wire - acknowledged after 2.5 s: %d, never acknowledged: %d\n",
         stub::outbox[stub::outbox.size() - 2].sends, stub::outbox.back().sends);

  // ---- commands --------------------------------------------------------------
  stub::message(kDown, "led:on");
  auto got = takeAll();
  check(got.size() == 1 && got[0] == "led:on", "MQTT: a command is taken whole");
  stub::message("chirp/someone-else/down", "led:on");
  check(takeAll().empty(), "MQTT: a message on another topic is not a command");
  std::string big(2000, 'x');
  stub::message(kDown, big);
  got = takeAll();
  check(got.size() == 1 && got[0] == big, "MQTT: a 2,000-byte command in two pieces arrives whole");
  stub::message(kDown, std::string(8193, 'y'));
  got = takeAll();
  check(got.size() == 1 && got[0] == "<dropped>", "MQTT: a command past 8 KiB is reported dropped, not cut");
  // Commands that wait for loop(). The client acknowledges each one before
  // the library sees it, so none may be dropped without a word, and a
  // command cut off on the way must not jam the queue.
  auto notes = [](const std::vector<std::pair<MqttNews, std::string>>& all, const char* text) {
    int k = 0;
    for (auto& x : all) k += x.second.find(text) != std::string::npos;
    return k;
  };
  news();
  stub::message(kDown, "led:on");
  stub::message(kDown, "led:off");
  stub::message(kDown, "led:blink");  // a third, before loop() took the first
  got = takeAll();
  check(got.size() == 3 && got[0] == "led:on" && got[1] == "led:off" && got[2] == "led:blink",
        "MQTT: three commands before loop() takes one: all three kept, in order");
  stub::dataPiece(kDown, std::string(1024, 'a'), 0, 3000);  // the first piece of 3,000 bytes, then nothing
  stub::dataPiece(kDown, std::string(1024, 'b'), 0, 3000);  // a second one cut off the same way
  stub::message(kDown, "led:on");
  got = takeAll();
  n = news();
  check(got.size() == 1 && got[0] == "led:on", "MQTT: after two commands cut off on the way, the next one arrives");
  check(notes(n, "cut off") == 2, "MQTT: ...and each cut-off command is said");
  stub::dataPiece(kDown, std::string(1024, 'c'), 0, 3000);
  stub::disconnected();  // the session drops mid-transfer
  stub::connected();
  stub::message(kDown, "led:off");
  got = takeAll();
  n = news();
  check(got.size() == 1 && got[0] == "led:off" && notes(n, "cut off") == 1,
        "MQTT: a command cut off by a dropped session is said, and the next one arrives");
  int sent = 0;
  for (int i = 0; i < 6; i++, sent++) stub::message(kDown, std::string(8000, (char)('0' + i)));  // 48 KB, none taken
  got = takeAll();
  n = news();
  int dropped = notes(n, "dropped");
  check(got.size() >= 4 && (int)got.size() + dropped == sent,
        "MQTT: when commands pile up, every one is kept or said dropped, none silently");
  check(got.size() == 4 && dropped == 2,
        "MQTT: commands waiting for loop() hold 32 KB at most: four of 8,000 bytes fit, the fifth and sixth do not");
  printf("      MQTT: 6 commands of 8,000 bytes while loop() was busy: %u kept, %d said dropped\n", (unsigned)got.size(),
         dropped);
  stub::message(kDown, std::string(8000, 'z'));
  got = takeAll();
  check(got.size() == 1 && got[0] == std::string(8000, 'z'), "MQTT: once loop() has taken them, there is room again");
  stub::message(kDown, "led:blink");
  mqttStop();  // new settings arrive: the session restarts
  mqttStart("mqtt.warbletiot.com", 8883, "esp-board-1", "esp-board-1", "TOKEN-2", kDown);
  got = takeAll();
  check(got.size() == 1 && got[0] == "led:blink", "MQTT: a command waiting when the session restarts is still handed over");
  check(simLocksHeld == 0, "MQTT: every lock is let go");
  mqttStop();

  printf("\nboard (ESP32 section on the PC): %d passed, %d failed\n%s\n", pass, fail, fail ? "FAILED" : "PASSED");
  return fail ? 1 : 0;
}
