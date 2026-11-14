// End-to-end host test: the real ESP32 link code (esp32-kiosk/src/relay_link.cpp) <-> the real STM32 controller core
// (stm32-relay/src/ctrl_core.h), connected through in-memory serial buffers with fault injection.
#include "Arduino.h"
#include "relay_link.h"
#include "ctrl_core.h"
#include <memory>

uint32_t g_ms = 0;
SerialStub Serial;

static std::string toCtrl;                        // bytes travelling ESP32 -> STM32 (split in lines when delivered)
static uint8_t relayState = 0;                    // what the "pins" of the STM32 show
static std::string ctrlTx;                        // STM32 -> ESP32
static bool dropEspToStm = false, dropStmToEsp = false;
static int dropNextEspFrames = 0;
static char dbgKey = 0;

static void ctrlTxFn(const char *f, size_t n) { ctrlTx.append(f, n); }
static void relayFn(uint8_t ch, bool on) { if (on) relayState |= (uint8_t)(1u << ch); else relayState &= (uint8_t)~(1u << ch); }
static CtrlHooks hooks = { ctrlTxFn, relayFn };
static std::unique_ptr<Controller> ctrl;
static void newController() { ctrl.reset(new Controller(hooks, "0.1.0")); ctrl->begin(g_ms); relayState = 0; }

static void deliver() {
  // ESP32 -> STM32: complete lines only
  size_t p;
  while ((p = Serial.tx.find('\n')) != std::string::npos) {
    std::string l = Serial.tx.substr(0, p);
    Serial.tx.erase(0, p + 1);
    bool isFrame = !l.empty() && l[0] == '{';
    if (isFrame && dropNextEspFrames > 0) { dropNextEspFrames--; continue; }
    if (dropEspToStm) continue;
    std::vector<char> b(l.begin(), l.end()); b.push_back(0);
    ctrl->onLine(b.data(), l.size(), g_ms);
  }
  // STM32 -> ESP32
  if (!dropStmToEsp) for (char c : ctrlTx) Serial.rx.push_back(c);
  ctrlTx.clear();
}
static void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) {
    g_ms += 10;
    ctrl->tick(g_ms);
    rlTick(g_ms);
    deliver();
  }
}

static int fails = 0;
static void check(const char *name, bool ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) fails++; }

int main() {
  newController();
  rlSetDebugHandler([](char c) { dbgKey = c; });
  rlBegin("03", "1.1.0");

  run(3000);
  check("link up + hello answered within 3 s", rl.up && rl.helloOk);
  check("controller firmware version learned", !strcmp(rl.fw, "0.1.0"));
  check("log text on the shared UART is ignored by the controller (no bad frames)", ctrl->badFrames() == 0);

  rlRelay(0x03, 0, 5);
  run(300);
  check("relay command: pins and reported mask = ch1+ch2", relayState == 3 && rl.mask == 3);
  run(5000);
  check("safety timer: relays switched off by the controller after ttl", relayState == 0 && rl.mask == 0);

  rlRelay(0x10, 0, 30);
  run(200);
  rlRelay(0, 0x10, 0);
  run(200);
  check("explicit off", relayState == 0);

  Serial.rx.push_back('#'); Serial.rx.push_back('h'); Serial.rx.push_back('\n');
  run(20);
  check("debug line '#h' reaches the handler", dbgKey == 'h');
  dbgKey = 0;
  for (char c : std::string("hello world, plain text\n")) Serial.rx.push_back(c);
  run(20);
  check("other text lines are ignored", dbgKey == 0);

  // retry: the first transmission of a command is lost
  dropNextEspFrames = 1;
  rlRelay(0x01, 0, 10);
  run(1000);
  check("command is retried after a lost frame", relayState == 1);
  uint32_t failsBefore = rl.cmdFail;
  rlAllOff();
  run(200);

  // controller silent (STM32 -> ESP32 lost): link goes down, then recovers and re-handshakes
  dropStmToEsp = true;
  run(4000);
  check("link down detected when the controller is silent", !rl.up && !rl.helloOk);
  dropStmToEsp = false;
  run(3000);
  check("link recovers and hello is repeated", rl.up && rl.helloOk);

  // ESP32 silent (e.g. frozen): the controller fails safe
  rlRelay(0x04, 0, 100);
  run(300);
  check("relay 3 on", relayState == 4);
  dropEspToStm = true;
  run(4000);
  check("controller failsafe: relay off after 3 s without heartbeat", relayState == 0);
  dropEspToStm = false;
  run(2000);
  check("link up again, controller reports mask 0", rl.up && rl.mask == 0);

  // controller reboot: uptime restarts -> ESP32 notices and re-handshakes
  rlRelay(0x01, 0, 100);
  run(300);
  run(5000);
  newController();                                // power cycle of the STM32: relays off, uptime counts from ~0
  run(4000);
  check("controller reboot: relays off and handshake repeated", relayState == 0 && rl.helloOk && rl.mask == 0);

  // too many commands in flight do not crash
  dropEspToStm = true;
  for (int i = 0; i < 20; i++) rlRelay(0x01, 0, 5);
  run(2000);
  dropEspToStm = false;
  run(2000);
  check("queue overflow handled, link still fine", rl.up && rl.helloOk);
  (void)failsBefore;

  printf("\nframes ok=%lu bad=%lu tx=%lu cmdFail=%lu\n", (unsigned long)rl.rxOk, (unsigned long)rl.rxBad, (unsigned long)rl.txFrames, (unsigned long)rl.cmdFail);
  printf("%s\n", fails ? "SOME FAILED" : "ALL PASSED");
  return fails ? 1 : 0;
}
