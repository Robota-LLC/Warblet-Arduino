// The ESP32 core's names that the ESP32 section of src/WarbletBoard.cpp uses,
// on top of the simulation's stand-in Arduino core (test/sim/Arduino.h), so
// that section can be compiled and driven on a PC (test/esp32/board_test.cpp).
// This is NOT the ESP32 core and proves nothing about compiling for a board.
#pragma once

#include "../sim/Arduino.h"

#define ESP_ARDUINO_VERSION_MAJOR 3

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef const char* esp_event_base_t;
typedef void (*esp_event_handler_t)(void* handler_args, esp_event_base_t base, int32_t id, void* data);

// FreeRTOS spinlocks: counted, so a test can see that every lock is let go.
struct portMUX_TYPE {
  int depth;
};
#define portMUX_INITIALIZER_UNLOCKED {0}
extern int simLocksHeld;
inline void simLockEnter(portMUX_TYPE* m) {
  m->depth++;
  simLocksHeld++;
}
inline void simLockExit(portMUX_TYPE* m) {
  m->depth--;
  simLocksHeld--;
}
#define portENTER_CRITICAL(m) simLockEnter(m)
#define portEXIT_CRITICAL(m) simLockExit(m)

struct SimEsp {
  uint32_t getFreeHeap() { return 155000; }
  void restart() { restarts++; }
  int restarts = 0;
};
extern SimEsp ESP;

void configTime(long gmtOffsetS, int daylightOffsetS, const char* server1, const char* server2);
