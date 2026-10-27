// See WarbletBoard.h.
//
// The ESP32 section is for the Arduino core for ESP32 3.x (and builds without
// MQTT on older cores); test/esp32 builds it on a PC against stand-ins. Any
// other board stops the build with a message.
#include "WarbletBoard.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "WarbletRoots.h"
#include "warblet/core/wire.h"

namespace warblet {
namespace board {

namespace {

uint64_t baseUnixMs = 0;
unsigned long baseMillis = 0;

void setClock(uint64_t unixSeconds) {
  baseUnixMs = unixSeconds * 1000ULL;
  baseMillis = millis();
}

}  // namespace

uint64_t clockMs() {
  if (baseUnixMs == 0) return 0;
  return baseUnixMs + (uint64_t)(unsigned long)(millis() - baseMillis);
}

}  // namespace board
}  // namespace warblet

// ---------------------------------------------------------------------------
#if defined(ARDUINO_ARCH_ESP32)
// ESP32 family on the Arduino core for ESP32, and the Arduino Nano ESP32
// (whose core is built from the same source). WiFiClientSecure with
// setCACert; Preferences (NVS) for settings; configTime for the clock.

#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_idf_version.h>
#include <esp_log.h>

// MQTT is written for Espressif's client as ESP-IDF 5 configures it (the
// Arduino core for ESP32 3.x). An older core, such as the Nano ESP32's
// (ESP-IDF 4.4), builds without it, and the console says so.
#if ESP_IDF_VERSION_MAJOR >= 5
#define WARBLET_HAS_MQTT 1
#include <esp_tls_errors.h>
#include <mqtt_client.h>
#endif

