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

// The kiosk firmware now ships a FAKE demo QRIS (QRIS_PAYLOAD in esp32-kiosk/src/main.cpp). The preview still swaps the page's QR for the same
// fake payload, so that tools/dashboard-shots/sheets.py (which fails the run if any published frame decodes to anything else) keeps working
// even if someone puts a real merchant's QRIS into the firmware again.
#define lv_qrcode_update web_qr_update
#include "../../esp32-kiosk/src/main.cpp"
#include "../../esp32-kiosk/src/i18n.cpp"
#undef lv_qrcode_update
extern "C" lv_res_t lv_qrcode_update(lv_obj_t *qrcode, const void *data, uint32_t data_len);   // the real LVGL function (its header declaration was renamed above)
extern "C" lv_res_t web_qr_update(lv_obj_t *qr, const void *, uint32_t) {
  static const char FAKE_QRIS[] = "00020101021126440014ID.CO.QRIS.WWW0215ID00000000000000303UMI5204721153033605802ID5923WASHABLE DEMO - FAKE QR6007JAKARTA61051011063047860";
  return lv_qrcode_update(qr, FAKE_QRIS, (uint32_t)strlen(FAKE_QRIS));
}

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

// every kiosk page (the Scr enum in main.cpp) plus the states worth looking at; the order is what the dashboard shows
static const char *SCENES[] = {
  "Idle", "Idle menu", "Service list", "Wash options", "Dryer options", "Payment (QRIS)", "Payment done",
  "Wash running", "Dry running", "Cycle finished", "Idle, both running",
  "Member sign-in", "Member phone", "Member check", "Member code", "Member home",
  "Admin PIN", "Settings", "Wi-Fi setup", "Wi-Fi networks", "Text editor", "Relay test",
  "Device info 1", "Device info 2", "Device info 3", "Device info 4", "Web form",
  "Health: healthy", "Health: attention", "Health: failure", "Firmware update", "About",
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

static void demoController() {       // what the documentation screenshots show on the controller pages
  rl.up = true; rl.helloOk = true; strlcpy(rl.fw, "0.2.0", sizeof rl.fw); strlcpy(rl.rst, "power", sizeof rl.rst);
  rl.rxOk = 18234; rl.rxBad = 2; rl.cmdFail = 1; rl.upSec = 5421; rl.mask = 0x02;
}
static void demoHealth(int level) {  // 0 healthy, 1 attention (watchdog reset, lost pings), 2 failure (no controller)
  rh = {}; rh.state = RH_DONE; rh.pingN = 10; rh.pingSent = 10; rh.pingOk = level == 1 ? 8 : 10; rh.rttAvgUs = 11800; rh.rttMaxUs = 15000;
  rh.stGot = true; rh.stN = 10; rh.stOk = level == 1 ? 9 : 10; rh.stRttMs = 12; rh.stMaxMs = 15; rh.infoGot = true; rh.ctlRx = 4210; rh.ctlTx = 4388;
  rh.ctlBad = level == 1 ? 3 : 0; rl.rxBad = 0; strlcpy(rl.rst, level == 1 ? "iwdg" : "power", sizeof rl.rst);
  if (level == 2) { rh = {}; rh.state = RH_DONE; rh.linkFail = true; }
}

// jump to a screen and load the demo data the documentation screenshots use
EMSCRIPTEN_KEEPALIVE void sim_scene(int id) {
  if (!g_up) return;
  jobs[0].st = jobs[1].st = JS_FREE; memberActive = false; mDigits[0] = 0;
  switch (id) {
    case 0: go(SC_IDLE); break;
    case 1: go(SC_ABOUT); go(SC_IDLE); evtMenu(NULL); break;                       // the dropdown opens over the idle page
    case 2: go(SC_SERVICE); break;
    case 3: svc = 0; pick = 1; go(SC_CYCLE); break;
    case 4: svc = 1; pick = 1; go(SC_CYCLE); break;
    case 5: svc = 0; pick = 1; go(SC_PAY); break;
    case 6: svc = 0; pick = 1; go(SC_SUCCESS); break;
    case 7: svc = 0; pick = 1; jobStart(0, 1); jobs[0].startMs = g_ms - jobs[0].durMs * 45 / 100; go(SC_RUN); break;
    case 8: svc = 1; pick = 1; jobStart(1, 1); jobs[1].startMs = g_ms - jobs[1].durMs * 30 / 100; go(SC_RUN); break;
    case 9: svc = 0; jobs[0].st = JS_DONE; go(SC_DONE); break;
    case 10:
      svc = 0; pick = 1; jobStart(0, 1); jobs[0].startMs = g_ms - jobs[0].durMs * 60 / 100;
      jobStart(1, 0); jobs[1].startMs = g_ms - jobs[1].durMs * 20 / 100; go(SC_IDLE); break;
    case 11: go(SC_MASK); break;
    case 12: strlcpy(mDigits, "81234567890", sizeof mDigits); go(SC_PHONE); break;
    case 13: strlcpy(mDigits, "81234567890", sizeof mDigits); go(SC_CHECK); break;
    case 14: strlcpy(mDigits, "81234567890", sizeof mDigits); go(SC_OTP); break;
    case 15: memberActive = true; go(SC_MHOME); break;
    case 16: go(SC_PIN); break;
    case 17: go(SC_SETTINGS); break;
    case 18: go(SC_WIFI); break;
    case 19: go(SC_WIFI); evtScan(NULL); step(1500); break;
    case 20: go(SC_SETTINGS); openEdit("Pemilik", "Nama pemilik atau usaha", cfg.owner, sizeof cfg.owner, false, false, 0, SC_SETTINGS, onCfgSaved); settle(); break;
    case 21: go(SC_RTEST); break;
    case 22: case 23: case 24: case 25:
      demoController(); svc = 0; jobStart(0, 1); jobs[0].startMs = g_ms - jobs[0].durMs * 40 / 100;
      go(SC_INFO); step(400);
      { static const int off[4] = { 0, 330, 660, 2000 }; if (infoList) lv_obj_scroll_to_y(infoList, off[id - 22], LV_ANIM_OFF); }
      break;
    case 26: go(SC_WEB); break;
    case 27: demoHealth(0); go(SC_HEALTH); break;
    case 28: demoHealth(1); go(SC_HEALTH); break;
    case 29: demoHealth(2); go(SC_HEALTH); break;
    case 30: strlcpy(ota.latest, "9.9.9", sizeof ota.latest); ota.state = OTA_DOWNLOADING; ota.progress = 62; go(SC_OTA); break;
    case 31: go(SC_ABOUT); break;
    default: break;
  }
  step(900);
}

}  // extern "C"
