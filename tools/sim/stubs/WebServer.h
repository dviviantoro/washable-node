#pragma once
#include "Arduino.h"
enum HTTPMethod { HTTP_GET, HTTP_POST };
#define BASIC_AUTH 0
struct WebServer {
  WebServer(int) {}
  void begin() {} void stop() {} void handleClient() {}
  void on(const char *, HTTPMethod, void (*)()) {}
  void collectHeaders(const char **, size_t) {}
  bool hasArg(const char *) { return false; }
  String arg(const char *) { return ""; }
  bool hasHeader(const char *) { return false; }
  bool authenticate(const char *, const char *) { return true; }
  void requestAuthentication(int, const char *, const char *) {}
  void sendHeader(const char *, const char *) {}
  void send(int) {}
  void send(int, const char *, const String &) {}
};
