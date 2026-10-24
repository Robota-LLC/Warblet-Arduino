// See fake_board.h.
#include "fake_board.h"

#include <string.h>

#include "WarbletBoard.h"

unsigned long simNowMs = 1000;
int simPin[64];
int simAnalog = 0;
SimSerial Serial;
SimState sim;
SimBroker broker;

namespace {

const uint64_t kSimEpochMs = 1759363200000ULL;  // 2025-10-02T00:00:00Z
bool wifiUp = false;
bool clockSet = false;

class SimClient : public Client {
 public:
  int connect(const char* host, uint16_t port) override {
    req_ = SimRequest();
    req_.host = host;
    req_.port = port;
    raw_.clear();
    answer_.clear();
    at_ = 0;
    answered_ = false;
    if (sim.connectFails) {
      simNowMs += 3000;  // a refused handshake or a failed lookup still takes a moment
      return 0;
    }
    return wifiUp ? 1 : 0;
  }
  size_t write(const uint8_t* buf, size_t size) override {
    raw_.append(reinterpret_cast<const char*>(buf), size);
    return size;
  }
  int available() override {
    answerIfWhole();
    return (int)(answer_.size() - at_);
  }
  int read(uint8_t* buf, size_t size) override {
    answerIfWhole();
    size_t n = answer_.size() - at_;
    if (n > size) n = size;
    memcpy(buf, answer_.data() + at_, n);
    at_ += n;
    return (int)n;
  }
  uint8_t connected() override { return !answered_ || at_ < answer_.size(); }
  void stop() override {}

 private:
  void answerIfWhole() {
    if (answered_ || sim.silent) return;  // a silent server never answers
    size_t end = raw_.find("\r\n\r\n");
    if (end == std::string::npos) return;
    req_.head = raw_.substr(0, end + 4);
    size_t want = (size_t)atoi(req_.header("Content-Length").c_str());
    if (raw_.size() < end + 4 + want) return;
    req_.body = raw_.substr(end + 4, want);
    req_.at = simNowMs;
    sim.requests.push_back(req_);
    simNowMs += sim.exchangeMs;  // the request takes time; the sketch is blocked
    answer_ = sim.server ? sim.server(req_) : "HTTP/1.1 500 No Server\r\nContent-Length: 0\r\n\r\n";
    answered_ = true;
  }
  SimRequest req_;
  std::string raw_, answer_;
  size_t at_ = 0;
  bool answered_ = false;
};

SimClient client;

}  // namespace

std::string SimRequest::header(const std::string& name) const {
  std::string key = "\r\n" + name + ": ";
  size_t at = head.find(key);
  if (at == std::string::npos) return "";
  at += key.size();
  return head.substr(at, head.find("\r\n", at) - at);
}

