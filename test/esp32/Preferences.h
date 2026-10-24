// The ESP32 core's Preferences (NVS), kept in memory. PC stand-in.
#pragma once

#include <stddef.h>
#include <string.h>

#include <map>
#include <string>

class Preferences {
 public:
  static std::map<std::string, std::string>& store() {
    static std::map<std::string, std::string> s;
    return s;
  }
  bool begin(const char* name, bool readOnly) {
    ns_ = name;
    readOnly_ = readOnly;
    return true;
  }
  void end() {}
  bool isKey(const char* key) { return store().count(ns_ + "/" + key) != 0; }
  size_t getString(const char* key, char* value, size_t maxLen) {
    auto it = store().find(ns_ + "/" + key);
    if (it == store().end() || it->second.size() + 1 > maxLen) return 0;
    memcpy(value, it->second.c_str(), it->second.size() + 1);
    return it->second.size() + 1;  // as nvs_get_str: the length counts the NUL
  }
  size_t putString(const char* key, const char* value) {
    if (readOnly_) return 0;
    store()[ns_ + "/" + key] = value;
    return strlen(value);
  }
  bool remove(const char* key) { return !readOnly_ && store().erase(ns_ + "/" + key) == 1; }

 private:
  std::string ns_;
  bool readOnly_ = true;
};
