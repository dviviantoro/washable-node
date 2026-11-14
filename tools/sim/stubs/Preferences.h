#pragma once
#include "Arduino.h"
extern std::map<std::string, std::string> g_nvs;
class Preferences {
  std::string ns;
 public:
  bool begin(const char *n, bool = false) { ns = n; return true; }
  void end() {}
  bool isKey(const char *k) { return g_nvs.count(ns + "/" + k); }
  String getString(const char *k, const char *d = "") { auto i = g_nvs.find(ns + "/" + k); return String(i == g_nvs.end() ? d : i->second.c_str()); }
  void putString(const char *k, const char *v) { g_nvs[ns + "/" + k] = v; }
  bool getBool(const char *k, bool d = false) { auto i = g_nvs.find(ns + "/" + k); return i == g_nvs.end() ? d : i->second == "1"; }
  void putBool(const char *k, bool v) { g_nvs[ns + "/" + k] = v ? "1" : "0"; }
  uint8_t getUChar(const char *k, uint8_t d = 0) { auto i = g_nvs.find(ns + "/" + k); return i == g_nvs.end() ? d : (uint8_t)atoi(i->second.c_str()); }
  void putUChar(const char *k, uint8_t v) { g_nvs[ns + "/" + k] = std::to_string(v); }
  uint32_t getUInt(const char *k, uint32_t d = 0) { auto i = g_nvs.find(ns + "/" + k); return i == g_nvs.end() ? d : (uint32_t)atol(i->second.c_str()); }
  void putUInt(const char *k, uint32_t v) { g_nvs[ns + "/" + k] = std::to_string(v); }
  void clear() { g_nvs.clear(); }
};
