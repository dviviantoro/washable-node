/*
 * washable - self-service laundry kiosk UI (ESP32-3248S035, 320x480 portrait, ST7796 + GT911 touch)
 *
 * Flow (same as the "Washable" design):
 *   Idle -> Choose service (Cuci / Keringkan) -> Choose cycle -> Pay with QRIS (demo: simulated)
 *   -> Payment success -> Washing / Drying in progress -> Cycle done
 *
 * Performance notes: screens are built on entry and deleted on exit (only the visible screen
 * animates), page changes are a backlight fade (no full-screen redraw), and all motion uses
 * small moving objects instead of large rotating/transparent ones.
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include "version.h"
#include "ota.h"
#include "relay_link.h"
#include <lvgl.h>
#include <TFT_eSPI.h>
#include "i18n.h"
// Every label text and every snprintf format goes through tr(): Indonesian source strings are shown in English when the language is EN.
#define lv_label_set_text(o, t) lv_label_set_text((o), tr(t))
#define snprintf(b, n, fmt, ...) snprintf((b), (n), tr(fmt), ##__VA_ARGS__)

extern "C" {
LV_FONT_DECLARE(f_fredoka_20)
LV_FONT_DECLARE(f_fredoka_28)
LV_FONT_DECLARE(f_fredoka_36)
LV_FONT_DECLARE(f_fredoka_48)
LV_FONT_DECLARE(f_fredoka_64)
LV_FONT_DECLARE(f_nunito_14)
LV_FONT_DECLARE(f_nunito_16)
LV_FONT_DECLARE(f_nunito_20)
}

// ============================== CONFIG ==============================
#ifndef DEBUG_KEYS
#define DEBUG_KEYS          0        // serial "#x" debug commands: off in production; enable locally with -DDEBUG_KEYS=1
#endif
#define WDT_SECONDS         20       // task watchdog on the main loop: a hung UI/loop restarts the kiosk
#define PAY_SEC             300
#define SUCCESS_AUTO_MS     12000UL
#define DONE_AUTO_MS        30000UL
#define IDLE_TIMEOUT_MS     60000UL
#define ADMIN_TIMEOUT_MS    180000UL

#define RELAY_WASH_PIN      -1       // optional relays (GPIO 21/25/32/33 belong to touch)
#define RELAY_DRY_PIN       -1
#define RELAY_ON            HIGH
#define RELAY_OFF           LOW

// GT911 capacitive touch
#define GT_SDA              33
#define GT_SCL              32
#define GT_INT              21
#define GT_RST              25
#define GT_ADDR             0x5D
#define TOUCH_SWAP_XY       0
#define TOUCH_INVERT_X      0
#define TOUCH_INVERT_Y      0

#define SCREEN_W            320
#define SCREEN_H            480
#define BL_PIN              27
#define BL_CH               0

// Demo QRIS payload (decoded from the supplied sample image), drawn as a crisp vector QR
static const char QRIS_PAYLOAD[] =
  "00020101021126590013ID.CO.BNI.WWW011893600009150418341802096015347460303URE5144"
  "0014ID.CO.QRIS.WWW0215ID10254102550180303URE5204866153033605802ID5925PEMBANGUNAN "
  "GEREJA GKPI P6015TAPANULI TENGAH61052261162070703A016304ECBA";
#define QR_SIZE             183      // 61 modules x 3 px

// ============================== PALETTE ==============================
#define C_NAVY    lv_color_hex(0x0B2545)
static bool g_noStroke = false;                                // set while drawing the logo
static bool g_light = false;                                  // light theme (cfg.theme)
#define C_NAVY2   (g_light ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x13315C))   // card
#define C_NAVY3   (g_light ? lv_color_hex(0xCFDDED) : lv_color_hex(0x1B3F73))   // field / inactive
#define C_TXT     (g_light ? lv_color_hex(0x0B2545) : lv_color_white())          // main text
#define C_TEAL    lv_color_hex(0x00B4C5)
#define C_BLUE    lv_color_hex(0x3FA9F5)
#define C_ORANGE  lv_color_hex(0xFF9F43)
#define C_MINT    lv_color_hex(0x2ECC71)
#define C_LIGHT   (g_light ? lv_color_hex(0x35506F) : lv_color_hex(0xBFD3E6))
#define C_MUTED   (g_light ? lv_color_hex(0x5E7893) : lv_color_hex(0x8DA9C4))
#define C_WHITE   lv_color_white()

// ============================== DATA ==============================
struct Opt { const char *name, *detail, *price; uint32_t amount; uint16_t temp, minutes; uint8_t lvl; };
static const Opt WASH_OPTS[3] = {
  { "Hemat",    "30° · 35 menit", "Rp 12.000", 12000, 30, 35, 40 },
  { "Reguler",  "40° · 45 menit", "Rp 15.000", 15000, 40, 45, 62 },
  { "Intensif", "60° · 60 menit", "Rp 20.000", 20000, 60, 60, 90 },
};
static const Opt DRY_OPTS[3] = {
  { "Hangat",       "30 menit", "Rp 10.000", 10000, 45, 30, 45 },
  { "Standar",      "45 menit", "Rp 14.000", 14000, 60, 45, 70 },
  { "Kering total", "60 menit", "Rp 18.000", 18000, 75, 60, 92 },
};

enum Scr { SC_IDLE, SC_SERVICE, SC_CYCLE, SC_PAY, SC_SUCCESS, SC_RUN, SC_DONE, SC_PIN, SC_SETTINGS, SC_WIFI, SC_EDIT, SC_OTA, SC_MASK, SC_PHONE, SC_CHECK, SC_OTP, SC_MHOME, SC_RTEST, SC_INFO, SC_WEB, SC_HEALTH, SC_ABOUT, SC_COUNT };

static int svc = 0;          // 0 = wash, 1 = dry
static int pick = 1;         // selected option
static Scr curScr = SC_IDLE;
static uint32_t lastActMs = 0;
static bool busy = false;     // a screen swap is in progress

static const Opt *curOpt() { return (svc ? DRY_OPTS : WASH_OPTS) + pick; }
static lv_color_t accentCol() { return svc ? C_ORANGE : C_BLUE; }

// ---- background jobs: one wash machine + one dry machine run independently of whatever screen is showing
enum JobState : uint8_t { JS_FREE, JS_RUN, JS_DONE };      // JS_DONE = finished, clothes not collected yet
struct Job { JobState st; uint8_t opt; uint32_t startMs, durMs, doneMs; };
static Job jobs[2];                                         // [0] wash, [1] dry
static bool anyJobRunning() { return jobs[0].st == JS_RUN || jobs[1].st == JS_RUN; }

// ============================== SETTINGS (persisted in NVS) ==============================
struct Cfg {
  char owner[32], ownerId[32], machineId[8], ssid[33], pass[65], apiUrl[96], apiKey[64], pin[8], otaUrl[160];
  uint8_t bright;     // 20..100 %
  uint8_t lang;       // 0 = Indonesian, 1 = English
  uint8_t theme;      // 0 = dark, 1 = light
  bool demo;          // true = 1 second per "minute"
};
static Cfg cfg;
static Preferences prefs;
static char machineName[24] = "Mesin 03";
static int blMax = 255;
static bool spiffsOk = false;

// All text settings in one table: used by NVS load/save and by the flash mirror (/cfg.bak).
struct CfgStr { const char *key; char *p; size_t cap; const char *def; };
static const CfgStr CFG_STR[] = {
  { "owner",   cfg.owner,     sizeof cfg.owner,     ""     },
  { "ownerid", cfg.ownerId,   sizeof cfg.ownerId,   ""     },
  { "mid",     cfg.machineId, sizeof cfg.machineId, "03"   },
  { "ssid",    cfg.ssid,      sizeof cfg.ssid,      ""     },
  { "pass",    cfg.pass,      sizeof cfg.pass,      ""     },
  { "apiurl",  cfg.apiUrl,    sizeof cfg.apiUrl,    ""     },
  { "apikey",  cfg.apiKey,    sizeof cfg.apiKey,    ""     },
  { "pin",     cfg.pin,       sizeof cfg.pin,       "123456" },
  { "otaurl",  cfg.otaUrl,    sizeof cfg.otaUrl,    ""     },
};

static void updateMachineName() { snprintf(machineName, sizeof machineName, "Mesin %s", cfg.machineId); }
static void applyBright() {
  if (cfg.bright < 20) cfg.bright = 20;
  if (cfg.bright > 100) cfg.bright = 100;
  blMax = 51 + ((int)cfg.bright - 20) * (255 - 51) / 80;
}
static const char *otaUrlEff() { return cfg.otaUrl[0] ? cfg.otaUrl : OTA_DEFAULT_URL; }

static uint32_t crc32buf(const uint8_t *d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
  }
  return ~c;
}

// Flash mirror of the settings (text "key=value" lines + CRC) in the SPIFFS partition. It survives OTA like NVS does
// and restores the settings if NVS is ever empty/corrupt. Layout-independent, so later firmware versions can read it.
static void backupWrite() {
  if (!spiffsOk) return;
  String s;
  for (const CfgStr &e : CFG_STR) {
    String v = e.p;
    v.replace('\n', ' ');
    v.replace('\r', ' ');
    s += e.key; s += '='; s += v; s += '\n';
  }
  s += "bright=" + String(cfg.bright) + "\n";
  s += "demo=" + String(cfg.demo ? 1 : 0) + "\n";
  s += "lang=" + String((int)cfg.lang) + "\n";
  s += "theme=" + String((int)cfg.theme) + "\n";
  char crc[24];
  snprintf(crc, sizeof crc, "crc=%08x\n", (unsigned)crc32buf((const uint8_t *)s.c_str(), s.length()));
  File f = SPIFFS.open("/cfg.tmp", FILE_WRITE);
  if (!f) return;
  f.print(s);
  f.print(crc);
  f.close();
  SPIFFS.remove("/cfg.bak");
  SPIFFS.rename("/cfg.tmp", "/cfg.bak");
}
static bool backupRestore() {
  if (!spiffsOk || !SPIFFS.exists("/cfg.bak")) return false;
  File f = SPIFFS.open("/cfg.bak", FILE_READ);
  if (!f) return false;
  String all = f.readString();
  f.close();
  int ci = all.lastIndexOf("crc=");
  if (ci < 0) return false;
  String body = all.substring(0, ci);
  if (crc32buf((const uint8_t *)body.c_str(), body.length()) != (uint32_t)strtoul(all.c_str() + ci + 4, NULL, 16)) return false;
  int pos = 0;
  while (pos < (int)body.length()) {
    int e = body.indexOf('\n', pos);
    if (e < 0) break;
    String line = body.substring(pos, e);
    pos = e + 1;
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String k = line.substring(0, eq), v = line.substring(eq + 1);
    for (const CfgStr &c : CFG_STR) if (k == c.key) strlcpy(c.p, v.c_str(), c.cap);
    if (k == "bright") cfg.bright = (uint8_t)v.toInt();
    if (k == "demo") cfg.demo = v.toInt() != 0;
    if (k == "lang") cfg.lang = v.toInt() == 1 ? 1 : 0;
    if (k == "theme") cfg.theme = v.toInt() == 1 ? 1 : 0;
  }
  return true;
}

static void saveCfg() {
  prefs.begin("washable", false);
  for (const CfgStr &c : CFG_STR) prefs.putString(c.key, c.p);
  prefs.putUChar("bright", cfg.bright);
  prefs.putBool("demo", cfg.demo);
  prefs.putUChar("lang", cfg.lang);
  prefs.putUChar("theme", cfg.theme);
  prefs.putUChar("cfgv", 1);
  prefs.end();
  backupWrite();
}
static void loadCfg() {
  for (const CfgStr &c : CFG_STR) strlcpy(c.p, c.def, c.cap);
  cfg.bright = 100;
  cfg.demo = true;
  cfg.lang = 0;
  cfg.theme = 0;
  bool have = false, restored = false;
  if (prefs.begin("washable", true)) {
    have = prefs.isKey("mid") || prefs.isKey("ssid") || prefs.isKey("pin") || prefs.isKey("cfgv") || prefs.isKey("owner");
    if (have) {
      for (const CfgStr &c : CFG_STR) strlcpy(c.p, prefs.getString(c.key, c.def).c_str(), c.cap);
      cfg.bright = prefs.getUChar("bright", 100);
      cfg.demo = prefs.getBool("demo", true);
      cfg.lang = prefs.getUChar("lang", 0) == 1 ? 1 : 0;
      cfg.theme = prefs.getUChar("theme", 0) == 1 ? 1 : 0;
    }
    prefs.end();
  }
  if (!have) restored = backupRestore();            // NVS empty: try the flash mirror
  if (strlen(cfg.pin) != 6) strlcpy(cfg.pin, "123456", sizeof cfg.pin);     // PIN is exactly 6 digits (older 4-digit PINs reset to the default)
  if (!cfg.machineId[0]) strlcpy(cfg.machineId, "03", sizeof cfg.machineId);
  if (restored) { saveCfg(); Serial.println("CFG: restored from flash backup"); }
  else if (have && spiffsOk && !SPIFFS.exists("/cfg.bak")) backupWrite();   // first boot after the OTA-capable update
  g_lang = cfg.lang;
  g_light = cfg.theme == 1;
  updateMachineName();
  applyBright();
}
// Before an OTA: write everything, read it back, and refuse to update if anything differs.
static bool cfgFlushVerify() {
  saveCfg();
  if (!prefs.begin("washable", true)) return false;
  bool ok = prefs.getUChar("bright", 0) == cfg.bright && prefs.getBool("demo", !cfg.demo) == cfg.demo;
  for (const CfgStr &c : CFG_STR) if (strcmp(prefs.getString(c.key, "\x01").c_str(), c.p) != 0) ok = false;
  prefs.end();
  return ok;
}
static uint32_t secPerMin() { return cfg.demo ? 1 : 60; }
static uint32_t wifiAttemptMs = 0;
static void wifiConnect() {
  if (!cfg.ssid[0]) return;
  wifiAttemptMs = millis();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(cfg.ssid, cfg.pass);
}

// ============================== RGB STATUS LED ==============================
// Colour code (priority top to bottom):
//   blue pulse ........ firmware update downloading
//   red x3 / x2 flash . error (payment timeout, wrong PIN / member code, update failed)
//   green x3 / x5 ..... payment received / cycle finished
//   yellow solid ...... wash / dry running, Wi-Fi OK      yellow<->red alternate: running but Wi-Fi lost
//   yellow slow blink . waiting for payment               yellow fast blink: checking member / entering code
//   purple solid ...... admin pages (PIN, settings, Wi-Fi, editor)
//   blue blink ........ Wi-Fi connecting (first 20 s)
//   green solid ....... Wi-Fi connected and ready         red solid: Wi-Fi lost and nothing running
#define LED_R_PIN       4
#define LED_G_PIN       16
#define LED_B_PIN       17
#define LED_ACTIVE_LOW  1        // the on-board RGB LED is common-anode
#define LED_LEVEL       90       // max brightness 0..255 (full brightness is very bright)
#define LED_SELFTEST    1        // red -> green -> blue at boot
static const uint8_t LED_CH[3] = { 1, 2, 3 };

struct Rgb { uint8_t r, g, b; };
static const Rgb LC_OFF = { 0, 0, 0 }, LC_RED = { 255, 0, 0 }, LC_GREEN = { 0, 255, 0 }, LC_YELLOW = { 255, 110, 0 },
                 LC_BLUE = { 0, 70, 255 }, LC_PURPLE = { 170, 0, 255 };
enum LedKind : uint8_t { LK_SOLID, LK_BLINK, LK_PULSE, LK_ALT };
struct LedPat { LedKind kind; Rgb a, b; uint16_t period; const char *name; };

static uint32_t ledFlashUntil = 0;
static Rgb ledFlashCol = LC_OFF;

static void ledWrite(Rgb c) {
  static int last[3] = { -1, -1, -1 };
  const uint8_t v[3] = { c.r, c.g, c.b };
  for (int i = 0; i < 3; i++) {
    int d = (int)v[i] * LED_LEVEL / 255;
    if (LED_ACTIVE_LOW) d = 255 - d;
    if (d != last[i]) { last[i] = d; ledcWrite(LED_CH[i], d); }
  }
}
static void ledFlash(Rgb c, int times) { ledFlashCol = c; ledFlashUntil = millis() + (uint32_t)times * 240; }

static LedPat ledDecide(uint32_t now) {
  bool wifi = WiFi.status() == WL_CONNECTED;
  if (ota.state == OTA_DOWNLOADING || ota.state == OTA_VERIFYING || curScr == SC_OTA) return { LK_PULSE, LC_BLUE, LC_OFF, 1600, "update" };
  switch (curScr) {
    case SC_PAY:
    case SC_SUCCESS:
      return wifi ? LedPat{ LK_BLINK, LC_YELLOW, LC_OFF, 1200, "payment" } : LedPat{ LK_ALT, LC_YELLOW, LC_RED, 1200, "payment-offline" };
    case SC_CHECK:
    case SC_OTP:
      return { LK_BLINK, LC_YELLOW, LC_OFF, 500, "member-check" };
    case SC_PIN:
    case SC_SETTINGS:
    case SC_WIFI:
    case SC_EDIT:
    case SC_RTEST:
    case SC_INFO:
    case SC_WEB:
    case SC_HEALTH:
      return { LK_SOLID, LC_PURPLE, LC_OFF, 0, "admin" };
    default:
      break;
  }
  if (anyJobRunning())
    return wifi ? LedPat{ LK_SOLID, LC_YELLOW, LC_OFF, 0, "running" } : LedPat{ LK_ALT, LC_YELLOW, LC_RED, 1200, "running-offline" };
  if (!wifi && cfg.ssid[0] && now - wifiAttemptMs < 20000) return { LK_BLINK, LC_BLUE, LC_OFF, 900, "wifi-connecting" };
  return wifi ? LedPat{ LK_SOLID, LC_GREEN, LC_OFF, 0, "ready" } : LedPat{ LK_SOLID, LC_RED, LC_OFF, 0, "wifi-lost" };
}

static void ledTick(uint32_t now) {
  if (now < ledFlashUntil) { ledWrite(((ledFlashUntil - now) / 120) % 2 ? ledFlashCol : LC_OFF); return; }
  LedPat p = ledDecide(now);
  static const char *lastName = NULL;
  if (p.name != lastName && strcmp(p.name, lastName ? lastName : "")) { lastName = p.name; Serial.printf("LED: %s\n", p.name); }
  switch (p.kind) {
    case LK_SOLID: ledWrite(p.a); break;
    case LK_BLINK: ledWrite((now % p.period) < p.period / 2 ? p.a : LC_OFF); break;
    case LK_ALT:   ledWrite((now % p.period) < p.period / 2 ? p.a : p.b); break;
    case LK_PULSE: {
      uint32_t ph = now % p.period, half = p.period / 2;
      uint32_t lv = ph < half ? ph * 255 / half : (p.period - ph) * 255 / half;
      ledWrite({ (uint8_t)(p.a.r * lv / 255), (uint8_t)(p.a.g * lv / 255), (uint8_t)(p.a.b * lv / 255) });
      break;
    }
  }
}
static void ledInit() {
  const uint8_t pins[3] = { LED_R_PIN, LED_G_PIN, LED_B_PIN };
  for (int i = 0; i < 3; i++) { ledcSetup(LED_CH[i], 5000, 8); ledcAttachPin(pins[i], LED_CH[i]); }
  ledWrite(LC_OFF);
#if LED_SELFTEST
  const Rgb seq[3] = { LC_RED, LC_GREEN, LC_BLUE };
  for (int i = 0; i < 3; i++) { ledWrite(seq[i]); delay(220); }
  ledWrite(LC_OFF);
#endif
}

// ============================== HARDWARE ==============================
TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t drawBuf;
#define DRAW_LINES 40                      // lines per render strip: fewer, taller strips = less per-strip overhead (buffers live in the DMA heap)
static lv_color_t *buf1, *buf2;
static lv_disp_drv_t dispDrv;
static lv_indev_drv_t indevDrv;

static void setRelays(bool wash, bool dry) {
#if RELAY_WASH_PIN >= 0
  digitalWrite(RELAY_WASH_PIN, wash ? RELAY_ON : RELAY_OFF);
#endif
#if RELAY_DRY_PIN >= 0
  digitalWrite(RELAY_DRY_PIN, dry ? RELAY_ON : RELAY_OFF);
#endif
  (void)wash; (void)dry;
}

static const Opt *jobOpt(int k) { return (k ? DRY_OPTS : WASH_OPTS) + jobs[k].opt; }
static uint32_t jobElapsed(int k, uint32_t now) {
  int32_t d = (int32_t)(now - jobs[k].startMs);          // signed: a start time slightly in the future counts as 0, never as a huge wrap-around
  if (d < 0) return 0;
  return (uint32_t)d > jobs[k].durMs ? jobs[k].durMs : (uint32_t)d;
}
static int jobRemainSec(int k, uint32_t now) { return (jobs[k].durMs - jobElapsed(k, now) + 999) / 1000; }
static void jobsApplyRelays() { setRelays(jobs[0].st == JS_RUN, jobs[1].st == JS_RUN); }

// ---- relay phases (placeholder until the wash/dry phase tables are agreed) -------------------------------------------
// A cycle drives the STM32 relay controller: for each service a list of {from % of the cycle, channels on} (bit0 = CH1).
// The last entry whose `fromPct` has been reached applies. CH1-4 are the wash machine (inlet, motor, drain, soap),
// CH5-8 the dryer (heater, motor, fan, spare). Default = only the main motor (CH2 / CH6) for the whole cycle.
struct Phase { uint8_t fromPct, mask; };
static const Phase WASH_PHASES[] = { { 0, 0x02 } };
static const Phase DRY_PHASES[]  = { { 0, 0x20 } };
#define RELAY_KEEPALIVE_MS  5000UL     // re-assert the wanted relays this often (the controller's ttl is 20 s, its heartbeat failsafe 3 s)
#define RELAY_RETRY_MS      1000UL     // ... and this often while the controller reports something else
#define RELAY_TTL_S         20

static uint8_t jobMask(int k, uint32_t now) {
  if (jobs[k].st != JS_RUN) return 0;
  int pct = (int)((jobElapsed(k, now) * 100ULL) / (jobs[k].durMs ? jobs[k].durMs : 1));
  const Phase *ph = k ? DRY_PHASES : WASH_PHASES;
  size_t n = k ? sizeof DRY_PHASES / sizeof *DRY_PHASES : sizeof WASH_PHASES / sizeof *WASH_PHASES;
  uint8_t m = 0;
  for (size_t i = 0; i < n; i++) if (pct >= ph[i].fromPct) m = ph[i].mask;
  return m;
}

// The controller drives the relays when it answers; in demo mode without a controller the cycle just runs on the timer.
static bool relayControl() { return rl.helloOk || !cfg.demo; }
static bool controllerReady() { return cfg.demo || (rl.up && rl.helloOk); }

// A running cycle survives a power cut: remaining time is stored (NVS, once a minute) and resumed at boot.
static void jobSave(int k, uint32_t now) {
  Preferences p;
  if (!p.begin("jobs", false)) return;
  char key[3] = { 'j', (char)('0' + k), 0 }, v[24];
  if (jobs[k].st == JS_RUN) snprintf(v, sizeof v, "%u,%lu", jobs[k].opt, (unsigned long)((jobs[k].durMs - jobElapsed(k, now)) / 1000));
  else v[0] = 0;
  if (p.getString(key, "") != String(v)) p.putString(key, v);
  p.end();
}
static void jobsRestore() {
  Preferences p;
  if (!p.begin("jobs", true)) return;
  for (int k = 0; k < 2; k++) {
    char key[3] = { 'j', (char)('0' + k), 0 };
    String v = p.getString(key, "");
    unsigned opt = 0; unsigned long rem = 0;
    if (sscanf(v.c_str(), "%u,%lu", &opt, &rem) == 2 && opt < 3 && rem > 0 && rem < 8UL * 3600) {
      const Opt *o = (k ? DRY_OPTS : WASH_OPTS) + opt;
      jobs[k].opt = (uint8_t)opt;
      jobs[k].durMs = o->minutes * secPerMin() * 1000UL;
      if ((uint32_t)rem * 1000UL > jobs[k].durMs) rem = jobs[k].durMs / 1000;       // demo mode toggled meanwhile
      jobs[k].startMs = millis() - (jobs[k].durMs - (uint32_t)rem * 1000UL);
      jobs[k].st = JS_RUN;
      Serial.printf("JOB %s resumed after restart, %lu s left\n", k ? "dry" : "wash", rem);
    }
  }
  p.end();
}

static void jobStart(int k, int opt) {
  if (jobs[k].st == JS_RUN) return;                        // already running: never restart a paid cycle
  jobs[k].opt = (uint8_t)opt;
  jobs[k].startMs = millis();
  jobs[k].durMs = (k ? DRY_OPTS : WASH_OPTS)[opt].minutes * secPerMin() * 1000UL;
  jobs[k].st = JS_RUN;
  jobSave(k, millis());
  jobsApplyRelays();
  Serial.printf("JOB %s start: %s, %lu s\n", k ? "dry" : "wash", jobOpt(k)->name, (unsigned long)(jobs[k].durMs / 1000));
}

// Runs from the 100 ms app timer, before any screen logic, so cycle timing never depends on the UI.
static uint32_t relaySyncMs = 0;
static void jobsTick(uint32_t now) {
  static uint32_t last = 0, lastSave = 0;
  uint32_t dt = last ? now - last : 0;
  last = now;
  bool ctl = relayControl();
  for (int k = 0; k < 2; k++) {
    if (jobs[k].st == JS_RUN) {
      uint8_t m = jobMask(k, now);
      if (ctl && !(rl.up && rl.helloOk && (rl.mask & m) == m)) {                         // paused: time only counts while its relays are confirmed on
        jobs[k].startMs += dt;
        if ((int32_t)(jobs[k].startMs - now) > 0) jobs[k].startMs = now;
      }
    }
    if (jobs[k].st == JS_RUN && jobElapsed(k, now) >= jobs[k].durMs) {
      jobs[k].st = JS_DONE;
      jobs[k].doneMs = now;
      jobSave(k, now);
      jobsApplyRelays();
      relaySyncMs = 0;                                      // switch the controller off at once
      ledFlash(LC_GREEN, 5);
      Serial.printf("JOB %s finished\n", k ? "dry" : "wash");
    } else if (jobs[k].st == JS_DONE && now - jobs[k].doneMs > 1800000UL) {
      jobs[k].st = JS_FREE;                                 // nobody collected: free the slot after 30 min
    }
  }
  if (now - lastSave > 60000UL) { lastSave = now; for (int k = 0; k < 2; k++) if (jobs[k].st == JS_RUN) jobSave(k, now); }
  if (ctl) {                                                // keep the controller in line with the wanted relays
    uint8_t want = jobMask(0, now) | jobMask(1, now);
    bool match = rl.up && rl.helloOk && rl.mask == want;
    if ((want || rl.mask) && now - relaySyncMs >= (match ? RELAY_KEEPALIVE_MS : RELAY_RETRY_MS)) {
      relaySyncMs = now;
      if (want) rlRelay(want, (uint8_t)~want, RELAY_TTL_S); else rlAllOff();
    }
  }
}

static uint32_t perfFlushUs = 0, perfWaitUs = 0, perfFlushPx = 0, perfHandlerUs = 0, perfHandlerMax = 0, perfLoops = 0;
static void dispFlush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  uint32_t t0 = micros();
  tft.dmaWait();                                    // previous strip finished
  perfWaitUs += micros() - t0;
  perfFlushPx += w * h;
  tft.setAddrWindow(area->x1, area->y1, w, h);       // the SPI transaction stays open (startWrite() once in setup): no per-strip begin/end
  tft.pushPixelsDMA((uint16_t *)&color_p->full, w * h);
  perfFlushUs += micros() - t0;
  lv_disp_flush_ready(drv);
}

// ---- performance counters (printed on Serial)
static uint32_t perfN = 0, perfT = 0, perfPx = 0;
static uint32_t monLast = 0;
static void monCb(lv_disp_drv_t *, uint32_t t, uint32_t px) {
  perfN++; perfT += t; perfPx += px;
  uint32_t now = millis();
  monLast = now;
}

// ---- GT911 minimal driver
static bool gtRead(uint16_t reg, uint8_t *b, uint8_t n) {
  Wire.beginTransmission(GT_ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)GT_ADDR, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) b[i] = Wire.read();
  return true;
}
static void gtClear() {
  Wire.beginTransmission(GT_ADDR);
  Wire.write(0x81); Wire.write(0x4E); Wire.write(0);
  Wire.endTransmission();
}
static void gtInit() {
  pinMode(GT_RST, OUTPUT);
  pinMode(GT_INT, OUTPUT);
  digitalWrite(GT_INT, LOW);
  digitalWrite(GT_RST, LOW);
  delay(10);
  digitalWrite(GT_RST, HIGH);
  delay(10);
  pinMode(GT_INT, INPUT);
  delay(60);
  Wire.begin(GT_SDA, GT_SCL);
  Wire.setClock(400000);
}
static void touchRead(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  static int16_t lx = 0, ly = 0;
  static bool pressed = false;
  uint8_t st;
  if (gtRead(0x814E, &st, 1) && (st & 0x80)) {
    uint8_t n = st & 0x0F;
    if (n > 0) {
      uint8_t p[5];
      if (gtRead(0x814F, p, 5)) {
        int x = p[1] | (p[2] << 8);
        int y = p[3] | (p[4] << 8);
#if TOUCH_SWAP_XY
        { int t = x; x = y; y = t; }
#endif
#if TOUCH_INVERT_X
        x = SCREEN_W - 1 - x;
#endif
#if TOUCH_INVERT_Y
        y = SCREEN_H - 1 - y;
#endif
        lx = constrain(x, 0, SCREEN_W - 1);
        ly = constrain(y, 0, SCREEN_H - 1);
        pressed = true;
        lastActMs = millis();
      }
    } else {
      pressed = false;
    }
    gtClear();
  }
  data->point.x = lx;
  data->point.y = ly;
  data->state = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

// ============================== WIDGET HELPERS ==============================
static void noClick(lv_obj_t *o) {
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *mkBox(lv_obj_t *p, int x, int y, int w, int h, lv_color_t c, int r) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, r, 0);
  if (g_light && !g_noStroke && w >= 28 && h >= 28) {                         // light theme: a stroke so cards and buttons stand out from the pale background
    uint16_t f = c.full;
    if (f == C_NAVY2.full || f == C_NAVY3.full || f == lv_color_white().full) {
      lv_obj_set_style_outline_width(o, 2, 0);
      lv_obj_set_style_outline_color(o, lv_color_hex(0x7E9BBE), 0);
    } else if (f == C_TEAL.full || f == C_BLUE.full || f == C_ORANGE.full || f == C_MINT.full) {
      lv_obj_set_style_outline_width(o, 2, 0);
      lv_obj_set_style_outline_color(o, lv_color_hex(0x0B2545), 0);
      lv_obj_set_style_outline_opa(o, LV_OPA_40, 0);
    }
  }
  noClick(o);
  return o;
}

static lv_obj_t *mkClickBox(lv_obj_t *p, int x, int y, int w, int h, lv_color_t c, int r,
                            lv_event_cb_t cb, void *ud) {
  lv_obj_t *o = mkBox(p, x, y, w, h, c, r);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, ud);
  return o;
}

static lv_obj_t *mkLbl(lv_obj_t *p, const char *txt, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, f, 0);
  if (g_light && c.full == lv_color_white().full) c = C_TXT;     // white text becomes dark ink on the light theme
  lv_obj_set_style_text_color(l, c, 0);
  return l;
}

// horizontally centred (multi-line) label, width w, top y
static lv_obj_t *mkLblC(lv_obj_t *p, const char *txt, const lv_font_t *f, lv_color_t c, int y, int w = 300) {
  lv_obj_t *l = mkLbl(p, txt, f, c);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, w);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  return l;
}

static lv_obj_t *mkScreen(lv_color_t bg) {
  lv_obj_t *s = lv_obj_create(NULL);
  lv_obj_remove_style_all(s);
  lv_obj_set_size(s, SCREEN_W, SCREEN_H);
  if (g_light) bg = bg.full == lv_color_hex(0x0B2545).full ? lv_color_hex(0xE6EFF8) : lv_color_hex(0xD9EAF6);
  lv_obj_set_style_bg_color(s, bg, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

static lv_obj_t *mkPill(lv_obj_t *p, const char *txt, int x, int y, int w, int h, lv_color_t bg,
                        lv_color_t fg, const lv_font_t *f, lv_event_cb_t cb, void *ud) {
  lv_obj_t *b = mkClickBox(p, x, y, w, h, bg, h / 2, cb, ud);
  lv_obj_set_style_bg_opa(b, LV_OPA_80, LV_STATE_PRESSED);
  lv_obj_t *l = mkLbl(b, txt, f, fg);
  lv_obj_center(l);
  return b;
}

static void animStart(void *var, lv_anim_exec_xcb_t cb, int32_t s, int32_t e, uint32_t time,
                      uint32_t delay = 0, bool infinite = false, uint32_t playback = 0) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, var);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_set_values(&a, s, e);
  lv_anim_set_time(&a, time);
  lv_anim_set_delay(&a, delay);
  if (playback) lv_anim_set_playback_time(&a, playback);
  if (infinite) lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
}

static void bgOpaCb(void *v, int32_t x)   { lv_obj_set_style_bg_opa((lv_obj_t *)v, (lv_opa_t)x, 0); }
static void textOpaCb(void *v, int32_t x) { lv_obj_set_style_text_opa((lv_obj_t *)v, (lv_opa_t)x, 0); }
static void transXCb(void *v, int32_t x)  { lv_obj_set_style_translate_x((lv_obj_t *)v, x, 0); }
static void posYCb(void *v, int32_t x)    { lv_obj_set_y((lv_obj_t *)v, x); }

// ---- per-build pools (two sets so the outgoing screen keeps valid data during a swap)
static int pool = 0;

// ---------- bubbles (screen background)
struct Bub { int16_t x, amp, ybot, rise, sz, lx, ly; uint8_t lastOp; };
// move only when the integer position really changed: every set_pos invalidates the old and the new area, and each area costs a full object-tree walk
static inline void movePos(lv_obj_t *o, Bub *b, int x, int y) { if (x != b->lx || y != b->ly) { b->lx = (int16_t)x; b->ly = (int16_t)y; lv_obj_set_pos(o, x, y); } }
static Bub bubs[2][40];
static int nBubs = 0;
static uint32_t rndState = 12345;
static uint32_t rnd() { rndState = rndState * 1664525UL + 1013904223UL; return rndState >> 8; }

static void bubbleCb(void *var, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)var;
  Bub *b = (Bub *)lv_obj_get_user_data(o);
  int16_t s = lv_trigo_sin((int16_t)((v * 360) / 255));
  movePos(o, b, b->x + ((s * b->amp) >> 15), b->ybot - (b->rise * v) / 255 - b->sz);
  int op = ((v < 40) ? v * 5 : (v > 190 ? (255 - v) * 3 : 200)) & ~15;      // 16 steps: a style change per frame is expensive, so only on a real step
  if (op != b->lastOp) { b->lastOp = (uint8_t)op; lv_obj_set_style_border_opa(o, (lv_opa_t)op, 0); }
}

static void addBubbles(lv_obj_t *parent, int n, lv_color_t col, int xmin, int xmax, int ybot, int rise,
                       uint32_t tmin, uint32_t tmax, int szmin, int szmax) {
  for (int i = 0; i < n && nBubs < 40; i++) {
    Bub *b = &bubs[pool][nBubs++];
    b->lastOp = 1; b->lx = b->ly = -1000;
    b->sz = szmin + rnd() % (szmax - szmin + 1);
    b->x = xmin + rnd() % (xmax - xmin + 1);
    b->amp = 5 + rnd() % 12;
    b->ybot = ybot;
    b->rise = rise;
    lv_obj_t *o = mkBox(parent, 0, ybot, b->sz, b->sz, C_WHITE, LV_RADIUS_CIRCLE);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_10, 0);
    lv_obj_set_style_border_color(o, g_light ? lv_color_hex(0x2B8FB0) : col, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_user_data(o, b);
    animStart(o, bubbleCb, 0, 255, tmin + rnd() % (tmax - tmin + 1), rnd() % 3500, true);
  }
}

// ---------- waves: only a thin crest band is redrawn every frame (a static body below it), the surface is a two-harmonic sine
struct Wave { lv_obj_t *crest; lv_color_t col; int16_t w, amp, wl, bandH; int32_t phase; };
static Wave waves[2][8];
static int nWaves = 0;

static void waveDraw(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target(e);
  Wave *w = (Wave *)lv_obj_get_user_data(o);
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = w->col;
  d.bg_opa = LV_OPA_COVER;
  d.radius = 0;
  d.border_width = 0;
  const int colW = w->w > 200 ? 6 : 4;
  const int mid = a.y1 + 1 + w->amp;
  for (int x = 0; x < w->w; x += colW) {
    lv_area_t r = { (lv_coord_t)(a.x1 + x), 0, (lv_coord_t)(a.x1 + x + colW - 1), a.y2 };
    if (r.x2 < ctx->clip_area->x1 || r.x1 > ctx->clip_area->x2) continue;
    int d1 = (x * 360 / w->wl + w->phase * 360 / 1000) % 360;
    int d2 = (x * 360 * 3 / (w->wl * 2) + 360 - (w->phase * 2 * 360 / 1000) % 360) % 360;      // a slower, shorter ripple against the main swell
    int s = ((int)lv_trigo_sin((int16_t)d1) * 7 + (int)lv_trigo_sin((int16_t)d2) * 3) / 10;
    r.y1 = (lv_coord_t)(mid + ((s * w->amp) >> 15));
    lv_draw_rect(ctx, &d, &r);
  }
}

static void waveCb(void *var, int32_t v) {
  lv_obj_t *c = (lv_obj_t *)var;
  Wave *w = (Wave *)lv_obj_get_user_data(c);
  w->phase = v;
  lv_obj_invalidate(c);
}

static lv_obj_t *mkWave(lv_obj_t *p, int x, int baseY, int w, int bottomY, lv_color_t col, int d, int amp,
                        uint32_t period) {
  Wave *wv = &waves[pool][nWaves++ % 8];
  wv->col = col; wv->w = (int16_t)w; wv->amp = (int16_t)amp; wv->wl = (int16_t)(d * 3); wv->phase = 0;
  int bandH = 2 * amp + 3;
  int top = baseY - amp - 1;
  lv_obj_t *crest = mkBox(p, x, top, w, bandH, col, 0);
  lv_obj_set_style_bg_opa(crest, LV_OPA_TRANSP, 0);                          // drawn by waveDraw
  lv_obj_set_user_data(crest, wv);
  lv_obj_add_event_cb(crest, waveDraw, LV_EVENT_DRAW_MAIN, NULL);
  wv->crest = crest;
  if (bottomY > top + bandH) mkBox(p, x, top + bandH, w, bottomY - (top + bandH), col, 0);   // static body under the crest
  animStart(crest, waveCb, 0, 1000, period, 0, true);
  return crest;
}

// ---------- logo: porthole + wave + W
static lv_point_t logoPts[2][6][5];
static int nLogo = 0;

static lv_obj_t *mkLogo(lv_obj_t *p, int x, int y, int s, bool animated) {
  g_noStroke = true;
  float k = s / 110.0f;
  int bw = (int)(10 * k + 0.5f);
  if (bw < 2) bw = 2;
  lv_obj_t *o = mkBox(p, x, y, s, s, C_NAVY, LV_RADIUS_CIRCLE);
  lv_obj_set_style_border_width(o, bw, 0);
  lv_obj_set_style_border_color(o, C_TEAL, 0);
  lv_obj_set_style_clip_corner(o, true, 0);
  int in = s - 2 * bw;
  int wy = (int)(55 * k);
  if (animated) {
    mkWave(o, 0, wy, in, in, C_BLUE, (int)(34 * k), (int)(5 * k) + 1, 2400);
  } else {
    mkBox(o, 0, wy, in, in - wy, C_BLUE, 0);
    int d = (int)(34 * k);
    for (int i = 0; i < 4; i++) mkBox(o, i * (d * 2 / 3) - d / 4, wy - d / 2 + (i & 1 ? 2 : -2), d, d, C_BLUE, LV_RADIUS_CIRCLE);
  }
  static const int W[5][2] = { {34, 36}, {47, 76}, {60, 50}, {73, 76}, {86, 36} };
  lv_point_t *pts = logoPts[pool][nLogo++ % 6];
  for (int i = 0; i < 5; i++) {
    pts[i].x = (lv_coord_t)((W[i][0] - 15) * k);
    pts[i].y = (lv_coord_t)((W[i][1] - 15) * k);
  }
  lv_obj_t *ln = lv_line_create(o);
  lv_line_set_points(ln, pts, 5);
  lv_obj_set_style_line_width(ln, (int)(9 * k) + 1, 0);
  lv_obj_set_style_line_color(ln, C_WHITE, 0);
  lv_obj_set_style_line_rounded(ln, true, 0);
  noClick(ln);
  g_noStroke = false;
  return o;
}

static lv_obj_t *mkWordmark(lv_obj_t *p, int x, int y, const lv_font_t *f) {
  lv_obj_t *l = mkLbl(p, "wash#00B4C5 able#", f, C_WHITE);
  lv_label_set_recolor(l, true);
  lv_obj_set_pos(l, x, y);
  return l;
}

// ---------- washing-machine drum (glass porthole with tumbling clothes)
struct Drum { lv_obj_t *cl[4]; int cx, cy, R, w[4], h[4]; };
static Drum drums[2][4];
static int nDrums = 0;

static void tumbleCb(void *var, int32_t v) {
  lv_obj_t *g = (lv_obj_t *)var;
  Drum *d = (Drum *)lv_obj_get_user_data(g);
  for (int i = 0; i < 4; i++) {
    int a = (v * 360 / 1000 + i * 90) % 360;
    int r = (i & 1) ? d->R * 9 / 10 : d->R * 6 / 10;
    int x = d->cx + ((lv_trigo_cos((int16_t)a) * r) >> 15) - d->w[i] / 2;
    int y = d->cy + ((lv_trigo_sin((int16_t)a) * r) >> 15) - d->h[i] / 2;
    lv_obj_set_pos(d->cl[i], x, y);
  }
}

// frame circle (size) -> glass circle (size - 2*fw). Returns the glass.
static lv_obj_t *mkDrum(lv_obj_t *p, int x, int y, int size, int fw, lv_color_t frameCol, lv_color_t glassCol,
                        const uint32_t cloth[4], uint32_t period) {
  lv_obj_t *frame = mkBox(p, x, y, size, size, frameCol, LV_RADIUS_CIRCLE);
  int g = size - 2 * fw;
  lv_obj_t *glass = mkBox(frame, fw, fw, g, g, glassCol, LV_RADIUS_CIRCLE);
  lv_obj_set_style_clip_corner(glass, true, 0);
  return glass;
}

// add clothes to a glass; call after the water so they draw on top
static void addClothes(lv_obj_t *glass, int g, const uint32_t cloth[4], uint32_t period) {
  Drum *d = &drums[pool][nDrums++ % 4];
  d->cx = g / 2; d->cy = g / 2; d->R = g * 3 / 10;
  const int W[4] = { g * 34 / 100, g * 24 / 100, g * 36 / 100, g * 14 / 100 };
  const int H[4] = { g * 22 / 100, g * 17 / 100, g * 22 / 100, g * 22 / 100 };
  const int R[4] = { LV_RADIUS_CIRCLE, 6, LV_RADIUS_CIRCLE, 5 };
  for (int i = 0; i < 4; i++) {
    d->w[i] = W[i]; d->h[i] = H[i];
    d->cl[i] = mkBox(glass, 0, 0, W[i], H[i], lv_color_hex(cloth[i]), R[i]);
  }
  lv_obj_set_user_data(glass, d);
  animStart(glass, tumbleCb, 0, 1000, period, 0, true);
}

static const uint32_t CLOTH_WASH[4] = { 0xFF9F43, 0x0B2545, 0x00B4C5, 0xFFFFFF };
static const uint32_t CLOTH_DRY[4]  = { 0x3FA9F5, 0x0B2545, 0xFF9F43, 0x00B4C5 };

// rising foam / heat / spark helpers (all small objects)
static void foamCb(void *var, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)var;
  Bub *b = (Bub *)lv_obj_get_user_data(o);
  movePos(o, b, b->x + ((lv_trigo_sin((int16_t)(v * 360 / 255)) * b->amp) >> 15), b->ybot - (b->rise * v) / 255);
  int op = ((v < 50) ? v * 4 : (255 - v)) & ~15;
  if (op != b->lastOp) { b->lastOp = (uint8_t)op; lv_obj_set_style_bg_opa(o, (lv_opa_t)op, 0); }
}
static void addFoam(lv_obj_t *parent, int n, int xmin, int xmax, int ybot, int rise) {
  for (int i = 0; i < n && nBubs < 40; i++) {
    Bub *b = &bubs[pool][nBubs++];
    b->lastOp = 1; b->lx = b->ly = -1000;
    b->sz = 6 + rnd() % 8; b->x = xmin + rnd() % (xmax - xmin); b->amp = 6; b->ybot = ybot; b->rise = rise;
    lv_obj_t *o = mkBox(parent, 0, ybot, b->sz, b->sz, C_WHITE, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_user_data(o, b);
    animStart(o, foamCb, 0, 255, 2400 + rnd() % 1200, i * 500, true);
  }
}
static void sparkCb(void *var, int32_t v) {
  int op = (v < 128 ? v * 2 : (255 - v) * 2) & ~15;
  if (lv_obj_get_style_bg_opa((lv_obj_t *)var, 0) != op) lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)op, 0);
}
static void heatCb(void *var, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)var;
  Bub *b = (Bub *)lv_obj_get_user_data(o);
  movePos(o, b, b->x, b->ybot - (b->rise * v) / 255);
  int op = ((v < 50) ? v * 4 : (255 - v)) & ~15;
  if (op != b->lastOp) { b->lastOp = (uint8_t)op; lv_obj_set_style_bg_opa(o, (lv_opa_t)op, 0); }
}
static void addHeat(lv_obj_t *parent, int n, const int *xs, int ybot, int rise, lv_color_t col) {
  for (int i = 0; i < n && nBubs < 40; i++) {
    Bub *b = &bubs[pool][nBubs++];
    b->lastOp = 1; b->lx = b->ly = -1000;
    b->sz = 16; b->x = xs[i]; b->amp = 0; b->ybot = ybot; b->rise = rise;
    lv_obj_t *o = mkBox(parent, xs[i], ybot, 5, 18, col, 3);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_user_data(o, b);
    animStart(o, heatCb, 0, 255, 1800, i * 600, true);
  }
}

static lv_obj_t *mkArc(lv_obj_t *p, int x, int y, int size, int width, lv_color_t track, lv_color_t ind) {
  lv_obj_t *a = lv_arc_create(p);
  lv_obj_set_size(a, size, size);
  lv_obj_set_pos(a, x, y);
  lv_arc_set_rotation(a, 270);
  lv_arc_set_bg_angles(a, 0, 360);
  lv_arc_set_range(a, 0, 1000);
  lv_arc_set_value(a, 0);
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(a, 0, 0);
  lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(a, 0, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, track, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(a, ind, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
  return a;
}

// ============================== NAVIGATION ==============================
static void showScreen(Scr id);
static void evtTheme(lv_event_t *e);

static void evtGo(lv_event_t *e) { showScreen((Scr)(intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t *mkHeader(lv_obj_t *scr, Scr backTo, const char *right) {
  int x = 16;
  if (backTo != SC_COUNT) {
    lv_obj_t *b = mkClickBox(scr, 10, 6, 44, 44, C_NAVY3, 22, evtGo, (void *)(intptr_t)backTo);
    lv_obj_set_style_bg_color(b, C_NAVY2, LV_STATE_PRESSED);
    lv_obj_t *l = mkLbl(b, LV_SYMBOL_LEFT, &lv_font_montserrat_16, C_WHITE);
    lv_obj_center(l);
    x = 64;
  }
  mkLogo(scr, x, 12, 30, false);
  mkWordmark(scr, x + 36, 11, &f_fredoka_20);
  if (right) {
    lv_obj_t *r = mkLbl(scr, right, &f_nunito_14, C_LIGHT);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -12, 17);
  }
  return NULL;
}

// ============================== MEMBERSHIP ==============================
// Demo data: any number is "found", the code is always MEMBER_DEMO_OTP, points/coupons are sample values kept in RAM.
// Replace memberLookup/verify with calls to the backend (cfg.apiUrl + cfg.apiKey) when it exists.
#define MEMBER_NAME      "Budi"
#define MEMBER_FULL      "Budi S."
#define MEMBER_TIER      "Member Silver"
#define MEMBER_DEMO_OTP  "123456"
#define OTP_RESEND_SEC   60

static bool memberActive = false;
static char mDigits[12] = "";
static char mOtp[8] = "";
static int mPoints = 1250, mTab = 0;
static bool mUsed[3], mClaimed[3];
static Scr memberBack = SC_IDLE;           // where "back" on the phone screen leads

static void memberReset() {
  memberActive = false; mDigits[0] = 0; mOtp[0] = 0; mPoints = 1250; mTab = 0;
  for (int i = 0; i < 3; i++) mUsed[i] = mClaimed[i] = false;
}
// mode 0: "812 3456 7890"   1: "+62 812-3456-7890"   2: "+62 812-••••-7890"
static void fmtPhone(char *out, size_t n, int mode) {
  String d = mDigits;
  String g1 = d.substring(0, 3), g2 = d.length() > 3 ? d.substring(3, 7) : "", g3 = d.length() > 7 ? d.substring(7, 11) : "";
  if (mode == 0) {
    String s = g1; if (g2.length()) s += " " + g2; if (g3.length()) s += " " + g3;
    snprintf(out, n, "%s", s.c_str());
  } else if (mode == 1) {
    snprintf(out, n, "+62 %s-%s-%s", g1.c_str(), g2.c_str(), g3.c_str());
  } else {
    snprintf(out, n, "+62 %s-••••-%s", g1.c_str(), d.length() >= 4 ? d.substring(d.length() - 4).c_str() : "");
  }
}

// 3x4 numeric keypad (0-9, 10 = clear, 11 = backspace)
static void mkKeypad(lv_obj_t *p, int x, int y, int kw, int kh, int gap, lv_event_cb_t cb, int vgap = 6) {
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 3; c++) {
      int code = (r < 3) ? r * 3 + c + 1 : (c == 0 ? 10 : (c == 1 ? 0 : 11));
      lv_obj_t *b = mkClickBox(p, x + c * (kw + gap), y + r * (kh + vgap), kw, kh, C_NAVY2, 14, cb, (void *)(intptr_t)code);
      lv_obj_set_style_bg_color(b, C_NAVY3, LV_STATE_PRESSED);
      lv_obj_t *l;
      if (code == 10) l = mkLbl(b, "C", &f_fredoka_20, C_LIGHT);
      else if (code == 11) l = mkLbl(b, LV_SYMBOL_BACKSPACE, &lv_font_montserrat_20, C_LIGHT);
      else { char t[2] = { (char)('0' + code), 0 }; l = mkLbl(b, t, &f_fredoka_28, C_WHITE); }
      lv_obj_center(l);
    }
}

static void coinCb(void *var, int32_t w) {          // coin "flips" by squeezing its face
  lv_obj_t *o = (lv_obj_t *)var;
  lv_obj_set_width(o, w);
  lv_obj_set_x(o, lv_obj_get_user_data(o) ? (int)(intptr_t)lv_obj_get_user_data(o) - w / 2 : 0);
}
static lv_obj_t *mkCoin(lv_obj_t *p, int x, int y, int size) {
  lv_obj_t *c = mkBox(p, x, y, size, size, C_ORANGE, LV_RADIUS_CIRCLE);
  int f = size * 70 / 100;
  lv_obj_t *face = mkBox(c, (size - f) / 2, (size - f) / 2, f, f, lv_color_hex(0xFFC98A), LV_RADIUS_CIRCLE);
  lv_obj_set_style_border_width(face, 2, 0);
  lv_obj_set_style_border_color(face, C_NAVY, 0);
  lv_obj_set_user_data(face, (void *)(intptr_t)(size / 2));
  lv_obj_t *l = mkLbl(face, "W", &f_fredoka_20, C_NAVY);
  lv_obj_center(l);
  animStart(face, coinCb, f, 4, 1500, 0, true, 1500);
  return c;
}

// ---------------------------------------- Ask: "Kamu member Washable?"
static void evtAskYes(lv_event_t *) { memberBack = SC_MASK; showScreen(SC_PHONE); }
static void evtAskNo(lv_event_t *)  { showScreen(SC_CYCLE); }
static void riseCb(void *var, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)var;
  lv_obj_set_y(o, 40 - v * 34 / 255);
  lv_obj_set_style_bg_opa(o, (lv_opa_t)(v < 40 ? v * 6 : 255 - v), 0);
}

static lv_obj_t *buildMask() {
  lv_obj_t *s = mkScreen(C_NAVY);
  addBubbles(s, 5, lv_color_hex(0x7FD8E2), 5, 290, 480, 470, 6000, 10000, 10, 24);
  mkHeader(s, SC_SERVICE, machineName);
  mkLblC(s, "Kamu member\nWashable?", &f_fredoka_28, C_WHITE, 56);                         // 56..126
  mkLblC(s, "Member dapat poin, kupon,\ndan voucher", &f_nunito_14, C_LIGHT, 128, 280);    // 128..168

  lv_obj_t *a = mkClickBox(s, 15, 176, 290, 120, C_TEAL, 26, evtAskYes, NULL);            // 176..296
  lv_obj_set_style_bg_opa(a, LV_OPA_80, LV_STATE_PRESSED);
  for (int i = 0; i < 3; i++) {                                                           // coins rising above the card
    lv_obj_t *c = mkBox(a, 20 + i * 26, 40, 12, 12, C_ORANGE, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    animStart(c, riseCb, 0, 255, 2000, i * 650, true);
  }
  lv_obj_t *card = mkBox(a, 14, 46, 84, 58, C_WHITE, 10);
  mkBox(card, 0, 10, 84, 10, C_NAVY, 0);
  lv_obj_t *lg = mkBox(card, 10, 30, 20, 20, C_NAVY, LV_RADIUS_CIRCLE);
  (void)lg;
  mkBox(card, 38, 32, 36, 6, C_BLUE, 3);
  mkBox(card, 38, 43, 24, 6, lv_color_hex(0xC9D6E3), 3);
  lv_obj_t *nm = mkLbl(a, "Ya, saya member", &f_fredoka_20, C_NAVY);
  lv_obj_set_pos(nm, 112, 30);
  lv_obj_t *sb = mkLbl(a, "Masuk pakai nomor\nWhatsApp", &f_nunito_14, C_NAVY);
  lv_obj_set_pos(sb, 112, 62);

  lv_obj_t *b = mkClickBox(s, 15, 306, 290, 104, C_NAVY2, 26, evtAskNo, NULL);            // 306..410
  lv_obj_set_style_border_width(b, 3, 0);
  lv_obj_set_style_border_color(b, C_BLUE, 0);
  lv_obj_t *qr = mkBox(b, 14, 20, 56, 56, C_WHITE, 10);
  mkBox(qr, 8, 8, 14, 14, C_NAVY, 3); mkBox(qr, 34, 8, 14, 14, C_NAVY, 3); mkBox(qr, 8, 34, 14, 14, C_NAVY, 3);
  mkBox(qr, 34, 34, 7, 7, C_BLUE, 0); mkBox(qr, 42, 42, 6, 6, C_BLUE, 0);
  for (int i = 0; i < 3; i++) {
    lv_obj_t *ch = mkLbl(b, LV_SYMBOL_RIGHT, &lv_font_montserrat_20, C_BLUE);
    lv_obj_set_pos(ch, 78 + i * 12, 14);
    animStart(ch, textOpaCb, 40, 255, 700, i * 230, true, 700);
  }
  lv_obj_t *n2 = mkLbl(b, "Lanjut tanpa member", &f_fredoka_20, C_WHITE);
  lv_obj_set_pos(n2, 78, 42);
  lv_obj_t *s2 = mkLbl(b, "Langsung pilih & bayar", &f_nunito_14, C_LIGHT);
  lv_obj_set_pos(s2, 78, 72);

  lv_obj_t *f = mkBox(s, 20, 424, 280, 44, C_NAVY2, 22);                                  // 424..468
  mkCoin(f, 8, 6, 32);
  lv_obj_t *fl = mkLbl(f, "Tiap cuci dapat poin & voucher", &f_nunito_14, C_WHITE);
  lv_obj_set_pos(fl, 50, 12);
  return s;
}

// ---------------------------------------- Phone number
static lv_obj_t *mPhoneVal, *mPhonePH, *mPhoneBtn, *mPhoneBtnLbl, *mCaret;

static void phoneRefresh() {
  char t[24];
  fmtPhone(t, sizeof t, 0);
  bool any = mDigits[0] != 0;
  lv_label_set_text(mPhoneVal, t);
  if (any) { lv_obj_clear_flag(mPhoneVal, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(mPhonePH, LV_OBJ_FLAG_HIDDEN); }
  else     { lv_obj_add_flag(mPhoneVal, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(mPhonePH, LV_OBJ_FLAG_HIDDEN); }
  lv_obj_update_layout(mPhoneVal);
  lv_obj_set_x(mCaret, 74 + (any ? lv_obj_get_width(mPhoneVal) : 0) + 3);
  bool ready = strlen(mDigits) >= 9;
  lv_obj_set_style_bg_color(mPhoneBtn, ready ? C_TEAL : C_NAVY3, 0);
  lv_obj_set_style_text_color(mPhoneBtnLbl, ready ? C_NAVY : C_LIGHT, 0);
}
static void evtKeyPhone(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);
  size_t n = strlen(mDigits);
  if (k == 10) mDigits[0] = 0;
  else if (k == 11) { if (n) mDigits[n - 1] = 0; }
  else if (n < 11 && !(n == 0 && k == 0)) { mDigits[n] = '0' + k; mDigits[n + 1] = 0; }
  phoneRefresh();
}
static void evtPhoneGo(lv_event_t *) { if (strlen(mDigits) >= 9) showScreen(SC_CHECK); }
// mock: fill a sample number and run the member flow (check -> code -> home)
static void evtMockMember(lv_event_t *) { strlcpy(mDigits, "81234567890", sizeof mDigits); showScreen(SC_CHECK); }
static void rippleSmallCb(void *var, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)var;
  int sz = 26 + v * 14 / 100;
  lv_obj_set_size(o, sz, sz);
  lv_obj_set_pos(o, 16 - sz / 2, 16 - sz / 2);
  lv_obj_set_style_border_opa(o, (lv_opa_t)(220 - v * 2), 0);
}

static lv_obj_t *buildPhone() {
  lv_obj_t *s = mkScreen(C_NAVY);
  addBubbles(s, 4, lv_color_hex(0x7FD8E2), 5, 290, 480, 470, 6000, 10000, 10, 22);
  mkHeader(s, memberBack, NULL);
  lv_obj_t *mock = mkPill(s, "Demo: masuk", 196, 9, 112, 32, C_NAVY, C_LIGHT, &f_nunito_14, evtMockMember, NULL);   // mock member login
  lv_obj_set_style_border_width(mock, 2, 0);
  lv_obj_set_style_border_color(mock, C_ORANGE, 0);
  lv_obj_t *t = mkLbl(s, "Masuk Member", &f_fredoka_28, C_WHITE);                         // 54..89
  lv_obj_set_pos(t, 16, 54);
  lv_obj_t *st = mkLbl(s, "Masukkan nomor WhatsApp kamu", &f_nunito_14, C_LIGHT);        // 90..110
  lv_obj_set_pos(st, 18, 90);

  lv_obj_t *fld = mkBox(s, 15, 114, 290, 60, C_NAVY2, 18);                                // 114..174
  lv_obj_set_style_border_width(fld, 3, 0);
  lv_obj_set_style_border_color(fld, C_TEAL, 0);
  lv_obj_t *cc = mkLbl(fld, "+62", &f_fredoka_28, C_LIGHT);
  lv_obj_set_pos(cc, 14, 8);
  mkBox(fld, 64, 12, 2, 30, C_NAVY3, 1);
  mPhoneVal = mkLbl(fld, "", &f_fredoka_28, C_WHITE);
  lv_obj_set_pos(mPhoneVal, 74, 8);
  mPhonePH = mkLbl(fld, "812 3456 7890", &f_fredoka_28, lv_color_hex(0x6F8CAB));
  lv_obj_set_pos(mPhonePH, 74, 8);
  mCaret = mkBox(fld, 74, 10, 3, 32, C_TEAL, 1);
  animStart(mCaret, bgOpaCb, 255, 0, 500, 0, true, 500);

  lv_obj_t *wa = mkBox(s, 15, 180, 32, 32, C_MINT, LV_RADIUS_CIRCLE);                     // 180..212
  lv_obj_t *rg = mkBox(wa, 0, 0, 26, 26, C_WHITE, LV_RADIUS_CIRCLE);
  lv_obj_set_style_bg_opa(rg, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rg, 2, 0);
  lv_obj_set_style_border_color(rg, C_MINT, 0);
  animStart(rg, rippleSmallCb, 0, 100, 1800, 0, true);
  lv_obj_t *wi = mkLbl(wa, LV_SYMBOL_ENVELOPE, &lv_font_montserrat_16, C_NAVY);
  lv_obj_center(wi);
  lv_obj_t *wt = mkLbl(s, "Kode dikirim lewat WhatsApp", &f_nunito_14, C_LIGHT);
  lv_obj_set_pos(wt, 56, 188);

  mPhoneBtn = mkPill(s, "Cek Member", 15, 214, 290, 38, C_NAVY3, C_LIGHT, &f_fredoka_20, evtPhoneGo, NULL);   // 214..252
  mPhoneBtnLbl = lv_obj_get_child(mPhoneBtn, 0);
  mkKeypad(s, 15, 260, 90, 48, 10, evtKeyPhone);   // 4 rows of 48 + gaps: y 260..470 (gap is 10 sideways, 6 vertically below)
  phoneRefresh();
  return s;
}

// ---------------------------------------- Checking
static uint32_t chkStart = 0;
static bool chkFound = false;
static lv_obj_t *chkSk[2], *chkScan, *chkOrb, *chkName, *chkTier, *chkPh, *chkOk, *chkTitle, *chkSub, *chkBtn, *chkDot[3];

static void shimCb(void *var, int32_t x) { lv_obj_set_x((lv_obj_t *)var, x); }
static void orbCb(void *var, int32_t a) {
  lv_obj_set_pos((lv_obj_t *)var, 145 + ((lv_trigo_cos((int16_t)a) * 105) >> 15) - 14, 112 + ((lv_trigo_sin((int16_t)a) * 42) >> 15) - 14);
}
static void hopCb(void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, 345 - v); }
static void evtSendCode(lv_event_t *) { showScreen(SC_OTP); }

static void checkFoundApply() {
  chkFound = true;
  for (int i = 0; i < 2; i++) { lv_anim_del(lv_obj_get_child(chkSk[i], 0), NULL); lv_obj_add_flag(chkSk[i], LV_OBJ_FLAG_HIDDEN); }
  lv_anim_del(chkScan, NULL); lv_obj_add_flag(chkScan, LV_OBJ_FLAG_HIDDEN);
  lv_anim_del(chkOrb, NULL);  lv_obj_add_flag(chkOrb, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < 3; i++) { lv_anim_del(chkDot[i], NULL); lv_obj_add_flag(chkDot[i], LV_OBJ_FLAG_HIDDEN); }
  lv_obj_clear_flag(chkName, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(chkTier, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(chkPh, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(chkOk, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(chkBtn, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(chkTitle, "Member ditemukan!");
  lv_obj_set_style_text_color(chkTitle, C_MINT, 0);
  lv_label_set_text(chkSub, "Halo, " MEMBER_NAME "! Kami kirim kode\nke WhatsApp kamu.");
}

static lv_obj_t *buildCheck() {
  lv_obj_t *s = mkScreen(C_NAVY);
  addBubbles(s, 4, lv_color_hex(0x7FD8E2), 5, 290, 480, 470, 6000, 10000, 10, 22);
  mkHeader(s, SC_PHONE, "Member");

  lv_obj_t *card = mkBox(s, 15, 60, 290, 170, C_TEAL, 24);                                 // 60..230
  lv_obj_set_style_clip_corner(card, true, 0);
  mkLogo(card, 14, 12, 30, false);
  lv_obj_t *wm = mkLbl(card, "washable", &f_fredoka_20, C_NAVY);
  lv_obj_set_pos(wm, 52, 11);
  for (int i = 0; i < 2; i++) {                                                            // skeleton lines with shimmer
    int w = i ? 140 : 200, h = i ? 18 : 26;
    chkSk[i] = mkBox(card, 16, 62 + i * 36, w, h, lv_color_hex(0x0A8FA0), 9);
    lv_obj_set_style_clip_corner(chkSk[i], true, 0);
    lv_obj_t *sh = mkBox(chkSk[i], -50, 0, 50, h, C_WHITE, 0);
    lv_obj_set_style_bg_opa(sh, LV_OPA_50, 0);
    animStart(sh, shimCb, -50, w, 1300, i * 200, true);
  }
  chkScan = mkBox(card, 0, 52, 290, 4, C_WHITE, 2);
  animStart(chkScan, posYCb, 52, 160, 1500, 0, true, 1500);
  chkOrb = mkBox(card, 0, 0, 28, 28, C_WHITE, LV_RADIUS_CIRCLE);                          // magnifier orbiting the card
  lv_obj_set_style_bg_opa(chkOrb, LV_OPA_20, 0);
  lv_obj_set_style_border_width(chkOrb, 3, 0);
  lv_obj_set_style_border_color(chkOrb, C_WHITE, 0);
  animStart(chkOrb, orbCb, 0, 359, 3000, 0, true);

  chkName = mkLbl(card, MEMBER_FULL, &f_fredoka_36, C_NAVY);
  lv_obj_set_pos(chkName, 16, 56);
  chkTier = mkLbl(card, MEMBER_TIER, &f_nunito_16, C_NAVY);
  lv_obj_set_pos(chkTier, 16, 108);
  char ph[40];
  fmtPhone(ph, sizeof ph, 2);
  chkPh = mkLbl(card, ph, &f_nunito_14, C_NAVY);
  lv_obj_set_pos(chkPh, 16, 134);
  chkOk = mkBox(card, 222, 12, 52, 52, C_MINT, LV_RADIUS_CIRCLE);
  lv_obj_t *ck = mkLbl(chkOk, LV_SYMBOL_OK, &lv_font_montserrat_28, C_NAVY);
  lv_obj_center(ck);
  lv_obj_add_flag(chkName, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(chkTier, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(chkPh, LV_OBJ_FLAG_HIDDEN);   lv_obj_add_flag(chkOk, LV_OBJ_FLAG_HIDDEN);

  chkTitle = mkLblC(s, "Memeriksa member", &f_fredoka_28, C_WHITE, 250);                   // 250..285
  char sub[64];
  fmtPhone(ph, sizeof ph, 1);
  snprintf(sub, sizeof sub, "Nomor %s", ph);
  chkSub = mkLblC(s, sub, &f_nunito_16, C_LIGHT, 292);                                     // 292..338
  for (int i = 0; i < 3; i++) {
    chkDot[i] = mkBox(s, 140 + i * 18, 345, 12, 12, C_TEAL, LV_RADIUS_CIRCLE);
    animStart(chkDot[i], hopCb, 345, 335, 450, i * 150, true, 450);
  }
  chkBtn = mkPill(s, "Kirim kode WhatsApp", 30, 392, 260, 56, C_MINT, C_NAVY, &f_fredoka_20, evtSendCode, NULL);
  lv_obj_add_flag(chkBtn, LV_OBJ_FLAG_HIDDEN);
  chkFound = false;
  chkStart = millis();
  return s;
}

// ---------------------------------------- Code (OTP)
static uint32_t otpStart = 0;
static int otpLast = -1;
static lv_obj_t *mBox[6], *mBoxLbl[6], *mOtpBtn, *mOtpBtnLbl, *mOtpMsg, *mOtpResend;

static void otpRefresh() {
  size_t n = strlen(mOtp);
  for (int i = 0; i < 6; i++) {
    char t[2] = { i < (int)n ? mOtp[i] : ' ', 0 };
    lv_label_set_text(mBoxLbl[i], t);
    lv_obj_center(mBoxLbl[i]);
    lv_obj_set_style_border_color(mBox[i], i == (int)n && n < 6 ? C_TEAL : (i < (int)n ? C_BLUE : C_NAVY3), 0);
  }
  bool ready = n == 6;
  lv_obj_set_style_bg_color(mOtpBtn, ready ? C_TEAL : C_NAVY3, 0);
  lv_obj_set_style_text_color(mOtpBtnLbl, ready ? C_NAVY : C_LIGHT, 0);
}
static void evtKeyOtp(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);
  size_t n = strlen(mOtp);
  lv_label_set_text(mOtpMsg, "");
  if (k == 10) mOtp[0] = 0;
  else if (k == 11) { if (n) mOtp[n - 1] = 0; }
  else if (n < 6) { mOtp[n] = '0' + k; mOtp[n + 1] = 0; }
  otpRefresh();
}
static void evtOtpGo(lv_event_t *) {
  if (strlen(mOtp) != 6) return;
  if (!strcmp(mOtp, MEMBER_DEMO_OTP)) { memberActive = true; showScreen(SC_MHOME); }
  else {
    lv_label_set_text(mOtpMsg, "Kode salah, coba lagi");
    mOtp[0] = 0;
    otpRefresh();
    ledFlash(LC_RED, 2);
  }
}
static void evtResend(lv_event_t *) {
  if (millis() - otpStart >= OTP_RESEND_SEC * 1000UL) { otpStart = millis(); otpLast = -1; lv_label_set_text(mOtpMsg, "Kode dikirim ulang"); }
}
static void otpTick(uint32_t now) {
  int left = OTP_RESEND_SEC - (int)((now - otpStart) / 1000);
  if (left < 0) left = 0;
  if (left != otpLast) {
    otpLast = left;
    char t[48];
    if (left) snprintf(t, sizeof t, "Kirim ulang kode dalam %d:%02d", left / 60, left % 60);
    else snprintf(t, sizeof t, "Tekan di sini untuk kirim ulang");
    lv_label_set_text(mOtpResend, t);
  }
}
static void dotHopCb(void *var, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)(100 + v * 155 / 100), 0);
}

static lv_obj_t *buildOtp() {
  lv_obj_t *s = mkScreen(C_NAVY);
  addBubbles(s, 3, lv_color_hex(0x7FD8E2), 5, 290, 480, 470, 6000, 10000, 10, 22);
  mkHeader(s, SC_PHONE, "Member");
  lv_obj_t *t = mkLbl(s, "Masukkan kode", &f_fredoka_28, C_WHITE);                          // 54..89
  lv_obj_set_pos(t, 16, 54);
  char ph[40], sub[96];
  fmtPhone(ph, sizeof ph, 1);
  snprintf(sub, sizeof sub, "6 digit dikirim ke WhatsApp\n%s", ph);
  lv_obj_t *st = mkLbl(s, sub, &f_nunito_14, C_LIGHT);                                       // 90..130
  lv_obj_set_pos(st, 18, 90);
  lv_obj_t *dm = mkLbl(s, "Demo: " MEMBER_DEMO_OTP, &f_nunito_14, C_MUTED);
  lv_obj_align(dm, LV_ALIGN_TOP_RIGHT, -14, 64);

  for (int i = 0; i < 6; i++) {                                                              // y 138..190
    mBox[i] = mkBox(s, 15 + i * 49, 138, 42, 52, C_NAVY2, 12);
    lv_obj_set_style_border_width(mBox[i], 3, 0);
    lv_obj_set_style_border_color(mBox[i], C_NAVY3, 0);
    mBoxLbl[i] = mkLbl(mBox[i], " ", &f_fredoka_28, C_WHITE);
    lv_obj_center(mBoxLbl[i]);
  }
  mOtpMsg = mkLbl(s, "", &f_nunito_14, lv_color_hex(0xFF6B6B));                              // 194..214
  lv_obj_set_pos(mOtpMsg, 18, 194);
  lv_obj_t *rs = mkClickBox(s, 15, 214, 290, 30, C_NAVY, 15, evtResend, NULL);               // 214..244
  lv_obj_set_style_bg_opa(rs, LV_OPA_TRANSP, 0);
  lv_obj_t *bub = mkBox(rs, 0, 3, 46, 24, C_MINT, 12);
  for (int i = 0; i < 3; i++) {
    lv_obj_t *d = mkBox(bub, 9 + i * 11, 9, 7, 7, C_NAVY, LV_RADIUS_CIRCLE);
    animStart(d, dotHopCb, 0, 100, 450, i * 150, true, 450);
  }
  mOtpResend = mkLbl(rs, "", &f_nunito_14, C_LIGHT);
  lv_obj_set_pos(mOtpResend, 56, 5);

  mOtpBtn = mkPill(s, "Verifikasi", 15, 248, 290, 38, C_NAVY3, C_LIGHT, &f_fredoka_20, evtOtpGo, NULL);   // 250..288
  mOtpBtnLbl = lv_obj_get_child(mOtpBtn, 0);
  mkKeypad(s, 15, 294, 90, 40, 8, evtKeyOtp, 6);                                                           // 294..472
  mOtp[0] = 0;
  otpStart = millis();
  otpLast = -1;
  otpRefresh();
  otpTick(millis());
  return s;
}

// ---------------------------------------- Member home (points, coupons, vouchers)
struct MItem { const char *value, *title, *note; uint32_t col; int cost; };
static const MItem KUPON[3] = {
  { "3rb",    "Diskon Rp 3.000",   "Cuci · 30 Nov",      0x3FA9F5, 0 },
  { "Gratis", "Kering 10 menit",   "Kering · 15 Des", 0xFF9F43, 0 },
  { "10%",    "Diskon 10%",        "Semua · 30 Nov",     0x00B4C5, 0 },
};
static const MItem TUKAR[3] = {
  { "5rb",    "Voucher Rp 5.000",  "300 poin",  0x3FA9F5, 300 },
  { "Gratis", "Cuci Hemat gratis", "600 poin",  0x2ECC71, 600 },
  { "10rb",   "Voucher Rp 10.000", "900 poin",  0xFF9F43, 900 },
};
static lv_obj_t *mhPts, *mhBar, *mhGold, *mhTabK, *mhTabT, *mhTabKL, *mhTabTL;
static lv_obj_t *mhStub[3], *mhStubL[3], *mhTitle[3], *mhNote[3], *mhBtn[3], *mhBtnL[3];

static void fmtInt(char *out, size_t n, int v) {
  char tmp[16];
  snprintf(tmp, sizeof tmp, "%d", v);
  int len = strlen(tmp), j = 0;
  char g[24];
  for (int i = 0; i < len && j < 22; i++) { if (i && (len - i) % 3 == 0) g[j++] = g_lang == LANG_EN ? ',' : '.'; g[j++] = tmp[i]; }
  g[j] = 0;
  snprintf(out, n, "%s", g);
}

static void mhRefresh() {
  char t[32];
  fmtInt(t, sizeof t, mPoints);
  lv_label_set_text(mhPts, t);
  lv_bar_set_value(mhBar, mPoints * 100 / 1500 > 100 ? 100 : mPoints * 100 / 1500, LV_ANIM_OFF);
  fmtInt(t, sizeof t, mPoints >= 1500 ? 0 : 1500 - mPoints);
  char g[48];
  snprintf(g, sizeof g, "%s poin lagi ke Gold", t);
  lv_label_set_text(mhGold, g);
  bool k = mTab == 0;
  lv_obj_set_style_bg_color(mhTabK, k ? C_TEAL : C_NAVY2, 0);
  lv_obj_set_style_bg_color(mhTabT, k ? C_NAVY2 : C_TEAL, 0);
  lv_obj_set_style_text_color(mhTabKL, k ? C_NAVY : C_TXT, 0);
  lv_obj_set_style_text_color(mhTabTL, k ? C_TXT : C_NAVY, 0);
  for (int i = 0; i < 3; i++) {
    const MItem &it = k ? KUPON[i] : TUKAR[i];
    lv_obj_set_style_bg_color(mhStub[i], lv_color_hex(it.col), 0);
    lv_label_set_text(mhStubL[i], it.value);
    lv_obj_center(mhStubL[i]);
    lv_label_set_text(mhTitle[i], it.title);
    lv_label_set_text(mhNote[i], (!k && mClaimed[i]) ? "Voucher siap dipakai" : it.note);
    const char *bt; lv_color_t bg, fg = C_NAVY;
    if (k) { bool u = mUsed[i]; bt = u ? "Dipakai" : "Pakai"; bg = u ? C_MINT : C_TEAL; }
    else if (mClaimed[i]) { bt = "Diklaim"; bg = C_MINT; }
    else if (mPoints >= it.cost) { bt = "Klaim"; bg = C_ORANGE; }
    else { bt = "Kurang"; bg = C_NAVY3; fg = C_LIGHT; }
    lv_obj_set_style_bg_color(mhBtn[i], bg, 0);
    lv_label_set_text(mhBtnL[i], bt);
    lv_obj_set_style_text_color(mhBtnL[i], fg, 0);
    lv_obj_center(mhBtnL[i]);
  }
}
static void evtTab(lv_event_t *e) { mTab = (int)(intptr_t)lv_event_get_user_data(e); mhRefresh(); }
static void evtItem(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (mTab == 0) mUsed[i] = true;
  else if (!mClaimed[i] && mPoints >= TUKAR[i].cost) { mPoints -= TUKAR[i].cost; mClaimed[i] = true; }
  mhRefresh();
}
static void evtMemberExit(lv_event_t *) { showScreen(SC_IDLE); }
static void evtMemberNext(lv_event_t *) { showScreen(SC_CYCLE); }

static lv_obj_t *buildMemberHome() {
  lv_obj_t *s = mkScreen(C_NAVY);
  addBubbles(s, 3, lv_color_hex(0x7FD8E2), 5, 290, 480, 470, 6000, 10000, 10, 22);
  mkLogo(s, 16, 12, 30, false);
  mkWordmark(s, 52, 11, &f_fredoka_20);
  mkPill(s, "Keluar", 236, 8, 72, 34, C_NAVY3, C_TXT, &f_nunito_14, evtMemberExit, NULL);

  lv_obj_t *card = mkBox(s, 15, 52, 290, 138, C_NAVY2, 24);                                   // 52..190
  mkCoin(card, 14, 14, 62);
  lv_obj_t *hi = mkLbl(card, "Halo, " MEMBER_NAME "!", &f_nunito_14, C_LIGHT);
  lv_obj_set_pos(hi, 88, 8);
  mhPts = mkLbl(card, "", &f_fredoka_36, C_WHITE);
  lv_obj_set_pos(mhPts, 88, 24);
  lv_obj_t *chip = mkBox(card, 88, 74, 118, 24, lv_color_hex(0xC9D6E3), 12);
  lv_obj_t *cl = mkLbl(chip, MEMBER_TIER, &f_nunito_14, C_NAVY);
  lv_obj_center(cl);
  mhBar = lv_bar_create(card);
  lv_obj_set_size(mhBar, 262, 10);
  lv_obj_set_pos(mhBar, 14, 108);
  lv_bar_set_range(mhBar, 0, 100);
  lv_obj_set_style_bg_color(mhBar, C_NAVY3, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(mhBar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(mhBar, C_BLUE, LV_PART_INDICATOR);
  lv_obj_set_style_radius(mhBar, 5, LV_PART_MAIN);
  lv_obj_set_style_radius(mhBar, 5, LV_PART_INDICATOR);
  lv_obj_t *sh = mkBox(card, 14, 108, 30, 10, C_WHITE, 5);                                    // shimmer over the bar
  lv_obj_set_style_bg_opa(sh, LV_OPA_30, 0);
  animStart(sh, shimCb, 14, 246, 1800, 0, true);
  mhGold = mkLbl(card, "", &f_nunito_14, C_LIGHT);
  lv_obj_set_pos(mhGold, 14, 118);

  mhTabK = mkClickBox(s, 15, 198, 142, 38, C_TEAL, 19, evtTab, (void *)(intptr_t)0);          // 198..236
  mhTabKL = mkLbl(mhTabK, "Kupon saya", &f_nunito_16, C_NAVY); lv_obj_center(mhTabKL);
  mhTabT = mkClickBox(s, 163, 198, 142, 38, C_NAVY2, 19, evtTab, (void *)(intptr_t)1);
  mhTabTL = mkLbl(mhTabT, "Tukar poin", &f_nunito_16, C_WHITE); lv_obj_center(mhTabTL);

  for (int i = 0; i < 3; i++) {                                                               // rows 244..434
    lv_obj_t *row = mkBox(s, 15, 244 + i * 66, 290, 58, C_NAVY2, 16);
    lv_obj_set_style_clip_corner(row, true, 0);
    mhStub[i] = mkBox(row, 0, 0, 68, 58, C_BLUE, 0);
    mhStubL[i] = mkLbl(mhStub[i], "", &f_fredoka_20, C_NAVY);
    mkBox(row, 68, 0, 3, 58, C_NAVY, 0);
    mhTitle[i] = mkLbl(row, "", &f_nunito_14, C_WHITE);
    lv_label_set_long_mode(mhTitle[i], LV_LABEL_LONG_DOT);
    lv_obj_set_size(mhTitle[i], 128, 20);
    lv_obj_set_pos(mhTitle[i], 80, 8);
    mhNote[i] = mkLbl(row, "", &f_nunito_14, C_MUTED);
    lv_label_set_long_mode(mhNote[i], LV_LABEL_LONG_DOT);
    lv_obj_set_size(mhNote[i], 128, 20);
    lv_obj_set_pos(mhNote[i], 80, 30);
    mhBtn[i] = mkClickBox(row, 212, 11, 70, 36, C_TEAL, 18, evtItem, (void *)(intptr_t)i);
    mhBtnL[i] = mkLbl(mhBtn[i], "", &f_nunito_14, C_NAVY);
  }
  mkPill(s, "Lanjut pilih program", 15, 440, 290, 36, C_TEAL, C_NAVY, &f_fredoka_20, evtMemberNext, NULL);   // 440..476
  mhRefresh();
  return s;
}

// ============================== SCREEN: IDLE ==============================
static lv_obj_t *idleWifi = NULL;
static void wifiIconUpdate() {
  if (idleWifi && curScr == SC_IDLE)
    lv_obj_set_style_bg_color(idleWifi, WiFi.status() == WL_CONNECTED ? C_MINT : C_ORANGE, 0);
}

static lv_obj_t *idleJob[2], *idleJobL[2];
static lv_obj_t *menuBack = NULL, *menuCard = NULL;
static void evtMenu(lv_event_t *) {
  if (!menuCard) return;
  bool open = lv_obj_has_flag(menuCard, LV_OBJ_FLAG_HIDDEN);
  if (open) { lv_obj_clear_flag(menuCard, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(menuBack, LV_OBJ_FLAG_HIDDEN); }
  else { lv_obj_add_flag(menuCard, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(menuBack, LV_OBJ_FLAG_HIDDEN); }
}

// language toggle (ID | EN), top right of the first page
static void evtTheme(lv_event_t *e) {
  (void)e;
  cfg.theme ^= 1;
  g_light = cfg.theme == 1;
  saveCfg();
  showScreen(curScr);                        // rebuild the page with the new palette
}
static void evtLang(lv_event_t *e) {
  uint8_t l = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  if (l == cfg.lang) return;
  cfg.lang = l;
  g_lang = l;
  updateMachineName();
  saveCfg();
  showScreen(SC_IDLE);                       // rebuild the page in the new language
}
static void idleJobsUpdate(uint32_t now) {
  for (int k = 0; k < 2; k++) {
    if (jobs[k].st == JS_FREE) { lv_obj_add_flag(idleJob[k], LV_OBJ_FLAG_HIDDEN); continue; }
    char t[32];
    if (jobs[k].st == JS_RUN) { int r = jobRemainSec(k, now); snprintf(t, sizeof t, "%s %02d:%02d", tr(k ? "Kering" : "Cuci"), r / 60, r % 60); }
    else snprintf(t, sizeof t, "%s selesai", tr(k ? "Kering" : "Cuci"));
    lv_label_set_text(idleJobL[k], t);
    lv_obj_center(idleJobL[k]);
    lv_obj_clear_flag(idleJob[k], LV_OBJ_FLAG_HIDDEN);
  }
}
static void evtIdleJob(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);
  svc = k;
  showScreen(jobs[k].st == JS_RUN ? SC_RUN : SC_DONE);
}

static lv_obj_t *buildIdle() {
  lv_obj_t *s = mkScreen(lv_color_hex(0x0F2B50));
  addBubbles(s, 9, lv_color_hex(0x7FD8E2), 5, 290, 440, 420, 5500, 9500, 10, 30);   // behind the content
  mkLogo(s, 16, 12, 30, false);
  mkWordmark(s, 52, 11, &f_fredoka_20);
  lv_obj_t *gear = mkClickBox(s, 272, 9, 36, 36, C_NAVY3, 18, evtMenu, NULL);          // opens the menu: theme, language, information, settings
  lv_obj_set_style_bg_color(gear, C_NAVY2, LV_STATE_PRESSED);
  lv_obj_t *gl = mkLbl(gear, LV_SYMBOL_SETTINGS, &lv_font_montserrat_16, C_LIGHT);
  lv_obj_center(gl);
  idleWifi = mkBox(gear, 27, 2, 9, 9, WiFi.status() == WL_CONNECTED ? C_MINT : C_ORANGE, LV_RADIUS_CIRCLE);   // Wi-Fi state dot

  mkLogo(s, 98, 54, 124, true);                                                       // 54..178
  mkLblC(s, "Cucian kotor?\nGampang.", &f_fredoka_28, C_WHITE, 186);                   // 186..256
  mkLblC(s, "Cuci atau keringkan sendiri,\nbayar pakai QRIS.", &f_nunito_16, C_LIGHT, 262);   // 262..308

  lv_obj_t *cta = mkClickBox(s, 35, 316, 250, 54, C_TEAL, 27, evtGo, (void *)(intptr_t)SC_SERVICE);   // 316..370: only this button continues
  lv_obj_set_style_bg_opa(cta, LV_OPA_80, LV_STATE_PRESSED);
  lv_obj_t *cl = mkLbl(cta, "Sentuh untuk mulai", &f_fredoka_20, C_NAVY);
  lv_obj_align(cl, LV_ALIGN_LEFT_MID, 22, 0);
  lv_obj_t *ar = mkLbl(cta, LV_SYMBOL_RIGHT, &lv_font_montserrat_20, C_NAVY);
  lv_obj_align(ar, LV_ALIGN_RIGHT_MID, -20, 0);
  animStart(ar, transXCb, 0, 8, 500, 0, true, 500);

  if (cfg.owner[0]) {                                                                  // 382..402
    char ow[64];
    snprintf(ow, sizeof ow, "Dikelola oleh %s", cfg.owner);
    lv_obj_t *o = mkLblC(s, ow, &f_nunito_14, C_MUTED, 382);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    lv_obj_set_height(o, 20);
  }

  // waves: crest tops are at y >= 410, below the button and the owner line
  mkWave(s, 0, 440, SCREEN_W, SCREEN_H, lv_color_hex(0x0C5F81), 48, 6, 5200);
  mkWave(s, 0, 456, SCREEN_W, SCREEN_H, lv_color_hex(0x2B73B0), 48, 6, 3400);

  addBubbles(s, 8, lv_color_hex(0x7FD8E2), 5, 290, 478, 66, 3000, 5500, 8, 22);    // small ones close to the surface
  for (int k = 0; k < 2; k++) {                                                        // running machines: 414..446
    idleJob[k] = mkClickBox(s, 12 + k * 150, 414, 146, 32, k ? C_ORANGE : C_BLUE, 16, evtIdleJob, (void *)(intptr_t)k);
    idleJobL[k] = mkLbl(idleJob[k], "", &f_nunito_14, C_NAVY);
  }
  idleJobsUpdate(millis());

  // dropdown menu (hidden): a transparent backdrop closes it, the card holds the four actions
  menuBack = mkClickBox(s, 0, 0, SCREEN_W, SCREEN_H, C_NAVY, 0, evtMenu, NULL);
  lv_obj_set_style_bg_opa(menuBack, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(menuBack, LV_OBJ_FLAG_HIDDEN);
  menuCard = mkBox(s, 140, 50, 168, 172, C_NAVY2, 16);
  lv_obj_set_style_border_width(menuCard, g_light ? 0 : 1, 0);
  lv_obj_set_style_border_color(menuCard, C_NAVY3, 0);
  lv_obj_add_flag(menuCard, LV_OBJ_FLAG_HIDDEN);
  struct Row { const char *txt; lv_event_cb_t cb; void *ud; };
  Row rows[4] = {
    { g_light ? "Mode gelap" : "Mode terang", evtTheme, NULL },
    { cfg.lang ? "Indonesia" : "English", evtLang, (void *)(intptr_t)(1 - cfg.lang) },
    { "Informasi", evtGo, (void *)(intptr_t)SC_ABOUT },
    { "Pengaturan", evtGo, (void *)(intptr_t)SC_PIN },
  };
  for (int k = 0; k < 4; k++) {
    lv_obj_t *r = mkClickBox(menuCard, 6, 6 + k * 40, 156, 38, C_NAVY2, 12, rows[k].cb, rows[k].ud);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(r, C_NAVY3, LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(r, 0, 0);
    lv_obj_t *ic = mkBox(r, 8, 6, 26, 26, C_NAVY3, LV_RADIUS_CIRCLE);                  // icon disc
    lv_obj_set_style_outline_width(ic, 0, 0);
    if (k == 0) {
      if (g_light) { mkBox(ic, 5, 5, 16, 16, C_MUTED, LV_RADIUS_CIRCLE); mkBox(ic, 10, 2, 14, 14, C_NAVY3, LV_RADIUS_CIRCLE); }
      else { mkBox(ic, 8, 8, 10, 10, C_ORANGE, LV_RADIUS_CIRCLE);
             for (int j = 0; j < 8; j++) { float a = j * 0.7854f; mkBox(ic, 12 + (int)(9 * cosf(a)), 12 + (int)(9 * sinf(a)), 2, 2, C_ORANGE, LV_RADIUS_CIRCLE); } }
    } else {
      lv_obj_t *g = k == 1 ? mkLbl(ic, cfg.lang ? "ID" : "EN", &f_nunito_14, C_TEAL)
                  : k == 2 ? mkLbl(ic, "i", &f_fredoka_20, C_TEAL)
                           : mkLbl(ic, LV_SYMBOL_SETTINGS, &lv_font_montserrat_14, C_LIGHT);
      lv_obj_center(g);
    }
    lv_obj_t *tl = mkLbl(r, rows[k].txt, &f_nunito_14, C_WHITE);
    lv_obj_set_pos(tl, 42, 9);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(tl, 112, 20);
  }

  return s;
}

// ============================== SCREEN: SERVICE ==============================
static lv_obj_t *svcBadge[2], *svcBadgeL[2];

static void svcBadgeUpdate(uint32_t now) {
  for (int k = 0; k < 2; k++) {
    if (jobs[k].st == JS_FREE) { lv_obj_add_flag(svcBadge[k], LV_OBJ_FLAG_HIDDEN); continue; }
    char t[24];
    if (jobs[k].st == JS_RUN) { int r = jobRemainSec(k, now); snprintf(t, sizeof t, "Berjalan %02d:%02d", r / 60, r % 60); }
    else snprintf(t, sizeof t, "Selesai · ambil");
    lv_label_set_text(svcBadgeL[k], t);
    lv_obj_center(svcBadgeL[k]);
    lv_obj_clear_flag(svcBadge[k], LV_OBJ_FLAG_HIDDEN);
  }
}

static void evtService(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);
  svc = k;
  pick = 1;
  if (jobs[k].st != JS_FREE) { showScreen(jobs[k].st == JS_RUN ? SC_RUN : SC_DONE); return; }   // machine in use: show its progress
  showScreen(memberActive ? SC_CYCLE : SC_MASK);
}

static lv_obj_t *buildService() {
  lv_obj_t *s = mkScreen(C_NAVY);
  addBubbles(s, 6, lv_color_hex(0x7FD8E2), 5, 290, 480, 470, 6000, 10000, 10, 26);
  mkHeader(s, SC_IDLE, machineName);
  mkLblC(s, "Mau diapakan\nhari ini?", &f_fredoka_28, C_WHITE, 62);

  for (int i = 0; i < 2; i++) {
    lv_color_t col = i ? C_ORANGE : C_BLUE;
    int y = 148 + i * 162;
    lv_obj_t *card = mkClickBox(s, 15, y, 290, 150, col, 28, evtService, (void *)(intptr_t)i);
    lv_obj_set_style_bg_opa(card, LV_OPA_80, LV_STATE_PRESSED);
    int g = 96;
    lv_obj_t *glass = mkDrum(card, 14, 36, g + 8, 4, C_NAVY, i ? lv_color_hex(0xFFF1DE) : lv_color_hex(0xE6F4FE), NULL, 0);
    if (!i) mkWave(glass, 0, g * 56 / 100, g, g, C_BLUE, 26, 5, 1800);
    addClothes(glass, g, i ? CLOTH_DRY : CLOTH_WASH, i ? 4200 : 5200);
    if (i) {
      static const int xs[3] = { 28, 52, 76 };
      addHeat(card, 3, xs, 18, 16, C_WHITE);
    }
    lv_obj_t *nm = mkLbl(card, i ? "Keringkan" : "Cuci", i ? &f_fredoka_28 : &f_fredoka_36, C_NAVY);
    lv_obj_set_pos(nm, 132, i ? 42 : 36);
    lv_obj_t *sub = mkLbl(card, i ? "Mulai dari\nRp 10.000" : "Mulai dari\nRp 12.000", &f_nunito_16, C_NAVY);
    lv_obj_set_pos(sub, 132, 88);
    svcBadge[i] = mkBox(card, 126, 8, 150, 24, C_NAVY, 12);
    lv_obj_clear_flag(svcBadge[i], LV_OBJ_FLAG_CLICKABLE);
    svcBadgeL[i] = mkLbl(svcBadge[i], "", &f_nunito_14, C_WHITE);
  }
  svcBadgeUpdate(millis());
  return s;
}

// ============================== SCREEN: CYCLE ==============================
static lv_obj_t *cycOpt[3], *cycName[3], *cycDetail[3], *cycPrice[3];
static lv_obj_t *cycTogW, *cycTogD, *cycTogWL, *cycTogDL, *cycTemp, *cycTotal, *cycMerc, *cycGlow, *cycBulb, *cycPay, *cycPayL;
static int mercBase = 40;

static void mercCb(void *v, int32_t x) {
  int h = mercBase * 32 / 100 + x;
  lv_obj_set_height(cycMerc, h);
  lv_obj_set_y(cycMerc, 46 - h);
}

static void refreshCycle() {
  const Opt *o = svc ? DRY_OPTS : WASH_OPTS;
  lv_color_t ac = accentCol();
  for (int i = 0; i < 3; i++) {
    bool sel = (i == pick);
    lv_label_set_text(cycName[i], o[i].name);
    lv_label_set_text(cycDetail[i], o[i].detail);
    lv_label_set_text(cycPrice[i], o[i].price);
    lv_obj_set_style_bg_color(cycOpt[i], sel ? C_NAVY3 : C_NAVY2, 0);
    lv_obj_set_style_border_color(cycOpt[i], ac, 0);
    lv_obj_set_style_border_width(cycOpt[i], sel ? 3 : 0, 0);
    lv_obj_align(cycPrice[i], LV_ALIGN_RIGHT_MID, -16, 0);
  }
  lv_obj_set_style_bg_color(cycTogW, svc ? C_NAVY2 : C_BLUE, 0);
  lv_obj_set_style_bg_color(cycTogD, svc ? C_ORANGE : C_NAVY2, 0);
  lv_obj_set_style_text_color(cycTogWL, svc ? C_TXT : C_NAVY, 0);
  lv_obj_set_style_text_color(cycTogDL, svc ? C_NAVY : C_TXT, 0);
  char t[8];
  snprintf(t, sizeof t, "%u°", o[pick].temp);
  lv_label_set_text(cycTemp, t);
  lv_obj_set_style_text_color(cycTemp, ac, 0);
  lv_label_set_text(cycTotal, o[pick].price);
  lv_obj_align(cycTotal, LV_ALIGN_TOP_RIGHT, -16, 26);
  lv_obj_set_style_bg_color(cycMerc, ac, 0);
  lv_obj_set_style_bg_color(cycBulb, ac, 0);
  lv_obj_set_style_bg_color(cycGlow, ac, 0);
  lv_obj_set_style_bg_color(cycPay, controllerReady() ? ac : C_NAVY3, 0);
  mercBase = o[pick].lvl;
}

static void evtPayGo(lv_event_t *) { if (controllerReady()) showScreen(SC_PAY); }     // never take money for a machine that cannot start

// "Bayar QRIS" is disabled while the relay controller does not answer (not in demo mode)
static void cycPayRefresh() {
  static int lastReady = -1;
  int r = controllerReady();
  if (r == lastReady) return;
  lastReady = r;
  lv_obj_set_style_bg_color(cycPay, r ? accentCol() : C_NAVY3, 0);
  lv_obj_t *l = lv_obj_get_child(cycPay, 0);
  lv_label_set_text(l, r ? "Bayar QRIS" : "Mesin belum siap");
  lv_obj_set_style_text_color(l, r ? C_NAVY : C_MUTED, 0);
  lv_obj_center(l);
}
static void evtPick(lv_event_t *e) { pick = (int)(intptr_t)lv_event_get_user_data(e); refreshCycle(); }
static void evtMode(lv_event_t *e) { svc = (int)(intptr_t)lv_event_get_user_data(e); refreshCycle(); }

static lv_obj_t *buildCycle() {
  lv_obj_t *s = mkScreen(C_NAVY);
  addBubbles(s, 5, lv_color_hex(0x7FD8E2), 5, 290, 480, 470, 6000, 10000, 10, 24);
  mkHeader(s, SC_SERVICE, machineName);

  lv_obj_t *tog = mkBox(s, 50, 60, 220, 44, C_NAVY2, 22);
  cycTogW = mkClickBox(tog, 4, 4, 104, 36, C_BLUE, 18, evtMode, (void *)(intptr_t)0);
  cycTogWL = mkLbl(cycTogW, "Cuci", &f_fredoka_20, C_NAVY); lv_obj_center(cycTogWL);
  cycTogD = mkClickBox(tog, 112, 4, 104, 36, C_NAVY2, 18, evtMode, (void *)(intptr_t)1);
  cycTogDL = mkLbl(cycTogD, "Keringkan", &f_fredoka_20, C_WHITE); lv_obj_center(cycTogDL);

  for (int i = 0; i < 3; i++) {
    cycOpt[i] = mkClickBox(s, 15, 114 + i * 74, 290, 68, C_NAVY2, 20, evtPick, (void *)(intptr_t)i);
    cycName[i] = mkLbl(cycOpt[i], "", &f_fredoka_20, C_WHITE);
    lv_obj_set_pos(cycName[i], 16, 8);
    cycDetail[i] = mkLbl(cycOpt[i], "", &f_nunito_14, C_LIGHT);
    lv_obj_set_pos(cycDetail[i], 16, 38);
    cycPrice[i] = mkLbl(cycOpt[i], "", &f_fredoka_20, C_WHITE);
  }

  lv_obj_t *panel = mkBox(s, 15, 340, 290, 130, C_NAVY2, 22);
  cycGlow = mkBox(panel, 11, 38, 34, 34, C_BLUE, LV_RADIUS_CIRCLE);          // y 38..72
  animStart(cycGlow, bgOpaCb, 40, 120, 1000, 0, true, 1000);
  lv_obj_t *tube = mkBox(panel, 22, 10, 12, 38, C_NAVY, 6);                  // y 10..48
  lv_obj_set_style_border_width(tube, 2, 0);
  lv_obj_set_style_border_color(tube, C_WHITE, 0);
  cycMerc = mkBox(panel, 25, 26, 6, 20, C_BLUE, 3);
  cycBulb = mkBox(panel, 17, 40, 22, 22, C_BLUE, LV_RADIUS_CIRCLE);          // y 40..62
  lv_obj_set_style_border_width(cycBulb, 2, 0);
  lv_obj_set_style_border_color(cycBulb, C_WHITE, 0);
  animStart(cycMerc, mercCb, 0, 4, 900, 0, true, 900);

  cycTemp = mkLbl(panel, "", &f_fredoka_36, C_BLUE);
  lv_obj_set_pos(cycTemp, 56, 10);
  lv_obj_t *tl = mkLbl(panel, "Total bayar", &f_nunito_14, C_LIGHT);
  lv_obj_align(tl, LV_ALIGN_TOP_RIGHT, -16, 8);
  cycTotal = mkLbl(panel, "", &f_fredoka_28, C_WHITE);
  cycPay = mkPill(panel, "Bayar QRIS", 16, 76, 258, 48, C_BLUE, C_NAVY, &f_fredoka_20, evtPayGo, NULL);
  refreshCycle();
  return s;
}

// ============================== SCREEN: PAY (QRIS) ==============================
static uint32_t payStartMs = 0;
static int payLastSec = -1;
static lv_obj_t *payArc, *payTime;

static void evtDemoPaid(lv_event_t *) { showScreen(SC_SUCCESS); }

static void orbitCb(void *var, int32_t v) {
  int16_t cx = 22, cy = 22, r = 24;
  lv_obj_set_pos((lv_obj_t *)var, cx + ((lv_trigo_cos((int16_t)v) * r) >> 15) - 4, cy + ((lv_trigo_sin((int16_t)v) * r) >> 15) - 4);
}

static lv_obj_t *buildPay() {
  lv_obj_t *s = mkScreen(C_NAVY);
  addBubbles(s, 5, lv_color_hex(0x7FD8E2), 5, 290, 480, 470, 6000, 10000, 10, 24);
  mkHeader(s, SC_CYCLE, machineName);

  char line[64];
  const Opt *o = curOpt();
  if (svc) snprintf(line, sizeof line, "Keringkan · %s · %s", tr(o->name), tr(o->detail));
  else     snprintf(line, sizeof line, "Cuci · %s · %s", tr(o->name), tr(o->detail));
  mkLblC(s, line, &f_nunito_14, C_LIGHT, 54);
  mkLblC(s, o->price, &f_fredoka_48, C_WHITE, 76);

  // white QRIS card
  lv_obj_t *card = mkBox(s, 56, 144, 208, 208, C_WHITE, 18);
  lv_obj_t *qr = lv_qrcode_create(card, QR_SIZE, C_NAVY, C_WHITE);
  lv_obj_center(qr);
  lv_qrcode_update(qr, QRIS_PAYLOAD, strlen(QRIS_PAYLOAD));
  lv_obj_t *scan = mkBox(card, 8, 12, 192, 5, C_TEAL, 2);
  animStart(scan, posYCb, 10, 190, 1500, 0, true, 1500);

  // corner brackets (pulsing)
  const int cx[2] = { 50, 264 - 24 + 6 }, cy[2] = { 138, 352 - 24 + 6 };
  for (int ix = 0; ix < 2; ix++)
    for (int iy = 0; iy < 2; iy++) {
      lv_obj_t *h = mkBox(s, cx[ix] + (ix ? -6 : 0), cy[iy] + (iy ? 18 : 0), 30, 6, C_TEAL, 3);
      lv_obj_t *v = mkBox(s, cx[ix] + (ix ? 18 : 0), cy[iy] + (iy ? -6 : 0), 6, 30, C_TEAL, 3);
      animStart(h, bgOpaCb, 255, 110, 800, 0, true, 800);
      animStart(v, bgOpaCb, 255, 110, 800, 0, true, 800);
    }

  // waiting row
  lv_obj_t *wl = mkLbl(s, "Menunggu pembayaran", &f_nunito_16, C_WHITE);
  lv_obj_set_pos(wl, 60, 362);
  for (int i = 0; i < 3; i++) {
    lv_obj_t *d = mkBox(s, 240 + i * 13, 369, 8, 8, C_TEAL, LV_RADIUS_CIRCLE);
    animStart(d, bgOpaCb, 60, 255, 500, i * 200, true, 500);
  }

  // timer ring + hint
  lv_obj_t *ringBox = mkBox(s, 20, 392, 44, 44, C_NAVY, 0);
  lv_obj_set_style_bg_opa(ringBox, LV_OPA_TRANSP, 0);
  payArc = mkArc(ringBox, 0, 0, 44, 5, C_NAVY3, C_ORANGE);
  lv_arc_set_value(payArc, 1000);
  lv_obj_t *dotO = mkBox(ringBox, 18, 18, 8, 8, C_WHITE, LV_RADIUS_CIRCLE);
  animStart(dotO, orbitCb, 270, 630, 4000, 0, true);
  payTime = mkLbl(s, "5:00", &f_fredoka_28, C_WHITE);
  lv_obj_set_pos(payTime, 74, 396);
  lv_obj_t *hint = mkLbl(s, "Scan pakai e-wallet\natau m-banking", &f_nunito_14, C_LIGHT);
  lv_obj_set_pos(hint, 150, 396);

  lv_obj_t *demoB = mkPill(s, "Demo: anggap sudah dibayar", 40, 442, 240, 34, C_NAVY, C_LIGHT, &f_nunito_14, evtDemoPaid, NULL);
  lv_obj_set_style_border_width(demoB, 2, 0);
  lv_obj_set_style_border_color(demoB, C_BLUE, 0);

  payStartMs = millis();
  payLastSec = -1;
  return s;
}

// ============================== SCREEN: SUCCESS ==============================
static void confettiCb(void *var, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)var;
  int idx = (int)(intptr_t)lv_obj_get_user_data(o);
  int a = idx * 30;
  int dist = 70 + (idx % 3) * 16;
  int r = (v * dist) / 255 + 40;
  lv_obj_set_pos(o, 160 + ((lv_trigo_cos((int16_t)a) * r) >> 15) - 5,
                 150 + (((lv_trigo_sin((int16_t)a) * r) >> 15) * 60 / 100) - 5);
  lv_obj_set_style_bg_opa(o, (lv_opa_t)(v < 30 ? v * 8 : 255 - v), 0);
}

static void rippleCb(void *var, int32_t v) {   // v 0..100 -> ring size
  lv_obj_t *o = (lv_obj_t *)var;
  int sz = 130 + v * 60 / 100;
  lv_obj_set_size(o, sz, sz);
  lv_obj_set_pos(o, 160 - sz / 2, 150 - sz / 2);
  lv_obj_set_style_border_opa(o, (lv_opa_t)(220 - v * 2), 0);
}

static void goRun() { jobStart(svc, pick); showScreen(SC_RUN); }    // payment confirmed: start the cycle, then show its progress
static void evtStartRun(lv_event_t *) { goRun(); }

static lv_obj_t *buildSuccess() {
  lv_obj_t *s = mkScreen(lv_color_hex(0x0C3A4A));
  addBubbles(s, 7, lv_color_hex(0x7FE0B0), 5, 290, 480, 470, 4500, 8000, 10, 28);
  mkLogo(s, 16, 12, 30, false);
  mkWordmark(s, 52, 11, &f_fredoka_20);
  lv_obj_t *r = mkLbl(s, machineName, &f_nunito_14, C_LIGHT);
  lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -14, 17);

  lv_obj_t *ring = mkBox(s, 0, 0, 130, 130, C_WHITE, LV_RADIUS_CIRCLE);
  lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ring, 4, 0);
  lv_obj_set_style_border_color(ring, C_MINT, 0);
  animStart(ring, rippleCb, 0, 100, 2200, 0, true);

  static const uint32_t cols[4] = { 0x3FA9F5, 0x00B4C5, 0xFF9F43, 0xFFFFFF };
  for (int i = 0; i < 12; i++) {
    lv_obj_t *c = mkBox(s, 160, 150, 10, 10, lv_color_hex(cols[i % 4]), LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_user_data(c, (void *)(intptr_t)i);
    animStart(c, confettiCb, 0, 255, 2400, i * 150, true);
  }

  lv_obj_t *circ = mkBox(s, 100, 90, 120, 120, C_MINT, LV_RADIUS_CIRCLE);
  static const lv_point_t chk[3] = { {35, 62}, {53, 80}, {86, 43} };
  lv_obj_t *ln = lv_line_create(circ);
  lv_line_set_points(ln, chk, 3);
  lv_obj_set_style_line_width(ln, 11, 0);
  lv_obj_set_style_line_color(ln, C_NAVY, 0);
  lv_obj_set_style_line_rounded(ln, true, 0);
  noClick(ln);

  mkLblC(s, "Pembayaran\nberhasil!", &f_fredoka_28, C_WHITE, 250);
  char t[64];
  snprintf(t, sizeof t, "%s diterima.\nPastikan pintu mesin tertutup rapat.", tr(curOpt()->price));
  mkLblC(s, t, &f_nunito_16, C_LIGHT, 328);
  mkPill(s, "Mulai sekarang", 30, 400, 260, 60, C_MINT, C_NAVY, &f_fredoka_20, evtStartRun, NULL);
  payStartMs = millis();
  return s;
}

// ============================== SCREEN: RUN (wash / dry) ==============================
static int runLastSec = -1, runStage = -1;
static lv_obj_t *runArc, *runTimeLbl, *runChip[3], *runChipLbl[3];

static void evtSkip(lv_event_t *) { if (jobs[svc].st == JS_RUN) jobs[svc].startMs = millis() - jobs[svc].durMs; }

static void setChips(int stage) {
  lv_color_t ac = accentCol();
  for (int i = 0; i < 3; i++) {
    lv_obj_set_style_bg_color(runChip[i], i == stage ? ac : C_NAVY2, 0);
    lv_obj_set_style_text_color(runChipLbl[i], i == stage ? C_NAVY : C_MUTED, 0);
  }
}

static lv_obj_t *buildRun() {
  bool dry = svc == 1;
  lv_color_t ac = accentCol();
  lv_obj_t *s = mkScreen(dry ? lv_color_hex(0x1A2444) : C_NAVY);
  addBubbles(s, 5, dry ? lv_color_hex(0xFDBA74) : lv_color_hex(0x7FD8E2), 5, 290, 480, 470, 6000, 10000, 10, 24);
  mkLogo(s, 16, 12, 30, false);
  mkWordmark(s, 52, 11, &f_fredoka_20);
  char lock[48];
  snprintf(lock, sizeof lock, "Terkunci · %s", machineName);
  lv_obj_t *lt = mkLbl(s, lock, &f_nunito_14, C_LIGHT);
  lv_obj_align(lt, LV_ALIGN_TOP_RIGHT, -14, 17);

  runArc = mkArc(s, 40, 52, 240, 10, C_NAVY3, ac);
  lv_obj_set_style_arc_color(runArc, ac, LV_PART_INDICATOR);

  int g = 196;
  lv_obj_t *glass = mkDrum(s, 58, 70, g + 12, 6, C_NAVY2, dry ? lv_color_hex(0xFFF1DE) : lv_color_hex(0xE6F4FE), NULL, 0);
  if (!dry) {
    mkWave(glass, 0, g * 52 / 100, g, g, C_BLUE, 44, 8, 2200);
  } else {
    static const int xs[4] = { 12, 24, 288, 300 };    // heat bars rise beside the drum, not over it
    addHeat(s, 4, xs, 230, 90, lv_color_hex(0xFFC98A));
    for (int i = 0; i < 3; i++) {
      lv_obj_t *sp = mkBox(glass, 36 + i * 55, 40 + (i & 1) * 90, 10, 10, C_ORANGE, LV_RADIUS_CIRCLE);
      lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
      animStart(sp, sparkCb, 0, 255, 1800, i * 600, true);
    }
  }
  addClothes(glass, g, dry ? CLOTH_DRY : CLOTH_WASH, dry ? 5000 : 3400);
  if (!dry) addFoam(glass, 4, 40, g - 40, g - 10, 110);

  mkLblC(s, "Sisa waktu", &f_nunito_14, C_LIGHT, 294);
  runTimeLbl = mkLblC(s, "00:00", &f_fredoka_64, C_WHITE, 308);

  static const char *WN[3] = { "Mencuci", "Membilas", "Memeras" };
  static const char *DN[3] = { "Panas", "Kering", "Dingin" };
  for (int i = 0; i < 3; i++) {
    runChip[i] = mkBox(s, 15 + i * 99, 398, 92, 32, C_NAVY2, 16);
    runChipLbl[i] = mkLbl(runChip[i], dry ? DN[i] : WN[i], &f_nunito_14, C_MUTED);
    lv_obj_center(runChipLbl[i]);
  }
  lv_obj_t *skip = mkPill(s, "Demo: lompat ke selesai", 50, 440, 220, 34, dry ? lv_color_hex(0x1A2444) : C_NAVY, C_LIGHT, &f_nunito_14, evtSkip, NULL);
  lv_obj_set_style_border_width(skip, 2, 0);
  lv_obj_set_style_border_color(skip, ac, 0);

  runLastSec = -1;
  runStage = -1;
  return s;
}

static void runTick(uint32_t now) {
  if (jobs[svc].st != JS_RUN) { showScreen(SC_DONE); return; }     // the job engine already switched the relays off
  uint32_t runDurMs = jobs[svc].durMs, el = jobElapsed(svc, now);
  int remain = jobRemainSec(svc, now);
  if (remain != runLastSec) {
    runLastSec = remain;
    char tb[12];
    snprintf(tb, sizeof tb, "%02d:%02d", remain / 60, remain % 60);
    lv_label_set_text(runTimeLbl, tb);
    lv_arc_set_value(runArc, (int16_t)((el * 1000ULL) / runDurMs));
  }
  int frac = (int)((el * 100ULL) / runDurMs);
  int stage = svc ? (frac < 50 ? 0 : (frac < 85 ? 1 : 2)) : (frac < 55 ? 0 : (frac < 85 ? 1 : 2));
  if (stage != runStage) { runStage = stage; setChips(stage); }
}

// ============================== SCREEN: DONE ==============================
static uint32_t doneStartMs = 0;

static void shakeCb(void *v, int32_t x) { lv_obj_set_style_translate_x((lv_obj_t *)v, x, 0); }
static void doneRippleCb(void *var, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)var;
  int sz = 116 + v * 60 / 100;
  lv_obj_set_size(o, sz, sz);
  lv_obj_set_pos(o, 160 - sz / 2, 128 - sz / 2);
  lv_obj_set_style_border_opa(o, (lv_opa_t)(220 - v * 2), 0);
}
static void evtDrySvc(lv_event_t *) {
  svc = 1; pick = 1;
  showScreen(jobs[1].st == JS_RUN ? SC_RUN : SC_CYCLE);
}

static lv_obj_t *buildDone() {
  if (jobs[svc].st == JS_DONE) jobs[svc].st = JS_FREE;           // customer is at the machine: collected
  lv_obj_t *s = mkScreen(lv_color_hex(0x0C3A4A));
  addBubbles(s, 8, lv_color_hex(0x7FE0B0), 5, 290, 480, 470, 4500, 8000, 10, 30);
  mkLogo(s, 16, 12, 30, false);
  mkWordmark(s, 52, 11, &f_fredoka_20);
  lv_obj_t *r = mkLbl(s, machineName, &f_nunito_14, C_LIGHT);
  lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -14, 17);

  lv_obj_t *ring = mkBox(s, 0, 0, 116, 116, C_WHITE, LV_RADIUS_CIRCLE);
  lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ring, 4, 0);
  lv_obj_set_style_border_color(ring, C_MINT, 0);
  animStart(ring, doneRippleCb, 0, 100, 1600, 0, true);

  lv_obj_t *circ = mkBox(s, 100, 68, 120, 120, C_MINT, LV_RADIUS_CIRCLE);
  lv_obj_t *bell = mkLbl(circ, LV_SYMBOL_BELL, &lv_font_montserrat_48, C_NAVY);
  lv_obj_center(bell);
  animStart(bell, shakeCb, -7, 7, 160, 0, true, 160);

  mkLblC(s, "Cucian sudah\nbersih!", &f_fredoka_28, C_WHITE, 224);
  mkLblC(s, "Ambil semua pakaian dan cek\nkantong sebelum pergi.", &f_nunito_16, C_LIGHT, 300);

  if (svc == 0) {
    mkPill(s, "Lanjut keringkan", 30, 360, 260, 56, C_ORANGE, C_NAVY, &f_fredoka_20, evtDrySvc, NULL);
    mkPill(s, "Selesai", 30, 424, 260, 44, C_MINT, C_NAVY, &f_fredoka_20, evtGo, (void *)(intptr_t)SC_IDLE);
  } else {
    mkPill(s, "Selesai", 30, 380, 260, 60, C_MINT, C_NAVY, &f_fredoka_20, evtGo, (void *)(intptr_t)SC_IDLE);
  }
  doneStartMs = millis();
  return s;
}

// ============================== SCREEN: PIN (admin lock) ==============================
static bool pinReveal = false;                 // PIN pad opened to reveal sensitive info
static uint32_t infoRevealUntil = 0;          // sensitive info visible until this time (ms)
static bool infoRevealed() { return infoRevealUntil && millis() < infoRevealUntil; }
static char pinBuf[8];
static int pinLen = 0;
static lv_obj_t *pinDots[6], *pinMsg;

static int pinTarget() { return 6; }

static void pinUpdateDots() {
  int n = pinTarget();
  for (int i = 0; i < 6; i++) {
    if (i >= n) lv_obj_add_flag(pinDots[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_set_style_bg_color(pinDots[i], i < pinLen ? C_TEAL : C_NAVY3, 0);
  }
}

static void evtPinKey(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);   // 0-9 digit, 10 clear, 11 backspace
  int n = pinTarget();
  if (k == 10) { pinLen = 0; pinBuf[0] = 0; }
  else if (k == 11) { if (pinLen) pinBuf[--pinLen] = 0; }
  else if (pinLen < n) { pinBuf[pinLen++] = '0' + k; pinBuf[pinLen] = 0; }
  lv_label_set_text(pinMsg, "");
  pinUpdateDots();
  if (pinLen == n) {
    if (strcmp(pinBuf, cfg.pin) == 0) {
      if (pinReveal) infoRevealUntil = millis() + 30000;
      showScreen(pinReveal ? SC_INFO : SC_SETTINGS);
    } else {
      lv_label_set_text(pinMsg, "PIN salah, coba lagi");
      ledFlash(LC_RED, 2);
      pinLen = 0; pinBuf[0] = 0;
      pinUpdateDots();
    }
  }
}

static lv_obj_t *buildPin() {
  lv_obj_t *s = mkScreen(C_NAVY);
  mkHeader(s, pinReveal ? SC_INFO : SC_IDLE, NULL);
  mkLblC(s, "PIN admin", &f_fredoka_28, C_WHITE, 60);                                  // 60..95
  mkLblC(s, pinReveal ? "Masukkan PIN untuk menampilkan\ndata sensitif" : "Masukkan PIN untuk membuka\npengaturan",
         &f_nunito_14, C_LIGHT, 100, 280);                                             // 100..140
  int n = pinTarget();
  int x0 = (SCREEN_W - (n * 16 + (n - 1) * 14)) / 2;
  for (int i = 0; i < 6; i++) pinDots[i] = mkBox(s, x0 + i * 30, 152, 16, 16, C_NAVY3, LV_RADIUS_CIRCLE);
  pinMsg = mkLblC(s, "", &f_nunito_16, lv_color_hex(0xFF6B6B), 176, 280);              // 176..199
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 3; c++) {
      int code = (r < 3) ? r * 3 + c + 1 : (c == 0 ? 10 : (c == 1 ? 0 : 11));
      lv_obj_t *b = mkClickBox(s, 20 + c * 98, 204 + r * 62, 84, 54, C_NAVY2, 16, evtPinKey, (void *)(intptr_t)code);
      lv_obj_set_style_bg_color(b, C_NAVY3, LV_STATE_PRESSED);
      lv_obj_t *l;
      if (code == 10) l = mkLbl(b, "C", &f_fredoka_20, C_LIGHT);
      else if (code == 11) l = mkLbl(b, LV_SYMBOL_BACKSPACE, &lv_font_montserrat_20, C_LIGHT);
      else { char t[2] = { (char)('0' + code), 0 }; l = mkLbl(b, t, &f_fredoka_28, C_WHITE); }
      lv_obj_center(l);
    }                                                                                 // ends y 444
  pinLen = 0; pinBuf[0] = 0;
  pinUpdateDots();
  return s;
}

// ============================== SCREEN: TEXT EDIT (generic) ==============================
struct EditCtx {
  const char *title, *hint;
  char *target;
  size_t cap;
  bool number, secret;
  uint8_t minLen;
  Scr ret;
  void (*onSave)();
};
static EditCtx ed;
static lv_obj_t *edTa, *edKb, *edMsg, *edEyeLbl;

static void openEdit(const char *title, const char *hint, char *target, size_t cap, bool number, bool secret,
                     uint8_t minLen, Scr ret, void (*onSave)()) {
  ed.title = title; ed.hint = hint; ed.target = target; ed.cap = cap; ed.number = number;
  ed.secret = secret; ed.minLen = minLen; ed.ret = ret; ed.onSave = onSave;
  showScreen(SC_EDIT);
}

static void edSave() {
  const char *t = lv_textarea_get_text(edTa);
  if (ed.secret && !t[0]) { showScreen(ed.ret); return; }      // empty secret = keep the old value
  if (strlen(t) < ed.minLen) {
    char m[40];
    snprintf(m, sizeof m, "Minimal %u karakter", (unsigned)ed.minLen);
    lv_label_set_text(edMsg, m);
    return;
  }
  strlcpy(ed.target, t, ed.cap);
  if (ed.onSave) ed.onSave();
  showScreen(ed.ret);
}
static void evtKb(lv_event_t *e) {
  lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_READY) edSave();
  else if (c == LV_EVENT_CANCEL) showScreen(ed.ret);
}
static void evtEdSave(lv_event_t *)   { edSave(); }
static void evtEdCancel(lv_event_t *) { showScreen(ed.ret); }
static void evtEdEye(lv_event_t *) {
  bool on = lv_textarea_get_password_mode(edTa);
  lv_textarea_set_password_mode(edTa, !on);
  lv_label_set_text(edEyeLbl, on ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static lv_obj_t *buildEdit() {
  lv_obj_t *s = mkScreen(C_NAVY);
  mkHeader(s, ed.ret, NULL);
  lv_obj_t *t = mkLbl(s, ed.title, &f_fredoka_28, C_WHITE);                 // y 58..93
  lv_obj_set_pos(t, 16, 58);
  lv_obj_t *h = mkLbl(s, ed.hint, &f_nunito_14, C_LIGHT);                    // y 94..114
  lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);
  lv_obj_set_size(h, 288, 20);
  lv_obj_set_pos(h, 16, 94);

  int taW = ed.secret ? 244 : 296;
  edTa = lv_textarea_create(s);                                              // y 120..168
  lv_obj_set_size(edTa, taW, 48);
  lv_obj_set_pos(edTa, 12, 120);
  lv_textarea_set_one_line(edTa, true);
  lv_textarea_set_max_length(edTa, ed.cap - 1);
  if (ed.number) lv_textarea_set_accepted_chars(edTa, "0123456789");
  if (!ed.secret) lv_textarea_set_text(edTa, ed.target);
  lv_textarea_set_password_mode(edTa, ed.secret);
  lv_obj_set_style_bg_color(edTa, C_NAVY2, 0);
  lv_obj_set_style_bg_opa(edTa, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(edTa, C_TXT, 0);
  lv_obj_set_style_text_font(edTa, &f_nunito_20, 0);
  lv_obj_set_style_border_width(edTa, 2, 0);
  lv_obj_set_style_border_color(edTa, C_TEAL, 0);
  lv_obj_set_style_radius(edTa, 14, 0);
  lv_obj_set_style_pad_hor(edTa, 12, 0);
  lv_obj_set_style_pad_ver(edTa, 10, 0);
  lv_obj_add_state(edTa, LV_STATE_FOCUSED);

  if (ed.secret) {
    lv_obj_t *eye = mkClickBox(s, 264, 120, 44, 48, C_NAVY2, 14, evtEdEye, NULL);
    edEyeLbl = mkLbl(eye, LV_SYMBOL_EYE_OPEN, &lv_font_montserrat_16, C_LIGHT);
    lv_obj_center(edEyeLbl);
  }

  edMsg = mkLbl(s, "", &f_nunito_14, lv_color_hex(0xFF6B6B));              // y 172..192
  lv_obj_set_pos(edMsg, 16, 172);
  mkPill(s, "Batal", 12, 196, 140, 38, C_NAVY3, C_TXT, &f_fredoka_20, evtEdCancel, NULL);   // y 196..234
  mkPill(s, "Simpan", 168, 196, 140, 38, C_TEAL, C_NAVY, &f_fredoka_20, evtEdSave, NULL);

  edKb = lv_keyboard_create(s);                                              // y 240..480
  lv_obj_set_size(edKb, SCREEN_W, 240);
  lv_obj_align(edKb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(edKb, edTa);
  if (ed.number) lv_keyboard_set_mode(edKb, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_set_style_bg_color(edKb, C_NAVY, 0);
  lv_obj_set_style_border_width(edKb, 0, 0);
  lv_obj_set_style_pad_all(edKb, 4, 0);
  lv_obj_set_style_pad_row(edKb, 4, 0);
  lv_obj_set_style_pad_column(edKb, 4, 0);
  lv_obj_set_style_bg_color(edKb, C_NAVY3, LV_PART_ITEMS);
  lv_obj_set_style_text_color(edKb, C_TXT, LV_PART_ITEMS);
  lv_obj_set_style_text_font(edKb, &lv_font_montserrat_16, LV_PART_ITEMS);
  lv_obj_set_style_radius(edKb, 8, LV_PART_ITEMS);
  lv_obj_set_style_border_width(edKb, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(edKb, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(edKb, C_BLUE, (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(edKb, C_TEAL, (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_add_event_cb(edKb, evtKb, LV_EVENT_ALL, NULL);
  return s;
}

// ============================== SCREEN: WI-FI ==============================
static char wifiSel[33], wifiPass[65], wifiHint[64];
static char scanSsid[10][33];
static int scanRssi[10];
static bool scanOpen[10];
static int nScan = 0;
static bool scanning = false;
static lv_obj_t *wifiStatus, *wifiList, *wifiScanLbl;

static void onWifiPass() {
  strlcpy(cfg.ssid, wifiSel, sizeof cfg.ssid);
  strlcpy(cfg.pass, wifiPass, sizeof cfg.pass);
  saveCfg();
  wifiConnect();
}

static void evtPickNet(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  strlcpy(wifiSel, scanSsid[i], sizeof wifiSel);
  if (scanOpen[i]) {
    strlcpy(cfg.ssid, wifiSel, sizeof cfg.ssid);
    cfg.pass[0] = 0;
    saveCfg();
    wifiConnect();
  } else {
    wifiPass[0] = 0;
    snprintf(wifiHint, sizeof wifiHint, "Jaringan: %s", wifiSel);
    openEdit("Kata sandi", wifiHint, wifiPass, sizeof wifiPass, false, true, 0, SC_WIFI, onWifiPass);
  }
}

static void buildScanRows() {
  lv_obj_clean(wifiList);
  if (nScan == 0) {
    lv_obj_t *l = mkLbl(wifiList, "Tidak ada jaringan ditemukan", &f_nunito_14, C_MUTED);
    lv_obj_set_pos(l, 8, 12);
    return;
  }
  for (int i = 0; i < nScan; i++) {
    lv_obj_t *row = mkClickBox(wifiList, 0, i * 50, 296, 44, C_NAVY2, 14, evtPickNet, (void *)(intptr_t)i);
    lv_obj_set_style_bg_color(row, C_NAVY3, LV_STATE_PRESSED);
    lv_obj_t *nm = mkLbl(row, scanSsid[i], &f_nunito_16, C_WHITE);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_size(nm, 150, 22);
    lv_obj_set_pos(nm, 14, 11);
    char r[32];
    snprintf(r, sizeof r, "%s  %d dBm", scanOpen[i] ? "terbuka" : "terkunci", scanRssi[i]);
    lv_obj_t *rl = mkLbl(row, r, &f_nunito_14, scanRssi[i] > -70 ? C_MINT : C_ORANGE);
    lv_obj_align(rl, LV_ALIGN_RIGHT_MID, -12, 0);
  }
}

static void collectScan(int n) {
  nScan = 0;
  for (int i = 0; i < n && nScan < 10; i++) {
    String ss = WiFi.SSID(i);
    if (ss.length() == 0) continue;
    bool dup = false;
    for (int j = 0; j < nScan; j++) if (!strcmp(scanSsid[j], ss.c_str())) { dup = true; break; }
    if (dup) continue;
    strlcpy(scanSsid[nScan], ss.c_str(), sizeof scanSsid[0]);
    scanRssi[nScan] = WiFi.RSSI(i);
    scanOpen[nScan] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    nScan++;
  }
  for (int a = 0; a < nScan; a++)                       // strongest first
    for (int b = a + 1; b < nScan; b++)
      if (scanRssi[b] > scanRssi[a]) {
        char tmp[33]; strlcpy(tmp, scanSsid[a], 33); strlcpy(scanSsid[a], scanSsid[b], 33); strlcpy(scanSsid[b], tmp, 33);
        int r = scanRssi[a]; scanRssi[a] = scanRssi[b]; scanRssi[b] = r;
        bool o = scanOpen[a]; scanOpen[a] = scanOpen[b]; scanOpen[b] = o;
      }
}

static void evtScan(lv_event_t *) {
  if (scanning) return;
  WiFi.mode(WIFI_STA);
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  scanning = true;
  lv_label_set_text(wifiScanLbl, "Mencari...");
  lv_obj_center(wifiScanLbl);
  lv_obj_clean(wifiList);
}

static void evtForget(lv_event_t *) {
  cfg.ssid[0] = 0; cfg.pass[0] = 0;
  saveCfg();
  WiFi.disconnect();
}

static void refreshWifiStatus() {
  static char last[160];
  char t[160];
  lv_color_t col = C_ORANGE;
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(t, sizeof t, "Terhubung ke %s\nIP %s   %d dBm", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    col = C_MINT;
  } else if (cfg.ssid[0]) {
    snprintf(t, sizeof t, "Menyambung ke %s...\nJika gagal, cek kata sandi", cfg.ssid);
  } else {
    snprintf(t, sizeof t, "Belum terhubung\nPilih jaringan di bawah");
  }
  if (strcmp(t, last)) {
    strlcpy(last, t, sizeof last);
    lv_label_set_text(wifiStatus, t);
    lv_obj_set_style_text_color(wifiStatus, col, 0);
  }
}

static lv_obj_t *buildWifi() {
  lv_obj_t *s = mkScreen(C_NAVY);
  mkHeader(s, SC_SETTINGS, NULL);
  lv_obj_t *t = mkLbl(s, "Wi-Fi", &f_fredoka_28, C_WHITE);                   // 58..93
  lv_obj_set_pos(t, 16, 58);
  lv_obj_t *card = mkBox(s, 12, 98, 296, 62, C_NAVY2, 16);                    // 98..160
  wifiStatus = mkLbl(card, "", &f_nunito_14, C_ORANGE);
  lv_label_set_long_mode(wifiStatus, LV_LABEL_LONG_DOT);
  lv_obj_set_size(wifiStatus, 270, 44);
  lv_obj_set_pos(wifiStatus, 14, 9);
  lv_obj_t *b1 = mkPill(s, "Cari jaringan", 12, 168, 176, 40, C_TEAL, C_NAVY, &f_fredoka_20, evtScan, NULL);   // 168..208
  wifiScanLbl = lv_obj_get_child(b1, 0);
  mkPill(s, "Lupakan", 196, 168, 112, 40, C_NAVY3, C_WHITE, &f_fredoka_20, evtForget, NULL);

  wifiList = mkBox(s, 12, 216, 296, 264, C_NAVY, 0);                          // 216..480
  lv_obj_set_style_bg_opa(wifiList, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(wifiList, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(wifiList, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(wifiList, LV_SCROLLBAR_MODE_OFF);
  if (nScan) buildScanRows();
  else {
    lv_obj_t *l = mkLbl(wifiList, "Tekan \"Cari jaringan\" untuk\nmelihat jaringan Wi-Fi sekitar", &f_nunito_14, C_MUTED);
    lv_obj_set_pos(l, 8, 12);
  }
  scanning = false;
  refreshWifiStatus();
  return s;
}

// ============================== WEB FORM (PC) ==============================
// A small web server for entering owner / machine ID / API URL / API key from a PC on the same network.
// It only runs while an admin page is open on the kiosk (Settings, Wi-Fi, editor, Bluetooth), is protected by
// HTTP Basic auth (user "admin", password = admin PIN) and locks for 60 s after 5 wrong logins.
static WebServer web(80);
static bool webOn = false, webInit = false, mdnsOn = false, webDirty = false;
static int webFails = 0;
static uint32_t webLockUntil = 0;

static void webHost(char *out, size_t n) {            // washable-03
  size_t j = 0;
  const char *pre = "washable-";
  for (size_t i = 0; pre[i] && j < n - 1; i++) out[j++] = pre[i];
  for (const char *c = cfg.machineId; *c && j < n - 1; c++) {
    char ch = (*c >= 'A' && *c <= 'Z') ? *c + 32 : *c;
    out[j++] = ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) ? ch : '-';
  }
  out[j] = 0;
}

static String htmlEsc(const char *s) {
  String o;
  for (; *s; s++) {
    if (*s == '&') o += "&amp;";
    else if (*s == '<') o += "&lt;";
    else if (*s == '>') o += "&gt;";
    else if (*s == '"') o += "&quot;";
    else o += *s;
  }
  return o;
}

static bool webAuth() {
  if (millis() < webLockUntil) {
    web.send(429, "text/plain", tr("Terlalu banyak percobaan login. Coba lagi 1 menit."));
    return false;
  }
  if (web.authenticate("admin", cfg.pin)) { webFails = 0; return true; }
  if (web.hasHeader("Authorization") && ++webFails >= 5) { webLockUntil = millis() + 60000; webFails = 0; }
  web.requestAuthentication(BASIC_AUTH, "washable", "Login: admin + PIN admin");
  return false;
}

static void webRoot() {
  if (!webAuth()) return;
  String msg;
  if (web.hasArg("ok")) msg = String("<p class=ok>") + tr("Tersimpan. Layar mesin ikut diperbarui.") + "</p>";
  else if (web.hasArg("e")) msg = String("<p class=err>") + tr(web.arg("e") == "1" ? "API URL harus diawali http:// atau https://" : (web.arg("e") == "3" ? "URL OTA harus diawali https://" : "Isi terlalu panjang atau tidak valid")) + "</p>";
  String h = F("<!doctype html><html lang=");
  h += g_lang == LANG_EN ? "en" : "id";
  h += F("><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>");
  h += tr("washable - pengaturan");
  h += F("</title><style>"
         "body{margin:0;background:#0B2545;color:#fff;font:16px/1.4 system-ui,sans-serif;display:flex;justify-content:center;padding:24px 16px}"
         "main{width:100%;max-width:440px}h1{margin:0 0 4px;font-size:28px}h1 span{color:#00B4C5}p{color:#BFD3E6;margin:4px 0 16px}"
         "label{display:block;font-weight:700;margin:14px 0 6px}input{width:100%;box-sizing:border-box;padding:14px;border-radius:14px;"
         "border:2px solid #13315C;background:#13315C;color:#fff;font-size:17px}button{width:100%;margin-top:22px;padding:16px;border:0;"
         "border-radius:999px;background:#00B4C5;color:#0B2545;font-size:18px;font-weight:800}.ok{color:#2ECC71}.err{color:#FF6B6B}small{color:#8DA9C4}"
         "</style></head><body><main><h1>wash<span>able</span></h1><p>");
  h += tr("Pengaturan mesin. Perubahan langsung disimpan di mesin.");
  h += "</p>";
  h += msg;
  h += F("<form method=post action=/save><label>");
  h += tr("Pemilik");
  h += F("</label><input name=owner maxlength=31 value=\"");
  h += htmlEsc(cfg.owner);
  h += F("\"><label>");
  h += tr("ID pemilik");
  h += F("</label><input name=ownerid maxlength=31 value=\"");
  h += htmlEsc(cfg.ownerId);
  h += F("\"><label>");
  h += tr("ID mesin");
  h += F("</label><input name=mid maxlength=7 value=\"");
  h += htmlEsc(cfg.machineId);
  h += F("\"><label>");
  h += tr("Server API (URL)");
  h += F("</label><input name=apiurl maxlength=95 placeholder=\"https://api.example.com\" value=\"");
  h += htmlEsc(cfg.apiUrl);
  h += F("\"><label>");
  h += tr("Kunci API");
  h += F("</label><input name=apikey type=password maxlength=63 autocomplete=off placeholder=\"");
  h += tr(cfg.apiKey[0] ? "tersimpan - kosongkan jika tidak diubah" : "belum diisi");
  h += F("\"><small>");
  h += tr("Kunci API tidak pernah ditampilkan ulang.");
  h += F("</small><label>");
  h += tr("URL manifest OTA (HTTPS)");
  h += F("</label><input name=otaurl maxlength=159 placeholder=\"https://github.com/OWNER/REPO/releases/latest/download/manifest.json\" value=\"");
  h += htmlEsc(cfg.otaUrl);
  h += F("\"><small>");
  h += tr("Kosongkan untuk menonaktifkan pembaruan firmware otomatis.");
  h += F("</small><button>");
  h += tr("Simpan");
  h += F("</button></form></main></body></html>");
  web.send(200, "text/html; charset=utf-8", h);
}

static void webSave() {
  if (!webAuth()) return;
  String owner = web.arg("owner"), oid = web.arg("ownerid"), mid = web.arg("mid"), url = web.arg("apiurl"), key = web.arg("apikey"), ota = web.arg("otaurl");
  ota.trim();
  owner.trim(); oid.trim(); mid.trim(); url.trim(); key.trim();
  if (owner.length() >= sizeof cfg.owner || oid.length() >= sizeof cfg.ownerId || mid.length() >= sizeof cfg.machineId || url.length() >= sizeof cfg.apiUrl ||
      key.length() >= sizeof cfg.apiKey || ota.length() >= sizeof cfg.otaUrl || mid.length() == 0) {
    web.sendHeader("Location", "/?e=2"); web.send(303); return;
  }
  if (url.length() && !url.startsWith("http://") && !url.startsWith("https://")) {
    web.sendHeader("Location", "/?e=1"); web.send(303); return;
  }
  if (ota.length() && !ota.startsWith("https://") && !(OTA_ALLOW_HTTP && ota.startsWith("http://"))) {
    web.sendHeader("Location", "/?e=3"); web.send(303); return;
  }
  strlcpy(cfg.otaUrl, ota.c_str(), sizeof cfg.otaUrl);
  strlcpy(cfg.owner, owner.c_str(), sizeof cfg.owner);
  strlcpy(cfg.ownerId, oid.c_str(), sizeof cfg.ownerId);
  strlcpy(cfg.machineId, mid.c_str(), sizeof cfg.machineId);
  strlcpy(cfg.apiUrl, url.c_str(), sizeof cfg.apiUrl);
  if (key.length()) strlcpy(cfg.apiKey, key.c_str(), sizeof cfg.apiKey);   // empty = keep the stored key
  updateMachineName();
  saveCfg();
  otaConfigure(otaUrlEff());
  Serial.printf("OTA: manifest URL = %s\n", otaUrlEff()[0] ? otaUrlEff() : "(not set)");
  webDirty = true;
  web.sendHeader("Location", "/?ok=1");
  web.send(303);
}

static void mdnsBegin() {
  if (mdnsOn || WiFi.status() != WL_CONNECTED) return;
  char host[32];
  webHost(host, sizeof host);
  if (MDNS.begin(host)) { MDNS.addService("http", "tcp", 80); mdnsOn = true; }
}
static void webStart() {
  if (webOn) return;
  if (!webInit) {
    static const char *hdrs[] = { "Authorization" };
    web.collectHeaders(hdrs, 1);
    web.on("/", HTTP_GET, webRoot);
    web.on("/save", HTTP_POST, webSave);
    webInit = true;
  }
  web.begin();
  webOn = true;
  mdnsBegin();
}
static void webStop() {
  if (!webOn) return;
  web.stop();
  if (mdnsOn) { MDNS.end(); mdnsOn = false; }
  webOn = false;
}
static void webTick() {
  if (!webOn) return;
  web.handleClient();
  if (!mdnsOn) mdnsBegin();
}
// ============================== SCREEN: ABOUT / INFORMATION (public, from the "i" button) ==============================
// contact details (placeholders: set the real number / site with -DCONTACT_WA_NUMBER=... etc. in platformio.ini)
#ifndef CONTACT_WA_NUMBER
#define CONTACT_WA_NUMBER "6281200000000"
#endif
#ifndef CONTACT_WA_TEXT
#define CONTACT_WA_TEXT "+62 812-0000-0000"
#endif
#ifndef CONTACT_WEB
#define CONTACT_WEB "washable.id"
#endif
#define CONTACT_WA_LINK "https://wa.me/" CONTACT_WA_NUMBER
static lv_obj_t *buildAbout() {
  lv_obj_t *s = mkScreen(C_NAVY);
  addBubbles(s, 4, lv_color_hex(0x7FD8E2), 5, 290, 480, 470, 6000, 10000, 10, 22);
  mkHeader(s, SC_IDLE, NULL);
  lv_obj_t *t = mkLbl(s, "Informasi", &f_fredoka_28, C_WHITE);                       // 58..93
  lv_obj_set_pos(t, 16, 58);

  lv_obj_t *c1 = mkBox(s, 15, 104, 290, 122, C_NAVY2, 18);                           // 104..226
  static const char *K[4] = { "Mesin", "Dikelola oleh", "Wi-Fi", "Versi" };
  char v[4][48];
  strlcpy(v[0], machineName, sizeof v[0]);
  strlcpy(v[1], cfg.owner[0] ? cfg.owner : "-", sizeof v[1]);
  strlcpy(v[2], WiFi.status() == WL_CONNECTED ? tr("terhubung") : tr("tidak terhubung"), sizeof v[2]);
  snprintf(v[3], sizeof v[3], "v%s", FW_VERSION);
  for (int i = 0; i < 4; i++) {
    lv_obj_t *k = mkLbl(c1, K[i], &f_nunito_14, C_MUTED);
    lv_obj_set_pos(k, 14, 10 + i * 28);
    lv_label_set_long_mode(k, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(k, 100, 20);
    lv_obj_t *l = mkLbl(c1, v[i], &f_nunito_16, C_WHITE);
    lv_obj_set_pos(l, 118, 8 + i * 28);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(l, 160, 22);
  }

  lv_obj_t *c2 = mkBox(s, 15, 236, 290, 104, C_NAVY2, 18);                           // 236..340
  lv_obj_t *ct = mkLbl(c2, "Cara pakai", &f_nunito_16, C_TEAL);
  lv_obj_set_pos(ct, 14, 8);
  static const char *ST[3] = { "1. Pilih cuci atau keringkan", "2. Bayar pakai QRIS", "3. Tunggu, lalu ambil cucian" };
  for (int i = 0; i < 3; i++) {
    lv_obj_t *l = mkLbl(c2, ST[i], &f_nunito_14, C_WHITE);
    lv_obj_set_pos(l, 14, 34 + i * 22);
  }

  lv_obj_t *c3 = mkBox(s, 15, 350, 290, 122, C_NAVY2, 18);                           // 350..472  contact us
  lv_obj_t *ht = mkLbl(c3, "Hubungi kami", &f_nunito_16, C_TEAL);
  lv_obj_set_pos(ht, 14, 8);
  static const char *CK[2] = { "WhatsApp", "Website" };
  const char *CV[2] = { CONTACT_WA_TEXT, CONTACT_WEB };
  for (int i = 0; i < 2; i++) {
    lv_obj_t *k = mkLbl(c3, CK[i], &f_nunito_14, C_MUTED);
    lv_obj_set_pos(k, 14, 34 + i * 40);
    lv_obj_t *l = mkLbl(c3, CV[i], &f_nunito_16, C_WHITE);
    lv_obj_set_pos(l, 14, 52 + i * 40);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(l, 170, 22);
  }
  lv_obj_t *qc = mkBox(c3, 198, 22, 82, 82, C_WHITE, 8);                              // scan = WhatsApp chat
  lv_obj_t *qr = lv_qrcode_create(qc, 72, lv_color_hex(0x0B2545), lv_color_white());
  lv_qrcode_update(qr, CONTACT_WA_LINK, strlen(CONTACT_WA_LINK));
  lv_obj_center(qr);
  return s;
}

// ============================== SCREEN: SETTINGS ==============================
static lv_obj_t *setVal[5];
static lv_obj_t *setBrightLbl, *setDemoLbl, *setResetLbl, *setWebLbl;
static bool resetArmed = false;

static void onCfgSaved() { updateMachineName(); saveCfg(); }

// device info card: owner / machine ID / server API / API key come from the web form;
// the API URL and key stay hidden until the admin PIN is confirmed (30 s)
static void fmtLastUpdate(char *out, size_t n) {
  uint32_t e;
  char v[16];
  if (otaLastUpdate(&e, v, sizeof v)) {
    static const char *MON[12] = { "Jan", "Feb", "Mar", "Apr", "Mei", "Jun", "Jul", "Agu", "Sep", "Okt", "Nov", "Des" };
    time_t t = (time_t)e + OTA_TZ_SECONDS;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(out, n, "%02d %s %d %02d:%02d WIB (v%s)", tmv.tm_mday, tr(MON[tmv.tm_mon % 12]), tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min, v);
  } else {
    snprintf(out, n, "belum ada (flash via USB)");
  }
}

static void webShort(char *out, size_t n) {
  if (WiFi.status() == WL_CONNECTED) { char host[32]; webHost(host, sizeof host); snprintf(out, n, "%s.local", host); }
  else snprintf(out, n, "Wi-Fi belum terhubung");
}
static const char *healthSummary();
static void settingsRefresh() {
  char t[112];
  if (cfg.ssid[0]) snprintf(t, sizeof t, "%s · %s", cfg.ssid, tr(WiFi.status() == WL_CONNECTED ? "terhubung" : "tidak terhubung"));
  else snprintf(t, sizeof t, "Belum diatur");
  lv_label_set_text(setVal[0], t);
  lv_label_set_text(setVal[1], "****");
  { char rt[48]; if (rl.helloOk) snprintf(rt, sizeof rt, "terhubung · fw %s", rl.fw); else snprintf(rt, sizeof rt, "belum terhubung"); lv_label_set_text(setVal[2], rt); }
  { char it[48]; snprintf(it, sizeof it, "v%s · lihat detail", FW_VERSION); lv_label_set_text(setVal[3], it); }
  { char wt[48]; webShort(wt, sizeof wt); lv_label_set_text(setWebLbl, wt); }
  lv_label_set_text(setVal[4], healthSummary());
}

static void evtSetWifi(lv_event_t *)  { showScreen(SC_WIFI); }
static void evtSetRelay(lv_event_t *) { showScreen(SC_RTEST); }
static void evtSetInfo(lv_event_t *)  { showScreen(SC_INFO); }
static void evtSetWeb(lv_event_t *)   { showScreen(SC_WEB); }
static void evtSetHealth(lv_event_t *) { showScreen(SC_HEALTH); }
static void evtSetPin(lv_event_t *)   { openEdit("PIN admin", "Tepat 6 angka", cfg.pin, 7, true, true, 6, SC_SETTINGS, onCfgSaved); }
static void infoRefresh();
static void evtReveal(lv_event_t *) {
  if (infoRevealed()) { infoRevealUntil = 0; infoRefresh(); }
  else { pinReveal = true; showScreen(SC_PIN); }
}

static void evtBright(lv_event_t *e) {
  lv_obj_t *sl = lv_event_get_target(e);
  lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_VALUE_CHANGED) {
    cfg.bright = (uint8_t)lv_slider_get_value(sl);
    applyBright();
    ledcWrite(BL_CH, blMax);
    char t[8];
    snprintf(t, sizeof t, "%d%%", cfg.bright);
    lv_label_set_text(setBrightLbl, t);
  } else if (c == LV_EVENT_RELEASED) {
    saveCfg();
  }
}
static void evtDemo(lv_event_t *e) {
  cfg.demo = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  saveCfg();
  lv_label_set_text(setDemoLbl, cfg.demo ? "1 detik = 1 menit" : "Durasi asli");
}
static void resetDisarmCb(lv_timer_t *) {
  resetArmed = false;
  if (curScr == SC_SETTINGS && !busy) lv_label_set_text(setResetLbl, "Hapus semua pengaturan");
}
static void evtReset(lv_event_t *) {
  if (!resetArmed) {
    resetArmed = true;
    lv_label_set_text(setResetLbl, "Tap lagi untuk konfirmasi!");
    lv_timer_t *t = lv_timer_create(resetDisarmCb, 4000, NULL);
    lv_timer_set_repeat_count(t, 1);
  } else {
    prefs.begin("washable", false);
    prefs.clear();
    prefs.end();
    if (spiffsOk) SPIFFS.remove("/cfg.bak");
    delay(100);
    ESP.restart();
  }
}
static void evtRestart(lv_event_t *) { ESP.restart(); }

static lv_obj_t *mkRow(lv_obj_t *p, int y, int h, const char *icon, const char *title, lv_color_t iconCol,
                       lv_event_cb_t cb, lv_obj_t **valueOut) {
  lv_obj_t *r = cb ? mkClickBox(p, 4, y, 288, h, C_NAVY2, 18, cb, NULL) : mkBox(p, 4, y, 288, h, C_NAVY2, 18);
  if (cb) lv_obj_set_style_bg_color(r, C_NAVY3, LV_STATE_PRESSED);
  lv_obj_t *ic = mkBox(r, 12, 12, 36, 36, C_NAVY3, 18);
  if (h != 60) lv_obj_set_y(ic, (h - 36) / 2);
  lv_obj_t *il = mkLbl(ic, icon, &lv_font_montserrat_16, iconCol);
  lv_obj_center(il);
  lv_obj_t *t = mkLbl(r, title, &f_nunito_16, C_WHITE);
  lv_obj_set_pos(t, 58, 7);
  if (valueOut) {
    lv_obj_t *v = mkLbl(r, "", &f_nunito_14, C_MUTED);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_size(v, 188, 20);
    lv_obj_set_pos(v, 58, 31);
    *valueOut = v;
  }
  if (cb) {
    lv_obj_t *c = mkLbl(r, LV_SYMBOL_RIGHT, &lv_font_montserrat_16, C_MUTED);
    lv_obj_align(c, LV_ALIGN_RIGHT_MID, -14, 0);
  }
  return r;
}

static lv_obj_t *buildSettings() {
  lv_obj_t *s = mkScreen(C_NAVY);
  mkHeader(s, SC_IDLE, NULL);
  lv_obj_t *t = mkLbl(s, "Pengaturan", &f_fredoka_28, C_WHITE);              // 58..93
  lv_obj_set_pos(t, 16, 58);

  lv_obj_t *list = mkBox(s, 12, 98, 296, 382, C_NAVY, 0);                     // 98..480, scrolls
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

  int y = 0;
  mkRow(list, y, 60, LV_SYMBOL_WIFI, "Wi-Fi", C_MINT, evtSetWifi, &setVal[0]);        y += 68;
  mkRow(list, y, 60, LV_SYMBOL_UPLOAD, "Form web (buka dari PC)", C_TEAL, evtSetWeb, &setWebLbl);   y += 68;
  mkRow(list, y, 60, LV_SYMBOL_KEYBOARD, "PIN admin", C_ORANGE, evtSetPin, &setVal[1]); y += 68;
  mkRow(list, y, 60, LV_SYMBOL_CHARGE, "Kesehatan STM32", C_MINT, evtSetHealth, &setVal[4]);   y += 68;
  mkRow(list, y, 60, LV_SYMBOL_SETTINGS, "Tes relay", C_BLUE, evtSetRelay, &setVal[2]);   y += 68;

  // brightness slider row
  lv_obj_t *br = mkRow(list, y, 80, LV_SYMBOL_IMAGE, "Kecerahan layar", C_LIGHT, NULL, NULL);
  setBrightLbl = mkLbl(br, "", &f_nunito_16, C_WHITE);
  char bt[8];
  snprintf(bt, sizeof bt, "%d%%", cfg.bright);
  lv_label_set_text(setBrightLbl, bt);
  lv_obj_align(setBrightLbl, LV_ALIGN_TOP_RIGHT, -16, 7);
  lv_obj_t *sl = lv_slider_create(br);
  lv_obj_set_size(sl, 262, 10);
  lv_obj_set_pos(sl, 16, 56);
  lv_slider_set_range(sl, 20, 100);
  lv_slider_set_value(sl, cfg.bright, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, C_NAVY3, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, C_BLUE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, C_WHITE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 4, LV_PART_KNOB);
  lv_obj_add_event_cb(sl, evtBright, LV_EVENT_ALL, NULL);
  y += 88;

  // demo mode switch row
  lv_obj_t *dm = mkRow(list, y, 60, LV_SYMBOL_PLAY, "Mode demo", C_MINT, NULL, &setDemoLbl);
  lv_obj_set_width(setDemoLbl, 150);
  lv_label_set_text(setDemoLbl, cfg.demo ? "1 detik = 1 menit" : "Durasi asli");
  lv_obj_t *sw = lv_switch_create(dm);
  lv_obj_set_size(sw, 52, 28);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -14, 0);
  lv_obj_set_style_bg_color(sw, C_NAVY3, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, C_MINT, (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw, C_WHITE, LV_PART_KNOB);
  if (cfg.demo) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, evtDemo, LV_EVENT_VALUE_CHANGED, NULL);
  y += 68;

  mkRow(list, y, 60, LV_SYMBOL_TRASH, "Reset pabrik", lv_color_hex(0xFF6B6B), evtReset, &setResetLbl);
  lv_label_set_text(setResetLbl, "Hapus semua pengaturan");
  y += 68;
  mkRow(list, y, 60, LV_SYMBOL_POWER, "Mulai ulang perangkat", C_ORANGE, evtRestart, NULL);
  y += 68;

  mkRow(list, y, 60, LV_SYMBOL_LIST, "Info perangkat", C_LIGHT, evtSetInfo, &setVal[3]);
  y += 68;
  resetArmed = false;
  settingsRefresh();
  mkBox(list, 0, y + 8, 4, 4, C_NAVY, 0);                                      // scroll end spacer
  lv_obj_set_style_bg_opa(lv_obj_get_child(list, lv_obj_get_child_cnt(list) - 1), LV_OPA_TRANSP, 0);
  return s;
}

// ============================== SCREEN: DEVICE INFO (detail) ==============================
// Everything about this kiosk on one scrolling page. The API URL and key stay hidden until the admin PIN is entered (30 s).
enum InfoRow { IR_OWNER, IR_OWNERID, IR_MID, IR_API, IR_KEY,
               IR_SSID, IR_WSTAT, IR_IP, IR_MAC, IR_HOST,
               IR_FWV, IR_OTAST, IR_LAST, IR_BUILD, IR_PART, IR_OTAURL,
               IR_RLST, IR_RLRST, IR_RLFR, IR_RLCMD, IR_RLUP, IR_RLMASK,
               IR_JWASH, IR_JDRY,
               IR_UP, IR_HEAP, IR_DEMO, IR_BRIGHT, IR_COUNT };
struct InfoDef { const char *section, *key; uint8_t h; };            // section != NULL starts a new card
static const InfoDef INFO_DEF[IR_COUNT] = {
  { "Pemilik dan mesin", "Pemilik", 24 }, { NULL, "ID pemilik", 24 }, { NULL, "ID mesin", 24 }, { NULL, "Server API", 62 }, { NULL, "Kunci API", 62 },
  { "Jaringan", "Wi-Fi", 24 }, { NULL, "Status", 24 }, { NULL, "Alamat IP", 24 }, { NULL, "MAC", 24 }, { NULL, "Form web", 44 },
  { "Firmware", "Versi", 24 }, { NULL, "Pembaruan", 44 }, { NULL, "Update terakhir", 44 }, { NULL, "Build", 24 }, { NULL, "Partisi", 24 }, { NULL, "URL OTA", 62 },
  { "Kontroler relay", "Status", 24 }, { NULL, "Restart", 24 }, { NULL, "Frame", 24 }, { NULL, "Cmd gagal", 24 }, { NULL, "Uptime", 24 }, { NULL, "Relay aktif", 24 },
  { "Siklus", "Mesin cuci", 24 }, { NULL, "Pengering", 24 },
  { "Sistem", "Uptime", 24 }, { NULL, "RAM bebas", 24 }, { NULL, "Mode demo", 24 }, { NULL, "Kecerahan", 24 },
};
static lv_obj_t *infoList, *infoVal[IR_COUNT], *infoBtnLbl;

static void infoSet(InfoRow r, const char *t) { if (strcmp(lv_label_get_text(infoVal[r]), tr(t))) lv_label_set_text(infoVal[r], t); }

static void infoRefresh() {
  if (!infoList) return;
  uint32_t now = millis();
  bool rv = infoRevealed();
  char t[200], u[64];
  infoSet(IR_OWNER, cfg.owner[0] ? cfg.owner : "-");
  infoSet(IR_OWNERID, cfg.ownerId[0] ? cfg.ownerId : "-");
  snprintf(t, sizeof t, "%s (%s)", cfg.machineId, machineName); infoSet(IR_MID, t);
  infoSet(IR_API, cfg.apiUrl[0] ? (rv ? cfg.apiUrl : "tersimpan (disembunyikan)") : "belum diisi");
  infoSet(IR_KEY, cfg.apiKey[0] ? (rv ? cfg.apiKey : "********") : "belum diisi");
  infoSet(IR_SSID, cfg.ssid[0] ? cfg.ssid : "belum diatur");
  if (WiFi.status() == WL_CONNECTED) snprintf(t, sizeof t, "terhubung · %d dBm", WiFi.RSSI()); else snprintf(t, sizeof t, "tidak terhubung");
  infoSet(IR_WSTAT, t);
  infoSet(IR_IP, WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-");
  infoSet(IR_MAC, WiFi.macAddress().c_str());
  webHost(u, sizeof u);
  snprintf(t, sizeof t, "http://%s.local", u); infoSet(IR_HOST, t);
  infoSet(IR_FWV, "v" FW_VERSION);
  otaStatusText(t, sizeof t); infoSet(IR_OTAST, t);
  fmtLastUpdate(t, sizeof t); infoSet(IR_LAST, t);
  snprintf(t, sizeof t, "%s %s", __DATE__, __TIME__); infoSet(IR_BUILD, t);
  infoSet(IR_PART, esp_ota_get_running_partition()->label);
  infoSet(IR_OTAURL, otaUrlEff()[0] ? otaUrlEff() : "belum diisi");
  if (rl.helloOk) snprintf(t, sizeof t, "terhubung (fw %s)", rl.fw); else snprintf(t, sizeof t, rl.up ? "menyapa..." : "tidak terhubung");
  infoSet(IR_RLST, t);
  infoSet(IR_RLRST, rl.rst[0] ? rl.rst : "-");
  snprintf(t, sizeof t, "%lu ok / %lu rusak", (unsigned long)rl.rxOk, (unsigned long)rl.rxBad); infoSet(IR_RLFR, t);
  snprintf(t, sizeof t, "%lu", (unsigned long)rl.cmdFail); infoSet(IR_RLCMD, t);
  snprintf(t, sizeof t, "%lu dtk", (unsigned long)rl.upSec); infoSet(IR_RLUP, t);
  for (int i = 0; i < 8; i++) t[i] = (rl.mask >> (7 - i)) & 1 ? '1' : '0';
  snprintf(t + 8, sizeof t - 8, "  (CH8..CH1)"); infoSet(IR_RLMASK, t);
  for (int k = 0; k < 2; k++) {
    if (jobs[k].st == JS_RUN) { int r = jobRemainSec(k, now); snprintf(t, sizeof t, "%s · sisa %02d:%02d", tr(jobOpt(k)->name), r / 60, r % 60); }
    else if (jobs[k].st == JS_DONE) snprintf(t, sizeof t, "selesai, belum diambil");
    else snprintf(t, sizeof t, "kosong");
    infoSet(k ? IR_JDRY : IR_JWASH, t);
  }
  uint32_t sec = now / 1000;
  snprintf(t, sizeof t, "%lu h %02lu j %02lu m", (unsigned long)(sec / 86400), (unsigned long)(sec / 3600 % 24), (unsigned long)(sec / 60 % 60)); infoSet(IR_UP, t);
  snprintf(t, sizeof t, "%u KB (maks %u KB)", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024)); infoSet(IR_HEAP, t);
  infoSet(IR_DEMO, cfg.demo ? "aktif, 1 dtk = 1 mnt" : "mati (durasi asli)");
  snprintf(t, sizeof t, "%d%%", cfg.bright); infoSet(IR_BRIGHT, t);
  lv_label_set_text(infoBtnLbl, rv ? "Sembunyikan data sensitif" : "Tampilkan data sensitif (PIN)");
  lv_obj_center(infoBtnLbl);
}

static lv_obj_t *buildInfo() {
  lv_obj_t *s = mkScreen(C_NAVY);
  mkHeader(s, SC_SETTINGS, NULL);
  lv_obj_t *t = mkLbl(s, "Info perangkat", &f_fredoka_28, C_WHITE);           // 58..93
  lv_obj_set_pos(t, 16, 58);
  infoList = mkBox(s, 12, 98, 296, 382, C_NAVY, 0);                           // 98..480, scrolls
  lv_obj_set_style_bg_opa(infoList, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(infoList, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(infoList, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(infoList, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *btn = mkPill(infoList, "", 4, 0, 288, 40, C_TEAL, C_NAVY, &f_nunito_16, evtReveal, NULL);
  infoBtnLbl = lv_obj_get_child(btn, 0);
  int y = 48;
  lv_obj_t *card = NULL;
  int cy = 0, cardTop = 0;
  for (int i = 0; i < IR_COUNT; i++) {
    const InfoDef &d = INFO_DEF[i];
    if (d.section) {
      if (card) { lv_obj_set_height(card, cy + 6); y = cardTop + cy + 6 + 8; }
      cardTop = y;
      card = mkBox(infoList, 4, cardTop, 288, 40, C_NAVY2, 18);
      lv_obj_t *st = mkLbl(card, d.section, &f_nunito_16, C_TEAL);
      lv_obj_set_pos(st, 14, 8);
      cy = 34;
    }
    lv_obj_t *k = mkLbl(card, d.key, &f_nunito_14, C_MUTED);
    lv_obj_set_pos(k, 14, cy + 1);
    lv_label_set_long_mode(k, LV_LABEL_LONG_DOT);
    lv_obj_set_size(k, 104, d.h > 24 ? d.h - 4 : 18);
    infoVal[i] = mkLbl(card, "", &f_nunito_14, C_WHITE);
    lv_obj_set_pos(infoVal[i], 120, cy + 1);
    lv_obj_set_width(infoVal[i], 160);
    lv_label_set_long_mode(infoVal[i], d.h > 24 ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_obj_set_height(infoVal[i], d.h - 4);
    cy += d.h;
  }
  lv_obj_set_height(card, cy + 6);
  y = cardTop + cy + 6 + 8;
  lv_obj_t *sp = mkBox(infoList, 0, y + 8, 4, 4, C_NAVY, 0);                  // scroll end spacer
  lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
  infoRefresh();
  return s;
}

// ============================== SCREEN: WEB FORM (how to open it from a PC / phone) ==============================
static lv_obj_t *webQrCard, *webQr, *webMsg, *webVal[4];
static char webQrLast[64];

static void webInfoRefresh() {
  bool up = WiFi.status() == WL_CONNECTED;
  char host[32], t[64];
  webHost(host, sizeof host);
  if (up) {
    snprintf(t, sizeof t, "http://%s/", WiFi.localIP().toString().c_str());
    if (strcmp(t, webQrLast)) { strlcpy(webQrLast, t, sizeof webQrLast); lv_qrcode_update(webQr, t, strlen(t)); }
    lv_obj_clear_flag(webQr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(webMsg, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(webQr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(webMsg, LV_OBJ_FLAG_HIDDEN);
    webQrLast[0] = 0;
  }
  snprintf(t, sizeof t, "%s.local", host);
  lv_label_set_text(webVal[0], up ? t : "-");
  lv_label_set_text(webVal[1], up ? WiFi.localIP().toString().c_str() : "-");
  lv_label_set_text(webVal[2], "admin");
  lv_label_set_text(webVal[3], strcmp(cfg.pin, "123456") ? "PIN admin" : "PIN admin (bawaan!)");
}

static lv_obj_t *buildWebInfo() {
  lv_obj_t *s = mkScreen(C_NAVY);
  addBubbles(s, 4, lv_color_hex(0x7FD8E2), 5, 290, 480, 470, 6000, 10000, 10, 22);
  mkHeader(s, SC_SETTINGS, NULL);
  lv_obj_t *t = mkLbl(s, "Form web", &f_fredoka_28, C_WHITE);                  // 58..93
  lv_obj_set_pos(t, 16, 58);
  webQrCard = mkBox(s, 85, 100, 150, 150, C_WHITE, 16);                       // 100..250
  webQr = lv_qrcode_create(webQrCard, 130, C_NAVY, C_WHITE);
  lv_obj_center(webQr);
  webMsg = mkLbl(webQrCard, "Wi-Fi belum\nterhubung", &f_nunito_16, C_NAVY);
  lv_obj_set_style_text_align(webMsg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(webMsg);
  mkLblC(s, "Scan dengan HP, atau buka\nalamat di bawah dari PC", &f_nunito_14, C_LIGHT, 256, 290);   // 256..296
  lv_obj_t *card = mkBox(s, 12, 304, 296, 164, C_NAVY2, 18);                  // 304..468
  static const char *KEYS[4] = { "Alamat", "Alamat IP", "Login", "Kata sandi" };
  for (int i = 0; i < 4; i++) {
    lv_obj_t *k = mkLbl(card, KEYS[i], &f_nunito_14, C_MUTED);
    lv_obj_set_pos(k, 14, 12 + i * 26);
    webVal[i] = mkLbl(card, "", &f_nunito_14, C_WHITE);
    lv_label_set_long_mode(webVal[i], LV_LABEL_LONG_DOT);
    lv_obj_set_size(webVal[i], 168, 20);
    lv_obj_set_pos(webVal[i], 112, 12 + i * 26);
  }
  lv_obj_t *note = mkLbl(card, "Isi dari PC: pemilik, ID pemilik, ID mesin,\nserver API, kunci API, URL OTA.", &f_nunito_14, C_LIGHT);
  lv_obj_set_pos(note, 14, 122);
  webQrLast[0] = 0;
  webInfoRefresh();
  return s;
}

// ============================== SCREEN: STM32 HEALTH CHECK ==============================
// Runs the health check of the serial link in both directions: 10 pings ESP32 -> STM32 (round-trip time), then the STM32 pings the
// ESP32 10 times (self-test), then the controller reports its counters. Result: one verdict plus a checklist.
enum HState : uint8_t { HS_WAIT, HS_OK, HS_WARN, HS_BAD, HS_NA };
struct HRow { HState st; char val[40]; };
#define H_ROWS 7
static lv_obj_t *hDot[H_ROWS], *hVal[H_ROWS], *hVerdict, *hSub, *hBtn, *hBtnLbl;
static HState hVerdictLast = HS_NA;

// verdict of the last finished test (for the Settings row)
static int hLastVerdict = -1;                       // -1 never run, 0 healthy, 1 attention, 2 problem
static const char *healthSummary() {
  if (rlHealthBusy()) return "menguji...";
  return hLastVerdict < 0 ? "belum diuji" : hLastVerdict == 0 ? "sehat" : hLastVerdict == 1 ? "perhatian" : "ada gangguan";
}

static void healthRows(HRow r[H_ROWS]) {
  bool fin = rh.state == RH_DONE;
  for (int i = 0; i < H_ROWS; i++) { r[i].st = HS_WAIT; strlcpy(r[i].val, "menunggu...", sizeof r[i].val); }
  // 1 serial link
  if (rh.linkFail || !rl.up || !rl.helloOk) { r[0].st = HS_BAD; strlcpy(r[0].val, "tidak terhubung", sizeof r[0].val); }
  else { r[0].st = HS_OK; snprintf(r[0].val, sizeof r[0].val, "OK · fw %s", rl.fw); }
  if (rh.linkFail) { for (int i = 1; i < H_ROWS; i++) { r[i].st = HS_NA; strlcpy(r[i].val, "-", sizeof r[i].val); } return; }
  // 2 ESP32 -> STM32
  if (rh.pingSent) {
    snprintf(r[1].val, sizeof r[1].val, "%u/%u · %lu.%lu ms", rh.pingOk, rh.pingN, (unsigned long)(rh.rttAvgUs / 1000), (unsigned long)(rh.rttAvgUs / 100 % 10));
    if (rh.state == RH_PING && rh.pingSent < rh.pingN) r[1].st = HS_WAIT;
    else r[1].st = rh.pingOk == rh.pingN ? HS_OK : rh.pingOk * 10 >= rh.pingN * 7 ? HS_WARN : HS_BAD;
    if (rh.state == RH_PING && rh.pingSent >= rh.pingN && rh.pingOk < rh.pingN) r[1].st = HS_WAIT;
  }
  // 3 STM32 -> ESP32
  if (rh.stGot) {
    snprintf(r[2].val, sizeof r[2].val, "%u/%u · %lu ms", rh.stOk, rh.stN, (unsigned long)rh.stRttMs);
    r[2].st = rh.stOk == rh.stN ? HS_OK : rh.stOk * 10 >= rh.stN * 7 ? HS_WARN : HS_BAD;
  } else if (fin || rh.state == RH_QUERY) { r[2].st = HS_BAD; strlcpy(r[2].val, "tidak ada hasil", sizeof r[2].val); }
  // 4 corrupted frames (CRC errors) on either side
  if (rh.infoGot || fin) {
    snprintf(r[3].val, sizeof r[3].val, "ESP %lu · STM %lu", (unsigned long)rl.rxBad, (unsigned long)rh.ctlBad);
    uint32_t b = rl.rxBad + rh.ctlBad;
    r[3].st = b == 0 ? HS_OK : b < 10 ? HS_WARN : HS_BAD;
  }
  // 5 last restart of the controller
  if (rl.rst[0]) {
    snprintf(r[4].val, sizeof r[4].val, "%s", !strcmp(rl.rst, "power") ? "nyala normal" : !strcmp(rl.rst, "pin") ? "tombol reset" : !strcmp(rl.rst, "iwdg") ? "watchdog!" : rl.rst);
    r[4].st = (!strcmp(rl.rst, "iwdg") || !strcmp(rl.rst, "wwdg")) ? HS_WARN : HS_OK;
  }
  // 6 frames seen by the controller
  if (rh.infoGot) { snprintf(r[5].val, sizeof r[5].val, "rx %lu · tx %lu", (unsigned long)rh.ctlRx, (unsigned long)rh.ctlTx); r[5].st = HS_OK; }
  else if (fin) { r[5].st = HS_BAD; strlcpy(r[5].val, "tidak ada laporan", sizeof r[5].val); }
  // 7 sensors (optional)
  if (rh.infoGot || fin) {
    if (rh.vddMv > 0) {
      snprintf(r[6].val, sizeof r[6].val, "%d.%02d V · %d C", rh.vddMv / 1000, rh.vddMv / 10 % 100, rh.tempC);
      r[6].st = (rh.vddMv < 2800 || rh.vddMv > 3600 || rh.tempC > 85) ? HS_WARN : HS_OK;
    } else { r[6].st = HS_NA; strlcpy(r[6].val, "tidak tersedia", sizeof r[6].val); }
  }
}

static lv_color_t hCol(HState st) {
  return st == HS_OK ? C_MINT : st == HS_WARN ? C_ORANGE : st == HS_BAD ? lv_color_hex(0xFF6B6B) : st == HS_NA ? C_MUTED : C_NAVY3;
}

static void healthRefresh() {
  HRow r[H_ROWS];
  healthRows(r);
  int bad = 0, warn = 0, wait = 0;
  for (int i = 0; i < H_ROWS; i++) {
    lv_obj_set_style_bg_color(hDot[i], hCol(r[i].st), 0);
    if (strcmp(lv_label_get_text(hVal[i]), tr(r[i].val))) lv_label_set_text(hVal[i], r[i].val);
    lv_obj_set_style_text_color(hVal[i], r[i].st == HS_WAIT ? C_MUTED : C_WHITE, 0);
    bad += r[i].st == HS_BAD; warn += r[i].st == HS_WARN; wait += r[i].st == HS_WAIT;
  }
  bool busy = rlHealthBusy();
  const char *v; lv_color_t col; const char *sub;
  if (busy) { v = "Menguji..."; col = C_TEAL; sub = rh.state == RH_PING ? "ESP32 ping ke STM32" : rh.state == RH_SELFTEST ? "STM32 ping ke ESP32" : "meminta laporan STM32"; }
  else if (rh.linkFail || r[0].st == HS_BAD) { v = "GANGGUAN"; col = hCol(HS_BAD); sub = "STM32 tidak menjawab, cek kabel"; hLastVerdict = 2; }
  else if (bad) { v = "GANGGUAN"; col = hCol(HS_BAD); sub = "ada pemeriksaan yang gagal"; hLastVerdict = 2; }
  else if (warn) { v = "PERHATIAN"; col = hCol(HS_WARN); sub = "jalan, tapi ada yang perlu dicek"; hLastVerdict = 1; }
  else { v = "SEHAT"; col = hCol(HS_OK); sub = "koneksi dua arah baik"; hLastVerdict = 0; }
  if (strcmp(lv_label_get_text(hVerdict), tr(v))) lv_label_set_text(hVerdict, v);
  lv_obj_set_style_text_color(hVerdict, col, 0);
  if (strcmp(lv_label_get_text(hSub), tr(sub))) lv_label_set_text(hSub, sub);
  lv_obj_set_style_bg_color(hBtn, busy ? C_NAVY3 : C_TEAL, 0);
  lv_obj_set_style_text_color(hBtnLbl, busy ? C_MUTED : C_NAVY, 0);
}

static void evtHealthRun(lv_event_t *) { if (!rlHealthBusy()) rlHealthStart(); }

static lv_obj_t *buildHealth() {
  lv_obj_t *s = mkScreen(C_NAVY);
  mkHeader(s, SC_SETTINGS, NULL);
  lv_obj_t *t = mkLbl(s, "Kesehatan STM32", &f_fredoka_28, C_WHITE);           // 58..93
  lv_obj_set_pos(t, 16, 58);
  lv_obj_t *vb = mkBox(s, 15, 100, 290, 62, C_NAVY2, 18);                      // 100..162
  hVerdict = mkLbl(vb, "", &f_fredoka_28, C_TEAL);
  lv_obj_set_style_text_align(hVerdict, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(hVerdict, LV_LABEL_LONG_CLIP);
  lv_obj_set_size(hVerdict, 270, 34);
  lv_obj_set_pos(hVerdict, 10, 2);
  hSub = mkLbl(vb, "", &f_nunito_14, C_LIGHT);
  lv_obj_set_style_text_align(hSub, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(hSub, LV_LABEL_LONG_CLIP);
  lv_obj_set_size(hSub, 270, 20);
  lv_obj_set_pos(hSub, 10, 36);
  static const char *NAMES[H_ROWS] = { "Koneksi serial", "Ping ke STM32", "Ping dari STM32", "Frame rusak", "Restart STM32", "Frame STM32", "Sensor MCU" };
  lv_obj_t *card = mkBox(s, 15, 170, 290, 7 * 36 + 12, C_NAVY2, 18);          // 170..434
  for (int i = 0; i < H_ROWS; i++) {
    int y = 8 + i * 36;
    hDot[i] = mkBox(card, 14, y + 12, 12, 12, C_NAVY3, LV_RADIUS_CIRCLE);
    lv_obj_t *k = mkLbl(card, NAMES[i], &f_nunito_14, C_MUTED);
    lv_label_set_long_mode(k, LV_LABEL_LONG_DOT);
    lv_obj_set_size(k, 128, 18);
    lv_obj_set_pos(k, 34, y + 9);
    hVal[i] = mkLbl(card, "", &f_nunito_14, C_WHITE);
    lv_label_set_long_mode(hVal[i], LV_LABEL_LONG_DOT);
    lv_obj_set_size(hVal[i], 118, 18);
    lv_obj_set_style_text_align(hVal[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(hVal[i], 160, y + 9);
  }
  hBtn = mkPill(s, "Uji ulang", 15, 442, 290, 34, C_TEAL, C_NAVY, &f_fredoka_20, evtHealthRun, NULL);    // 442..476
  hBtnLbl = lv_obj_get_child(hBtn, 0);
  rlHealthStart();
  healthRefresh();
  return s;
}

// ============================== SCREEN: OTA UPDATE ==============================
static lv_obj_t *otaBar, *otaPct, *otaMsg;
static bool otaStarted = false, otaForceInstall = false;
static uint32_t otaErrAt = 0;

static lv_obj_t *buildOta() {
  lv_obj_t *s = mkScreen(C_NAVY);
  addBubbles(s, 6, lv_color_hex(0x7FD8E2), 5, 290, 480, 470, 5000, 9000, 10, 26);
  mkLogo(s, 96, 44, 128, true);                                                       // 44..172
  mkLblC(s, "Memperbarui\nfirmware", &f_fredoka_28, C_WHITE, 190);                     // 190..260
  char v[48];
  snprintf(v, sizeof v, "v%s  >  v%s", FW_VERSION, ota.latest);
  mkLblC(s, v, &f_nunito_16, C_TEAL, 268);                                             // 268..291
  otaBar = lv_bar_create(s);                                                           // 312..326
  lv_obj_set_size(otaBar, 240, 14);
  lv_obj_set_pos(otaBar, 40, 312);
  lv_bar_set_range(otaBar, 0, 100);
  lv_obj_set_style_bg_color(otaBar, C_NAVY3, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(otaBar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(otaBar, C_TEAL, LV_PART_INDICATOR);
  lv_obj_set_style_radius(otaBar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_radius(otaBar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  otaPct = mkLblC(s, "0%", &f_fredoka_36, C_WHITE, 338);                               // 338..383
  otaMsg = mkLblC(s, "Jangan matikan daya.\nMesin akan menyala ulang sendiri.", &f_nunito_14, C_LIGHT, 400);   // 400..440
  otaStarted = false;
  otaErrAt = 0;
  return s;
}

// install gate: idle screen (checked by the caller), nobody touched the kiosk for 5 min, inside the night window, enough heap
static bool otaGate(uint32_t now) {
  if (anyJobRunning()) return false;   // never reboot in the middle of a paid cycle (also when forced)
  if (otaForceInstall) return true;
  if (now - lastActMs < 300000UL) return false;
  if (!otaInWindow()) return false;
  return ESP.getFreeHeap() >= 80000;
}
static void startOtaFlow() {
  otaForceInstall = false;
  if (!cfgFlushVerify()) {                          // never update unless the settings are safely stored
    strlcpy(ota.msg, "gagal menyimpan pengaturan", sizeof ota.msg);
    ota.state = OTA_ERROR;
    return;
  }
  setRelays(false, false);
  rlAllOff();
  showScreen(SC_OTA);
}

// ============================== SCREEN: RELAY TEST (installer) ==============================
// Switches the 8 relays of the STM32 controller one by one (20 s safety timer, refreshed while this page is open).
static lv_obj_t *rtBtn[8], *rtName[8], *rtState[8], *rtStatus;
static uint8_t rtWant = 0;

static void rtUpdate() {
  char t[96];
  if (rl.helloOk) snprintf(t, sizeof t, "Kontroler terhubung  ·  fw %s", rl.fw);
  else if (rl.up) snprintf(t, sizeof t, "Kontroler menjawab, menyambung...");
  else snprintf(t, sizeof t, "Kontroler tidak terhubung");
  lv_label_set_text(rtStatus, t);
  lv_obj_set_style_text_color(rtStatus, rl.helloOk ? C_MINT : lv_color_hex(0xFF6B6B), 0);
  for (int i = 0; i < 8; i++) {
    bool on = rl.mask & (1u << i), want = rtWant & (1u << i);
    lv_obj_set_style_bg_color(rtBtn[i], on ? (i < 4 ? C_BLUE : C_ORANGE) : C_NAVY2, 0);
    lv_obj_set_style_text_color(rtName[i], on ? C_NAVY : C_WHITE, 0);
    lv_obj_set_style_text_color(rtState[i], on ? C_NAVY : C_MUTED, 0);
    lv_label_set_text(rtState[i], on ? "HIDUP" : (want ? "menunggu..." : "MATI"));
  }
}
static void evtRt(lv_event_t *e) {
  uint8_t bit = (uint8_t)(1u << (int)(intptr_t)lv_event_get_user_data(e));
  if (rtWant & bit) { rtWant &= (uint8_t)~bit; rlRelay(0, bit, 0); }
  else { rtWant |= bit; rlRelay(bit, 0, 20); }
  rtUpdate();
}
static void evtRtOff(lv_event_t *) { rtWant = 0; rlAllOff(); rtUpdate(); }

static lv_obj_t *buildRelayTest() {
  lv_obj_t *s = mkScreen(C_NAVY);
  mkHeader(s, SC_SETTINGS, NULL);
  lv_obj_t *t = mkLbl(s, "Tes relay", &f_fredoka_28, C_WHITE);                       // 58..93
  lv_obj_set_pos(t, 16, 58);
  rtStatus = mkLbl(s, "", &f_nunito_14, C_MUTED);                                     // 96..116
  lv_label_set_long_mode(rtStatus, LV_LABEL_LONG_DOT);
  lv_obj_set_size(rtStatus, 290, 20);
  lv_obj_set_pos(rtStatus, 16, 96);
  lv_obj_t *g1 = mkLbl(s, "CUCI  ·  kanal 1-4", &f_nunito_14, C_BLUE);                // 120..140
  lv_obj_set_pos(g1, 16, 120);
  lv_obj_t *g2 = mkLbl(s, "KERING  ·  kanal 5-8", &f_nunito_14, C_ORANGE);
  lv_obj_set_pos(g2, 166, 120);
  for (int i = 0; i < 8; i++) {                                                       // rows 142..422
    int col = i / 4, row = i % 4;
    rtBtn[i] = mkClickBox(s, 15 + col * 150, 142 + row * 72, 140, 64, C_NAVY2, 16, evtRt, (void *)(intptr_t)i);
    char n[12];
    snprintf(n, sizeof n, "Relay %d", i + 1);
    rtName[i] = mkLbl(rtBtn[i], n, &f_fredoka_20, C_WHITE);
    lv_obj_set_pos(rtName[i], 14, 6);
    rtState[i] = mkLbl(rtBtn[i], "MATI", &f_nunito_14, C_MUTED);
    lv_obj_set_pos(rtState[i], 14, 36);
  }
  mkPill(s, "Semua mati", 15, 432, 290, 40, lv_color_hex(0xEF4444), C_WHITE, &f_fredoka_20, evtRtOff, NULL);   // 432..472
  rtWant = 0;
  rtUpdate();
  return s;
}

// ============================== SCREEN SWAP + BACKLIGHT FADE ==============================
typedef lv_obj_t *(*BuildFn)();
static const BuildFn BUILDERS[SC_COUNT] = { buildIdle, buildService, buildCycle, buildPay, buildSuccess, buildRun, buildDone,
                                          buildPin, buildSettings, buildWifi, buildEdit, buildOta,
                                          buildMask, buildPhone, buildCheck, buildOtp, buildMemberHome, buildRelayTest, buildInfo, buildWebInfo, buildHealth, buildAbout };

static Scr pendingScr = SC_IDLE;
static int blDummy = 0;

static void blCb(void *, int32_t v) { ledcWrite(BL_CH, v); }
static void blAnim(int from, int to, uint32_t t, lv_anim_ready_cb_t ready) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &blDummy);
  lv_anim_set_exec_cb(&a, blCb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_time(&a, t);
  if (ready) lv_anim_set_ready_cb(&a, ready);
  lv_anim_start(&a);
}

static void swapNow(Scr id, bool first) {
  if (curScr == SC_RTEST && id != SC_RTEST) { rtWant = 0; rlAllOff(); }
  if (curScr == SC_INFO && id != SC_INFO) { infoList = NULL; if (id != SC_PIN) infoRevealUntil = 0; }
  pool ^= 1;
  nBubs = nWaves = nLogo = nDrums = 0;
  lv_obj_t *old = lv_scr_act();
  lv_obj_t *n = BUILDERS[id]();
  curScr = id;
  if (id == SC_SETTINGS || id == SC_WIFI || id == SC_EDIT || id == SC_WEB) webStart(); else webStop();
  if (id != SC_PIN) pinReveal = false;
  if (id == SC_IDLE) { infoRevealUntil = 0; memberReset(); }
  if (id == SC_SUCCESS) ledFlash(LC_GREEN, 3);
  else if (id == SC_DONE) ledFlash(LC_GREEN, 5);
  lv_scr_load(n);
  if (!first && old && old != n) lv_obj_del(old);
}

static void fadeInTimer(lv_timer_t *) { blAnim(0, blMax, 180, [](lv_anim_t *) { busy = false; }); }
static void fadeOutReady(lv_anim_t *) {
  swapNow(pendingScr, false);
  lv_timer_t *t = lv_timer_create(fadeInTimer, 140, NULL);
  lv_timer_set_repeat_count(t, 1);
}
static void showScreen(Scr id) {
  if (busy) return;
  busy = true;
  pendingScr = id;
  lastActMs = millis();
  blAnim(blMax, 0, 130, fadeOutReady);
}

// ============================== MAIN TICK ==============================
static void appTick(lv_timer_t *) {
  uint32_t now = millis();
  jobsTick(now);                       // cycles keep running during screen swaps, payments, admin pages...
  if (busy) return;
  otaTick(now, WiFi.status() == WL_CONNECTED, curScr == SC_IDLE);
  static OtaState lastOs = OTA_IDLE;
  static OtaState lastLog = (OtaState)255;
  if (ota.state != lastLog) { lastLog = ota.state; Serial.printf("OTA: state=%d latest=%s msg=%s url=%s\n", (int)ota.state, ota.latest, ota.msg, otaUrlEff()[0] ? otaUrlEff() : "(not set)"); }
  if (ota.state == OTA_ERROR && lastOs != OTA_ERROR) ledFlash(LC_RED, 3);
  lastOs = ota.state;
  switch (curScr) {
    case SC_IDLE: {
      static uint32_t lw = 0;
      if (now - lw > 1000) { lw = now; wifiIconUpdate(); idleJobsUpdate(now); }
      if (otaUpdateReady() && otaGate(now)) startOtaFlow();
      break;
    }
    case SC_OTA: {
      if (!otaStarted) { otaStarted = true; otaInstall(); }
      static uint32_t lw = 0;
      if (now - lw > 250) {
        lw = now;
        lv_bar_set_value(otaBar, ota.progress, LV_ANIM_OFF);
        char pc[8];
        snprintf(pc, sizeof pc, "%u%%", (unsigned)ota.progress);
        lv_label_set_text(otaPct, pc);
        if (ota.state == OTA_ERROR) {
          char m[96];
          snprintf(m, sizeof m, "Gagal: %s\nMencoba lagi nanti.", tr(ota.msg));
          lv_label_set_text(otaMsg, m);
          lv_obj_set_style_text_color(otaMsg, lv_color_hex(0xFF6B6B), 0);
          if (!otaErrAt) otaErrAt = now;
          else if (now - otaErrAt > 6000) { otaErrAt = 0; otaAck(); showScreen(SC_IDLE); }
        } else if (otaStarted && ota.state == OTA_IDLE) {
          showScreen(SC_IDLE);
        }
      }
      break;
    }
    case SC_ABOUT:
      if (now - lastActMs > IDLE_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    case SC_SERVICE:
      { static uint32_t lb = 0; if (now - lb > 1000) { lb = now; svcBadgeUpdate(now); } }
      if (now - lastActMs > IDLE_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    case SC_CYCLE:
      cycPayRefresh();
      if (now - lastActMs > IDLE_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    case SC_MASK:
    case SC_PHONE:
    case SC_MHOME:
      if (now - lastActMs > IDLE_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    case SC_CHECK:
      if (!chkFound && now - chkStart > 2800) checkFoundApply();
      if (now - lastActMs > IDLE_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    case SC_OTP:
      otpTick(now);
      if (now - lastActMs > IDLE_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    case SC_SETTINGS: {
      static uint32_t lw = 0, li = 0;
      static bool lastLink = false;
      if (now - li > 10000 || rl.helloOk != lastLink) { li = now; lastLink = rl.helloOk; settingsRefresh(); }
      if (webDirty) { webDirty = false; lastActMs = now; settingsRefresh(); }
      if (now - lw > 1000) {
        lw = now;
        char wt[48];
        webShort(wt, sizeof wt);
        if (strcmp(lv_label_get_text(setWebLbl), tr(wt))) lv_label_set_text(setWebLbl, wt);
        lv_label_set_text(setVal[4], healthSummary());
      }
      if (now - lastActMs > ADMIN_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    }
    case SC_INFO: {
      static uint32_t li = 0;
      if (now - li > 1000 || (infoRevealUntil && now >= infoRevealUntil)) {
        li = now;
        if (infoRevealUntil && now >= infoRevealUntil) infoRevealUntil = 0;
        infoRefresh();
      }
      if (now - lastActMs > ADMIN_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    }
    case SC_WEB: {
      static uint32_t lw2 = 0;
      if (now - lw2 > 1000) { lw2 = now; webInfoRefresh(); }
      if (webDirty) { webDirty = false; lastActMs = now; webInfoRefresh(); }
      if (now - lastActMs > ADMIN_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    }
    case SC_HEALTH: {
      static uint32_t lh = 0;
      static RlHealthState lastHs = RH_IDLE;
      if (now - lh > 250) { lh = now; healthRefresh(); }
      if (rh.state != lastHs) {
        lastHs = rh.state;
        if (rh.state == RH_DONE) {
          healthRefresh();
          Serial.printf("HEALTH: %s | esp->stm %u/%u avg %lu us | stm->esp %u/%u %lu ms | stm rx=%lu tx=%lu bad=%lu | esp bad=%lu | rst=%s\n", healthSummary(),
                        rh.pingOk, rh.pingN, (unsigned long)rh.rttAvgUs, rh.stOk, rh.stN, (unsigned long)rh.stRttMs, (unsigned long)rh.ctlRx,
                        (unsigned long)rh.ctlTx, (unsigned long)rh.ctlBad, (unsigned long)rl.rxBad, rl.rst);
        }
      }
      if (rlHealthBusy()) lastActMs = now;
      if (now - lastActMs > ADMIN_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    }
    case SC_PIN:
    case SC_EDIT:
      if (now - lastActMs > ADMIN_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    case SC_RTEST: {
      static uint32_t lu = 0, lk = 0;
      if (now - lu > 300) { lu = now; rtUpdate(); }
      if (now - lk > 8000) { lk = now; if (rtWant) rlRelay(rtWant, 0, 20); }     // keep the safety timer alive while the page is open
      if (now - lastActMs > ADMIN_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    }
    case SC_WIFI: {
      static uint32_t lw = 0;
      if (scanning) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
          collectScan(n);
          Serial.printf("scan done: %d raw, %d listed\n", n, nScan);
          WiFi.scanDelete();
          scanning = false;
          lv_label_set_text(wifiScanLbl, "Cari jaringan");
          lv_obj_center(wifiScanLbl);
          buildScanRows();
        } else if (n == WIFI_SCAN_FAILED) {
          scanning = false;
          lv_label_set_text(wifiScanLbl, "Cari jaringan");
          lv_obj_center(wifiScanLbl);
        }
      }
      if (now - lw > 800) { lw = now; refreshWifiStatus(); }
      if (now - lastActMs > ADMIN_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    }
    case SC_PAY: {
      uint32_t el = (now - payStartMs) / 1000;
      if (el >= PAY_SEC) { ledFlash(LC_RED, 3); showScreen(SC_IDLE); break; }
      int left = PAY_SEC - (int)el;
      if (left != payLastSec) {
        payLastSec = left;
        char tb[8];
        snprintf(tb, sizeof tb, "%d:%02d", left / 60, left % 60);
        lv_label_set_text(payTime, tb);
        lv_arc_set_value(payArc, left * 1000 / PAY_SEC);
      }
      break;
    }
    case SC_SUCCESS:
      if (now - payStartMs > SUCCESS_AUTO_MS) goRun();
      break;
    case SC_RUN:
      runTick(now);
      break;
    case SC_DONE:
      if (now - doneStartMs > DONE_AUTO_MS) showScreen(SC_IDLE);
      break;
    default:
      break;
  }
}

// ============================== SETUP / LOOP ==============================
#if DEBUG_KEYS
static void debugKey(char c);
#endif

void setup() {
  Serial.begin(115200);
  otaBoot();
  Serial.printf("FW %s running from %s\n", FW_VERSION, esp_ota_get_running_partition()->label);
  spiffsOk = SPIFFS.begin(true);
  loadCfg();
  Serial.printf("reset reason: %d\n", (int)esp_reset_reason());
  Serial.printf("language: %s\n", cfg.lang ? "EN" : "ID");
  rlBegin(cfg.machineId, FW_VERSION);
#if DEBUG_KEYS
  rlSetDebugHandler(debugKey);
#endif
  jobsRestore();
  otaConfigure(otaUrlEff());

#if RELAY_WASH_PIN >= 0
  pinMode(RELAY_WASH_PIN, OUTPUT);
#endif
#if RELAY_DRY_PIN >= 0
  pinMode(RELAY_DRY_PIN, OUTPUT);
#endif
  setRelays(false, false);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  tft.initDMA();
  tft.startWrite();                                   // keep the display transaction open; flush only waits for the previous DMA

  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(BL_PIN, BL_CH);
  ledcWrite(BL_CH, 0);
  ledInit();

  gtInit();

  lv_init();
  buf1 = (lv_color_t *)heap_caps_malloc(SCREEN_W * DRAW_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA);
  buf2 = (lv_color_t *)heap_caps_malloc(SCREEN_W * DRAW_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA);
  if (!buf1 || !buf2) { Serial.println("FATAL: draw buffers"); delay(1000); ESP.restart(); }
  lv_disp_draw_buf_init(&drawBuf, buf1, buf2, SCREEN_W * DRAW_LINES);
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = SCREEN_W;
  dispDrv.ver_res = SCREEN_H;
  dispDrv.flush_cb = dispFlush;
  dispDrv.monitor_cb = monCb;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);

  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = touchRead;
  lv_indev_drv_register(&indevDrv);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wifiAttemptMs = millis();
  wifiConnect();

  swapNow(SC_IDLE, true);
  lv_timer_create(appTick, 100, NULL);
  busy = true;
  lv_timer_t *t = lv_timer_create(fadeInTimer, 250, NULL);
  lv_timer_set_repeat_count(t, 1);
  lastActMs = millis();
  esp_task_wdt_init(WDT_SECONDS, true);          // last: SPIFFS formatting / LED self-test above may take a while
  esp_task_wdt_add(NULL);
}

#if DEBUG_KEYS
static void debugKey(char c) {
  {
    if (c >= '0' && c <= '9') {
      showScreen((Scr)(c - '0'));
    } else if (c == 'w') { svc = 0; pick = 1; goRun(); }
    else if (c == 'd') { svc = 1; pick = 1; goRun(); }
    else if (c == 'a') showScreen(SC_MASK);
    else if (c == 'f') { memberBack = SC_IDLE; showScreen(SC_PHONE); }
    else if (c == 'k') { strlcpy(mDigits, "81234567890", sizeof mDigits); showScreen(SC_CHECK); }
    else if (c == 'l') { strlcpy(mDigits, "81234567890", sizeof mDigits); showScreen(SC_OTP); }
    else if (c == 'h') { memberActive = true; showScreen(SC_MHOME); }
    else if (c == 'o') { otaForceCheck(); Serial.println("OTA: forced check"); }
    else if (c == 'O') { otaForceInstall = true; Serial.println("OTA: gate bypassed"); }
    else if (c == 'e') openEdit("Pemilik", "Nama pemilik atau usaha", cfg.owner, sizeof cfg.owner, false, false, 0, SC_SETTINGS, onCfgSaved);
    else if (c == 'p') openEdit("PIN admin", "Tepat 6 angka", cfg.pin, 7, true, true, 6, SC_SETTINGS, onCfgSaved);
    else if (c == 'S' && curScr == SC_WIFI) { evtScan(NULL); Serial.println("scan started"); }
    else if (c == 'r') { rlRelay((rl.mask & 1) ? 0 : 1, (rl.mask & 1) ? 1 : 0, 10); Serial.println("relay 1 toggled"); }
    else if (c == 'x') { rlAllOff(); Serial.println("all relays off"); }
    else if (c == 't') { showScreen(SC_RTEST); }
    else if (c == 'L') { cfg.lang ^= 1; g_lang = cfg.lang; updateMachineName(); saveCfg(); Serial.printf("language: %s\n", cfg.lang ? "EN" : "ID"); showScreen(SC_IDLE); }
    else if (c == 'I') showScreen(SC_INFO);
    else if (c == 'W') showScreen(SC_WEB);
    else if (c == 'H') showScreen(SC_HEALTH);
  }
}
#endif

void loop() {
  esp_task_wdt_reset();
  static uint32_t last = millis(), perfLast = millis();
  uint32_t now = millis();
  lv_tick_inc(now - last);
  last = now;
  uint32_t th = micros();
  uint32_t d = lv_timer_handler();
  th = micros() - th;
  perfHandlerUs += th; if (th > perfHandlerMax) perfHandlerMax = th; perfLoops++;
  ledTick(now);
  webTick();
  otaHealthTick(now);
  rlTick(now);
  if (now - perfLast >= 2000) {
    if (perfN) Serial.printf("scr=%d frames/s=%lu kpx/frame=%lu | handler avg=%luus max=%luus | dmaWait=%luus/frame | loops/s=%lu\n", (int)curScr,
                             (unsigned long)(perfN * 1000UL / (now - perfLast)), (unsigned long)(perfPx / perfN / 1000),
                             (unsigned long)(perfHandlerUs / (perfLoops ? perfLoops : 1)), (unsigned long)perfHandlerMax,
                             (unsigned long)(perfWaitUs / perfN), (unsigned long)(perfLoops * 1000UL / (now - perfLast)));
    Serial.printf("   flush cpu total=%luus/frame\n", (unsigned long)(perfFlushUs / perfN));
    perfN = perfT = perfPx = 0; perfFlushUs = 0; perfWaitUs = perfHandlerUs = perfHandlerMax = perfLoops = 0;
    perfLast = now;
  }
  delay(d > 5 ? 5 : (d ? d : 1));
}