namespace warblet {
namespace board {

void serialBegin(unsigned long baud) {
  // HardwareSerial, HWCDC and USBCDC all take setRxBufferSize(size_t), and
  // keep it when it is set before begin() (default 256; core 3.3.12 and the
  // Nano ESP32 core). A push with two networks or a key is over 256 bytes.
  Serial.setRxBufferSize(2048);
  Serial.begin(baud);
}

void wifiBegin(const char* ssid, const char* pass) {
  WiFi.mode(WIFI_STA);
  if (pass[0] == '\0') {
    WiFi.begin(ssid);
  } else {
    WiFi.begin(ssid, pass);
  }
}

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

uint32_t freeHeap() { return ESP.getFreeHeap(); }

void restart() {
  Serial.flush();
  delay(100);
  ESP.restart();
}

void wifiStop() { WiFi.disconnect(); }

bool syncClock(unsigned long timeoutMs) {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    time_t now = time(nullptr);
    if ((uint64_t)now >= NonceClock::kClockSaneMs / 1000) {  // below it, never set
      setClock((uint64_t)now);
      return true;
    }
    delay(100);
  }
  return false;
}

Client& tlsClient() {
  static WiFiClientSecure client;
  static bool ready = false;
  if (!ready) {
    client.setCACert(kRootCertificates);
    // Every wait gets a limit (the core's defaults are a 30 s connect and a
    // 120 s handshake). setConnectionTimeout() sets the connect limit and
    // each read and write limit of core 3.x's TLS client.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    client.setConnectionTimeout(10000);
#endif
    client.setHandshakeTimeout(15);  // seconds
    ready = true;
  }
  return client;
}

bool loadSettings(char* out, size_t cap, size_t& len) {
  Preferences p;
  if (!p.begin("warblet", true)) return false;
  bool ok = false;
  if (p.isKey("settings")) {
    len = p.getString("settings", out, cap);
    ok = len > 0 && len < cap;
    if (ok && out[len - 1] == '\0') len--;  // the stored length counts the NUL
  }
  p.end();
  return ok;
}

bool saveSettings(const char* text, size_t len) {
  Preferences p;
  if (!p.begin("warblet", false)) return false;
  size_t wrote = p.putString("settings", text);
  p.end();
  return wrote == len;
}

bool loadRefusedMark(char* out, size_t cap) {
  Preferences p;
  if (!p.begin("warblet", true)) return false;
  bool ok = false;
  if (p.isKey("refused")) {
    size_t len = p.getString("refused", out, cap);
    ok = len > 1 && len <= cap && out[0] != '\0';
  }
  p.end();
  return ok;
}

bool saveRefusedMark(const char* text) {
  Preferences p;
  if (!p.begin("warblet", false)) return false;
  bool ok = text[0] == '\0' ? (!p.isKey("refused") || p.remove("refused")) : p.putString("refused", text) == strlen(text);
  p.end();
  return ok;
}

#if defined(WARBLET_HAS_MQTT)
// ---- MQTT: Espressif's own client (esp-mqtt), part of the ESP32 core --------
// It runs on its own task. Its events land here; the sketch's loop takes the
// commands from a queue, so the command function runs in loop().

namespace {

const size_t kMqttWaitingMax = 32768;  // what commands waiting for loop() may hold together

// One command, sized to it: filled on the client's task, then queued for
// loop(), which copies it out and frees it. The client has acknowledged it
// by then, so one that cannot be kept is said, never dropped silently.
struct Command {
  Command* next;
  size_t total;  // its length, as the first piece says
  size_t len;    // bytes arrived so far
  bool tooLong;  // over kCommandMax: nothing kept, handed over as dropped
  uint8_t data[1];
};

esp_mqtt_client_handle_t mqttClient = nullptr;
portMUX_TYPE mqttLock = portMUX_INITIALIZER_UNLOCKED;
volatile bool mqttUp = false;
char mqttHost[129], mqttId[129], mqttPass[129], mqttSub[160];
Command* waitingHead = nullptr;  // under mqttLock
Command* waitingTail = nullptr;  // under mqttLock
size_t waitingBytes = 0;         // under mqttLock
int waitingCount = 0;            // under mqttLock
Command* filling = nullptr;      // the client's task only (and mqttStop once it stopped)
int ackedIds[8];        // read and written under mqttLock only
uint8_t ackedAt = 0;
// Changes of the session, oldest first, so a drop and its reconnect are both
// said even when they come between two passes of the sketch's loop.
struct News {
  MqttNews kind;
  char detail[96];
};
News newsRing[4];
uint8_t newsHead = 0, newsCount = 0;  // under mqttLock only

void setNews(MqttNews n, const char* detail) {
  portENTER_CRITICAL(&mqttLock);
  if (newsCount == 4) {  // full: the oldest goes
    newsHead = (newsHead + 1) % 4;
    newsCount--;
  }
  News& slot = newsRing[(newsHead + newsCount) % 4];
  slot.kind = n;
  strncpy(slot.detail, detail, sizeof slot.detail - 1);
  slot.detail[sizeof slot.detail - 1] = '\0';
  newsCount++;
  portEXIT_CRITICAL(&mqttLock);
}

// A command still being filled when the next one starts, or when the
// session drops, was cut off on the way: its memory goes, and it is said.
void dropCutOff() {
  if (filling == nullptr) return;
  free(filling);
  filling = nullptr;
  setNews(kMqttNote, "a command was cut off on the way and was dropped");
}

// The first piece of a command: room for all of it, or say it was dropped.
void startCommand(esp_mqtt_event_handle_t e) {
  size_t total = e->total_data_len > 0 ? (size_t)e->total_data_len : 0;
  bool tooLong = total > kCommandMax;
  size_t keep = tooLong ? 0 : total;
  portENTER_CRITICAL(&mqttLock);
  bool room = waitingBytes + keep <= kMqttWaitingMax;
  int waiting = waitingCount;
  portEXIT_CRITICAL(&mqttLock);
  Command* c = room ? static_cast<Command*>(malloc(sizeof(Command) + keep)) : nullptr;
  if (c == nullptr) {
    char start[25];
    size_t n = e->data_len > 0 ? (size_t)e->data_len : 0;
    if (n > sizeof start - 1) n = sizeof start - 1;
    for (size_t i = 0; i < n; i++) {
      char ch = e->data[i];
      start[i] = ((unsigned char)ch < 0x20 || ch == 0x7f) ? '?' : ch;
    }
    start[n] = '\0';
    char d[96];
    snprintf(d, sizeof d, "no room for a command while %d wait, so it was dropped: %s", waiting, start);
    setNews(kMqttNote, d);
    return;  // filling stays empty: the rest of its pieces are passed over
  }
  c->next = nullptr;
  c->total = keep;
  c->len = 0;
  c->tooLong = tooLong;
  filling = c;
}

void onMqttEvent(void*, esp_event_base_t, int32_t, void* data) {
  esp_mqtt_event_handle_t e = static_cast<esp_mqtt_event_handle_t>(data);
  switch ((esp_mqtt_event_id_t)e->event_id) {
    case MQTT_EVENT_CONNECTED:
      mqttUp = true;
      esp_mqtt_client_subscribe(mqttClient, mqttSub, 1);
      setNews(kMqttConnected, "connected");
      break;
    case MQTT_EVENT_DISCONNECTED:
      dropCutOff();
      // The client also ends each failed attempt with this; only an open session is lost.
      if (mqttUp) setNews(kMqttLost, "session lost");
      mqttUp = false;
      break;
    case MQTT_EVENT_PUBLISHED:
      portENTER_CRITICAL(&mqttLock);
      ackedIds[ackedAt++ % 8] = e->msg_id;
      portEXIT_CRITICAL(&mqttLock);
      break;
    case MQTT_EVENT_DATA: {
      if (e->current_data_offset == 0) {
        dropCutOff();  // a new message: one still being filled never finished
        // The first piece names its topic; only the board's own command topic counts.
        if ((size_t)e->topic_len != strlen(mqttSub) || strncmp(e->topic, mqttSub, e->topic_len) != 0) break;
        startCommand(e);
      }
      if (filling == nullptr) break;
      if (!filling->tooLong && e->data_len > 0 && e->current_data_offset >= 0 &&
          (size_t)e->current_data_offset + (size_t)e->data_len <= filling->total) {
        memcpy(filling->data + e->current_data_offset, e->data, e->data_len);
        filling->len = (size_t)(e->current_data_offset + e->data_len);
      }
      if (e->current_data_offset + e->data_len >= e->total_data_len) {
        portENTER_CRITICAL(&mqttLock);
        if (waitingTail != nullptr) {
          waitingTail->next = filling;
        } else {
          waitingHead = filling;
        }
        waitingTail = filling;
        waitingBytes += filling->total;
        waitingCount++;
        portEXIT_CRITICAL(&mqttLock);
        filling = nullptr;
      }
      break;
    }
    case MQTT_EVENT_ERROR:
      if (e->error_handle == nullptr) break;
      if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        char d[48];
        snprintf(d, sizeof d, "CONNECT refused (code %d)", (int)e->error_handle->connect_return_code);
        // Contract v0.47: a credential for an id that is no device is refused
        // with code 4; a wrong token for a device that exists stays code 5.
        bool noDevice = e->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME;
        setNews(noDevice ? kMqttNoDevice : kMqttRefused, d);
      } else if (e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT && !mqttUp) {
        // Only while connecting: an established session that the broker
        // closes is reported as "session lost" by the disconnect that follows.
        char d[64];
        esp_err_t tls = e->error_handle->esp_tls_last_esp_err;
        const char* why = tls == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME   ? "the host name did not resolve"
                          : tls == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT      ? "no answer within 10 s"
                          : tls == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST  ? "the host refused the connection"
                          : tls == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED    ? "the TLS handshake failed"
                          : tls == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT ? "the TLS handshake timed out"
                                                                           : nullptr;
        if (why != nullptr) {
          snprintf(d, sizeof d, "%s", why);
        } else {
          snprintf(d, sizeof d, "tls 0x%x, socket errno %d", (unsigned)tls, e->error_handle->esp_transport_sock_errno);
        }
        setNews(kMqttUnreachable, d);
      }
      break;
    default:
      break;
  }
}

void copyText(char* dst, size_t cap, const char* src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = '\0';
}

}  // namespace

