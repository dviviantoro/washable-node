#pragma once
#include "Arduino.h"
#define TFT_BLACK 0
extern uint8_t g_fb[320 * 480 * 3];
struct TFT_eSPI {
  int x0 = 0, y0 = 0, w = 0, h = 0, cur = 0;
  void init() {} void setRotation(int) {} void fillScreen(int) {} void initDMA() {} void dmaWait() {} void startWrite() {} void endWrite() {}
  void setAddrWindow(int x, int y, int ww, int hh) { x0 = x; y0 = y; w = ww; h = hh; cur = 0; }
  void pushPixelsDMA(uint16_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
      uint16_t c = (uint16_t)((p[i] >> 8) | (p[i] << 8));            // LV_COLOR_16_SWAP
      int x = x0 + (cur % w), y = y0 + (cur / w); cur++;
      uint8_t *d = g_fb + (y * 320 + x) * 3;
      d[0] = ((c >> 11) & 31) * 255 / 31; d[1] = ((c >> 5) & 63) * 255 / 63; d[2] = (c & 31) * 255 / 31;
    }
  }
};
