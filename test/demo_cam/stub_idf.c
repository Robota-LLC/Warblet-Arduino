/* Stand-ins for the ESP-IDF calls the camera demo's chirp_send.c makes. Only
 * esp_http_client_set_header() does anything: it records each header. */
#include <string.h>

#include "chirp_store.h"
#include "idf_stub.h"
#include "wifi_ladder.h"

#define MAX_HEADERS 16
static char names[MAX_HEADERS][32];
static char values[MAX_HEADERS][256];
static int count;

void stub_headers_clear(void) { count = 0; }

const char *stub_header(const char *name)
{
    for (int i = count - 1; i >= 0; i--) {
        if (strcmp(names[i], name) == 0) return values[i];
    }
    return "";
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    (void)client;
    if (count < MAX_HEADERS) {
        strncpy(names[count], key, sizeof names[count] - 1);
        names[count][sizeof names[count] - 1] = '\0';
        strncpy(values[count], value, sizeof values[count] - 1);
        values[count][sizeof values[count] - 1] = '\0';
        count++;
    }
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int priority,
                       void *handle)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    return pdPASS;
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) { (void)config; return 0; }
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int n) { (void)c; (void)n; return ESP_FAIL; }
int esp_http_client_write(esp_http_client_handle_t c, const char *b, int n) { (void)c; (void)b; (void)n; return -1; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { (void)c; return -1; }
int esp_http_client_read_response(esp_http_client_handle_t c, char *b, int n) { (void)c; (void)b; (void)n; return -1; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return 0; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) { (void)config; return ESP_OK; }
esp_err_t esp_netif_sntp_sync_wait(TickType_t ticks) { (void)ticks; return ESP_OK; }
esp_err_t wifi_ladder_start(const chirp_config_t *config) { (void)config; return ESP_OK; }
bool wifi_ladder_wait_connected(TickType_t ticks) { (void)ticks; return false; }
const char *chirp_store_hwid(const chirp_config_t *config) { return config->hwid; }
