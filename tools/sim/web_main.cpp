// Browser build of the kiosk simulator: the REAL kiosk UI (esp32-kiosk/src/main.cpp) + LVGL compiled to WebAssembly
// by the OTA dashboard (ota-server/scripts/preview-build.sh). Same stubs as sim_main.cpp (no hardware); the page drives it:
//   sim_init(lang, theme)  boot the kiosk      sim_frame(dtMs)  run the UI     sim_pixels()  RGBA framebuffer 320x480
//   sim_touch(x, y, down)  mouse / finger      sim_scene(n)     jump to a screen with demo data (names: sim_scene_names())
// Not part of any firmware.
#include <emscripten.h>
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
OtaTelemetry otaTel;
RelayLink rl;
RlHealth rh;

void otaBoot() {} void otaHealthTick(uint32_t) {} void otaConfigure(const char *) {} void otaTick(uint32_t, bool, bool) {}
bool otaUpdateReady() { return false; } void otaInstall() {} void otaAck() {} void otaForceCheck() {} bool otaInWindow() { return false; }
void otaStatusText(char *o, size_t n) { strlcpy(o, "terbaru", n); }
bool otaLastUpdate(uint32_t *e, char *v, size_t n) { *e = 1794000000UL; strlcpy(v, "1.1.0", n); return true; }
void rlBegin(const char *, const char *) {} void rlTick(uint32_t) {} void rlSetDebugHandler(void (*)(char)) {}
static uint8_t g_cmdOn = 0; static int g_allOff = 0;
bool rlRelay(uint8_t on, uint8_t, uint16_t) { g_cmdOn = on; return true; } void rlHealthStart() {}
bool rlAllOff() { g_cmdOn = 0; g_allOff++; return true; } bool rlStatus() { return true; }

#include "../../esp32-kiosk/src/main.cpp"
#include "../../esp32-kiosk/src/i18n.cpp"

// ---- pointer input: a second LVGL pointer device fed by the page (the kiosk's own touch driver reads nothing here)
static int g_tx = 0, g_ty = 0, g_td = 0;
static lv_indev_drv_t webIndev;
static void webTouchRead(lv_indev_drv_t *, lv_indev_data_t *d) {
  d->point.x = g_tx; d->point.y = g_ty;
  d->state = g_td ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

static uint32_t g_rgba[320 * 480];
static bool g_up = false;

// the emulated controller always answers and follows the last relay command
static void controllerTick() { rl.up = true; rl.helloOk = true; rl.mask = g_cmdOn; strlcpy(rl.fw, "0.2.0", sizeof rl.fw); }
static void step(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 5) { g_ms += 5; controllerTick(); loop(); } }
static void settle() { int guard = 0; while (busy && guard++ < 2000) step(5); }
static void go(Scr s) { showScreen(s); settle(); }

static const char *SCENES[] = {
  "Idle", "Service list", "Wash options", "Dryer options", "Payment (QRIS)", "Payment done", "Wash running", "Dry running", "Cycle finished",
  "Idle, both running", "Member sign-in", "Settings", "Wi-Fi setup", "Relay test", "Device info", "Controller health", "Firmware update", "About",
};
static const int NSCENES = sizeof SCENES / sizeof *SCENES;

