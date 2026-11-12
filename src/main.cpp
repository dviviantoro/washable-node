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
#include "version.h"
#include "ota.h"
#include <lvgl.h>
#include <TFT_eSPI.h>

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
#define C_NAVY2   lv_color_hex(0x13315C)
#define C_NAVY3   lv_color_hex(0x1B3F73)
#define C_TEAL    lv_color_hex(0x00B4C5)
#define C_BLUE    lv_color_hex(0x3FA9F5)
#define C_ORANGE  lv_color_hex(0xFF9F43)
#define C_MINT    lv_color_hex(0x2ECC71)
#define C_LIGHT   lv_color_hex(0xBFD3E6)
#define C_MUTED   lv_color_hex(0x8DA9C4)
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

enum Scr { SC_IDLE, SC_SERVICE, SC_CYCLE, SC_PAY, SC_SUCCESS, SC_RUN, SC_DONE, SC_PIN, SC_SETTINGS, SC_WIFI, SC_EDIT, SC_OTA, SC_COUNT };

static int svc = 0;          // 0 = wash, 1 = dry
static int pick = 1;         // selected option
static Scr curScr = SC_IDLE;
static uint32_t lastActMs = 0;
static bool busy = false;     // a screen swap is in progress

static const Opt *curOpt() { return (svc ? DRY_OPTS : WASH_OPTS) + pick; }
static lv_color_t accentCol() { return svc ? C_ORANGE : C_BLUE; }

// ============================== SETTINGS (persisted in NVS) ==============================
struct Cfg {
  char owner[32], ownerId[32], machineId[8], ssid[33], pass[65], apiUrl[96], apiKey[64], pin[8], otaUrl[160];
  uint8_t bright;     // 20..100 %
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
  { "pin",     cfg.pin,       sizeof cfg.pin,       "1234" },
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
  }
  return true;
}

static void saveCfg() {
  prefs.begin("washable", false);
  for (const CfgStr &c : CFG_STR) prefs.putString(c.key, c.p);
  prefs.putUChar("bright", cfg.bright);
  prefs.putBool("demo", cfg.demo);
  prefs.putUChar("cfgv", 1);
  prefs.end();
  backupWrite();
}
static void loadCfg() {
  for (const CfgStr &c : CFG_STR) strlcpy(c.p, c.def, c.cap);
  cfg.bright = 100;
  cfg.demo = true;
  bool have = false, restored = false;
  if (prefs.begin("washable", true)) {
    have = prefs.isKey("mid") || prefs.isKey("ssid") || prefs.isKey("pin") || prefs.isKey("cfgv") || prefs.isKey("owner");
    if (have) {
      for (const CfgStr &c : CFG_STR) strlcpy(c.p, prefs.getString(c.key, c.def).c_str(), c.cap);
      cfg.bright = prefs.getUChar("bright", 100);
      cfg.demo = prefs.getBool("demo", true);
    }
    prefs.end();
  }
  if (!have) restored = backupRestore();            // NVS empty: try the flash mirror
  if (strlen(cfg.pin) < 4 || strlen(cfg.pin) > 6) strlcpy(cfg.pin, "1234", sizeof cfg.pin);
  if (!cfg.machineId[0]) strlcpy(cfg.machineId, "03", sizeof cfg.machineId);
  if (restored) { saveCfg(); Serial.println("CFG: restored from flash backup"); }
  else if (have && spiffsOk && !SPIFFS.exists("/cfg.bak")) backupWrite();   // first boot after the OTA-capable update
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
static void wifiConnect() {
  if (!cfg.ssid[0]) return;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(cfg.ssid, cfg.pass);
}

// ============================== HARDWARE ==============================
TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t drawBuf;
static lv_color_t buf1[SCREEN_W * 24];
static lv_color_t buf2[SCREEN_W * 24];
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

static void dispFlush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.dmaWait();
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushPixelsDMA((uint16_t *)&color_p->full, w * h);
  tft.endWrite();
  lv_disp_flush_ready(drv);
}

