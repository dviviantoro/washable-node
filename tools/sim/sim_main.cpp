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


// ---- layout linter: text that crosses the edge of the card it sits on, text beyond the screen, and overlapping texts
#include <vector>
struct LNode { lv_obj_t *o; lv_area_t a; bool label; };
static void collect(lv_obj_t *o, std::vector<LNode> &v) {
  uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t *c = lv_obj_get_child(o, i);
    if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_area_t a; lv_obj_get_coords(c, &a);
    v.push_back({ c, a, lv_obj_check_type(c, &lv_label_class) });
    collect(c, v);
  }
}
static bool inside(const lv_area_t &in, const lv_area_t &out, int tol = 1) { return in.x1 >= out.x1 - tol && in.x2 <= out.x2 + tol && in.y1 >= out.y1 - tol && in.y2 <= out.y2 + tol; }
static bool overlap(const lv_area_t &a, const lv_area_t &b) { return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2; }
static lv_area_t tightRect(lv_obj_t *l, const lv_area_t &a) {            // where the glyphs really are inside the label box
  const char *txt = lv_label_get_text(l);
  lv_point_t sz; lv_label_long_mode_t lm = lv_label_get_long_mode(l);
  int boxw = a.x2 - a.x1 + 1;
  lv_txt_get_size(&sz, txt, lv_obj_get_style_text_font(l, 0), 0, 0, lm == LV_LABEL_LONG_WRAP ? boxw : LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  lv_area_t r = a;
  int w = sz.x < boxw || lm != LV_LABEL_LONG_WRAP ? sz.x : boxw;
  if (lm == LV_LABEL_LONG_DOT || lm == LV_LABEL_LONG_CLIP) w = w > boxw ? boxw : w;
  lv_text_align_t al = lv_obj_get_style_text_align(l, 0);
  if (al == LV_TEXT_ALIGN_CENTER) r.x1 = a.x1 + (boxw - w) / 2;
  else if (al == LV_TEXT_ALIGN_RIGHT) r.x1 = a.x2 - w + 1;
  r.x2 = r.x1 + w - 1;
  int h = sz.y; if (h > a.y2 - a.y1 + 1 && lm != LV_LABEL_LONG_WRAP) h = a.y2 - a.y1 + 1;
  r.y2 = r.y1 + h - 1;
  return r;
}
static int lintScreen(const char *name) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_update_layout(scr);
  std::vector<LNode> v; collect(scr, v);
  int bad = 0;
  auto clipArea = [&](lv_obj_t *l, lv_area_t a) {      // visible part: clipped by the scrolling list it lives in
    for (lv_obj_t *p = lv_obj_get_parent(l); p && p != scr; p = lv_obj_get_parent(p))
      if (lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) { lv_area_t pa; lv_obj_get_coords(p, &pa); if (a.y1 < pa.y1) a.y1 = pa.y1; if (a.y2 > pa.y2) a.y2 = pa.y2; }
    return a;
  };
  for (size_t i = 0; i < v.size(); i++) {
    if (!v[i].label) continue;
    lv_obj_t *l = v[i].o; lv_area_t a = v[i].a;
    const char *txt = lv_label_get_text(l);
    if (!txt || !txt[0] || lv_label_get_recolor(l)) continue;      // (the recolored wordmark has a bogus box)
    char t[40]; snprintf(t, sizeof t, "%.34s", txt); for (char *c = t; *c; c++) if (*c == '\n') *c = '/';
    if (g_lang == LANG_EN) {                                       // untranslated Indonesian left on an English page?
      static const char *ID_WORDS[] = { "belum", "tidak", "dengan", "untuk", "yang", " dan ", "sudah", "kamu", "saya", "bayar", "cuci", "kering", "mesin", "pemilik",
        "pengatur", "jaringan", "kode ", "masuk", "simpan", "batal", "selesai", "terhubung", "gagal", "menunggu", "menit", "detik", "kembali", "lanjut", "pilih", "ambil", "diperbarui", "pakai", "kata sandi", "mulai", "sisa", "aktif", " mati", "salah", "coba" };
      char low[160]; size_t n = 0; for (const char *c = txt; *c && n < 158; c++) low[n++] = (char)tolower((unsigned char)*c); low[n] = 0;
      for (const char *w : ID_WORDS) if (strstr(low, w)) { printf("LINT %-14s INDONESIAN '%s' (%s)\n", name, t, w); bad++; break; }
    }
    // the label's own box is cropped by DOT/CLIP: compare the real text size with the box
    const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
    lv_point_t sz; lv_label_long_mode_t lm = lv_label_get_long_mode(l);
    int boxw = a.x2 - a.x1 + 1, boxh = a.y2 - a.y1 + 1;
    // LV_LABEL_LONG_DOT wraps at the box width and cuts the lines that do not fit the height; CLIP cuts at the width
    lv_coord_t maxw = (lm == LV_LABEL_LONG_WRAP || lm == LV_LABEL_LONG_DOT) ? boxw : LV_COORD_MAX;
    lv_txt_get_size(&sz, txt, f, 0, 0, maxw, LV_TEXT_FLAG_NONE);
    if ((lm == LV_LABEL_LONG_DOT && ((lv_label_t *)l)->dot_end != 0xFFFFFFFF) || (lm == LV_LABEL_LONG_DOT && sz.y > boxh + 6) || (lm == LV_LABEL_LONG_CLIP && sz.x > boxw + 1)) { printf("LINT %-14s TRUNC  '%s' text %dx%d in box %dx%d\n", name, t, sz.x, sz.y, boxw, boxh); bad++; }
    lv_area_t ta = tightRect(l, a);
    ta = clipArea(l, ta);
    if (ta.x1 < 0 || ta.x2 > 319 || ta.y2 > 479) { printf("LINT %-14s SCREEN '%s' area %d,%d..%d,%d\n", name, t, ta.x1, ta.y1, ta.x2, ta.y2); bad++; }
    // container = last opaque non-label object before the label whose area holds the label's centre
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    const LNode *cont = nullptr;
    for (size_t j = 0; j < i; j++) {
      if (v[j].label || v[j].o == l) continue;
      if (lv_obj_get_style_bg_opa(v[j].o, 0) < LV_OPA_80) continue;
      int w = v[j].a.x2 - v[j].a.x1 + 1, h = v[j].a.y2 - v[j].a.y1 + 1;
      if (w >= 319 && h >= 479) continue;                       // the screen background
      if (cx >= v[j].a.x1 && cx <= v[j].a.x2 && cy >= v[j].a.y1 && cy <= v[j].a.y2 && w < 320 && w > 20) cont = &v[j];
    }
    if (cont) {
      lv_area_t ca = cont->a;                                    // card clipped by its own scrolling list
      ca = clipArea(cont->o, ca);
      if (!inside(ta, ca, 1)) { printf("LINT %-14s OUTSIDE '%s' text %d,%d..%d,%d card %d,%d..%d,%d\n", name, t, ta.x1, ta.y1, ta.x2, ta.y2, ca.x1, ca.y1, ca.x2, ca.y2); bad++; }
    }
    for (size_t j = i + 1; j < v.size(); j++) {                      // something drawn on top of the text
      if (v[j].label || lv_obj_get_style_bg_opa(v[j].o, 0) < LV_OPA_80) continue;
      lv_obj_t *anc = l; bool own = false; for (; anc; anc = lv_obj_get_parent(anc)) if (anc == v[j].o) own = true;   // a card that holds the label
      if (own) continue;
      lv_area_t b = clipArea(v[j].o, v[j].a);
      if (overlap(ta, b) && (b.x2 - b.x1) < 300) { printf("LINT %-14s COVERED '%s' by object %d,%d..%d,%d\n", name, t, b.x1, b.y1, b.x2, b.y2); bad++; }
    }
    for (size_t j = i + 1; j < v.size(); j++) {
      if (!v[j].label) continue;
      const char *t2 = lv_label_get_text(v[j].o); if (!t2 || !t2[0]) continue;
      if (strchr(t2, '#') || strchr(txt, '#')) continue;                  // the recolored wordmark
      lv_area_t b = clipArea(v[j].o, tightRect(v[j].o, v[j].a));
      lv_area_t a2 = ta; a2.x2 -= 1; a2.y2 -= 1; b.x2 -= 1; b.y2 -= 1;
      int iw = (a2.x2 < b.x2 ? a2.x2 : b.x2) - (a2.x1 > b.x1 ? a2.x1 : b.x1) + 1, ih = (a2.y2 < b.y2 ? a2.y2 : b.y2) - (a2.y1 > b.y1 ? a2.y1 : b.y1) + 1;
      if (overlap(a2, b) && iw > 4 && ih > 6) { printf("LINT %-14s OVERLAP '%s' x '%.30s'\n", name, t, t2); bad++; }
    }
  }
  return bad;
}

