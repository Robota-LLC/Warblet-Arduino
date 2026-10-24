// See WarbletBoard.h. One section per board family.
//
// The ESP32 section is for the Arduino core for ESP32 3.x (and builds without
// MQTT on older cores); test/esp32 builds it on a PC against stand-ins. The
// UNO R4 WiFi and Pico W sections are NOT READY (known problems listed in
// each): they stop the build unless WARBLET_UNREADY_BOARDS is defined.
#include "WarbletBoard.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "WarbletRoots.h"

namespace warblet {
namespace board {

namespace {

const uint64_t kClockSaneSeconds = 1735689600ULL;  // 2025-01-01: below this, never set

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
    if ((uint64_t)now >= kClockSaneSeconds) {
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

const size_t kMqttCommandMax = 8192;   // the queue's own cap on a command
const size_t kMqttWaitingMax = 32768;  // what commands waiting for loop() may hold together

// One command, sized to it: filled on the client's task, then queued for
// loop(), which copies it out and frees it. The client has acknowledged it
// by then, so one that cannot be kept is said, never dropped silently.
struct Command {
  Command* next;
  size_t total;  // its length, as the first piece says
  size_t len;    // bytes arrived so far
  bool tooLong;  // over kMqttCommandMax: nothing kept, handed over as dropped
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
  bool tooLong = total > kMqttCommandMax;
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
#elif defined(ARDUINO_UNOR4_WIFI)
// Arduino UNO R4 WiFi: WiFiS3 and WiFiSSLClient, whose TLS runs on the
// board's ESP32-S3 module. The clock comes from that module (whole
// seconds). Settings live in the 8 KB EEPROM emulated in data flash.
//
// NOT READY: KNOWN PROBLEMS. Do not ship.
//  - Stack: the main stack is 1 KB (BSP_CFG_STACK_MAIN_BYTES 0x400 in core
//    1.6.0). The request buffers now live in the Warblet object, but signing
//    still needs several hundred bytes of stack; not measured on this chip.
//  - Settings save: EEPROM.write() per byte reprograms a whole 1 KB data-flash
//    block (about 50 ms per changed byte, about 100,000 erases per block), so
//    a 400-byte save takes about 20 s, past CHIRP-PROV's 5 s reply deadline,
//    and wears the block out after a few hundred saves. Needs: build the
//    record in RAM and write each block once.
//  - setCACert sends 5.3 KB of roots over the bridge's internal serial line
//    on every connect (about 0.5 s), and the bridge may read past an
//    unterminated buffer (not verified).
//  - Static RAM is about 8 KB of 32 KB, with two settings copies and two
//    2 KB text buffers.
//  - The Serial receive buffer size is not raised here (no call known).
#if !defined(WARBLET_UNREADY_BOARDS)
#error "Warblet: UNO R4 WiFi support is not ready (known problems: see the comment above)."
#endif

#include <EEPROM.h>
#include <WiFiS3.h>

namespace warblet {
namespace board {

void serialBegin(unsigned long baud) { Serial.begin(baud); }

void wifiBegin(const char* ssid, const char* pass) {
  if (pass[0] == '\0') {
    WiFi.begin(ssid);
  } else {
    WiFi.begin(ssid, pass);
  }
}

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

uint32_t freeHeap() { return 0; }

void restart() { NVIC_SystemReset(); }

void wifiStop() { WiFi.disconnect(); }

bool syncClock(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    unsigned long now = WiFi.getTime();
    if ((uint64_t)now >= kClockSaneSeconds) {
      setClock((uint64_t)now);
      return true;
    }
    delay(500);
  }
  return false;
}

Client& tlsClient() {
  static WiFiSSLClient client;
  static bool ready = false;
  if (!ready) {
    client.setCACert(kRootCertificates);
    ready = true;
  }
  return client;
}

#define WARBLET_EEPROM_RECORD 1

}  // namespace board
}  // namespace warblet

// ---------------------------------------------------------------------------
#elif defined(ARDUINO_ARCH_RP2040) && defined(ARDUINO_RASPBERRY_PI_PICO_W)
// Raspberry Pi Pico W on the arduino-pico core: WiFiClientSecure (BearSSL),
// which checks certificate dates, so the clock is set before any TLS.
// NTP.begin for the clock; settings in the 4 KB emulated EEPROM.
//
// NOT READY: KNOWN PROBLEMS. Do not ship.
//  - Never compiled or run; BearSSL's memory use with four roots is not
//    checked.
//  - The Serial receive buffer size is not raised here (no call known), so
//    only the quiet window after the setup page's probe protects a push.
//  - The board macro ARDUINO_RASPBERRY_PI_PICO_W is read about, not seen.
#if !defined(WARBLET_UNREADY_BOARDS)
#error "Warblet: Pico W support is not ready (known problems: see the comment above)."
#endif

#include <EEPROM.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace warblet {
namespace board {

void serialBegin(unsigned long baud) { Serial.begin(baud); }

void wifiBegin(const char* ssid, const char* pass) {
  if (pass[0] == '\0') {
    WiFi.begin(ssid);
  } else {
    WiFi.begin(ssid, pass);
  }
}

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

uint32_t freeHeap() { return rp2040.getFreeHeap(); }

void restart() { rp2040.reboot(); }

void wifiStop() { WiFi.disconnect(); }

bool syncClock(unsigned long timeoutMs) {
  NTP.begin("pool.ntp.org", "time.google.com");
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    time_t now = time(nullptr);
    if ((uint64_t)now >= kClockSaneSeconds) {
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
    ready = true;
  }
  return client;
}

#define WARBLET_EEPROM_RECORD 1
#define WARBLET_EEPROM_BEGIN 4096

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

// ---------------------------------------------------------------------------
#if defined(WARBLET_EEPROM_RECORD)
// One record: "WB1", a two-byte length, then the text.

namespace warblet {
namespace board {

bool loadSettings(char* out, size_t cap, size_t& len) {
#if defined(WARBLET_EEPROM_BEGIN)
  EEPROM.begin(WARBLET_EEPROM_BEGIN);
#endif
  if (EEPROM.read(0) != 'W' || EEPROM.read(1) != 'B' || EEPROM.read(2) != '1') return false;
  size_t n = (size_t)EEPROM.read(3) << 8 | EEPROM.read(4);
  if (n == 0 || n + 1 > cap || n + 5 > (size_t)EEPROM.length()) return false;
  for (size_t i = 0; i < n; i++) out[i] = (char)EEPROM.read((int)(5 + i));
  out[n] = '\0';
  len = n;
  return true;
}

bool saveSettings(const char* text, size_t len) {
#if defined(WARBLET_EEPROM_BEGIN)
  EEPROM.begin(WARBLET_EEPROM_BEGIN);
#endif
  if (len + 5 > (size_t)EEPROM.length() || len > 0xffff) return false;
  EEPROM.write(0, 'W');
  EEPROM.write(1, 'B');
  EEPROM.write(2, '1');
  EEPROM.write(3, (uint8_t)(len >> 8));
  EEPROM.write(4, (uint8_t)(len & 0xff));
  for (size_t i = 0; i < len; i++) EEPROM.write((int)(5 + i), (uint8_t)text[i]);
#if defined(WARBLET_EEPROM_BEGIN)
  if (!EEPROM.commit()) return false;
#endif
  // Read it back: a record that did not land is a store failure, said aloud.
  for (size_t i = 0; i < len; i++) {
    if (EEPROM.read((int)(5 + i)) != (uint8_t)text[i]) return false;
  }
  return true;
}

// The refused-setup mark: "WM" and up to 32 characters at offset 3072, past
// the settings record's 2,048 + 5 bytes.
const int kMarkAt = 3072;

bool loadRefusedMark(char* out, size_t cap) {
#if defined(WARBLET_EEPROM_BEGIN)
  EEPROM.begin(WARBLET_EEPROM_BEGIN);
#endif
  if (EEPROM.read(kMarkAt) != 'W' || EEPROM.read(kMarkAt + 1) != 'M') return false;
  size_t n = 0;
  while (n + 1 < cap && n < 32) {
    char c = (char)EEPROM.read(kMarkAt + 2 + (int)n);
    if (c == '\0') break;
    out[n++] = c;
  }
  out[n] = '\0';
  return n > 0;
}

bool saveRefusedMark(const char* text) {
#if defined(WARBLET_EEPROM_BEGIN)
  EEPROM.begin(WARBLET_EEPROM_BEGIN);
#endif
  size_t n = strlen(text);
  if (n > 32) return false;
  EEPROM.write(kMarkAt, n ? 'W' : 0);
  EEPROM.write(kMarkAt + 1, n ? 'M' : 0);
  for (size_t i = 0; i <= n; i++) EEPROM.write(kMarkAt + 2 + (int)i, (uint8_t)text[i]);
#if defined(WARBLET_EEPROM_BEGIN)
  return EEPROM.commit();
#else
  return true;
#endif
}

}  // namespace board
}  // namespace warblet

#endif
