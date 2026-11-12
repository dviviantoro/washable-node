#include "ota.h"
#include "version.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <time.h>

// Mozilla CA bundle embedded via board_build.embed_files (data/cert/x509_crt_bundle.bin)
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");

OtaInfo ota = { OTA_DISABLED, 0, "", "", 0 };

static char gManifestUrl[160] = "";
static char gFwUrl[200] = "";
static char gSha[65] = "";
static uint32_t gSize = 0;
static uint32_t nextCheckMs = OTA_FIRST_CHECK_MS;
static uint8_t failCount = 0;
static bool ntpStarted = false, healthDone = false, pendingFlag = false, forceCheck = false;

// ---------------------------------------------------------------- version helpers
static bool parseVer(const char *s, int v[3]) {
  v[0] = v[1] = v[2] = 0;
  if (*s == 'v' || *s == 'V') s++;
  return sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) >= 2;
}
static int cmpVer(const char *a, const char *b) {   // <0 a older, 0 equal, >0 a newer
  int x[3], y[3];
  if (!parseVer(a, x) || !parseVer(b, y)) return 0;
  for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] - y[i];
  return 0;
}

// ---------------------------------------------------------------- tiny JSON field readers
static bool jsonStr(const String &b, const char *key, char *out, size_t n) {
  String k = String("\"") + key + "\"";
  int i = b.indexOf(k);
  if (i < 0) return false;
  i = b.indexOf(':', i);
  if (i < 0) return false;
  i = b.indexOf('"', i);
  if (i < 0) return false;
  int j = b.indexOf('"', i + 1);
  if (j < 0) return false;
  strlcpy(out, b.substring(i + 1, j).c_str(), n);
  return true;
}
static bool jsonNum(const String &b, const char *key, uint32_t &out) {
  String k = String("\"") + key + "\"";
  int i = b.indexOf(k);
  if (i < 0) return false;
  i = b.indexOf(':', i);
  if (i < 0) return false;
  out = (uint32_t)strtoul(b.c_str() + i + 1, NULL, 10);
  return true;
}

static void setError(const char *m) {
  strlcpy(ota.msg, m, sizeof ota.msg);
  ota.state = OTA_ERROR;
  if (failCount < 3) failCount++;
}

// ---------------------------------------------------------------- HTTP open (TLS with CA bundle, redirects)
static bool httpBegin(HTTPClient &http, WiFiClientSecure &sc, WiFiClient &pc, const char *url) {
  if (!strncmp(url, "https://", 8)) {
    sc.setCACertBundle(rootca_crt_bundle_start);
    sc.setTimeout(15);
    if (!http.begin(sc, url)) return false;
  } else {
#if OTA_ALLOW_HTTP
    if (!http.begin(pc, url)) return false;
#else
    return false;
#endif
  }
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  http.setReuse(false);
  http.setUserAgent("washable-ota/" FW_VERSION);
  return true;
}

// ---------------------------------------------------------------- time
bool otaInWindow() {
  time_t t = time(nullptr);
  if (t < 1700000000) return false;            // clock not synced yet
  struct tm tmv;
  localtime_r(&t, &tmv);
  return tmv.tm_hour >= OTA_WINDOW_START && tmv.tm_hour < OTA_WINDOW_END;
}

