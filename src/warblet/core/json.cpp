// See json.h.
#include "json.h"

#include <string.h>

namespace warblet {

namespace {

const int kMaxDepth = 8;

void skipSpace(const char*& p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// p at the opening quote. On success p is after the closing quote and
// [s, s+n) is the raw content.
bool scanString(const char*& p, const char* end, const char*& s, size_t& n) {
  if (p >= end || *p != '"') return false;
  p++;
  s = p;
  while (p < end) {
    unsigned char c = (unsigned char)*p;
    if (c == '"') {
      n = (size_t)(p - s);
      p++;
      return true;
    }
    if (c < 0x20) return false;  // raw control characters are not JSON
    if (c == '\\') {
      p++;
      if (p >= end) return false;
      char e = *p;
      if (e == 'u') {
        for (int i = 1; i <= 4; i++) {
          if (p + i >= end || hexDigit(p[i]) < 0) return false;
        }
        p += 4;
      } else if (!strchr("\"\\/bfnrt", e) || e == '\0') {
        return false;
      }
    }
    p++;
  }
  return false;
}

bool scanNumber(const char*& p, const char* end) {
  const char* start = p;
  if (p < end && *p == '-') p++;
  if (p >= end) return false;
  if (*p == '0') {
    p++;
  } else if (*p >= '1' && *p <= '9') {
    while (p < end && *p >= '0' && *p <= '9') p++;
  } else {
    return false;
  }
  if (p < end && *p == '.') {
    p++;
    if (p >= end || *p < '0' || *p > '9') return false;
    while (p < end && *p >= '0' && *p <= '9') p++;
  }
  if (p < end && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < end && (*p == '+' || *p == '-')) p++;
    if (p >= end || *p < '0' || *p > '9') return false;
    while (p < end && *p >= '0' && *p <= '9') p++;
  }
  return p > start;
}

bool scanValue(const char*& p, const char* end, JsonValue& v, int depth);

bool scanContainer(const char*& p, const char* end, char close, int depth) {
  if (depth > kMaxDepth) return false;
  p++;  // the opening bracket
  skipSpace(p, end);
  if (p < end && *p == close) {
    p++;
    return true;
  }
  for (;;) {
    JsonValue v;
    if (close == '}') {
      const char* s;
      size_t n;
      if (!scanString(p, end, s, n)) return false;
      skipSpace(p, end);
      if (p >= end || *p != ':') return false;
      p++;
      skipSpace(p, end);
    }
    if (!scanValue(p, end, v, depth + 1)) return false;
    skipSpace(p, end);
    if (p >= end) return false;
    if (*p == close) {
      p++;
      return true;
    }
    if (*p != ',') return false;
    p++;
    skipSpace(p, end);
  }
}

bool scanWord(const char*& p, const char* end, const char* word) {
  size_t n = strlen(word);
  if ((size_t)(end - p) < n || strncmp(p, word, n) != 0) return false;
  p += n;
  return true;
}

bool scanValue(const char*& p, const char* end, JsonValue& v, int depth) {
  if (p >= end) return false;
  const char* start = p;
  switch (*p) {
    case '"': {
      const char* s;
      size_t n;
      if (!scanString(p, end, s, n)) return false;
      v.type = JsonValue::kString;
      v.p = s;
      v.n = n;
      return true;
    }
    case '{':
      v.type = JsonValue::kObject;
      if (!scanContainer(p, end, '}', depth)) return false;
      break;
    case '[':
      v.type = JsonValue::kArray;
      if (!scanContainer(p, end, ']', depth)) return false;
      break;
    case 't':
      v.type = JsonValue::kTrue;
      if (!scanWord(p, end, "true")) return false;
      break;
    case 'f':
      v.type = JsonValue::kFalse;
      if (!scanWord(p, end, "false")) return false;
      break;
    case 'n':
      v.type = JsonValue::kNull;
      if (!scanWord(p, end, "null")) return false;
      break;
    default:
      v.type = JsonValue::kNumber;
      if (!scanNumber(p, end)) return false;
      break;
  }
  v.p = start;
  v.n = (size_t)(p - start);
  return true;
}

void putUtf8(uint32_t cp, char* out, size_t& n) {
  if (cp < 0x80) {
    out[n++] = (char)cp;
  } else if (cp < 0x800) {
    out[n++] = (char)(0xc0 | (cp >> 6));
    out[n++] = (char)(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    out[n++] = (char)(0xe0 | (cp >> 12));
    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[n++] = (char)(0x80 | (cp & 0x3f));
  } else {
    out[n++] = (char)(0xf0 | (cp >> 18));
    out[n++] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[n++] = (char)(0x80 | (cp & 0x3f));
  }
}

uint32_t hex4(const char* p) {
  return (uint32_t)(hexDigit(p[0]) << 12 | hexDigit(p[1]) << 8 | hexDigit(p[2]) << 4 | hexDigit(p[3]));
}

}  // namespace

JsonObject::JsonObject(const char* text, size_t len) : p_(text), end_(text + len) {
  skipSpace(p_, end_);
  if (p_ >= end_ || *p_ != '{') {
    ok_ = false;
    done_ = true;
    return;
  }
  // Check the whole text first, so a caller never acts on half an object.
  const char* q = p_;
  JsonValue whole;
  if (!scanValue(q, end_, whole, 0)) {
    ok_ = false;
    done_ = true;
    return;
  }
  skipSpace(q, end_);
  if (q != end_) {  // junk, or a second object, after the first
    ok_ = false;
    done_ = true;
    return;
  }
  p_++;  // into the object
}

bool JsonObject::next(JsonValue& key, JsonValue& value) {
  if (done_) return false;
  skipSpace(p_, end_);
  if (*p_ == '}') {
    done_ = true;
    return false;
  }
  if (!first_) {
    p_++;  // the comma, already validated
    skipSpace(p_, end_);
  }
  first_ = false;
  const char* s;
  size_t n;
  scanString(p_, end_, s, n);
  key.type = JsonValue::kString;
  key.p = s;
  key.n = n;
  skipSpace(p_, end_);
  p_++;  // the colon
  skipSpace(p_, end_);
  scanValue(p_, end_, value, 1);
  skipSpace(p_, end_);
  return true;
}

int jsonString(const JsonValue& v, char* out, size_t cap) {
  if (v.type != JsonValue::kString || cap == 0) return -1;
  size_t n = 0;
  const char* p = v.p;
  const char* end = v.p + v.n;
  while (p < end) {
    if (n + 2 > cap) return -1;  // room for one more byte and the NUL
    char c = *p++;
    if (c != '\\') {
      out[n++] = c;
      continue;
    }
    char e = *p++;
    switch (e) {
      case 'b': out[n++] = '\b'; break;
      case 'f': out[n++] = '\f'; break;
      case 'n': out[n++] = '\n'; break;
      case 'r': out[n++] = '\r'; break;
      case 't': out[n++] = '\t'; break;
      case 'u': {
        uint32_t cp = hex4(p);
        p += 4;
        if (cp >= 0xd800 && cp <= 0xdbff && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
          uint32_t lo = hex4(p + 2);
          if (lo >= 0xdc00 && lo <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
            p += 6;
          }
        }
        if (cp == 0) return -1;  // a NUL would cut the setting short
        if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;  // a lone surrogate
        size_t need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (n + need + 1 > cap) return -1;
        putUtf8(cp, out, n);
        break;
      }
      default: out[n++] = e; break;  // \" \\ \/
    }
  }
  if (memchr(out, '\0', n) != nullptr) return -1;
  out[n] = '\0';
  return (int)n;
}

bool jsonKeyIs(const JsonValue& key, const char* name) {
  size_t n = strlen(name);
  return key.type == JsonValue::kString && key.n == n && strncmp(key.p, name, n) == 0;
}

void JsonWriter::put(char c) {
  if (len_ + 1 >= cap_) {
    ok_ = false;
    return;
  }
  out_[len_++] = c;
  out_[len_] = '\0';
}

void JsonWriter::raw(const char* s) {
  while (*s) put(*s++);
}

void JsonWriter::string(const char* s) {
  static const char H[] = "0123456789abcdef";
  put('"');
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      put('\\');
      put((char)c);
    } else if (c < 0x20) {
      put('\\'); put('u'); put('0'); put('0');
      put(H[c >> 4]);
      put(H[c & 0x0f]);
    } else {
      put((char)c);
    }
  }
  put('"');
}

void JsonWriter::member(const char* name) {
  comma();
  string(name);
  put(':');
}

}  // namespace warblet
