// The ESP32 core's WiFi object, as far as src/WarbletBoard.cpp uses it.
// PC stand-in: it joins whatever it is given.
#pragma once

#define WIFI_STA 1
#define WL_CONNECTED 3
#define WL_DISCONNECTED 6

class SimWiFi {
 public:
  void mode(int) {}
  void begin(const char*, const char* = nullptr) { status_ = WL_CONNECTED; }
  int status() { return status_; }
  bool disconnect() {
    status_ = WL_DISCONNECTED;
    return true;
  }

 private:
  int status_ = WL_DISCONNECTED;
};
extern SimWiFi WiFi;
