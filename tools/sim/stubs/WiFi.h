#pragma once
#include "Arduino.h"
#define WL_CONNECTED 3
#define WL_DISCONNECTED 6
#define WIFI_STA 1
#define WIFI_SCAN_FAILED (-2)
#define WIFI_AUTH_OPEN 0
struct IPAddressStub { String toString() const { return "192.168.1.42"; } };
struct WiFiStub {
  bool up = true;
  int status() { return up ? WL_CONNECTED : WL_DISCONNECTED; }
  void mode(int) {} void disconnect() {} void begin(const char *, const char *) {} void setAutoReconnect(bool) {}
  IPAddressStub localIP() { return {}; }
  String macAddress() { return "24:6F:28:AB:12:34"; }
  String SSID() { return "WarungWiFi"; }
  String SSID(int i) { static const char *n[] = { "WarungWiFi", "Indihome-4F2A", "Laundry-Guest", "TP-Link_8890", "Rumah_Pak_Budi" }; return n[i % 5]; }
  int RSSI() { return -58; }
  int RSSI(int i) { return -45 - i * 9; }
  int encryptionType(int i) { return i == 2 ? 0 : 3; }
  int scanNetworks(bool) { return 0; }
  int scanComplete() { return 5; }
  void scanDelete() {}
};
extern WiFiStub WiFi;
