#pragma once
// Minimal host stub of the ESP32 Arduino API used by esp32-kiosk/src/relay_link.cpp (for tests/integration/link_test.cpp only).
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <deque>
extern uint32_t g_ms;
inline uint32_t millis() { return g_ms; }
inline uint32_t micros() { return g_ms * 1000u; }
struct SerialStub {
  std::string tx;                 // everything the ESP32 writes (frames + log text), like the real UART0 TX
  std::deque<char> rx;            // bytes the ESP32 receives
  size_t write(const uint8_t *b, size_t n) { tx.append((const char *)b, n); return n; }
  int available() { return (int)rx.size(); }
  int read() { if (rx.empty()) return -1; int c = (unsigned char)rx.front(); rx.pop_front(); return c; }
  void println(const char *s) { tx.append(s); tx.append("\n"); }
  int printf(const char *fmt, ...) { char b[256]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b, fmt, a); va_end(a); tx.append(b); return n; }
};
extern SerialStub Serial;
