#include "relay_link.h"
#include "relay_proto.h"

RelayLink rl = {};
RlHealth rh = {};
static uint16_t healthId = 0, selfId = 0;
static uint32_t rhNext = 0, rhStep = 0, rhPingAt[10] = {}, rhRttSum = 0;
static bool rhGot[10] = {};

static char gMachine[8] = "";
static char gFw[16] = "";
static void (*gDebug)(char) = nullptr;

struct Pending { bool used; uint16_t id; char frame[200]; size_t len; uint32_t sentAt; uint8_t tries; };
static Pending pend[4];
static uint16_t nextId = 1, helloId = 0;
static uint32_t lastRx = 0, lastHb = 0, lastHello = 0, hbSeq = 0;

static char line[256];
static size_t lineLen = 0;
static bool lineOverflow = false;

static void sendRaw(const char *frame, size_t n) {
  Serial.write((const uint8_t *)frame, n);
  rl.txFrames++;
}
static void sendJson(const char *json) {
  char f[220];
  size_t n = rp::frame(f, sizeof f, json);
  if (n) sendRaw(f, n);
}

// queue a command; `args` is the part after "t":"cmd", e.g.  "op":"relay","on":[1],"ttl":10
static bool sendCmd(const char *args, bool isHello = false) {
  Pending *p = nullptr;
  for (auto &q : pend) if (!q.used) { p = &q; break; }
  if (!p) { rl.cmdFail++; return false; }
  char json[190];
  uint16_t id = nextId++;
  if (!nextId) nextId = 1;
  snprintf(json, sizeof json, "{\"v\":1,\"id\":%u,\"t\":\"cmd\",%s}", id, args);
  p->len = rp::frame(p->frame, sizeof p->frame, json);
  if (!p->len) return false;
  p->used = true; p->id = id; p->tries = 1; p->sentAt = millis();
  if (isHello) helloId = id;
  sendRaw(p->frame, p->len);
  return true;
}

bool rlRelay(uint8_t onMask, uint8_t offMask, uint16_t ttlSec) {
#if RELAY_LINK_ENABLED
  char on[24], off[24], args[120];
  rp::maskList(on, sizeof on, onMask);
  rp::maskList(off, sizeof off, offMask);
  int n = snprintf(args, sizeof args, "\"op\":\"relay\"");
  if (onMask) n += snprintf(args + n, sizeof args - n, ",\"on\":[%s],\"ttl\":%u", on, ttlSec);
  if (offMask) n += snprintf(args + n, sizeof args - n, ",\"off\":[%s]", off);
  return sendCmd(args);
#else
  return false;
#endif
}
bool rlAllOff() { return sendCmd("\"op\":\"alloff\""); }
bool rlStatus() { return sendCmd("\"op\":\"status\""); }

void rlBegin(const char *machineId, const char *fwVersion) {
  strlcpy(gMachine, machineId ? machineId : "", sizeof gMachine);
  strlcpy(gFw, fwVersion ? fwVersion : "", sizeof gFw);
  memset(&rl, 0, sizeof rl);
  lastHello = millis() - 2000;
}
void rlHealthStart() {
  memset(&rh, 0, sizeof rh);
  memset(rhGot, 0, sizeof rhGot);
  rhRttSum = 0; healthId = 0; selfId = 0;
  rh.startedMs = millis();
  if (!rl.up || !rl.helloOk) { rh.linkFail = true; rh.state = RH_DONE; rh.doneMs = rh.startedMs; return; }
  rh.pingN = 10;
  rh.state = RH_PING;
  rhNext = rh.startedMs;
}

static void healthTick(uint32_t now) {
  if (rh.state == RH_IDLE || rh.state == RH_DONE) return;
  if (rh.state == RH_PING) {
    if (rh.pingSent < rh.pingN && (int32_t)(now - rhNext) >= 0) {       // 10 pings, 100 ms apart
      char j[48];
      rhPingAt[rh.pingSent] = micros();
      rh.pingSent++;
      snprintf(j, sizeof j, "{\"v\":1,\"t\":\"ping\",\"n\":%u}", rh.pingSent);
      sendJson(j);
      rhNext = now + 100;
      rhStep = now;
    } else if (rh.pingSent >= rh.pingN && (rh.pingOk >= rh.pingN || now - rhStep > 600)) {
      char a[48];
      snprintf(a, sizeof a, "\"op\":\"selftest\",\"n\":%u", rh.pingN);       // now ask the STM32 to ping us
      sendCmd(a);
      rh.state = RH_SELFTEST;
      rhStep = now;
    }
  } else if (rh.state == RH_SELFTEST) {
    if (rh.stGot || now - rhStep > 3500) {
      sendCmd("\"op\":\"health\"");
      healthId = nextId ? nextId - 1 : 65535;
      rh.state = RH_QUERY;
      rhStep = now;
    }
  } else if (rh.state == RH_QUERY) {
    if (rh.infoGot || now - rhStep > 1500) { rh.state = RH_DONE; rh.doneMs = now; }
  }
}

void rlSetDebugHandler(void (*cb)(char)) { gDebug = cb; }

static void dropPending(uint16_t id) {
  for (auto &q : pend) if (q.used && q.id == id) q.used = false;
}

