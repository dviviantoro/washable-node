#pragma once
// Host stand-in for the ESP32 Arduino core: just enough for esp32-kiosk/src/main.cpp to compile on a PC (screenshot renderer).
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <map>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define F(x) x
#define constrain(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define PROGMEM
extern uint32_t g_ms;
inline uint32_t millis() { return g_ms; }
inline void delay(uint32_t) {}
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 0; }
inline void ledcSetup(int, int, int) {}
inline void ledcAttachPin(int, int) {}
inline void ledcWrite(int, int) {}
inline size_t strlcpy(char *d, const char *s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l >= n ? n - 1 : l; memcpy(d, s, c); d[c] = 0; } return l; }

class String {
 public:
  std::string s;
  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(const std::string &x) : s(x) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  const char *c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  String substring(size_t a, size_t b = (size_t)-1) const { if (a > s.size()) a = s.size(); return String(s.substr(a, b == (size_t)-1 ? std::string::npos : b - a)); }
  int indexOf(char c, size_t from = 0) const { size_t p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  int lastIndexOf(const char *c) const { size_t p = s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
  bool startsWith(const char *c) const { return s.rfind(c, 0) == 0; }
  void replace(char a, char b) { for (auto &x : s) if (x == a) x = b; }
  int lastIndexOf(char c) const { size_t p = s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
  int toInt() const { return atoi(s.c_str()); }
  bool equals(const char *c) const { return s == c; }
  bool operator==(const char *c) const { return s == c; }
  bool operator!=(const char *c) const { return s != c; }
  bool operator==(const String &o) const { return s == o.s; }
  String &operator+=(const String &o) { s += o.s; return *this; }
  String &operator+=(const char *c) { s += c; return *this; }
  String &operator+=(char c) { s += c; return *this; }
  String &operator+=(int v) { s += std::to_string(v); return *this; }
  void concat(const String &o) { s += o.s; }
  void replace(const char *a, const char *b) { std::string f = a, r = b; size_t p = 0; while ((p = s.find(f, p)) != std::string::npos) { s.replace(p, f.size(), r); p += r.size(); } }
  void trim() { size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n"); s = a == std::string::npos ? "" : s.substr(a, b - a + 1); }
  void reserve(size_t) {}
  char operator[](size_t i) const { return s[i]; }
  operator const char *() const { return s.c_str(); }
};
inline String operator+(const String &a, const String &b) { return String(a.s + b.s); }
inline String operator+(const char *a, const String &b) { return String(std::string(a) + b.s); }
inline String operator+(const String &a, const char *b) { return String(a.s + b); }

struct SerialStub {
  void begin(int) {}
  int printf(const char *, ...) { return 0; }
  void println(const char *) {}
  int available() { return 0; }
  int read() { return -1; }
  size_t write(const uint8_t *, size_t n) { return n; }
};
extern SerialStub Serial;
inline void configTime(long, int, const char *, const char * = nullptr) {}
struct EspStub { uint32_t getFreeHeap() { return 168000; } uint32_t getMaxAllocHeap() { return 110000; } void restart() {} };
static EspStub ESP;
