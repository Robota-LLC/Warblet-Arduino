/* Just enough of ESP-IDF's declarations to compile the public camera demo's
 * freenove-wrover-cam/main/chirp_send.c and chirp_sign.c, unmodified, on a
 * PC. Every function is a stand-in defined in ../stub_idf.c. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
const char *esp_err_to_name(esp_err_t err);

#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))

typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY ((TickType_t)0xffffffffu)
#define pdPASS 1
void vTaskDelay(TickType_t ticks);
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                       int priority, void *handle);

typedef struct stub_http *esp_http_client_handle_t;
typedef enum { HTTP_METHOD_GET = 0, HTTP_METHOD_POST = 1 } esp_http_client_method_t;
typedef struct {
  const char *url;
  esp_http_client_method_t method;
  int timeout_ms;
  esp_err_t (*crt_bundle_attach)(void *conf);
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_write(esp_http_client_handle_t client, const char *buffer, int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_read_response(esp_http_client_handle_t client, char *buffer, int len);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t esp_crt_bundle_attach(void *conf);

typedef struct {
  const char *server;
} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(s) {s}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
esp_err_t esp_netif_sntp_sync_wait(TickType_t ticks);
