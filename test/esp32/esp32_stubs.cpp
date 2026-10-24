// The stand-in ESP32 core and esp-mqtt client behind test/esp32's headers.
//
// The MQTT part follows ESP-IDF v5.5.5's components/mqtt/esp-mqtt/mqtt_client.c
// where the library's behaviour depends on it:
// - a failed connect ends with MQTT_EVENT_ERROR, then MQTT_EVENT_DISCONNECTED;
// - a message longer than the client's buffer arrives as several
//   MQTT_EVENT_DATA events, the topic only on the first;
// - esp_mqtt_task sends a transmitted, unacknowledged QoS 1 publish again
//   once it is older than message_retransmit_timeout (MQTT_DEFAULT_RETRANSMIT_
//   TIMEOUT_MS, 1000 ms, when the config leaves it at 0), checking at that
//   interval, and deletes outbox entries older than OUTBOX_EXPIRED_TIMEOUT_MS
//   (30 s; the core's sdkconfig leaves CONFIG_MQTT_USE_CUSTOM_CONFIG unset).
#include "stub_control.h"

#include <string.h>

#include "Preferences.h"
#include "WiFi.h"

unsigned long simNowMs = 1000;
int simPin[64];
int simAnalog = 0;
SimSerial Serial;
SimEsp ESP;
SimWiFi WiFi;

void configTime(long, int, const char*, const char*) {}

namespace stub {
Config config;
std::vector<Outgoing> outbox;
std::vector<std::string> subscribes;
std::vector<std::string> logTags;
int clientsAlive = 0;
}  // namespace stub

int simLocksHeld = 0;

void esp_log_level_set(const char* tag, esp_log_level_t) { stub::logTags.push_back(tag); }

struct esp_mqtt_client {
  esp_event_handler_t handler = nullptr;
  void* arg = nullptr;
  bool started = false;
};

namespace {
esp_mqtt_client* current = nullptr;
int nextId = 1;
unsigned long lastRetransmit = 0;
const unsigned long kOutboxExpiredMs = 30000;  // OUTBOX_EXPIRED_TIMEOUT_MS
const unsigned long kDefaultRetransmitMs = 1000;  // MQTT_DEFAULT_RETRANSMIT_TIMEOUT_MS

void dispatch(esp_mqtt_event_t& e) {
  if (current == nullptr || current->handler == nullptr || !current->started) return;
  e.client = current;
  current->handler(current->arg, "MQTT_EVENTS", e.event_id, &e);
}

unsigned long retransmitMs() {
  int t = stub::config.raw.session.message_retransmit_timeout;
  return t <= 0 ? kDefaultRetransmitMs : (unsigned long)t;
}
}  // namespace

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t* c) {
  stub::config = stub::Config();
  stub::config.set = true;
  stub::config.raw = *c;
  stub::config.host = c->broker.address.hostname ? c->broker.address.hostname : "";
  stub::config.username = c->credentials.username ? c->credentials.username : "";
  stub::config.clientId = c->credentials.client_id ? c->credentials.client_id : "";
  stub::config.passwordNull = c->credentials.authentication.password == nullptr;
  stub::config.password = c->credentials.authentication.password ? c->credentials.authentication.password : "";
  stub::config.certificate = c->broker.verification.certificate ? c->broker.verification.certificate : "";
  current = new esp_mqtt_client();
  stub::clientsAlive++;
  return current;
}

esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, esp_mqtt_event_id_t,
                                         esp_event_handler_t handler, void* arg) {
  client->handler = handler;
  client->arg = arg;
  return ESP_OK;
}

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client) {
  client->started = true;
  return ESP_OK;
}

esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client) {
  client->started = false;
  return ESP_OK;
}

esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client) {
  if (client == current) current = nullptr;
  delete client;
  stub::clientsAlive--;
  return ESP_OK;
}

int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t, const char* topic, int) {
  stub::subscribes.push_back(topic);
  return nextId++;
}

int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char* topic, const char* data, int len, int qos, int) {
  stub::Outgoing o;
  o.topic = topic;
  o.data.assign(data, (size_t)len);
  o.msgId = nextId++;
  o.tick = millis();
  o.sends = 1;
  if (qos == 0) o.acked = true;
  stub::outbox.push_back(o);
  return o.msgId;
}

namespace stub {

void connected() {
  esp_mqtt_event_t e = {};
  e.event_id = MQTT_EVENT_CONNECTED;
  dispatch(e);
}

void disconnected() {
  esp_mqtt_event_t e = {};
  e.event_id = MQTT_EVENT_DISCONNECTED;
  dispatch(e);
}

void refused(esp_mqtt_connect_return_code_t code) {
  esp_mqtt_error_codes_t err = {};
  err.error_type = MQTT_ERROR_TYPE_CONNECTION_REFUSED;
  err.connect_return_code = code;
  esp_mqtt_event_t e = {};
  e.event_id = MQTT_EVENT_ERROR;
  e.error_handle = &err;
  dispatch(e);
  disconnected();
}

void transportError(esp_err_t tlsError) {
  esp_mqtt_error_codes_t err = {};
  err.error_type = MQTT_ERROR_TYPE_TCP_TRANSPORT;
  err.esp_tls_last_esp_err = tlsError;
  esp_mqtt_event_t e = {};
  e.event_id = MQTT_EVENT_ERROR;
  e.error_handle = &err;
  dispatch(e);
  disconnected();
}

void dataPiece(const std::string& topic, const std::string& piece, int offset, int total) {
  std::string t = topic, p = piece;  // the client hands out pointers into its own buffer
  esp_mqtt_event_t e = {};
  e.event_id = MQTT_EVENT_DATA;
  e.topic = offset == 0 ? &t[0] : nullptr;
  e.topic_len = offset == 0 ? (int)t.size() : 0;
  e.data = p.empty() ? nullptr : &p[0];
  e.data_len = (int)p.size();
  e.current_data_offset = offset;
  e.total_data_len = total;
  e.qos = 1;
  dispatch(e);
}

void message(const std::string& topic, const std::string& payload, size_t piece) {
  if (payload.empty()) {
    dataPiece(topic, "", 0, 0);
    return;
  }
  for (size_t at = 0; at < payload.size(); at += piece) {
    dataPiece(topic, payload.substr(at, piece), (int)at, (int)payload.size());
  }
}

void run(unsigned long ms) {
  unsigned long end = millis() + ms;
  while ((long)(end - millis()) > 0) {
    delay(10);
    unsigned long now = millis();
    for (Outgoing& o : outbox) {
      if (!o.acked && !o.expired && o.ackAt != 0 && now >= o.ackAt) {
        o.acked = true;
        esp_mqtt_event_t e = {};
        e.event_id = MQTT_EVENT_PUBLISHED;
        e.msg_id = o.msgId;
        dispatch(e);
      }
      if (!o.acked && now - o.tick > kOutboxExpiredMs) o.expired = true;
    }
    if (now - lastRetransmit > retransmitMs()) {
      lastRetransmit = now;
      for (Outgoing& o : outbox) {  // the oldest transmitted one, once per interval
        if (!o.acked && !o.expired && now - o.tick > retransmitMs()) {
          o.sends++;
          break;
        }
      }
    }
  }
}

}  // namespace stub