// ---------------------------------------------------------------- check task
static void checkTask(void *) {
  ota.state = OTA_CHECKING;
  char url[160];
  strlcpy(url, gManifestUrl, sizeof url);
  WiFiClientSecure sc;
  WiFiClient pc;
  HTTPClient http;
  if (!httpBegin(http, sc, pc, url)) { setError("URL harus https"); vTaskDelete(NULL); return; }
  int code = http.GET();
  if (code != 200) {
    char m[56]; snprintf(m, sizeof m, "manifest HTTP %d", code);
    http.end(); setError(m); vTaskDelete(NULL); return;
  }
  if (http.getSize() > 4096) { http.end(); setError("manifest terlalu besar"); vTaskDelete(NULL); return; }
  String body = http.getString();
  http.end();

  char ver[16] = "", minv[16] = "", sha[65] = "";
  uint32_t size = 0;
  if (!jsonStr(body, "version", ver, sizeof ver) || !jsonStr(body, "sha256", sha, sizeof sha) || !jsonNum(body, "size", size) || strlen(sha) != 64 || size < 100000) {
    setError("manifest tidak valid"); vTaskDelete(NULL); return;
  }
  jsonStr(body, "min_version", minv, sizeof minv);
  strlcpy(ota.latest, ver, sizeof ota.latest);
  ota.lastCheckMs = millis();

  Preferences p;
  p.begin("ota", true);
  String bad = p.getString("badver", "");
  p.end();

  if (cmpVer(ver, FW_VERSION) <= 0) { strlcpy(ota.msg, "terbaru", sizeof ota.msg); failCount = 0; ota.state = OTA_IDLE; vTaskDelete(NULL); return; }
  if (bad.length() && cmpVer(ver, bad.c_str()) == 0) { strlcpy(ota.msg, "versi ditolak (gagal sebelumnya)", sizeof ota.msg); ota.state = OTA_IDLE; vTaskDelete(NULL); return; }
  if (minv[0] && cmpVer(FW_VERSION, minv) < 0) { strlcpy(ota.msg, "perlu pembaruan bertahap", sizeof ota.msg); ota.state = OTA_IDLE; vTaskDelete(NULL); return; }

  // firmware.bin always lives next to manifest.json (never a URL taken from the manifest)
  strlcpy(gFwUrl, url, sizeof gFwUrl);
  char *slash = strrchr(gFwUrl, '/');
  if (!slash || (size_t)(slash - gFwUrl) + 14 >= sizeof gFwUrl) { setError("URL manifest salah"); vTaskDelete(NULL); return; }
  strcpy(slash + 1, "firmware.bin");
  strlcpy(gSha, sha, sizeof gSha);
  gSize = size;
  ota.state = OTA_AVAILABLE;
  vTaskDelete(NULL);
}