// ---- performance counters (printed on Serial)
static uint32_t perfN = 0, perfT = 0, perfPx = 0;
static uint32_t monLast = 0;
static void monCb(lv_disp_drv_t *, uint32_t t, uint32_t px) {
  perfN++; perfT += t; perfPx += px;
  uint32_t now = millis();
  if (px > 60000) Serial.printf("BIG gap=%lums px=%lu scr=%d\n", (unsigned long)(now - monLast), (unsigned long)px, (int)curScr);
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
struct Bub { int16_t x, amp, ybot, rise, sz; };
static Bub bubs[2][40];
static int nBubs = 0;
static uint32_t rndState = 12345;
static uint32_t rnd() { rndState = rndState * 1664525UL + 1013904223UL; return rndState >> 8; }

static void bubbleCb(void *var, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)var;
  Bub *b = (Bub *)lv_obj_get_user_data(o);
  int16_t s = lv_trigo_sin((int16_t)((v * 360) / 255));
  lv_obj_set_pos(o, b->x + ((s * b->amp) >> 15), b->ybot - (b->rise * v) / 255 - b->sz);
  int op = (v < 40) ? v * 5 : (v > 190 ? (255 - v) * 3 : 200);
  lv_obj_set_style_border_opa(o, (lv_opa_t)op, 0);
}

