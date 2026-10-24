// ESP-IDF's log levels, as far as src/WarbletBoard.cpp uses them. The
// stand-in records which tags the library sets. PC only.
#pragma once

typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;

void esp_log_level_set(const char* tag, esp_log_level_t level);
