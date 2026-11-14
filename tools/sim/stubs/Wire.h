#pragma once
#include "Arduino.h"
struct TwoWireStub { void begin(int, int) {} void setClock(int) {} void beginTransmission(int) {} int endTransmission() { return 1; } void write(int) {} int requestFrom(int, int) { return 0; } int read() { return 0; } };
extern TwoWireStub Wire;
