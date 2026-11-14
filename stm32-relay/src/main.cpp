/*
 * washable relay controller - STM32G030F6P6 (WeAct Studio core board)
 *
 * Talks JSON lines to the kiosk (ESP32) on USART2 (PA2 TX, PA3 RX, 115200 8N1) and drives 8 relays:
 *   channels 1-4 = wash machine, channels 5-8 = dryer (the roles of each channel are decided by the ESP32).
 * Protocol: docs/relay-protocol.md. Protocol logic: src/ctrl_core.h (host-tested).
 *
 * Wiring: ESP32 GPIO1 (TX0) -> PA3, ESP32 GPIO3 (RX0) <- PA2, common GND, both 3.3 V.
 */
#include <Arduino.h>
#include <IWatchdog.h>
#include "ctrl_core.h"

#define FW_VERSION          "0.1.0"
#define RELAY_ACTIVE_HIGH   1                      // 0 for relay boards that switch on with a LOW input
#define LED_PIN             PNUM_NOT_DEFINED       // optional status LED pin (blinks while the link is alive)

// Relay outputs, channel 1..8. Check them against the pinout of your board before connecting loads.
static const uint32_t RELAY_PIN[CTRL_RELAYS] = { PA0, PA1, PA4, PA5, PA6, PA7, PB0, PB1 };

static void txFrame(const char *f, size_t n) { Serial.write((const uint8_t *)f, n); }
static void relayWrite(uint8_t ch, bool on) {
  digitalWrite(RELAY_PIN[ch], (on == (RELAY_ACTIVE_HIGH != 0)) ? HIGH : LOW);
}

static CtrlHooks hooks = { txFrame, relayWrite };
static Controller ctrl(hooks, FW_VERSION);

static char line[256];
static size_t lineLen = 0;
static bool lineOverflow = false;

void setup() {
  // relays first: make sure they are off before anything else happens
  for (uint8_t i = 0; i < CTRL_RELAYS; i++) {
    digitalWrite(RELAY_PIN[i], RELAY_ACTIVE_HIGH ? LOW : HIGH);
    pinMode(RELAY_PIN[i], OUTPUT);
  }
  Serial.begin(115200);                            // USART2 on PA3 (RX) / PA2 (TX) in this variant
  ctrl.begin(millis());
  IWatchdog.begin(2000000);                        // 2 s independent watchdog: a hung MCU resets and relays go off
}

void loop() {
  IWatchdog.reload();
  uint32_t now = millis();
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (!lineOverflow && lineLen) { line[lineLen] = 0; ctrl.onLine(line, lineLen, now); }
      lineLen = 0;
      lineOverflow = false;
    } else if (lineLen < sizeof line - 1) {
      line[lineLen++] = c;
    } else {
      lineOverflow = true;
    }
  }
  ctrl.tick(now);
}
