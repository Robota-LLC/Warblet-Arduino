// What board_test.cpp sees of, and does to, the stand-in esp-mqtt client.
#pragma once

#include <string>
#include <vector>

#include "esp_log.h"
#include "mqtt_client.h"

namespace stub {

// The configuration the library last handed to esp_mqtt_client_init, with
// its strings copied.
struct Config {
  bool set = false;
  esp_mqtt_client_config_t raw;
  std::string host, username, clientId, password, certificate;
  bool passwordNull = true;
};
extern Config config;

// One QoS 1 report in the client's outbox, as esp-mqtt keeps it.
struct Outgoing {
  std::string topic, data;
  int msgId = 0;
  unsigned long tick = 0;  // when it was first sent
  int sends = 0;           // how many times it went on the wire
  bool acked = false;
  bool expired = false;
  unsigned long ackAt = 0;  // when the broker's PUBACK arrives (0: never)
};
extern std::vector<Outgoing> outbox;
extern std::vector<std::string> subscribes;
extern std::vector<std::string> logTags;  // tags given to esp_log_level_set
extern int clientsAlive;                  // initialised and not yet destroyed

// Events the real client produces, delivered to the library's handler.
void connected();
void disconnected();
void refused(esp_mqtt_connect_return_code_t code);  // ERROR then DISCONNECTED, as the client does
void transportError(esp_err_t tlsError);            // ERROR then DISCONNECTED
// One DATA event: a piece of a message. topic is only on the first piece.
void dataPiece(const std::string& topic, const std::string& piece, int offset, int total);
// A whole message, cut into pieces of the client's 1 KiB buffer.
void message(const std::string& topic, const std::string& payload, size_t piece = 1024);
// Let time pass in 10 ms steps, as the client's task does: PUBACKs arrive,
// unacknowledged reports are sent again after message_retransmit_timeout,
// and outbox entries expire after 30 s (ESP-IDF v5.5.5 mqtt_client.c).
void run(unsigned long ms);

}  // namespace stub
