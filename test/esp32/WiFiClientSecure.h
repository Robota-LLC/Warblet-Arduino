// The ESP32 core's TLS client, as far as src/WarbletBoard.cpp configures it.
// PC stand-in: it records its settings and connects to nothing.
#pragma once

#include <stdint.h>

#include "Client.h"

class WiFiClientSecure : public Client {
 public:
  void setCACert(const char* roots) { caCert = roots; }
  void setConnectionTimeout(uint32_t ms) { connectionTimeoutMs = ms; }
  void setHandshakeTimeout(unsigned long s) { handshakeTimeoutS = s; }
  int connect(const char*, uint16_t) override { return 0; }
  size_t write(const uint8_t*, size_t size) override { return size; }
  int available() override { return 0; }
  int read(uint8_t*, size_t) override { return 0; }
  uint8_t connected() override { return 0; }
  void stop() override {}
  const char* caCert = nullptr;
  uint32_t connectionTimeoutMs = 0;
  unsigned long handshakeTimeoutS = 0;
};
