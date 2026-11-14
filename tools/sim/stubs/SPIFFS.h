#pragma once
#include "Arduino.h"
#define FILE_WRITE "w"
#define FILE_READ "r"
struct File { operator bool() const { return false; } String readString() { return ""; } void print(const String &) {} void close() {} };
struct SPIFFSStub { bool begin(bool) { return false; } bool exists(const char *) { return false; } File open(const char *, const char *) { return {}; } bool remove(const char *) { return true; } bool rename(const char *, const char *) { return true; } };
extern SPIFFSStub SPIFFS;
