// Just enough JSON for one CHIRP-PROV config line: walk one object's members,
// decode strings, skip anything nested it does not need, refuse anything that
// is not strict JSON. Portable C++, no Arduino headers, no heap. Host-tested.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace warblet {

struct JsonValue {
  enum Type { kString, kNumber, kTrue, kFalse, kNull, kObject, kArray };
  Type type;
  const char* p;  // the raw text: inside the quotes for a string, the whole value otherwise
  size_t n;
};

// Walks the members of one object. Construct it on the text of an object
// (leading and trailing whitespace allowed), then call next() until it
// returns false; ok() then says whether the whole text was valid JSON.
class JsonObject {
 public:
  JsonObject(const char* text, size_t len);
  bool next(JsonValue& key, JsonValue& value);
  bool ok() const { return ok_; }

 private:
  const char* p_;
  const char* end_;
  bool first_ = true;
  bool done_ = false;
  bool ok_ = true;
};

// Decode a kString value (escapes, \u including surrogate pairs, to UTF-8)
// into out with a NUL. Returns the length, or -1 if it does not fit, or it
// holds a NUL character.
int jsonString(const JsonValue& v, char* out, size_t cap);

// True when key (a kString) is exactly name, escapes not allowed.
bool jsonKeyIs(const JsonValue& key, const char* name);

// Appends JSON text into a bounded buffer.
class JsonWriter {
 public:
  JsonWriter(char* out, size_t cap) : out_(out), cap_(cap) {
    if (cap_ > 0) out_[0] = '\0';
  }
  void raw(const char* s);
  void string(const char* s);           // quoted and escaped
  void member(const char* name);        // writes ,"name": as needed
  void beginObject() { raw("{"); first_ = true; }
  void endObject() { raw("}"); first_ = false; }
  void beginArray() { raw("["); first_ = true; }
  void endArray() { raw("]"); first_ = false; }
  void comma() { if (!first_) raw(","); first_ = false; }
  size_t done() const { return ok_ ? len_ : 0; }

 private:
  void put(char c);
  char* out_;
  size_t cap_;
  size_t len_ = 0;
  bool first_ = true;
  bool ok_ = true;
};

}  // namespace warblet