static int lintTotal = 0;
static void step(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 5) { g_ms += 5; loop(); } }
static void settle() { int guard = 0; while (busy && guard++ < 2000) step(5); }
static void shot(const char *dir, const char *name, uint32_t animMs = 900) {
  step(animMs);
  if (strcmp(name, "idle-menu")) lintTotal += lintScreen(name);   // the dropdown overlays the page on purpose
  char p[256]; snprintf(p, sizeof p, "%s/%s.rgb", dir, name);
  FILE *f = fopen(p, "wb"); fwrite(g_fb, 1, sizeof g_fb, f); fclose(f);
  printf("shot %s\n", name);
}
static void go(Scr s) { showScreen(s); settle(); }

// ---- headless checks of the cycle / relay logic (no screenshots): `sim --test`
static int fails = 0;
static void check(const char *n, bool ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", n); if (!ok) fails++; }
static int runTests() {
  setup(); settle();
  cfg.demo = false;                                   // real time: 1 "minute" = 60 s
  rl.up = true; rl.helloOk = true; rl.mask = 0;
  auto ctl = [&](uint32_t ms) { for (uint32_t t = 0; t < ms; t += 100) { g_ms += 100; jobsTick(g_ms); rl.mask = g_cmdOn; } };   // emulated controller follows the last command
  check("pay is allowed with a controller", controllerReady());
  rl.up = false; check("pay is refused when the controller is gone", !controllerReady()); rl.up = true;
  svc = 0; jobStart(0, 0);                            // wash, 1st program
  uint32_t dur = jobs[0].durMs;
  ctl(3000);
  check("wash relay (CH2) commanded", g_cmdOn == 0x02);
  uint32_t r1 = jobRemainSec(0, g_ms);
  rl.up = false; for (int i = 0; i < 100; i++) { g_ms += 100; jobsTick(g_ms); }       // controller lost for 10 s
  uint32_t r2 = jobRemainSec(0, g_ms);
  check("clock stands still while the controller is lost", r2 == r1);
  rl.up = true; ctl(5000);
  check("clock runs again after the link returns", jobRemainSec(0, g_ms) < r2);
  jobSave(0, g_ms); uint32_t before = jobRemainSec(0, g_ms);
  jobs[0].st = JS_FREE;
 jobsRestore();
  check("running cycle is restored after a restart", jobs[0].st == JS_RUN && jobRemainSec(0, g_ms) + 1 >= before && jobRemainSec(0, g_ms) <= before + 1);
  svc = 1; jobStart(1, 0); ctl(2000);
  check("both machines run at once (CH2 + CH6)", g_cmdOn == 0x22);
  jobs[0].startMs = g_ms - dur; ctl(2000);
  check("wash finishes, dryer keeps running", jobs[0].st == JS_DONE && jobs[1].st == JS_RUN && g_cmdOn == 0x20);
  jobs[1].startMs = g_ms - jobs[1].durMs; ctl(2000);
  check("all off when everything is finished", jobs[1].st == JS_DONE && g_cmdOn == 0 && g_allOff > 0);
  { Preferences p; p.begin("jobs", true); check("nothing left to resume", p.getString("j0", "x") == "" && p.getString("j1", "x") == ""); }
  printf(fails ? "SIM TESTS FAILED\n" : "SIM TESTS PASSED\n");
  return fails ? 1 : 0;
}

// ---- render benchmark: `sim --bench [seconds]` renders the heaviest screens continuously (use with `sample`/perf to find hot spots)
#include <chrono>
static int runBench(int secs) {
  setup(); settle();
  struct B { const char *n; int scr; };
  const B list[] = { { "idle", SC_IDLE }, { "service", SC_SERVICE }, { "pay", SC_PAY }, { "run-wash", SC_RUN }, { "done", SC_DONE } };
  for (const B &b : list) {
    svc = 0; pick = 1;
    if (b.scr == SC_RUN) { jobs[0].st = JS_FREE; jobStart(0, 1); }
    go((Scr)b.scr); step(600);
    int frames = 0; auto t0 = std::chrono::steady_clock::now();
    double lim = secs;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < lim) { g_ms += 16; loop(); frames++; }
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("bench %-9s %6.2f ms/frame (%d frames)\n", b.n, el * 1000 / frames, frames);
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--bench")) return runBench(argc > 2 ? atoi(argv[2]) : 2);
  if (argc > 1 && !strcmp(argv[1], "--test")) return runTests();
  const char *dir = argc > 1 ? argv[1] : ".";
  g_nvs["washable/owner"] = "Laundry Bersih Jaya";
  g_nvs["washable/ownerid"] = "OWN-0042";
  g_nvs["washable/mid"] = "03";
  g_nvs["washable/ssid"] = "WarungWiFi";
  g_nvs["washable/apiurl"] = "https://api.washable.id";
  g_nvs["washable/bright"] = "80";
  if (getenv("SIM_LANG")) g_nvs["washable/lang"] = getenv("SIM_LANG");
  if (getenv("SIM_THEME")) g_nvs["washable/theme"] = getenv("SIM_THEME");
  setup();
  settle();
  shot(dir, "idle", 1200);
  evtMenu(NULL); shot(dir, "idle-menu", 300); evtMenu(NULL);        // dropdown open
  go(SC_ABOUT); shot(dir, "about", 500);
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
  evtScan(NULL); step(1500); shot(dir, "wifi-list", 600);
  go(SC_SETTINGS); openEdit("Pemilik", "Nama pemilik atau usaha", cfg.owner, sizeof cfg.owner, false, false, 0, SC_SETTINGS, onCfgSaved); settle(); shot(dir, "edit", 500);
  rl.up = true; rl.helloOk = true; go(SC_RTEST); shot(dir, "relay-test", 500);
  rl.up = true; rl.helloOk = true; strlcpy(rl.fw, "0.1.0", sizeof rl.fw); strlcpy(rl.rst, "power", sizeof rl.rst);
  rl.rxOk = 18234; rl.rxBad = 2; rl.cmdFail = 1; rl.upSec = 5421; rl.mask = 0x02;
  svc = 0; jobStart(0, 1); jobs[0].startMs = g_ms - jobs[0].durMs * 40 / 100;
  go(SC_INFO); shot(dir, "info", 400);
  lv_obj_scroll_to_y(infoList, 330, LV_ANIM_OFF); shot(dir, "info-2", 400);
  lv_obj_scroll_to_y(infoList, 660, LV_ANIM_OFF); shot(dir, "info-3", 400);
  lv_obj_scroll_to_y(infoList, 2000, LV_ANIM_OFF); shot(dir, "info-4", 400);
  jobs[0].st = JS_FREE;
  go(SC_WEB); shot(dir, "web", 600);
  WiFi.up = false; step(1200); shot(dir, "web-nowifi", 300); WiFi.up = true;
  // health check: healthy, attention (watchdog reset, a few lost pings), failure (no controller)
  rh = {}; rh.state = RH_DONE; rh.pingN = 10; rh.pingSent = 10; rh.pingOk = 10; rh.rttAvgUs = 11800; rh.rttMaxUs = 15000; rh.stGot = true; rh.stN = 10; rh.stOk = 10; rh.stRttMs = 12; rh.stMaxMs = 15;
  rh.infoGot = true; rh.ctlRx = 4210; rh.ctlTx = 4388; rh.ctlBad = 0; rl.rxBad = 0; strlcpy(rl.rst, "power", sizeof rl.rst);
  go(SC_HEALTH); shot(dir, "health-ok", 700);
  rh.pingOk = 8; rh.stOk = 9; rh.ctlBad = 3; strlcpy(rl.rst, "iwdg", sizeof rl.rst);
  step(400); shot(dir, "health-warn", 300);
  rh = {}; rh.state = RH_DONE; rh.linkFail = true; rl.up = false; rl.helloOk = false;
  step(400); shot(dir, "health-fail", 300);
  rl.up = true; rl.helloOk = true;
  rh = {}; rh.state = RH_PING; rh.pingN = 10; rh.pingSent = 6; rh.pingOk = 6; rh.rttAvgUs = 12100;
  step(400); shot(dir, "health-run", 300);
  svc = 0; jobs[0].st = JS_DONE; go(SC_DONE); shot(dir, "done-wash", 900); jobs[0].st = JS_FREE;
  jobs[0].st = JS_FREE;
  strlcpy(ota.latest, "1.2.0", sizeof ota.latest); ota.state = OTA_DOWNLOADING; ota.progress = 62;
  go(SC_OTA); shot(dir, "ota", 1200);
  printf("LINT total: %d\n", lintTotal);
  return 0;
}
