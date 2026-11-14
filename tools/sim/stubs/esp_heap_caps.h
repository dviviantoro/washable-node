#pragma once
inline void *heap_caps_malloc(unsigned long n, int) { return malloc(n); }
#define MALLOC_CAP_DMA 0
