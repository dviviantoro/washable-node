#pragma once
/*
 * Relay controller protocol core (hardware independent, so it runs on the STM32 and in host tests).
 * Safety rules implemented here:
 *   - all relays are off at start;
 *   - every "on" needs a ttl (1..MAX_TTL_S seconds) after which the relay switches itself off;
 *   - no valid frame for HB_TIMEOUT_MS -> all relays off ("failsafe");
 *   - the controller only transmits as a reply or shortly after it last heard from the ESP32 (it stays silent while the
 *     ESP32 is being flashed over the shared USB-serial lines).
 */
#include "relay_proto.h"

#define CTRL_RELAYS      8
#define HB_TIMEOUT_MS    3000UL
#define SPEAK_WINDOW_MS  10000UL
#define MAX_TTL_S        7200L
#define ST_MAX           20          // pings in one self-test (STM32 -> ESP32 direction of the health check)
#define ST_GAP_MS        100UL
#define ST_WAIT_MS       800UL       // wait this long after the last ping for late answers

struct CtrlHooks {
  void (*tx)(const char *frame, size_t n);     // send a complete "json*CRC\n" frame
  void (*relay)(uint8_t ch, bool on);          // ch 0..7
};

class Controller {
 public:
  Controller(const CtrlHooks &h, const char *fw) : hk(h), fw(fw) {}

  uint8_t mask() const { return relayMask; }
  uint32_t badFrames() const { return bad; }
  void setSensors(int vddMv, int tempC) { vdd = vddMv; temp = tempC; }     // optional: MCU supply (mV) and temperature (C), 0 = unknown
  void setResetCause(const char *c) { strlcpy_(rst, c, sizeof rst); }   // reported in the hello reply (why did the controller restart?)

  void begin(uint32_t now) { for (uint8_t i = 0; i < CTRL_RELAYS; i++) hk.relay(i, false); relayMask = 0; lastValid = 0; linked = false; (void)now; }

  // one received line (without the newline)
  void onLine(char *line, size_t len, uint32_t now) {
    if (len == 0 || line[0] != '{') return;                  // log lines etc. are ignored
    char *body = rp::check(line, len);
    if (!body) {
      bad++;
      if (speakable(now)) reply("{\"v\":1,\"t\":\"nack\",\"err\":\"crc\"}");
      return;
    }
    lastValid = now;
    linked = true;
    rxOk++;
    if (failsafePending) { failsafePending = false; evtFailsafe(); }

    char t[8] = "";
    long id = -1;
    rp::str(body, "t", t, sizeof t);
    bool hasId = rp::num(body, "id", &id);
    if (!strcmp(t, "hb")) {
      long seq = 0;
      rp::num(body, "seq", &seq);
      char j[96];
      snprintf(j, sizeof j, "{\"v\":1,\"t\":\"hb\",\"seq\":%ld,\"mask\":%u,\"up\":%lu}", seq, relayMask, (unsigned long)(now / 1000));
      reply(j);
    } else if (!strcmp(t, "ping")) {                          // health check, ESP32 -> STM32: echo the number back
      long n = 0;
      rp::num(body, "n", &n);
      char j[48];
      snprintf(j, sizeof j, "{\"v\":1,\"t\":\"pong\",\"n\":%ld}", n);
      reply(j);
    } else if (!strcmp(t, "pong")) {                          // answer to one of our self-test pings
      long n = 0;
      if (stActive && rp::num(body, "n", &n) && n >= 1 && n <= stSent && !stGot[n - 1]) {
        stGot[n - 1] = true;
        stOk++;
        uint32_t r = now - stSentAt[n - 1];
        stRttSum += r;
        if (r > stRttMax) stRttMax = r;
      }
    } else if (!strcmp(t, "cmd")) {
      command(body, hasId, id, now);
    } else {
      nack(hasId, id, "type");
    }
  }

  // call often (every loop iteration)
  void tick(uint32_t now) {
    for (uint8_t i = 0; i < CTRL_RELAYS; i++)
      if ((relayMask & (1u << i)) && (int32_t)(now - offAt[i]) >= 0) {
        setRelay(i, false);
        if (speakable(now)) evtRelay(i, "ttl");
      }
    if (stActive) selfTestTick(now);
    if (relayMask && linkLost(now)) {                         // heartbeat lost -> everything off
      for (uint8_t i = 0; i < CTRL_RELAYS; i++) setRelay(i, false);
      failsafePending = true;
    }
  }

 private:
  CtrlHooks hk;
  const char *fw;
  uint8_t relayMask = 0;
  uint32_t offAt[CTRL_RELAYS] = {};
  uint32_t lastValid = 0, bad = 0, rxOk = 0, txN = 0;
  int vdd = 0, temp = 0;
  // self-test (STM32 -> ESP32 pings)
  bool stActive = false;
  uint8_t stN = 0, stSent = 0, stOk = 0;
  uint32_t stNext = 0, stLastSent = 0, stRttSum = 0, stRttMax = 0, stSentAt[ST_MAX] = {};
  bool stGot[ST_MAX] = {};
  void selfTestTick(uint32_t now) {
    if (stSent < stN && (int32_t)(now - stNext) >= 0) {
      char j[48];
      stSentAt[stSent] = now;
      stSent++;
      snprintf(j, sizeof j, "{\"v\":1,\"t\":\"ping\",\"n\":%u}", stSent);
      reply(j);
      stNext = now + ST_GAP_MS;
      stLastSent = now;
    } else if (stSent >= stN && (stOk >= stN || (now - stLastSent) > ST_WAIT_MS)) {
      char j[120];
      snprintf(j, sizeof j, "{\"v\":1,\"t\":\"evt\",\"e\":\"selftest\",\"n\":%u,\"ok\":%u,\"rtt\":%lu,\"max\":%lu}", stN, stOk,
               (unsigned long)(stOk ? stRttSum / stOk : 0), (unsigned long)stRttMax);
      reply(j);
      stActive = false;
    }
  }
  char rst[8] = "";
  static void strlcpy_(char *d, const char *s, size_t n) { size_t i = 0; for (; i + 1 < n && s[i]; i++) d[i] = s[i]; d[i] = 0; }
  bool linked = false, failsafePending = false;

