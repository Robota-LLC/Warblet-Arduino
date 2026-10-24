// What differs from board to board: Wi-Fi, the TLS client and its roots,
// the clock, and where settings are kept. Everything else is in Warblet.cpp
// and the portable core (src/warblet/core), which is host-tested.
//
// ESP32 boards on the Arduino core for ESP32 3.x; the UNO R4 WiFi and Pico W
// sections are not ready and stop the build.
#pragma once

#include <Arduino.h>
#include <Client.h>
#include <stddef.h>
#include <stdint.h>

namespace warblet {
namespace board {

// Start Serial with a receive buffer big enough for a whole setup push
// (2 KB where the core allows it), so a push that arrives while the sketch
// is busy waits in the buffer instead of being dropped.
void serialBegin(unsigned long baud);

// Start joining one network. Returns at once; poll wifiConnected().
void wifiBegin(const char* ssid, const char* pass);
bool wifiConnected();
void wifiStop();

// Ask the network for the time. Returns true once the clock is set; it
// waits at most timeoutMs.
bool syncClock(unsigned long timeoutMs);
// Unix milliseconds, or 0 while the clock has never been set.
uint64_t clockMs();

// Free heap in bytes, for the opt-in verbose console line (0 where unknown).
uint32_t freeHeap();

// Restart the board, as after a reset. It does not return.
void restart();

// The one TLS client, with the pinned roots loaded.
Client& tlsClient();

// Settings storage: one text record of at most cap bytes.
bool loadSettings(char* out, size_t cap, size_t& len);
bool saveSettings(const char* text, size_t len);

// ---- MQTT over TLS: Espressif's own client, inside the ESP32 core -----------

enum MqttNews {
  kMqttNone,         // nothing changed since the last call
  kMqttConnected,    // the session is up and subscribed
  kMqttLost,         // the session dropped; the client reconnects by itself
  kMqttRefused,      // the broker refused the CONNECT (credential)
  kMqttNoDevice,     // the broker refused the CONNECT: no device has this id (code 4)
  kMqttUnreachable,  // no session: name lookup, TCP or TLS failed
  kMqttNote,         // something to say as it is, in the detail (a dropped command)
};

// Start one session to host:port over TLS (the pinned roots), as clientId
// with user and pass (pass nullptr: no password, at Open), subscribing to
// `subscribe` at QoS 1 each time it connects. It connects and reconnects in
// the background. Returns false where MQTT is not available.
bool mqttStart(const char* host, uint16_t port, const char* clientId, const char* user, const char* pass,
               const char* subscribe);
void mqttStop();
bool mqttConnected();
// Publish at QoS 1. Returns the message id (> 0), or -1 when not handed over.
int mqttPublish(const char* topic, const uint8_t* data, size_t len);
// True once the broker acknowledged that message id.
bool mqttAcked(int msgId);
// Take one received command into out, oldest first: its length, 0 when none
// waits, or -1 for one longer than cap (dropped). Commands that arrived stay
// here until taken, across a dropped or restarted session.
int mqttTake(uint8_t* out, size_t cap);
// The latest change of the session since the last call, with a short detail.
MqttNews mqttNews(char* detail, size_t cap);

// The refused-setup mark: a short fingerprint kept after Warblet answered
// "unclaimed" (never the code itself), so a restart stays stopped. An empty
// text clears it.
bool loadRefusedMark(char* out, size_t cap);
bool saveRefusedMark(const char* text);

}  // namespace board
}  // namespace warblet
