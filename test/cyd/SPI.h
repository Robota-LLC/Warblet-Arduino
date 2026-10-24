// The ESP32 core's SPI class, as far as the desk display's screen.cpp uses
// it, with a screen chip behind it: it records each byte sent with the
// command/data line's level, the clock shape of each transaction, and
// answers reads as the chosen chip does. PC stand-in for test/cyd.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "Arduino.h"

#define HSPI 2
#define MSBFIRST 1
#define SPI_MODE0 0
#define SPI_MODE3 3

class SPISettings {
 public:
  SPISettings(uint32_t clock = 1000000, uint8_t order = MSBFIRST, uint8_t mode = SPI_MODE0)
      : clock(clock), order(order), mode(mode) {}
  uint32_t clock;
  uint8_t order;
  uint8_t mode;
};

// One byte on the wire: a command (command/data line low) or data.
struct SimByte {
  bool command;
  uint8_t value;
  uint32_t clock;
  uint8_t mode;
};

// The chip behind the bus: "ILI9341" answers 0xD3 (picked with 0xD9) with
// 00 93 41, "ST7789" answers 0xDA..0xDC with 85 85 52, "none" answers 0.
struct SimPanel {
  std::string chip = "ILI9341";
  std::vector<SimByte> wire;      // every byte written
  std::vector<SimByte> reads;     // the clock shape of every read
  uint8_t lastCommand = 0;
  uint8_t picked = 0;             // the index set with 0xD9
  int transactions = 0;           // open now
};
extern SimPanel simPanel;

class SPIClass {
 public:
  explicit SPIClass(int) {}
  void begin(int, int, int, int) {}
  void beginTransaction(const SPISettings& s) {
    settings_ = s;
    simPanel.transactions++;
  }
  void endTransaction() { simPanel.transactions--; }
  uint8_t transfer(uint8_t b) {
    bool command = simPin[2] == LOW;
    if (command) {
      record(true, b);
      simPanel.lastCommand = b;
      return 0;
    }
    if (afterCommand_ && simPanel.lastCommand >= 0xD3 && simPanel.lastCommand <= 0xDC && simPanel.lastCommand != 0xD9 &&
        b == 0) {
      simPanel.reads.push_back(SimByte{false, 0, settings_.clock, settings_.mode});
      return answer(simPanel.lastCommand);
    }
    record(false, b);
    if (simPanel.lastCommand == 0xD9) simPanel.picked = b;
    return 0;
  }
  void writeBytes(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; i++) transfer(data[i]);
  }
  void writePattern(const uint8_t* data, uint8_t size, uint32_t repeat) {
    for (uint32_t r = 0; r < repeat; r++) writeBytes(data, size);
  }

 private:
  void record(bool command, uint8_t b) {
    afterCommand_ = command;
    simPanel.wire.push_back(SimByte{command, b, settings_.clock, settings_.mode});
  }
  uint8_t answer(uint8_t code) {
    if (simPanel.chip == "ST7789") return code == 0xDA ? 0x85 : code == 0xDB ? 0x85 : code == 0xDC ? 0x52 : 0;
    if (simPanel.chip == "ILI9341" && code == 0xD3) {
      return simPanel.picked == 0x12 ? 0x93 : simPanel.picked == 0x13 ? 0x41 : 0x00;
    }
    return 0;
  }
  SPISettings settings_;
  bool afterCommand_ = false;
};
