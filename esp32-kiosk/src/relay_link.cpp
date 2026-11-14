#include "relay_link.h"
#include "relay_proto.h"

RelayLink rl = {};

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
  if (!strcmp(t, "ack")) {
    long id = -1;
    if (rp::num(body, "id", &id)) {
      if (id == helloId) {
        rp::str(body, "fw", rl.fw, sizeof rl.fw);
        rl.helloOk = true;
        Serial.printf("RL: hello ok, controller fw %s\n", rl.fw);
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
  for (auto &q : pend) {                                                // retries
    if (!q.used || now - q.sentAt < RL_ACK_MS) continue;
    if (q.tries >= RL_RETRIES) { q.used = false; rl.cmdFail++; continue; }
    q.tries++;
    q.sentAt = now;
    sendRaw(q.frame, q.len);
  }
#endif
}
