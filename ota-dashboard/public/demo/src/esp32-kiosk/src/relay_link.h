#pragma once
/*
 * Serial link to the relay controller (STM32). Uses UART0 (GPIO1 TX / GPIO3 RX, the same lines as the USB console),
 * 115200 8N1, JSON lines with a CRC - see docs/relay-protocol.md.
 *
 * Because the console shares the lines:
 *   - log text printed with Serial.printf() is ignored by the controller (it only reacts to "{...}*CRC" frames);
 *   - serial debug keys are now typed as a line starting with '#', e.g.  "#h\n"  (see rlSetDebugHandler).
 */
#include <Arduino.h>

#ifndef RELAY_LINK_ENABLED
#define RELAY_LINK_ENABLED 1
#endif
#define RL_HB_MS        1000     // heartbeat period
#define RL_TIMEOUT_MS   3000     // no frame for this long = link down
#define RL_ACK_MS       300      // command retry interval
#define RL_RETRIES      3

struct RelayLink {
  bool up;               // a valid frame arrived within RL_TIMEOUT_MS
  bool helloOk;          // the controller answered "hello"
  uint8_t mask;          // relays as reported by the controller (bit0 = channel 1)
  char fw[16];           // controller firmware version
  char rst[8];           // why the controller last restarted (from the hello reply)
  uint32_t upSec;        // controller uptime (s); a smaller value than before = it rebooted
  uint32_t rxOk, rxBad, txFrames, cmdFail;
  char lastErr[24];      // last nack reason
};
extern RelayLink rl;

// ---- health check (both directions): ESP32 pings the STM32, the STM32 pings the ESP32, then the controller reports counters/sensors
enum RlHealthState : uint8_t { RH_IDLE, RH_PING, RH_SELFTEST, RH_QUERY, RH_DONE };
struct RlHealth {
  RlHealthState state;
  bool linkFail;                  // the controller was not reachable when the test started
  uint8_t pingN, pingSent, pingOk;          // ESP32 -> STM32
  uint32_t rttAvgUs, rttMaxUs;
  bool stGot;                     // STM32 -> ESP32 result arrived
  uint8_t stN, stOk;
  uint32_t stRttMs, stMaxMs;
  bool infoGot;                   // health report arrived
  uint32_t ctlRx, ctlTx, ctlBad;
  int vddMv, tempC;
  uint32_t startedMs, doneMs;
};
extern RlHealth rh;
void rlHealthStart();
inline bool rlHealthBusy() { return rh.state != RH_IDLE && rh.state != RH_DONE; }

void rlBegin(const char *machineId, const char *fwVersion);
void rlTick(uint32_t nowMs);                        // call every loop() iteration
void rlSetDebugHandler(void (*cb)(char));           // called for lines like "#x"
bool rlRelay(uint8_t onMask, uint8_t offMask, uint16_t ttlSec);   // switch channels (bit0 = ch1); ttl is required for on
bool rlAllOff();
bool rlStatus();
