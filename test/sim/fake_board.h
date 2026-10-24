// A simulated board for running src/Warblet.cpp on a PC: Wi-Fi that joins,
// a clock, settings kept in memory, and a TLS client that hands each request
// to a test's fake server. Stands in for src/WarbletBoard.cpp.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Arduino.h"
#include "Client.h"

struct SimRequest {
  std::string host;
  int port = 0;
  std::string head;  // up to and including the blank line
  std::string body;
  unsigned long at = 0;  // simulated millis() when the request was complete
  std::string header(const std::string& name) const;  // "" when absent
};

struct SimState {
  bool wifiJoins = true;
  std::string joinable;     // when set, only this network joins
  bool clockAnswers = true;
  int wifiBegins = 0;
  std::vector<std::pair<unsigned long, std::string>> joins;  // when, which network
  unsigned long exchangeMs = 0;  // how long each HTTPS request takes
  bool connectFails = false;     // TLS refused, or DNS failing: connect() returns 0
  bool silent = false;           // the server takes the request and never answers
  std::string stored;       // the settings record, as saved
  bool storeFails = false;
  std::string refusedMark;  // the refused-setup fingerprint, as kept
  std::vector<unsigned long> restarts;  // when board::restart() was called
  std::vector<SimRequest> requests;
  // Answers one request with the full HTTP response text.
  std::function<std::string(const SimRequest&)> server;
};
extern SimState sim;

// A broker standing in for mqtt.warbletiot.com:8883 behind board::mqtt*.
struct SimBroker {
  int starts = 0;              // how many sessions the board started
  std::string host, clientId, user, pass, subscribe;
  int port = 0;
  bool passNull = false;       // no password flag (Open)
  bool refuse = false;         // CONNECT refused (a credential the broker does not accept)
  bool noDevice = false;       // CONNECT refused with code 4: no device has this id (v0.47)
  bool unreachable = false;    // name lookup, TCP or TLS fails
  bool up = false;
  std::vector<std::pair<std::string, std::string>> published;  // topic, payload
  std::vector<unsigned long> publishedAt;
  std::vector<std::string> inbox;  // commands waiting for the board
};
extern SimBroker broker;
namespace warblet {
namespace board {
void simDropSession();  // the broker closes the board's session
void simNote(const char* text);  // the board layer reports something to say as it is
void simClockLost();             // the clock is gone, as after a power cut
}  // namespace board
}  // namespace warblet
