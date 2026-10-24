// A stand-in for the Arduino core, for running src/Warblet.cpp and the
// example sketches on a PC. Time is simulated: delay() moves millis().
// This is NOT an Arduino core and proves nothing about compiling for a board.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>

#include <string>
#include <vector>

// ---- time -------------------------------------------------------------------
extern unsigned long simNowMs;
inline unsigned long millis() { return simNowMs; }
inline void delay(unsigned long ms) { simNowMs += ms; }
inline void delayMicroseconds(unsigned) {}

// ---- pins -------------------------------------------------------------------
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define LED_BUILTIN 13
#define A0 26
extern int simPin[64];
extern int simAnalog;
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int v) { simPin[pin] = v; }
inline int digitalRead(int pin) { return simPin[pin]; }
inline int analogRead(int) { return simAnalog; }
inline void analogReadResolution(int) {}

// ---- String -----------------------------------------------------------------
class String {
 public:
  String(const char* s = "") : s_(s ? s : "") {}
  String(const std::string& s) : s_(s) {}
  String(char c) : s_(1, c) {}
  String(int v) : s_(std::to_string(v)) {}
  String(long v) : s_(std::to_string(v)) {}
  String(unsigned v) : s_(std::to_string(v)) {}
  String(unsigned long v) : s_(std::to_string(v)) {}
  const char* c_str() const { return s_.c_str(); }
  unsigned length() const { return (unsigned)s_.size(); }
  char operator[](unsigned i) const { return i < s_.size() ? s_[i] : '\0'; }
  bool operator==(const char* o) const { return s_ == o; }
  bool operator==(const String& o) const { return s_ == o.s_; }
  bool operator!=(const String& o) const { return s_ != o.s_; }
  // As Arduino's: from..to, clamped to the end; an empty String past it.
  String substring(unsigned from) const { return from >= s_.size() ? String("") : String(s_.substr(from)); }
  String substring(unsigned from, unsigned to) const {
    if (to > s_.size()) to = (unsigned)s_.size();
    if (from >= to) return String("");
    return String(s_.substr(from, to - from));
  }
  int indexOf(const char* what) const {
    size_t at = s_.find(what);
    return at == std::string::npos ? -1 : (int)at;
  }
  int indexOf(char what) const {
    size_t at = s_.find(what);
    return at == std::string::npos ? -1 : (int)at;
  }
  bool startsWith(const char* what) const { return s_.compare(0, strlen(what), what) == 0; }
  void trim() {
    size_t a = 0, b = s_.size();
    while (a < b && isspace((unsigned char)s_[a])) a++;
    while (b > a && isspace((unsigned char)s_[b - 1])) b--;
    s_ = s_.substr(a, b - a);
  }
  String& operator+=(const String& o) {
    s_ += o.s_;
    return *this;
  }
  String& operator+=(const char* o) {
    s_ += o;
    return *this;
  }
  String& operator+=(char c) {
    s_ += c;
    return *this;
  }
  friend String operator+(const char* a, const String& b) { return String(std::string(a) + b.s_); }
  friend String operator+(const String& a, const char* b) { return String(a.s_ + b); }
  friend String operator+(const String& a, const String& b) { return String(a.s_ + b.s_); }

 private:
  std::string s_;
};

// ---- Serial -----------------------------------------------------------------
// The USB serial line. Bytes the setup page sends arrive over time (64 per
// millisecond, one USB full-speed packet per frame) into a receive buffer of
// rxCap bytes; what arrives while the buffer is full is lost, as the ESP32
// core's HWCDC driver drops it.
class SimSerial {
 public:
  void begin(unsigned long) { rxCapAtBegin = rxCap; }
  void flush() {}
  void setRxBufferSize(size_t n) { rxCap = n; }
  size_t rxCapAtBegin = 0;  // the receive buffer begin() started with (0: not begun)
  int available() {
    arrive();
    return (int)rx.size();
  }
  int read() {
    arrive();
    if (rx.empty()) return -1;
    unsigned char c = (unsigned char)rx[0];
    rx.erase(0, 1);
    return c;
  }
  // Queue bytes from the setup page, starting afterMs from now.
  void type(const std::string& s, unsigned long afterMs = 0) {
    for (size_t i = 0; i < s.size(); i++) wire.push_back(Byte{(unsigned long)(simNowMs + afterMs + i / 64), s[i]});
  }
  size_t rxCap = 256;  // the ESP32 core's default
  size_t dropped = 0;
  std::string rx;
  void print(const char* s) { out += s; }
  void print(const String& s) { out += s.c_str(); }
  void print(int v) { out += std::to_string(v); }
  void print(unsigned v) { out += std::to_string(v); }
  void println(const char* s) { out += s; out += "\r\n"; }
  void println(const String& s) { println(s.c_str()); }
  void println(int v) { out += std::to_string(v) + "\r\n"; }
  void println(unsigned v) { out += std::to_string(v) + "\r\n"; }
  std::string out;

 private:
  struct Byte {
    unsigned long at;
    char c;
  };
  void arrive() {
    size_t n = 0;
    while (n < wire.size() && wire[n].at <= simNowMs) {
      if (rx.size() < rxCap) {
        rx += wire[n].c;
      } else {
        dropped++;
      }
      n++;
    }
    wire.erase(wire.begin(), wire.begin() + (long)n);
  }
  std::vector<Byte> wire;
};
extern SimSerial Serial;
