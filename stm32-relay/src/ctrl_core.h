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

struct CtrlHooks {
  void (*tx)(const char *frame, size_t n);     // send a complete "json*CRC\n" frame
  void (*relay)(uint8_t ch, bool on);          // ch 0..7
};

class Controller {
 public:
  Controller(const CtrlHooks &h, const char *fw) : hk(h), fw(fw) {}

  uint8_t mask() const { return relayMask; }
  uint32_t badFrames() const { return bad; }

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
  uint32_t lastValid = 0, bad = 0;
  bool linked = false, failsafePending = false;

  bool linkLost(uint32_t now) const { return !linked || (now - lastValid) > HB_TIMEOUT_MS; }
  bool speakable(uint32_t now) const { return linked && (now - lastValid) < SPEAK_WINDOW_MS; }

  void reply(const char *json) {
    char buf[200];
    size_t n = rp::frame(buf, sizeof buf, json);
    if (n) hk.tx(buf, n);
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
      char x[64];
      snprintf(x, sizeof x, ",\"fw\":\"%s\",\"relays\":%d", fw, CTRL_RELAYS);
      ack(id, x, now);
    } else if (!strcmp(op, "status")) {
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