  bool linkLost(uint32_t now) const { return !linked || (now - lastValid) > HB_TIMEOUT_MS; }
  bool speakable(uint32_t now) const { return linked && (now - lastValid) < SPEAK_WINDOW_MS; }

  void reply(const char *json) {
    char buf[200];
    size_t n = rp::frame(buf, sizeof buf, json);
    if (n) { hk.tx(buf, n); txN++; }
  }
  void nack(bool hasId, long id, const char *err) {
    char j[96];
    if (hasId) snprintf(j, sizeof j, "{\"v\":1,\"t\":\"nack\",\"id\":%ld,\"err\":\"%s\"}", id, err);
    else snprintf(j, sizeof j, "{\"v\":1,\"t\":\"nack\",\"err\":\"%s\"}", err);
    reply(j);
  }
  void ack(long id, const char *extra, uint32_t now) {
    char j[160];
    snprintf(j, sizeof j, "{\"v\":1,\"t\":\"ack\",\"id\":%ld,\"ok\":1,\"mask\":%u,\"up\":%lu%s}", id, relayMask, (unsigned long)(now / 1000), extra);
    reply(j);
  }
  void evtRelay(uint8_t ch, const char *why) {
    char j[120];
    snprintf(j, sizeof j, "{\"v\":1,\"t\":\"evt\",\"e\":\"relay\",\"ch\":%u,\"on\":%u,\"why\":\"%s\",\"mask\":%u}", ch + 1, (relayMask >> ch) & 1, why, relayMask);
    reply(j);
  }
  void evtFailsafe() {
    char j[120];
    snprintf(j, sizeof j, "{\"v\":1,\"t\":\"evt\",\"e\":\"failsafe\",\"why\":\"hb_timeout\",\"mask\":%u}", relayMask);
    reply(j);
  }
  void setRelay(uint8_t ch, bool on) {
    if (on) relayMask |= (uint8_t)(1u << ch); else relayMask &= (uint8_t)~(1u << ch);
    hk.relay(ch, on);
  }

  void command(const char *body, bool hasId, long id, uint32_t now) {
    if (!hasId) { nack(false, 0, "id"); return; }
    char op[10] = "";
    rp::str(body, "op", op, sizeof op);
    if (!strcmp(op, "hello")) {
      char x[80];
      snprintf(x, sizeof x, ",\"fw\":\"%s\",\"relays\":%d,\"rst\":\"%s\",\"bad\":%lu", fw, CTRL_RELAYS, rst, (unsigned long)bad);
      ack(id, x, now);
    } else if (!strcmp(op, "status")) {
      ack(id, "", now);
    } else if (!strcmp(op, "health")) {                       // counters and sensors, for the kiosk's health page
      char x[140];
      snprintf(x, sizeof x, ",\"fw\":\"%s\",\"rst\":\"%s\",\"rx\":%lu,\"tx\":%lu,\"bad\":%lu,\"vdd\":%d,\"temp\":%d",
               fw, rst, (unsigned long)rxOk, (unsigned long)txN, (unsigned long)bad, vdd, temp);
      ack(id, x, now);
    } else if (!strcmp(op, "selftest")) {                     // "ping me n times": the STM32 -> ESP32 half of the health check
      long n = 10;
      rp::num(body, "n", &n);
      if (n < 1 || n > ST_MAX) { nack(true, id, "args"); return; }
      stN = (uint8_t)n; stSent = 0; stOk = 0; stRttSum = 0; stRttMax = 0; stLastSent = now; stNext = now + ST_GAP_MS; stActive = true;
      for (uint8_t i = 0; i < ST_MAX; i++) stGot[i] = false;
      ack(id, "", now);
    } else if (!strcmp(op, "alloff")) {
      for (uint8_t i = 0; i < CTRL_RELAYS; i++) setRelay(i, false);
      ack(id, "", now);
    } else if (!strcmp(op, "relay")) {
      uint8_t on = 0, off = 0;
      int ron = rp::list(body, "on", &on), roff = rp::list(body, "off", &off);
      if (ron < 0 || roff < 0) { nack(true, id, "bad_ch"); return; }
      if (ron == 0 && roff == 0) { nack(true, id, "args"); return; }
      if (on & off) { nack(true, id, "conflict"); return; }
      long ttl = 0;
      if (on) {
        if (!rp::num(body, "ttl", &ttl) || ttl < 1 || ttl > MAX_TTL_S) { nack(true, id, "ttl"); return; }
      }
      for (uint8_t i = 0; i < CTRL_RELAYS; i++) {
        if (off & (1u << i)) setRelay(i, false);
        if (on & (1u << i)) { setRelay(i, true); offAt[i] = now + (uint32_t)ttl * 1000UL; }
      }
      ack(id, "", now);
    } else {
      nack(true, id, "op");
    }
  }
};
