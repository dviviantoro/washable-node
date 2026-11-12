#pragma once
/*
 * Over-the-air firmware updates (outbound poller, see plan):
 *   - every OTA_CHECK_MS (and shortly after boot) the device fetches a small manifest.json over HTTPS
 *   - if a newer version exists it waits for the install gate (decided in main.cpp), then downloads firmware.bin,
 *     verifies SHA-256, flashes the other OTA slot and reboots
 *   - a software rollback guard returns to the previous image if the new one keeps crashing
 * Saved settings live in NVS (separate partition) and are never touched here.
 */
#include <Arduino.h>

#ifndef OTA_ALLOW_HTTP
#define OTA_ALLOW_HTTP 0          // 1 only for bench tests against a laptop (plain HTTP)
#endif
#ifndef OTA_DEFAULT_URL
#define OTA_DEFAULT_URL ""        // e.g. https://github.com/OWNER/REPO/releases/latest/download/manifest.json
#endif
#define OTA_CHECK_MS        (6UL * 3600UL * 1000UL)
#define OTA_FIRST_CHECK_MS  (120UL * 1000UL)
#define OTA_WINDOW_START    1     // install window, local hour [start, end)
#define OTA_WINDOW_END      5
#define OTA_TZ_SECONDS      (7 * 3600)   // WIB

enum OtaState : uint8_t { OTA_DISABLED, OTA_IDLE, OTA_CHECKING, OTA_AVAILABLE, OTA_DOWNLOADING, OTA_VERIFYING, OTA_ERROR };

struct OtaInfo {
  volatile OtaState state;
  volatile uint8_t progress;      // 0..100 while downloading
  char latest[16];                // newest version seen
  char msg[56];                   // last error / note
  volatile uint32_t lastCheckMs;  // millis() of the last finished check (0 = never)
};
extern OtaInfo ota;

void otaBoot();                                   // first thing in setup(): rollback guard
void otaHealthTick(uint32_t nowMs);               // call from loop(): marks the new image good after 60 s
void otaConfigure(const char *manifestUrl);       // "" = disabled
void otaTick(uint32_t nowMs, bool wifiUp, bool canCheck);
bool otaUpdateReady();                            // a newer version is waiting
void otaInstall();                                // start download+flash task
void otaAck();                                    // error acknowledged -> back to idle
void otaForceCheck();                             // bench/debug: check now
bool otaInWindow();                               // time synced and inside the install window
void otaStatusText(char *out, size_t n);
