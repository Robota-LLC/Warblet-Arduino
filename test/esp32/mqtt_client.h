// Espressif's MQTT client (esp-mqtt), as the Arduino core for ESP32 3.3.12
// ships it (ESP-IDF v5.5.5): the types and calls src/WarbletBoard.cpp uses,
// with the same names, and a PC stand-in behind them (esp32_stubs.cpp) that
// a test drives with the events the real client produces.
#pragma once

#include <stdint.h>

#include "Arduino.h"

typedef struct esp_mqtt_client* esp_mqtt_client_handle_t;

typedef enum esp_mqtt_event_id_t {
  MQTT_EVENT_ANY = -1,
  MQTT_EVENT_ERROR = 0,
  MQTT_EVENT_CONNECTED,
  MQTT_EVENT_DISCONNECTED,
  MQTT_EVENT_SUBSCRIBED,
  MQTT_EVENT_UNSUBSCRIBED,
  MQTT_EVENT_PUBLISHED,
  MQTT_EVENT_DATA,
  MQTT_EVENT_BEFORE_CONNECT,
  MQTT_EVENT_DELETED,
  MQTT_USER_EVENT,
} esp_mqtt_event_id_t;

typedef enum esp_mqtt_connect_return_code_t {
  MQTT_CONNECTION_ACCEPTED = 0,
  MQTT_CONNECTION_REFUSE_PROTOCOL,
  MQTT_CONNECTION_REFUSE_ID_REJECTED,
  MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE,
  MQTT_CONNECTION_REFUSE_BAD_USERNAME,
  MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
} esp_mqtt_connect_return_code_t;

typedef enum esp_mqtt_error_type_t {
  MQTT_ERROR_TYPE_NONE = 0,
  MQTT_ERROR_TYPE_TCP_TRANSPORT,
  MQTT_ERROR_TYPE_CONNECTION_REFUSED,
  MQTT_ERROR_TYPE_SUBSCRIBE_FAILED
} esp_mqtt_error_type_t;

typedef enum esp_mqtt_transport_t {
  MQTT_TRANSPORT_UNKNOWN = 0x0,
  MQTT_TRANSPORT_OVER_TCP,
  MQTT_TRANSPORT_OVER_SSL,
  MQTT_TRANSPORT_OVER_WS,
  MQTT_TRANSPORT_OVER_WSS
} esp_mqtt_transport_t;

typedef struct esp_mqtt_error_codes {
  esp_err_t esp_tls_last_esp_err;
  int esp_tls_stack_err;
  int esp_tls_cert_verify_flags;
  esp_mqtt_error_type_t error_type;
  esp_mqtt_connect_return_code_t connect_return_code;
  int esp_transport_sock_errno;
} esp_mqtt_error_codes_t;

typedef struct esp_mqtt_event_t {
  esp_mqtt_event_id_t event_id;
  esp_mqtt_client_handle_t client;
  char* data;
  int data_len;
  int total_data_len;
  int current_data_offset;
  char* topic;
  int topic_len;
  int msg_id;
  int session_present;
  esp_mqtt_error_codes_t* error_handle;
  bool retain;
  int qos;
  bool dup;
} esp_mqtt_event_t;
typedef esp_mqtt_event_t* esp_mqtt_event_handle_t;

typedef struct esp_mqtt_client_config_t {
  struct broker_t {
    struct address_t {
      const char* uri;
      const char* hostname;
      esp_mqtt_transport_t transport;
      const char* path;
      uint32_t port;
    } address;
    struct verification_t {
      bool use_global_ca_store;
      const char* certificate;
      size_t certificate_len;
      bool skip_cert_common_name_check;
      const char* common_name;
    } verification;
  } broker;
  struct credentials_t {
    const char* username;
    const char* client_id;
    bool set_null_client_id;
    struct authentication_t {
      const char* password;
    } authentication;
  } credentials;
  struct session_t {
    bool disable_clean_session;
    int keepalive;
    bool disable_keepalive;
    int message_retransmit_timeout;
  } session;
  struct network_t {
    int reconnect_timeout_ms;
    int timeout_ms;
    int refresh_connection_after_ms;
    bool disable_auto_reconnect;
  } network;
  struct buffer_t {
    int size;
    int out_size;
  } buffer;
  struct outbox_config_t {
    uint64_t limit;
  } outbox;
} esp_mqtt_client_config_t;

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t* config);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, esp_mqtt_event_id_t event,
                                         esp_event_handler_t event_handler, void* event_handler_arg);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char* topic, int qos);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t client, const char* topic, const char* data, int len, int qos,
                            int retain);