static void addBubbles(lv_obj_t *parent, int n, lv_color_t col, int xmin, int xmax, int ybot, int rise,
                       uint32_t tmin, uint32_t tmax, int szmin, int szmax) {
  for (int i = 0; i < n && nBubs < 40; i++) {
    Bub *b = &bubs[pool][nBubs++];
    b->sz = szmin + rnd() % (szmax - szmin + 1);
    b->x = xmin + rnd() % (xmax - xmin + 1);
    b->amp = 5 + rnd() % 12;
    b->ybot = ybot;
    b->rise = rise;
    lv_obj_t *o = mkBox(parent, 0, ybot, b->sz, b->sz, C_WHITE, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_10, 0);
    lv_obj_set_style_border_color(o, col, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_user_data(o, b);
    animStart(o, bubbleCb, 0, 255, tmin + rnd() % (tmax - tmin + 1), rnd() % 3500, true);
  }
}

// ---------- waves (row of bobbing circles over a body rect)
struct Wave { lv_obj_t *c[12]; int n, baseY, d, amp; };
static Wave waves[2][8];
static int nWaves = 0;

static void waveCb(void *var, int32_t v) {
  lv_obj_t *b = (lv_obj_t *)var;
  Wave *w = (Wave *)lv_obj_get_user_data(b);
  for (int i = 0; i < w->n; i++) {
    int16_t s = lv_trigo_sin((int16_t)((v * 360 / 1000 + i * 120) % 360));
    lv_obj_set_y(w->c[i], w->baseY - w->d / 2 + ((s * w->amp) >> 15));
  }
}

static lv_obj_t *mkWave(lv_obj_t *p, int x, int baseY, int w, int bottomY, lv_color_t col, int d, int amp,
                        uint32_t period) {
  Wave *wv = &waves[pool][nWaves++ % 8];
  wv->n = 0; wv->baseY = baseY; wv->d = d; wv->amp = amp;
  lv_obj_t *body = mkBox(p, x, baseY, w, bottomY - baseY, col, 0);
  int step = d * 2 / 3;
  for (int cx = x - d / 4; cx < x + w && wv->n < 12; cx += step) {
    lv_obj_t *c = mkBox(p, cx, baseY - d / 2, d, d, col, LV_RADIUS_CIRCLE);
    wv->c[wv->n++] = c;
  }
  lv_obj_set_user_data(body, wv);
  animStart(body, waveCb, 0, 1000, period, 0, true);
  return body;
}

// ---------- logo: porthole + wave + W
static lv_point_t logoPts[2][6][5];
static int nLogo = 0;

static lv_obj_t *mkLogo(lv_obj_t *p, int x, int y, int s, bool animated) {
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
  lv_obj_set_pos(o, b->x + ((lv_trigo_sin((int16_t)(v * 360 / 255)) * b->amp) >> 15), b->ybot - (b->rise * v) / 255);
  lv_obj_set_style_bg_opa(o, (lv_opa_t)((v < 50) ? v * 4 : (255 - v)), 0);
}
static void addFoam(lv_obj_t *parent, int n, int xmin, int xmax, int ybot, int rise) {
  for (int i = 0; i < n && nBubs < 40; i++) {
    Bub *b = &bubs[pool][nBubs++];
    b->sz = 6 + rnd() % 8; b->x = xmin + rnd() % (xmax - xmin); b->amp = 6; b->ybot = ybot; b->rise = rise;
    lv_obj_t *o = mkBox(parent, 0, ybot, b->sz, b->sz, C_WHITE, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_user_data(o, b);
    animStart(o, foamCb, 0, 255, 2400 + rnd() % 1200, i * 500, true);
  }
}
static void sparkCb(void *var, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)(v < 128 ? v * 2 : (255 - v) * 2), 0);
}
static void heatCb(void *var, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)var;
  Bub *b = (Bub *)lv_obj_get_user_data(o);
  lv_obj_set_y(o, b->ybot - (b->rise * v) / 255);
  lv_obj_set_style_bg_opa(o, (lv_opa_t)((v < 50) ? v * 4 : (255 - v)), 0);
}
static void addHeat(lv_obj_t *parent, int n, const int *xs, int ybot, int rise, lv_color_t col) {
  for (int i = 0; i < n && nBubs < 40; i++) {
    Bub *b = &bubs[pool][nBubs++];
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

// ============================== SCREEN: IDLE ==============================
static lv_obj_t *idleWifi = NULL;
static void wifiIconUpdate() {
  if (idleWifi && curScr == SC_IDLE)
    lv_obj_set_style_text_color(idleWifi, WiFi.status() == WL_CONNECTED ? C_MINT : C_ORANGE, 0);
}

static lv_obj_t *buildIdle() {
  lv_obj_t *s = mkScreen(lv_color_hex(0x0F2B50));
  addBubbles(s, 9, lv_color_hex(0x7FD8E2), 5, 290, 440, 420, 5500, 9500, 10, 30);

  mkLogo(s, 16, 12, 30, false);
  mkWordmark(s, 52, 11, &f_fredoka_20);
  lv_obj_t *pill = mkBox(s, 158, 12, 104, 30, C_NAVY3, 15);
  idleWifi = mkLbl(pill, LV_SYMBOL_WIFI, &lv_font_montserrat_16, WiFi.status() == WL_CONNECTED ? C_MINT : C_ORANGE);
  lv_obj_align(idleWifi, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_t *pl = mkLbl(pill, machineName, &f_nunito_14, C_WHITE);
  lv_obj_align(pl, LV_ALIGN_LEFT_MID, 34, 0);
  lv_obj_t *gear = mkClickBox(s, 272, 9, 36, 36, C_NAVY3, 18, evtGo, (void *)(intptr_t)SC_PIN);
  lv_obj_set_style_bg_color(gear, C_NAVY2, LV_STATE_PRESSED);
  lv_obj_t *gl = mkLbl(gear, LV_SYMBOL_SETTINGS, &lv_font_montserrat_16, C_LIGHT);
  lv_obj_center(gl);

  mkLogo(s, 98, 54, 124, true);                                     // y 54..178
  mkLblC(s, "Cucian kotor?\nGampang.", &f_fredoka_28, C_WHITE, 186);  // y 186..256
  mkLblC(s, "Cuci atau keringkan sendiri,\nbayar pakai QRIS.", &f_nunito_16, C_LIGHT, 262);  // ..308

  lv_obj_t *cta = mkBox(s, 35, 316, 250, 54, C_TEAL, 27);           // y 316..370
  lv_obj_t *cl = mkLbl(cta, "Sentuh untuk mulai", &f_fredoka_20, C_NAVY);
  lv_obj_align(cl, LV_ALIGN_LEFT_MID, 22, 0);
  lv_obj_t *ar = mkLbl(cta, LV_SYMBOL_RIGHT, &lv_font_montserrat_20, C_NAVY);
  lv_obj_align(ar, LV_ALIGN_RIGHT_MID, -20, 0);
  animStart(ar, transXCb, 0, 8, 500, 0, true, 500);

  if (cfg.owner[0]) {                                               // y 382..402
    char ow[64];
    snprintf(ow, sizeof ow, "Dikelola oleh %s", cfg.owner);
    lv_obj_t *o = mkLblC(s, ow, &f_nunito_14, C_MUTED, 382);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    lv_obj_set_height(o, 20);
  }

  // waves: crest tops are now at y >= 410, below the button and owner line
  mkWave(s, 0, 440, SCREEN_W, SCREEN_H, lv_color_hex(0x0C5F81), 48, 6, 5200);
  mkWave(s, 0, 456, SCREEN_W, SCREEN_H, lv_color_hex(0x2B73B0), 48, 6, 3400);

  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s, evtGo, LV_EVENT_CLICKED, (void *)(intptr_t)SC_SERVICE);
  return s;
}

// ============================== SCREEN: SERVICE ==============================
static void evtService(lv_event_t *e) {
  svc = (int)(intptr_t)lv_event_get_user_data(e);
  pick = 1;
  showScreen(SC_CYCLE);
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
    if (!i) mkWave(glass, 0, g * 56 / 100, g, g, C_BLUE, 26, 4, 1800);
    addClothes(glass, g, i ? CLOTH_DRY : CLOTH_WASH, i ? 4200 : 5200);
    if (i) {
      static const int xs[3] = { 28, 52, 76 };
      addHeat(card, 3, xs, 18, 16, C_WHITE);
    }
    lv_obj_t *nm = mkLbl(card, i ? "Keringkan" : "Cuci", i ? &f_fredoka_28 : &f_fredoka_36, C_NAVY);
    lv_obj_set_pos(nm, 132, i ? 42 : 36);
    lv_obj_t *sub = mkLbl(card, i ? "Mulai dari\nRp 10.000" : "Mulai dari\nRp 12.000", &f_nunito_16, C_NAVY);
    lv_obj_set_pos(sub, 132, 88);
  }
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
  lv_obj_set_style_text_color(cycTogWL, svc ? C_WHITE : C_NAVY, 0);
  lv_obj_set_style_text_color(cycTogDL, svc ? C_NAVY : C_WHITE, 0);
  char t[8];
  snprintf(t, sizeof t, "%u°", o[pick].temp);
  lv_label_set_text(cycTemp, t);
  lv_obj_set_style_text_color(cycTemp, ac, 0);
  lv_label_set_text(cycTotal, o[pick].price);
  lv_obj_align(cycTotal, LV_ALIGN_TOP_RIGHT, -16, 26);
  lv_obj_set_style_bg_color(cycMerc, ac, 0);
  lv_obj_set_style_bg_color(cycBulb, ac, 0);
  lv_obj_set_style_bg_color(cycGlow, ac, 0);
  lv_obj_set_style_bg_color(cycPay, ac, 0);
  mercBase = o[pick].lvl;
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
  cycPay = mkPill(panel, "Bayar QRIS", 16, 76, 258, 48, C_BLUE, C_NAVY, &f_fredoka_20, evtGo, (void *)(intptr_t)SC_PAY);
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
  if (svc) snprintf(line, sizeof line, "Keringkan · %s · %s", o->name, o->detail);
  else     snprintf(line, sizeof line, "Cuci · %s · %s", o->name, o->detail);
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
    lv_obj_t *d = mkBox(s, 224 + i * 13, 369, 8, 8, C_TEAL, LV_RADIUS_CIRCLE);
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
  lv_obj_t *hint = mkLbl(s, "Scan pakai e-wallet\natau m-banking apa saja", &f_nunito_14, C_LIGHT);
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

static void evtStartRun(lv_event_t *) { showScreen(SC_RUN); }

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
  snprintf(t, sizeof t, "%s diterima.\nPastikan pintu mesin tertutup rapat.", curOpt()->price);
  mkLblC(s, t, &f_nunito_16, C_LIGHT, 328);
  mkPill(s, "Mulai sekarang", 30, 400, 260, 60, C_MINT, C_NAVY, &f_fredoka_20, evtStartRun, NULL);
  payStartMs = millis();
  return s;
}

// ============================== SCREEN: RUN (wash / dry) ==============================
static uint32_t runStartMs = 0, runDurMs = 0;
static int runLastSec = -1, runStage = -1;
static lv_obj_t *runArc, *runTimeLbl, *runChip[3], *runChipLbl[3];

static void evtSkip(lv_event_t *) { runStartMs = millis() - runDurMs; }

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
    mkWave(glass, 0, g * 52 / 100, g, g, C_BLUE, 44, 6, 2200);
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

  runStartMs = millis();
  runDurMs = curOpt()->minutes * secPerMin() * 1000UL;
  runLastSec = -1;
  runStage = -1;
  setRelays(!dry, dry);
  return s;
}