// ---------------------------------------------------------------- install task
static void installTask(void *) {
  ota.state = OTA_DOWNLOADING;
  ota.progress = 0;
  WiFiClientSecure sc;
  WiFiClient pc;
  HTTPClient http;
  if (!httpBegin(http, sc, pc, gFwUrl)) { setError("URL firmware salah"); vTaskDelete(NULL); return; }
  int code = http.GET();
  if (code != 200) { char m[56]; snprintf(m, sizeof m, "firmware HTTP %d", code); http.end(); setError(m); vTaskDelete(NULL); return; }
  int len = http.getSize();
  if (len <= 0 || (uint32_t)len != gSize) { http.end(); setError("ukuran firmware tidak cocok"); vTaskDelete(NULL); return; }
  if (!Update.begin(len)) { http.end(); setError("ruang OTA tidak cukup"); vTaskDelete(NULL); return; }

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  WiFiClient *stream = http.getStreamPtr();
  static uint8_t buf[1436];
  int done = 0;
  uint32_t lastData = millis();
  while (done < len) {
    size_t avail = stream->available();
    if (avail) {
      size_t want = avail > sizeof buf ? sizeof buf : avail;
      if ((int)want > len - done) want = len - done;
      int n = stream->readBytes(buf, want);
      if (n <= 0) continue;
      mbedtls_sha256_update(&ctx, buf, n);
      if (Update.write(buf, n) != (size_t)n) { Update.abort(); mbedtls_sha256_free(&ctx); http.end(); setError("gagal menulis flash"); vTaskDelete(NULL); return; }
      done += n;
      ota.progress = (uint8_t)((uint64_t)done * 100 / len);
      lastData = millis();
    } else {
      if (!stream->connected() && !stream->available()) break;
      if (millis() - lastData > 20000) { Update.abort(); mbedtls_sha256_free(&ctx); http.end(); setError("unduhan terputus"); vTaskDelete(NULL); return; }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  http.end();
  if (done != len) { Update.abort(); mbedtls_sha256_free(&ctx); setError("unduhan tidak lengkap"); vTaskDelete(NULL); return; }

  ota.state = OTA_VERIFYING;
  uint8_t h[32];
  mbedtls_sha256_finish(&ctx, h);
  mbedtls_sha256_free(&ctx);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", h[i]);
  if (strcasecmp(hex, gSha) != 0) { Update.abort(); setError("SHA-256 tidak cocok"); vTaskDelete(NULL); return; }

  Preferences p;                                   // arm the rollback guard BEFORE the new image can boot
  p.begin("ota", false);
  p.putBool("pend", true);
  p.putUChar("boots", 0);
  p.end();
  if (!Update.end(true)) { Update.abort(); setError("verifikasi image gagal"); vTaskDelete(NULL); return; }
  vTaskDelay(pdMS_TO_TICKS(800));
  ESP.restart();
}

// ---------------------------------------------------------------- public API
void otaBoot() {
  Preferences p;
  if (!p.begin("ota", false)) return;
  if (p.getBool("pend", false)) {
    pendingFlag = true;
    uint8_t b = p.getUChar("boots", 0) + 1;
    p.putUChar("boots", b);
    Serial.printf("OTA: first boots of new image: %u/3\n", b);
    if (b >= 3) {                                  // new image keeps failing -> go back to the other slot
      p.putString("badver", FW_VERSION);
      p.putBool("pend", false);
      p.putUChar("boots", 0);
      const esp_partition_t *run = esp_ota_get_running_partition();
      const esp_partition_t *other = esp_ota_get_next_update_partition(run);
      Serial.println("OTA: rolling back to the previous image");
      if (other && esp_ota_set_boot_partition(other) == ESP_OK) { p.end(); delay(100); ESP.restart(); }
      Serial.println("OTA: rollback failed (other slot not bootable)");
    }
  }
  p.end();
}

void otaHealthTick(uint32_t nowMs) {
  if (healthDone || !pendingFlag || nowMs < 60000UL) return;
  Preferences p;
  if (p.begin("ota", false)) {
    p.putBool("pend", false);
    p.putUChar("boots", 0);
    p.end();
  }
  healthDone = true;
  Serial.println("OTA: new image marked good");
}

void otaConfigure(const char *url) {
  strlcpy(gManifestUrl, url ? url : "", sizeof gManifestUrl);
  if (!gManifestUrl[0]) { ota.state = OTA_DISABLED; strlcpy(ota.msg, "URL belum diatur", sizeof ota.msg); }
  else if (ota.state == OTA_DISABLED) { ota.state = OTA_IDLE; ota.msg[0] = 0; }
}

void otaForceCheck() { forceCheck = true; }

void otaTick(uint32_t nowMs, bool wifiUp, bool canCheck) {
  if (wifiUp && !ntpStarted) {
    configTime(OTA_TZ_SECONDS, 0, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
  }
  if (!gManifestUrl[0]) return;
  if (ota.state == OTA_ERROR || ota.state == OTA_IDLE) {
    bool due = forceCheck || (nowMs >= nextCheckMs);
    if (due && wifiUp && canCheck && ota.state != OTA_CHECKING) {
      if (ESP.getMaxAllocHeap() < 60000) return;     // not enough room for a TLS session
      forceCheck = false;
      static const uint32_t backoff[4] = { OTA_CHECK_MS, 3600000UL, 7200000UL, 14400000UL };
      nextCheckMs = nowMs + (failCount ? backoff[failCount > 3 ? 3 : failCount] : OTA_CHECK_MS);
      ota.state = OTA_CHECKING;
      xTaskCreatePinnedToCore(checkTask, "ota_chk", 12288, NULL, 1, NULL, 0);
    }
  }
}

bool otaUpdateReady() { return ota.state == OTA_AVAILABLE; }

void otaInstall() {
  if (ota.state != OTA_AVAILABLE) return;
  ota.state = OTA_DOWNLOADING;
  ota.progress = 0;
  xTaskCreatePinnedToCore(installTask, "ota_dl", 12288, NULL, 1, NULL, 0);
}

void otaAck() {
  if (ota.state == OTA_ERROR) ota.state = OTA_IDLE;
}

void otaStatusText(char *out, size_t n) {
  switch (ota.state) {
    case OTA_DISABLED:    snprintf(out, n, "OTA belum diatur"); break;
    case OTA_CHECKING:    snprintf(out, n, "memeriksa pembaruan..."); break;
    case OTA_AVAILABLE:   snprintf(out, n, "tersedia v%s", ota.latest); break;
    case OTA_DOWNLOADING: snprintf(out, n, "mengunduh %u%%", (unsigned)ota.progress); break;
    case OTA_VERIFYING:   snprintf(out, n, "memverifikasi..."); break;
    case OTA_ERROR:       snprintf(out, n, "gagal: %s", ota.msg); break;
    default:              snprintf(out, n, "%s", ota.msg[0] ? ota.msg : (ota.lastCheckMs ? "terbaru" : "belum dicek")); break;
  }
}