bool mqttStart(const char* host, uint16_t port, const char* clientId, const char* user, const char* pass,
               const char* subscribe) {
  mqttStop();
  copyText(mqttHost, sizeof mqttHost, host);
  copyText(mqttId, sizeof mqttId, clientId);
  copyText(mqttPass, sizeof mqttPass, pass);
  copyText(mqttSub, sizeof mqttSub, subscribe);
  (void)user;  // the contract's username is the device id, as is the client id
  // The library says what happened in its own words; the MQTT client's own
  // error lines would only repeat it. The TLS layer's tags (esp-tls,
  // transport_base) are shared with the sketch, so they are left alone: when
  // the broker cannot be reached, those layers may print their own E lines.
  esp_log_level_set("mqtt_client", ESP_LOG_NONE);
  esp_mqtt_client_config_t c = {};
  c.broker.address.hostname = mqttHost;
  c.broker.address.port = port;
  c.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
  c.broker.verification.certificate = kRootCertificates;  // the four pinned roots, PEM
  c.credentials.username = mqttId;
  c.credentials.client_id = mqttId;
  c.credentials.authentication.password = pass ? mqttPass : nullptr;  // no password flag at Open
  c.session.keepalive = 60;
  // ESP-IDF v5.5.5's esp_mqtt_client_config_t::session.message_retransmit_timeout:
  // left at 0, the client sends an unacknowledged report again every second,
  // and the broker stores every copy (no nonce on MQTT). The library waits 10 s
  // for the acknowledgement, and the client drops a report from its outbox after
  // 30 s (OUTBOX_EXPIRED_TIMEOUT_MS), so 60 s means a report goes out once.
  c.session.message_retransmit_timeout = 60000;
  c.network.timeout_ms = 10000;            // every network wait has a limit
  c.network.reconnect_timeout_ms = 10000;  // and it comes back by itself
  mqttClient = esp_mqtt_client_init(&c);
  if (mqttClient == nullptr) return false;
  esp_mqtt_client_register_event(mqttClient, MQTT_EVENT_ANY, onMqttEvent, nullptr);
  return esp_mqtt_client_start(mqttClient) == ESP_OK;
}