static void runTick(uint32_t now) {
  uint32_t el = now - runStartMs;
  if (el >= runDurMs) {
    setRelays(false, false);
    showScreen(SC_DONE);
    return;
  }
  int remain = (runDurMs - el + 999) / 1000;
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
static void evtDrySvc(lv_event_t *) { svc = 1; pick = 1; showScreen(SC_CYCLE); }

static lv_obj_t *buildDone() {
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

static int pinTarget() { int n = (int)strlen(cfg.pin); return (n < 4 || n > 6) ? 4 : n; }

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
      showScreen(SC_SETTINGS);
    } else {
      lv_label_set_text(pinMsg, "PIN salah, coba lagi");
      pinLen = 0; pinBuf[0] = 0;
      pinUpdateDots();
    }
  }
}

static lv_obj_t *buildPin() {
  lv_obj_t *s = mkScreen(C_NAVY);
  mkHeader(s, pinReveal ? SC_SETTINGS : SC_IDLE, NULL);
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
  lv_obj_set_style_text_color(edTa, C_WHITE, 0);
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
  mkPill(s, "Batal", 12, 196, 140, 38, C_NAVY3, C_WHITE, &f_fredoka_20, evtEdCancel, NULL);   // y 196..234
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
  lv_obj_set_style_text_color(edKb, C_WHITE, LV_PART_ITEMS);
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
  lv_obj_add_flag(wifiList, LV_OBJ_FLAG_SCROLLABLE);
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
    web.send(429, "text/plain", "Terlalu banyak percobaan login. Coba lagi 1 menit.");
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
  if (web.hasArg("ok")) msg = "<p class=ok>Tersimpan. Layar mesin ikut diperbarui.</p>";
  else if (web.hasArg("e")) msg = "<p class=err>" + String(web.arg("e") == "1" ? "API URL harus diawali http:// atau https://" : (web.arg("e") == "3" ? "URL OTA harus diawali https://" : "Isi terlalu panjang atau tidak valid")) + "</p>";
  String h = F("<!doctype html><html lang=id><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>washable - pengaturan</title><style>"
               "body{margin:0;background:#0B2545;color:#fff;font:16px/1.4 system-ui,sans-serif;display:flex;justify-content:center;padding:24px 16px}"
               "main{width:100%;max-width:440px}h1{margin:0 0 4px;font-size:28px}h1 span{color:#00B4C5}p{color:#BFD3E6;margin:4px 0 16px}"
               "label{display:block;font-weight:700;margin:14px 0 6px}input{width:100%;box-sizing:border-box;padding:14px;border-radius:14px;"
               "border:2px solid #13315C;background:#13315C;color:#fff;font-size:17px}button{width:100%;margin-top:22px;padding:16px;border:0;"
               "border-radius:999px;background:#00B4C5;color:#0B2545;font-size:18px;font-weight:800}.ok{color:#2ECC71}.err{color:#FF6B6B}small{color:#8DA9C4}"
               "</style></head><body><main><h1>wash<span>able</span></h1><p>Pengaturan mesin. Perubahan langsung disimpan di mesin.</p>");
  h += msg;
  h += F("<form method=post action=/save><label>Pemilik</label><input name=owner maxlength=31 value=\"");
  h += htmlEsc(cfg.owner);
  h += F("\"><label>ID pemilik</label><input name=ownerid maxlength=31 value=\"");
  h += htmlEsc(cfg.ownerId);
  h += F("\"><label>ID mesin</label><input name=mid maxlength=7 value=\"");
  h += htmlEsc(cfg.machineId);
  h += F("\"><label>Server API (URL)</label><input name=apiurl maxlength=95 placeholder=\"https://api.contoh.com\" value=\"");
  h += htmlEsc(cfg.apiUrl);
  h += F("\"><label>Kunci API</label><input name=apikey type=password maxlength=63 autocomplete=off placeholder=\"");
  h += cfg.apiKey[0] ? "tersimpan - kosongkan jika tidak diubah" : "belum diisi";
  h += F("\"><small>Kunci API tidak pernah ditampilkan ulang.</small><label>URL manifest OTA (HTTPS)</label><input name=otaurl maxlength=159 placeholder=\"https://github.com/OWNER/REPO/releases/latest/download/manifest.json\" value=\"");
  h += htmlEsc(cfg.otaUrl);
  h += F("\"><small>Kosongkan untuk menonaktifkan pembaruan firmware otomatis.</small><button>Simpan</button></form></main></body></html>");
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
static void webUrlText(char *out, size_t n) {
  if (WiFi.status() == WL_CONNECTED) {
    char host[32];
    webHost(host, sizeof host);
    snprintf(out, n, "http://%s.local\nhttp://%s\nLogin: admin + PIN admin%s", host, WiFi.localIP().toString().c_str(),
             strcmp(cfg.pin, "1234") ? "" : "\nPIN masih 1234 - segera ganti!");
  } else {
    snprintf(out, n, "Wi-Fi belum terhubung.\nHubungkan Wi-Fi dulu agar PC bisa\nmembuka form pengaturan.");
  }
}

// ============================== SCREEN: SETTINGS ==============================
static lv_obj_t *setVal[2];
static lv_obj_t *setBrightLbl, *setDemoLbl, *setResetLbl, *setWebLbl;
static lv_obj_t *infoCard, *infoLbl, *infoBtn, *infoBtnLbl;
static bool resetArmed = false;

static void onCfgSaved() { updateMachineName(); saveCfg(); }

// device info card: owner / machine ID / server API / API key come from the web form;
// the API URL and key stay hidden until the admin PIN is confirmed (30 s)
static void infoRefresh() {
  bool rv = infoRevealed();
  char ost[72];
  otaStatusText(ost, sizeof ost);
  char t[700];
  snprintf(t, sizeof t, "Pemilik: %s\nID pemilik: %s\nID mesin: %s (%s)\nServer API: %s\nKunci API: %s\nFirmware v%s  ·  %s\nRAM %u KB\nMAC %s\nIP %s",
           cfg.owner[0] ? cfg.owner : "-", cfg.ownerId[0] ? cfg.ownerId : "-", cfg.machineId, machineName,
           cfg.apiUrl[0] ? (rv ? cfg.apiUrl : "tersimpan (disembunyikan)") : "belum diisi",
           cfg.apiKey[0] ? (rv ? cfg.apiKey : "********") : "belum diisi",
           FW_VERSION, ost, (unsigned)(ESP.getFreeHeap() / 1024), WiFi.macAddress().c_str(),
           WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-");
  lv_label_set_text(infoLbl, t);
  lv_obj_update_layout(infoLbl);
  int h = lv_obj_get_height(infoLbl);
  lv_obj_set_pos(infoBtn, 58, 30 + h + 10);
  lv_obj_set_height(infoCard, 30 + h + 10 + 34 + 12);
  lv_label_set_text(infoBtnLbl, rv ? "Sembunyikan" : "Tampilkan (PIN admin)");
  lv_obj_center(infoBtnLbl);
}

static void settingsRefresh() {
  char t[112];
  if (cfg.ssid[0]) snprintf(t, sizeof t, "%s · %s", cfg.ssid, WiFi.status() == WL_CONNECTED ? "terhubung" : "tidak terhubung");
  else snprintf(t, sizeof t, "Belum diatur");
  lv_label_set_text(setVal[0], t);
  lv_label_set_text(setVal[1], "****");
  infoRefresh();
}

static void evtSetWifi(lv_event_t *)  { showScreen(SC_WIFI); }
static void evtSetPin(lv_event_t *)   { openEdit("PIN admin", "4 sampai 6 angka", cfg.pin, 7, true, true, 4, SC_SETTINGS, onCfgSaved); }
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
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

  int y = 0;
  mkRow(list, y, 60, LV_SYMBOL_WIFI, "Wi-Fi", C_MINT, evtSetWifi, &setVal[0]);        y += 68;
  // local address of the PC web form
  lv_obj_t *wr = mkRow(list, y, 112, LV_SYMBOL_UPLOAD, "Form web (buka dari PC)", C_TEAL, NULL, NULL);
  setWebLbl = mkLbl(wr, "", &f_nunito_14, C_LIGHT);
  lv_obj_set_pos(setWebLbl, 58, 30);
  { char wt[160]; webUrlText(wt, sizeof wt); lv_label_set_text(setWebLbl, wt); }
  y += 120;
  mkRow(list, y, 60, LV_SYMBOL_KEYBOARD, "PIN admin", C_ORANGE, evtSetPin, &setVal[1]); y += 68;

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

  // device info (last row: its height changes when sensitive data is revealed)
  infoCard = mkRow(list, y, 150, LV_SYMBOL_LIST, "Info perangkat", C_LIGHT, NULL, NULL);
  lv_obj_set_y(lv_obj_get_child(infoCard, 0), 12);
  infoLbl = mkLbl(infoCard, "", &f_nunito_14, C_MUTED);
  lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(infoLbl, 214);
  lv_obj_set_pos(infoLbl, 58, 30);
  infoBtn = mkPill(infoCard, "", 58, 100, 214, 34, C_TEAL, C_NAVY, &f_nunito_16, evtReveal, NULL);
  infoBtnLbl = lv_obj_get_child(infoBtn, 0);
  resetArmed = false;
  settingsRefresh();
  y += lv_obj_get_height(infoCard) + 8;
  mkBox(list, 0, y + 8, 4, 4, C_NAVY, 0);                                      // scroll end spacer
  lv_obj_set_style_bg_opa(lv_obj_get_child(list, lv_obj_get_child_cnt(list) - 1), LV_OPA_TRANSP, 0);
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
  showScreen(SC_OTA);
}

// ============================== SCREEN SWAP + BACKLIGHT FADE ==============================
typedef lv_obj_t *(*BuildFn)();
static const BuildFn BUILDERS[SC_COUNT] = { buildIdle, buildService, buildCycle, buildPay, buildSuccess, buildRun, buildDone,
                                          buildPin, buildSettings, buildWifi, buildEdit, buildOta };

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
  pool ^= 1;
  nBubs = nWaves = nLogo = nDrums = 0;
  lv_obj_t *old = lv_scr_act();
  lv_obj_t *n = BUILDERS[id]();
  curScr = id;
  if (id == SC_SETTINGS || id == SC_WIFI || id == SC_EDIT) webStart(); else webStop();
  if (id != SC_PIN) pinReveal = false;
  if (id == SC_IDLE) infoRevealUntil = 0;
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
  if (busy) return;
  otaTick(now, WiFi.status() == WL_CONNECTED, curScr == SC_IDLE);
  switch (curScr) {
    case SC_IDLE: {
      static uint32_t lw = 0;
      if (now - lw > 1000) { lw = now; wifiIconUpdate(); }
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
          snprintf(m, sizeof m, "Gagal: %s\nMencoba lagi nanti.", ota.msg);
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
    case SC_SERVICE:
    case SC_CYCLE:
      if (now - lastActMs > IDLE_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    case SC_SETTINGS: {
      static uint32_t lw = 0, li = 0;
      if (now - li > 10000) { li = now; infoRefresh(); }
      if (webDirty) { webDirty = false; lastActMs = now; settingsRefresh(); }
      if (infoRevealUntil && now >= infoRevealUntil) { infoRevealUntil = 0; infoRefresh(); }
      if (now - lw > 1000) {
        lw = now;
        char wt[160];
        static char last[160];
        webUrlText(wt, sizeof wt);
        if (strcmp(wt, last)) { strlcpy(last, wt, sizeof last); lv_label_set_text(setWebLbl, wt); }
      }
      if (now - lastActMs > ADMIN_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
    }
    case SC_PIN:
    case SC_EDIT:
      if (now - lastActMs > ADMIN_TIMEOUT_MS) showScreen(SC_IDLE);
      break;
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
      if (el >= PAY_SEC) { showScreen(SC_IDLE); break; }
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
      if (now - payStartMs > SUCCESS_AUTO_MS) showScreen(SC_RUN);
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
void setup() {
  Serial.begin(115200);
  otaBoot();
  Serial.printf("FW %s running from %s\n", FW_VERSION, esp_ota_get_running_partition()->label);
  spiffsOk = SPIFFS.begin(true);
  loadCfg();
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

  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(BL_PIN, BL_CH);
  ledcWrite(BL_CH, 0);

  gtInit();

  lv_init();
  lv_disp_draw_buf_init(&drawBuf, buf1, buf2, SCREEN_W * 24);
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
  wifiConnect();

  swapNow(SC_IDLE, true);
  lv_timer_create(appTick, 100, NULL);
  busy = true;
  lv_timer_t *t = lv_timer_create(fadeInTimer, 250, NULL);
  lv_timer_set_repeat_count(t, 1);
  lastActMs = millis();
}

static void debugKeys() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c >= '0' && c <= '9') {
      showScreen((Scr)(c - '0'));
    } else if (c == 'w') { svc = 0; pick = 1; showScreen(SC_RUN); }
    else if (c == 'd') { svc = 1; pick = 1; showScreen(SC_RUN); }
    else if (c == 'o') { otaForceCheck(); Serial.println("OTA: forced check"); }
    else if (c == 'O') { otaForceInstall = true; Serial.println("OTA: gate bypassed"); }
    else if (c == 'e') openEdit("Pemilik", "Nama pemilik atau usaha", cfg.owner, sizeof cfg.owner, false, false, 0, SC_SETTINGS, onCfgSaved);
    else if (c == 'p') openEdit("PIN admin", "4 sampai 6 angka", cfg.pin, 7, true, true, 4, SC_SETTINGS, onCfgSaved);
    else if (c == 'S' && curScr == SC_WIFI) { evtScan(NULL); Serial.println("scan started"); }
  }
}

void loop() {
  static uint32_t last = millis(), perfLast = millis();
  uint32_t now = millis();
  lv_tick_inc(now - last);
  last = now;
  uint32_t d = lv_timer_handler();
  webTick();
  otaHealthTick(now);
  debugKeys();
  if (now - perfLast >= 2000) {
    if (perfN) Serial.printf("scr=%d refresh/s=%lu avg_ms=%lu avg_kpx=%lu\n", (int)curScr,
                             (unsigned long)(perfN * 1000UL / (now - perfLast)),
                             (unsigned long)(perfT / perfN), (unsigned long)(perfPx / perfN / 1000));
    perfN = perfT = perfPx = 0;
    perfLast = now;
  }
  delay(d > 5 ? 5 : (d ? d : 1));
}