extern "C" {

EMSCRIPTEN_KEEPALIVE void sim_init(int lang, int theme) {
  g_nvs.clear();
  g_nvs["washable/owner"] = "Laundry Bersih Jaya";
  g_nvs["washable/ownerid"] = "OWN-0042";
  g_nvs["washable/mid"] = "03";
  g_nvs["washable/ssid"] = "WarungWiFi";
  g_nvs["washable/apiurl"] = "https://api.washable.id";
  g_nvs["washable/bright"] = "80";
  g_nvs["washable/lang"] = lang ? "1" : "0";
  g_nvs["washable/theme"] = theme ? "1" : "0";
  controllerTick();
  setup();
  settle();
  lv_indev_drv_init(&webIndev);
  webIndev.type = LV_INDEV_TYPE_POINTER;
  webIndev.read_cb = webTouchRead;
  lv_indev_drv_register(&webIndev);
  g_up = true;
}

// advance the kiosk by dtMs of wall-clock time
EMSCRIPTEN_KEEPALIVE void sim_frame(int dtMs) {
  if (!g_up) return;
  if (dtMs < 1) dtMs = 1;
  if (dtMs > 100) dtMs = 100;
  step((uint32_t)dtMs < 5 ? 5 : (uint32_t)dtMs);
}

EMSCRIPTEN_KEEPALIVE const uint32_t *sim_pixels() {
  for (int i = 0; i < 320 * 480; i++) {
    const uint8_t *s = g_fb + i * 3;
    g_rgba[i] = 0xFF000000u | ((uint32_t)s[2] << 16) | ((uint32_t)s[1] << 8) | s[0];   // little-endian RGBA
  }
  return g_rgba;
}

EMSCRIPTEN_KEEPALIVE void sim_touch(int x, int y, int down) { g_tx = x; g_ty = y; g_td = down; }
EMSCRIPTEN_KEEPALIVE int sim_screen() { return (int)curScr; }
EMSCRIPTEN_KEEPALIVE int sim_relays() { return g_cmdOn; }
EMSCRIPTEN_KEEPALIVE int sim_scene_count() { return NSCENES; }
EMSCRIPTEN_KEEPALIVE const char *sim_scene_name(int i) { return i >= 0 && i < NSCENES ? SCENES[i] : ""; }
EMSCRIPTEN_KEEPALIVE const char *sim_fw_version() { return FW_VERSION; }

// fast-forward every running cycle by `sec` seconds (a real wash takes tens of minutes)
EMSCRIPTEN_KEEPALIVE void sim_skip(int sec) {
  for (int k = 0; k < 2; k++) if (jobs[k].st == JS_RUN) jobs[k].startMs -= (uint32_t)sec * 1000u;
  step(300);
}

// jump to a screen and load the demo data the documentation screenshots use
EMSCRIPTEN_KEEPALIVE void sim_scene(int id) {
  if (!g_up) return;
  jobs[0].st = jobs[1].st = JS_FREE; memberActive = false;
  switch (id) {
    case 0: go(SC_IDLE); break;
    case 1: go(SC_SERVICE); break;
    case 2: svc = 0; pick = 1; go(SC_CYCLE); break;
    case 3: svc = 1; pick = 1; go(SC_CYCLE); break;
    case 4: svc = 0; pick = 1; go(SC_PAY); break;
    case 5: svc = 0; pick = 1; go(SC_SUCCESS); break;
    case 6: svc = 0; pick = 1; jobStart(0, 1); jobs[0].startMs = g_ms - jobs[0].durMs * 45 / 100; go(SC_RUN); break;
    case 7: svc = 1; pick = 1; jobStart(1, 1); jobs[1].startMs = g_ms - jobs[1].durMs * 30 / 100; svc = 1; go(SC_RUN); break;
    case 8: svc = 0; jobs[0].st = JS_DONE; go(SC_DONE); break;
    case 9:
      svc = 0; pick = 1; jobStart(0, 1); jobs[0].startMs = g_ms - jobs[0].durMs * 60 / 100;
      jobStart(1, 0); jobs[1].startMs = g_ms - jobs[1].durMs * 20 / 100; go(SC_IDLE); break;
    case 10: go(SC_MASK); break;
    case 11: go(SC_SETTINGS); break;
    case 12: go(SC_WIFI); break;
    case 13: go(SC_RTEST); break;
    case 14: go(SC_INFO); break;
    case 15:
      rh = {}; rh.state = RH_DONE; rh.pingN = 10; rh.pingSent = 10; rh.pingOk = 10; rh.rttAvgUs = 11800; rh.rttMaxUs = 15000; rh.stGot = true; rh.stN = 10; rh.stOk = 10;
      rh.stRttMs = 12; rh.stMaxMs = 15; rh.infoGot = true; rh.ctlRx = 4210; rh.ctlTx = 4388; go(SC_HEALTH); break;
    case 16: strlcpy(ota.latest, "9.9.9", sizeof ota.latest); ota.state = OTA_DOWNLOADING; ota.progress = 62; go(SC_OTA); break;
    case 17: go(SC_ABOUT); break;
    default: break;
  }
  step(900);
}

}  // extern "C"