static void onFrame(char *body, uint32_t now) {
  rl.rxOk++;
  lastRx = now;
  if (!rl.up) { rl.up = true; Serial.println("RL: link up"); }

  char t[8] = "";
  long v;
  rp::str(body, "t", t, sizeof t);
  if (rp::num(body, "mask", &v)) rl.mask = (uint8_t)v;
  if (rp::num(body, "up", &v)) {
    if ((uint32_t)v + 2 < rl.upSec) { rl.helloOk = false; Serial.println("RL: controller rebooted"); }   // uptime went backwards
    rl.upSec = (uint32_t)v;
  }
  if (!strcmp(t, "ping")) {                                             // STM32 -> ESP32 half of the health check: echo the number
    long n = 0;
    rp::num(body, "n", &n);
    char j[48];
    snprintf(j, sizeof j, "{\"v\":1,\"t\":\"pong\",\"n\":%ld}", n);
    sendJson(j);
  } else if (!strcmp(t, "pong")) {                                      // answer to one of our pings
    long n = 0;
    if (rh.state == RH_PING && rp::num(body, "n", &n) && n >= 1 && n <= rh.pingSent && !rhGot[n - 1]) {
      rhGot[n - 1] = true;
      rh.pingOk++;
      uint32_t r = micros() - rhPingAt[n - 1];
      rhRttSum += r;
      if (r > rh.rttMaxUs) rh.rttMaxUs = r;
      rh.rttAvgUs = rhRttSum / rh.pingOk;
    }
  } else if (!strcmp(t, "ack")) {
    long id = -1;
    if (rp::num(body, "id", &id)) {
      if (id == healthId && healthId) {
        long v;
        if (rp::num(body, "rx", &v)) rh.ctlRx = (uint32_t)v;
        if (rp::num(body, "tx", &v)) rh.ctlTx = (uint32_t)v;
        if (rp::num(body, "bad", &v)) rh.ctlBad = (uint32_t)v;
        if (rp::num(body, "vdd", &v)) rh.vddMv = (int)v;
        if (rp::num(body, "temp", &v)) rh.tempC = (int)v;
        rp::str(body, "rst", rl.rst, sizeof rl.rst);
        rh.infoGot = true;
      }
      if (id == helloId) {
        rp::str(body, "fw", rl.fw, sizeof rl.fw);
        rp::str(body, "rst", rl.rst, sizeof rl.rst);
        rl.helloOk = true;
        Serial.printf("RL: hello ok, controller fw %s, last reset: %s%s\n", rl.fw, rl.rst, strcmp(rl.rst, "iwdg") ? "" : "  <-- WATCHDOG RESET, check the controller");
      }
      dropPending((uint16_t)id);
    }
  } else if (!strcmp(t, "nack")) {
    long id = -1;
    if (rp::num(body, "id", &id)) dropPending((uint16_t)id);
    rp::str(body, "err", rl.lastErr, sizeof rl.lastErr);
    rl.cmdFail++;
    Serial.printf("RL: nack %s\n", rl.lastErr);
  } else if (!strcmp(t, "evt")) {
    char e[12] = "", why[12] = "";
    rp::str(body, "e", e, sizeof e);
    rp::str(body, "why", why, sizeof why);
    if (!strcmp(e, "selftest")) {
      long v;
      if (rp::num(body, "n", &v)) rh.stN = (uint8_t)v;
      if (rp::num(body, "ok", &v)) rh.stOk = (uint8_t)v;
      if (rp::num(body, "rtt", &v)) rh.stRttMs = (uint32_t)v;
      if (rp::num(body, "max", &v)) rh.stMaxMs = (uint32_t)v;
      rh.stGot = true;
    }
    Serial.printf("RL: event %s (%s) mask=0x%02X\n", e, why, rl.mask);
  }
}

static void onLine(char *l, size_t len, uint32_t now) {
  if (!len) return;
  if (l[0] == '#') { if (len > 1 && gDebug) gDebug(l[1]); return; }     // debug command line
  if (l[0] != '{') return;                                              // anything else (log text) is ignored
  char *body = rp::check(l, len);
  if (!body) { rl.rxBad++; return; }
  onFrame(body, now);
}

void rlTick(uint32_t now) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (!lineOverflow) { line[lineLen] = 0; onLine(line, lineLen, now); }
      lineLen = 0;
      lineOverflow = false;
    } else if (lineLen < sizeof line - 1) {
      line[lineLen++] = c;
    } else {
      lineOverflow = true;
    }
  }
#if RELAY_LINK_ENABLED
  if (rl.up && now - lastRx > RL_TIMEOUT_MS) {
    rl.up = false;
    rl.helloOk = false;
    Serial.println("RL: link down");
  }
  if (now - lastHb >= RL_HB_MS) {                                       // heartbeat (the controller answers with its own hb + mask)
    lastHb = now;
    char j[64];
    snprintf(j, sizeof j, "{\"v\":1,\"t\":\"hb\",\"seq\":%lu}", (unsigned long)hbSeq++);
    sendJson(j);
  }
  if (!rl.helloOk && now - lastHello >= 2000) {                         // handshake until the controller answers
    lastHello = now;
    char a[96];
    snprintf(a, sizeof a, "\"op\":\"hello\",\"fw\":\"%s\",\"machine\":\"%s\"", gFw, gMachine);
    sendCmd(a, true);
  }
  healthTick(now);
  for (auto &q : pend) {                                                // retries
    if (!q.used || now - q.sentAt < RL_ACK_MS) continue;
    if (q.tries >= RL_RETRIES) { q.used = false; rl.cmdFail++; continue; }
    q.tries++;
    q.sentAt = now;
    sendRaw(q.frame, q.len);
  }
#endif
}
