#pragma once

#include <stdint.h>

void displayInit();
void displayClear(uint16_t color);
void displayFillRect(int x, int y, int w, int h, uint16_t color);
void displayDrawRect(int x, int y, int w, int h, uint16_t color, int thickness = 1);
void displayText(int x, int y, const char* text, uint16_t color, int scale = 1);
void displayFlush();

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
