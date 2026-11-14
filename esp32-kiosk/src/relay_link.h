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
  uint32_t upSec;        // controller uptime (s); a smaller value than before = it rebooted
  uint32_t rxOk, rxBad, txFrames, cmdFail;
  char lastErr[24];      // last nack reason
};
extern RelayLink rl;

void rlBegin(const char *machineId, const char *fwVersion);
void rlTick(uint32_t nowMs);                        // call every loop() iteration
void rlSetDebugHandler(void (*cb)(char));           // called for lines like "#x"
bool rlRelay(uint8_t onMask, uint8_t offMask, uint16_t ttlSec);   // switch channels (bit0 = ch1); ttl is required for on
bool rlAllOff();
bool rlStatus();
