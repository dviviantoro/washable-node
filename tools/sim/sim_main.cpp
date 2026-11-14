// Host renderer: compiles the REAL kiosk UI (esp32-kiosk/src/main.cpp) against stubs and writes each screen as raw RGB.
// Used by tools/sim/screenshots.sh to produce docs/img/screens/*.png. Not part of any firmware.
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <esp_ota_ops.h>
#include "version.h"
#include "ota.h"
#include "relay_link.h"

uint32_t g_ms = 100000;
SerialStub Serial;
TwoWireStub Wire;
WiFiStub WiFi;
MDNSStub MDNS;
SPIFFSStub SPIFFS;
std::map<std::string, std::string> g_nvs;
uint8_t g_fb[320 * 480 * 3];
OtaInfo ota;
RelayLink rl;

void otaBoot() {} void otaHealthTick(uint32_t) {} void otaConfigure(const char *) {} void otaTick(uint32_t, bool, bool) {}
bool otaUpdateReady() { return false; } void otaInstall() {} void otaAck() {} void otaForceCheck() {} bool otaInWindow() { return false; }
void otaStatusText(char *o, size_t n) { strlcpy(o, "terbaru", n); }
bool otaLastUpdate(uint32_t *e, char *v, size_t n) { *e = 1794000000UL; strlcpy(v, "1.1.0", n); return true; }
void rlBegin(const char *, const char *) {} void rlTick(uint32_t) {} void rlSetDebugHandler(void (*)(char)) {}
bool rlRelay(uint8_t, uint8_t, uint16_t) { return true; } bool rlAllOff() { return true; } bool rlStatus() { return true; }

#include "../../esp32-kiosk/src/main.cpp"

static void step(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 5) { g_ms += 5; loop(); } }
static void settle() { int guard = 0; while (busy && guard++ < 2000) step(5); }
static void shot(const char *dir, const char *name, uint32_t animMs = 900) {
  step(animMs);
  char p[256]; snprintf(p, sizeof p, "%s/%s.rgb", dir, name);
  FILE *f = fopen(p, "wb"); fwrite(g_fb, 1, sizeof g_fb, f); fclose(f);
  printf("shot %s\n", name);
}
static void go(Scr s) { showScreen(s); settle(); }

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  g_nvs["washable/owner"] = "Laundry Bersih Jaya";
  g_nvs["washable/ownerid"] = "OWN-0042";
  g_nvs["washable/mid"] = "03";
  g_nvs["washable/ssid"] = "WarungWiFi";
  g_nvs["washable/apiurl"] = "https://api.washable.id";
  g_nvs["washable/bright"] = "80";
  setup();
  settle();
  shot(dir, "idle", 1200);
  go(SC_SERVICE);  shot(dir, "service");
  svc = 0; pick = 1; go(SC_CYCLE); shot(dir, "cycle-wash");
  svc = 1; pick = 1; go(SC_CYCLE); shot(dir, "cycle-dry");
  svc = 0; pick = 1; go(SC_PAY);   shot(dir, "pay", 600);
  go(SC_SUCCESS); shot(dir, "success", 1000);
  goRun(); settle();
  jobs[0].startMs = g_ms - jobs[0].durMs * 45 / 100; step(1200);
  shot(dir, "run-wash", 300);
  svc = 1; pick = 1; jobStart(1, 1); jobs[1].startMs = g_ms - jobs[1].durMs * 30 / 100; showScreen(SC_RUN); settle();
  shot(dir, "run-dry", 1100);
  jobs[1].startMs = g_ms - jobs[1].durMs; step(300); settle();
  shot(dir, "done", 900);
  svc = 0; jobs[0].st = JS_RUN; jobs[0].opt = 1; jobs[0].startMs = g_ms - jobs[0].durMs * 60 / 100;
  jobStart(1, 0); jobs[1].startMs = g_ms - jobs[1].durMs * 20 / 100;
  go(SC_IDLE); shot(dir, "idle-running", 1500);
  go(SC_SERVICE); shot(dir, "service-busy", 1200);
  jobs[0].st = jobs[1].st = JS_FREE;
  go(SC_MASK); shot(dir, "member-ask");
  mDigits[0] = 0; strlcpy(mDigits, "81234567890", sizeof mDigits); go(SC_PHONE); shot(dir, "member-phone", 500);
  go(SC_CHECK); shot(dir, "member-check", 600);
  go(SC_OTP); shot(dir, "member-otp", 500);
  memberActive = true; go(SC_MHOME); shot(dir, "member-home");
  memberActive = false;
  go(SC_PIN); shot(dir, "pin", 500);
  go(SC_SETTINGS); shot(dir, "settings", 600);
  go(SC_WIFI); shot(dir, "wifi", 600);
  strlcpy(ota.latest, "1.2.0", sizeof ota.latest); ota.state = OTA_DOWNLOADING; ota.progress = 62;
  go(SC_OTA); shot(dir, "ota", 1200);
  return 0;
}
