#pragma once
/*
 * washable relay link - shared protocol helpers (ESP32 firmware, STM32 firmware and host tests all include this file).
 * Plain C++, no Arduino/HAL dependencies. See docs/relay-protocol.md.
 *
 * Frame on the wire:  {json}*CCCC\n      CCCC = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) of the json bytes, upper-case hex.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace rp {

inline uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

// Validates "{...}*CCCC" (newline already stripped or still present). On success NUL-terminates the json part and
// returns it (same buffer); returns nullptr if it is not a frame or the CRC does not match.
inline char *check(char *line, size_t len) {
  while (len && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) len--;
  if (len < 8 || line[0] != '{' || line[len - 5] != '*') return nullptr;
  uint16_t got = 0;
  for (int i = 0; i < 4; i++) {
    char c = line[len - 4 + i];
    uint8_t v;
    if (c >= '0' && c <= '9') v = (uint8_t)(c - '0');
    else if (c >= 'A' && c <= 'F') v = (uint8_t)(c - 'A' + 10);
    else if (c >= 'a' && c <= 'f') v = (uint8_t)(c - 'a' + 10);
    else return nullptr;
    got = (uint16_t)((got << 4) | v);
  }
  size_t blen = len - 5;
  if (line[blen - 1] != '}') return nullptr;
  if (crc16((const uint8_t *)line, blen) != got) return nullptr;
  line[blen] = 0;
  return line;
}

// Pointer to the value after  "key":  or nullptr.
inline const char *find(const char *body, const char *key) {
  char pat[24];
  snprintf(pat, sizeof pat, "\"%s\":", key);
  const char *p = strstr(body, pat);
  if (!p) return nullptr;
  p += strlen(pat);
  while (*p == ' ') p++;
  return p;
}
inline bool num(const char *body, const char *key, long *out) {
  const char *p = find(body, key);
  if (!p || !((*p >= '0' && *p <= '9') || *p == '-')) return false;
  *out = strtol(p, nullptr, 10);
  return true;
}
inline bool str(const char *body, const char *key, char *out, size_t cap) {
  const char *p = find(body, key);
  if (!p || *p != '"') return false;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
  out[i] = 0;
  return *p == '"';
}
// Channel list "[1,2,5]" -> bit mask (channel 1 = bit 0). Returns 0 = key missing, 1 = ok, -1 = invalid channel/syntax.
inline int list(const char *body, const char *key, uint8_t *mask) {
  const char *p = find(body, key);
  if (!p) return 0;
  if (*p != '[') return -1;
  p++;
  uint8_t m = 0;
  while (*p && *p != ']') {
    if (*p == ',' || *p == ' ') { p++; continue; }
    if (*p < '0' || *p > '9') return -1;
    long ch = strtol(p, (char **)&p, 10);
    if (ch < 1 || ch > 8) return -1;
    m |= (uint8_t)(1u << (ch - 1));
  }
  if (*p != ']') return -1;
  *mask = m;
  return 1;
}
// bit mask -> "1,2,5"
inline void maskList(char *out, size_t cap, uint8_t m) {
  size_t n = 0;
  out[0] = 0;
  for (int i = 0; i < 8; i++)
    if (m & (1u << i)) n += snprintf(out + n, cap - n, "%s%d", n ? "," : "", i + 1);
}
// json -> "json*CCCC\n". Returns the length written (0 if it does not fit).
inline size_t frame(char *out, size_t cap, const char *json) {
  size_t n = strlen(json);
  if (n + 7 > cap) return 0;
  memcpy(out, json, n);
  snprintf(out + n, 7, "*%04X\n", crc16((const uint8_t *)json, n));
  return n + 6;
}

}  // namespace rp
