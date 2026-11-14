#pragma once
#include "Arduino.h"
struct MDNSStub { bool begin(const char *) { return true; } void end() {} void addService(const char *, const char *, int) {} };
extern MDNSStub MDNS;