void mqttStop() {
  if (mqttClient == nullptr) return;
  esp_mqtt_client_stop(mqttClient);
  esp_mqtt_client_destroy(mqttClient);
  mqttClient = nullptr;
  mqttUp = false;
  free(filling);  // the client's task has stopped: nothing else touches it now
  filling = nullptr;
  newsHead = 0;
  newsCount = 0;
  // Commands that arrived whole stay queued: the broker counts them delivered.
}

bool mqttConnected() { return mqttClient != nullptr && mqttUp; }

int mqttPublish(const char* topic, const uint8_t* data, size_t len) {
  if (!mqttConnected()) return -1;
  int id = esp_mqtt_client_publish(mqttClient, topic, reinterpret_cast<const char*>(data), (int)len, 1, 0);
  return id > 0 ? id : -1;
}

bool mqttAcked(int msgId) {
  bool found = false;
  portENTER_CRITICAL(&mqttLock);
  for (int i = 0; i < 8; i++) found = found || ackedIds[i] == msgId;
  portEXIT_CRITICAL(&mqttLock);
  return found;
}

int mqttTake(uint8_t* out, size_t cap) {
  for (;;) {
    portENTER_CRITICAL(&mqttLock);
    Command* c = waitingHead;
    if (c != nullptr) {
      waitingHead = c->next;
      if (waitingHead == nullptr) waitingTail = nullptr;
      waitingBytes -= c->total;
      waitingCount--;
    }
    portEXIT_CRITICAL(&mqttLock);
    if (c == nullptr) return 0;
    int result;
    if (c->tooLong || c->len > cap) {
      result = -1;
    } else {
      memcpy(out, c->data, c->len);
      result = (int)c->len;
    }
    free(c);
    if (result != 0) return result;  // an empty command is nothing to hand over
  }
}

MqttNews mqttNews(char* detail, size_t cap) {
  MqttNews n = kMqttNone;
  portENTER_CRITICAL(&mqttLock);
  if (newsCount > 0) {
    n = newsRing[newsHead].kind;
    copyText(detail, cap, newsRing[newsHead].detail);
    newsHead = (newsHead + 1) % 4;
    newsCount--;
  } else if (cap > 0) {
    detail[0] = '\0';
  }
  portEXIT_CRITICAL(&mqttLock);
  return n;
}
#endif  // WARBLET_HAS_MQTT

}  // namespace board
}  // namespace warblet

// ---------------------------------------------------------------------------
#else
#error "Warblet: this board is not supported yet. Supported first: ESP32 boards and the Arduino Nano ESP32 (UNO R4 WiFi and Pico W are not ready)."
#endif

// ---------------------------------------------------------------------------
#if !defined(WARBLET_HAS_MQTT)
// MQTT is built on Espressif's own client as ESP-IDF 5 has it; elsewhere
// mqttStart() fails and the library says MQTT is not available.

namespace warblet {
namespace board {

bool mqttStart(const char*, uint16_t, const char*, const char*, const char*, const char*) { return false; }
void mqttStop() {}
bool mqttConnected() { return false; }
int mqttPublish(const char*, const uint8_t*, size_t) { return -1; }
bool mqttAcked(int) { return false; }
int mqttTake(uint8_t*, size_t) { return 0; }
MqttNews mqttNews(char* detail, size_t cap) {
  if (cap > 0) detail[0] = '\0';
  return kMqttNone;
}

}  // namespace board
}  // namespace warblet

#endif
