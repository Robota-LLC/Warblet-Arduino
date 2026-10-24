// The ESP32 core's WiFi object, as far as the desk display uses it: the
// signal of the joined network. PC stand-in for test/cyd.
#pragma once

class SimSignal {
 public:
  int RSSI() { return rssi; }
  int rssi = -52;
};
extern SimSignal WiFi;

#include <stdint.h>

// The ESP32 core's ESP object, as far as the desk display uses it: the heap.
class SimEsp {
 public:
  uint32_t getFreeHeap() { return freeHeap; }
  uint32_t getMaxAllocHeap() { return largest; }
  uint32_t freeHeap = 172000, largest = 110000;
};
extern SimEsp ESP;