namespace warblet {
namespace board {

void serialBegin(unsigned long baud) {
  Serial.setRxBufferSize(2048);  // as the ESP32 section does
  Serial.begin(baud);
}

void wifiBegin(const char* ssid, const char*) {
  sim.wifiBegins++;
  sim.joins.push_back(std::make_pair(millis(), std::string(ssid)));
  if (sim.wifiJoins && (sim.joinable.empty() || sim.joinable == ssid)) wifiUp = true;
}
bool wifiConnected() { return wifiUp; }
void wifiStop() { wifiUp = false; }

uint32_t freeHeap() { return 200000; }

void restart() { sim.restarts.push_back(simNowMs); }  // a real board does not come back from it

bool syncClock(unsigned long) {
  if (sim.clockAnswers) clockSet = true;
  return clockSet;
}
uint64_t clockMs() { return clockSet ? kSimEpochMs + millis() : 0; }
void simClockLost() { clockSet = false; }

Client& tlsClient() { return client; }

bool loadSettings(char* out, size_t cap, size_t& len) {
  if (sim.stored.empty() || sim.stored.size() + 1 > cap) return false;
  memcpy(out, sim.stored.data(), sim.stored.size());
  out[sim.stored.size()] = '\0';
  len = sim.stored.size();
  return true;
}


// ---- the simulated broker (board::mqtt*) ------------------------------------
namespace {
bool mqttStarted = false;
MqttNews brokerState = kMqttNone;
std::vector<MqttNews> pendingNews;  // changes not yet read, oldest first
int nextMsgId = 1;
unsigned long lastAttemptMs = 0;

// Move the session to what the broker's switches allow, and note the change.
void settle() {
  if (!mqttStarted) return;
  bool knownHost = broker.host == "mqtt.warbletiot.com";  // any other name does not resolve here
  MqttNews want = (broker.unreachable || !wifiUp || !knownHost) ? kMqttUnreachable
                : broker.noDevice ? kMqttNoDevice : broker.refuse ? kMqttRefused : kMqttConnected;
  if (want != brokerState) {
    brokerState = want;
    pendingNews.push_back(want);
    broker.up = want == kMqttConnected;
    lastAttemptMs = simNowMs;
    if (want != kMqttConnected) pendingNews.push_back(kMqttLost);  // like esp-mqtt: an error, then a disconnect
  } else if (want == kMqttUnreachable && simNowMs - lastAttemptMs >= 15000) {
    lastAttemptMs = simNowMs;  // like esp-mqtt: every failed attempt posts both again
    pendingNews.push_back(kMqttUnreachable);
    pendingNews.push_back(kMqttLost);
  }
}
}  // namespace

std::string noteText;
void simNote(const char* text) {  // the board layer has something to say (a dropped command)
  noteText = text;
  pendingNews.push_back(kMqttNote);
}

void simDropSession() {  // the broker closes the board's session
  if (!mqttStarted || brokerState != kMqttConnected) return;
  brokerState = kMqttLost;
  pendingNews.push_back(kMqttLost);
  broker.up = false;
}

bool mqttStart(const char* host, uint16_t port, const char* clientId, const char* user, const char* pass,
               const char* subscribe) {
  broker.starts++;
  broker.host = host;
  broker.port = port;
  broker.clientId = clientId;
  broker.user = user;
  broker.passNull = pass == nullptr;
  broker.pass = pass ? pass : "";
  broker.subscribe = subscribe;
  mqttStarted = true;
  brokerState = kMqttNone;
  return true;
}

void mqttStop() {
  mqttStarted = false;
  brokerState = kMqttNone;
  pendingNews.clear();  // as on the ESP32: a stopped client's news is gone
  broker.up = false;
}

bool mqttConnected() {
  settle();
  return broker.up;
}

int mqttPublish(const char* topic, const uint8_t* data, size_t len) {
  settle();
  if (!broker.up) return -1;
  broker.published.push_back(std::make_pair(std::string(topic), std::string(reinterpret_cast<const char*>(data), len)));
  broker.publishedAt.push_back(simNowMs);
  return nextMsgId++;
}

bool mqttAcked(int msgId) { return msgId > 0 && broker.up; }

int mqttTake(uint8_t* out, size_t cap) {
  settle();
  if (!broker.up || broker.inbox.empty()) return 0;
  std::string c = broker.inbox.front();
  broker.inbox.erase(broker.inbox.begin());
  if (c.size() > cap) return -1;
  memcpy(out, c.data(), c.size());
  return (int)c.size();
}

MqttNews mqttNews(char* detail, size_t cap) {
  settle();
  MqttNews n = kMqttNone;
  if (!pendingNews.empty()) {
    n = pendingNews.front();
    pendingNews.erase(pendingNews.begin());
  }
  const char* d = n == kMqttConnected ? "connected" : n == kMqttLost ? "session lost"
                : n == kMqttRefused ? "CONNECT refused (code 5)" : n == kMqttNoDevice ? "CONNECT refused (code 4)"
                : n == kMqttUnreachable ? "simulated"
                : n == kMqttNote ? noteText.c_str() : "";
  strncpy(detail, d, cap - 1);
  detail[cap - 1] = '\0';
  return n;
}

bool loadRefusedMark(char* out, size_t cap) {
  if (sim.refusedMark.empty() || sim.refusedMark.size() + 1 > cap) return false;
  memcpy(out, sim.refusedMark.c_str(), sim.refusedMark.size() + 1);
  return true;
}

bool saveRefusedMark(const char* text) {
  sim.refusedMark = text;
  return true;
}

bool saveSettings(const char* text, size_t len) {
  if (sim.storeFails) return false;
  sim.stored.assign(text, len);
  return true;
}

}  // namespace board
}  // namespace warblet
