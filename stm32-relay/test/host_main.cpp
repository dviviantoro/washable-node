// Host test driver: reads "T<ms> <line>" from stdin (simulated clock + received line), prints what the controller sends.
// Lines starting with "TICK <ms>" only advance time. Output lines: "TX <frame>", "RELAY <ch> <0|1>".
#include <stdio.h>
#include <string.h>
#include "../src/ctrl_core.h"

static void tx(const char *f, size_t n) { printf("TX %.*s", (int)n, f); fflush(stdout); }
static void relay(uint8_t ch, bool on) { printf("RELAY %u %d\n", ch + 1, on ? 1 : 0); fflush(stdout); }

int main() {
  CtrlHooks h = { tx, relay };
  Controller c(h, "0.1.0");
  c.setResetCause("iwdg");
  c.begin(0);
  char line[512];
  while (fgets(line, sizeof line, stdin)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!strncmp(line, "TICK ", 5)) { c.tick((uint32_t)strtoul(line + 5, nullptr, 10)); continue; }
    if (line[0] == 'T') {
      char *sp = strchr(line, ' ');
      uint32_t now = (uint32_t)strtoul(line + 1, nullptr, 10);
      c.tick(now);
      if (sp) c.onLine(sp + 1, strlen(sp + 1), now);
      c.tick(now);
    }
  }
  return 0;
}
