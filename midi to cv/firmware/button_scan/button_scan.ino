#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include "Display.h"

Adafruit_USBD_MIDI usb_midi;

static const uint8_t scanPins[] = {
  0, 1, 2, 3, 4, 11, 12, 13, 14,
  15, 16, 17, 18, 19, 20, 21, 22,
  23, 24, 25
};
static const uint8_t N = sizeof(scanPins) / sizeof(scanPins[0]);

static uint8_t lastState[sizeof(scanPins)];
static uint8_t stableState[sizeof(scanPins)];
static uint32_t changeMs[sizeof(scanPins)];
static uint8_t foundPin[8];
static uint8_t foundLevel[8];
static uint8_t foundCount = 0;
static bool ready = false;
static uint32_t readyMs = 0;

static void drawWait() {
  displayClear(rgb565(5, 8, 14));
  displayText(10, 14, "BUTTON SCAN", rgb565(255, 164, 58), 2);
  displayText(10, 48, "RELEASE ALL", rgb565(235, 238, 242), 2);
  displayText(10, 78, "WAIT 1 SECOND", rgb565(120, 128, 140), 1);
  displayText(10, 92, "DO NOT PRESS YET", rgb565(120, 128, 140), 1);
  displayFlush();
}

static void drawReady() {
  displayClear(rgb565(5, 8, 14));
  displayText(10, 14, "BUTTON SCAN", rgb565(255, 164, 58), 2);
  displayText(10, 48, "START", rgb565(80, 220, 120), 2);
  displayText(10, 78, "PRESS ANY KEY", rgb565(235, 238, 242), 1);
  displayText(10, 92, "ANY EDGE", rgb565(120, 128, 140), 1);
  displayText(10, 112, "PINS 0-25", rgb565(120, 128, 140), 1);
  displayText(10, 126, "NOT DISPLAY/DAC", rgb565(120, 128, 140), 1);
  displayFlush();
}

static void drawResult() {
  displayClear(rgb565(5, 8, 14));
  displayText(10, 12, "DETECTED", rgb565(80, 220, 120), 2);
  char buf[32];
  for (uint8_t i = 0; i < foundCount && i < 6; i++) {
    snprintf(buf, sizeof(buf), "GPIO %d  %s", foundPin[i],
             foundLevel[i] ? "HIGH" : "LOW");
    displayText(10, 45 + i * 16, buf, rgb565(235, 238, 242), 1);
  }
  displayText(10, 160, "PRESS ANOTHER KEY", rgb565(120, 128, 140), 1);
  displayText(10, 174, "OR RESET TO SCAN", rgb565(120, 128, 140), 1);
  displayFlush();
}

void setup() {
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);
  TinyUSBDevice.setManufacturerDescriptor("OV");
  TinyUSBDevice.setProductDescriptor("OV-SCAN");
  TinyUSBDevice.setSerialDescriptor("OVSCAN01");
  usb_midi.setStringDescriptor("OV-SCAN");
  usb_midi.begin();

  for (uint8_t i = 0; i < N; i++) {
    pinMode(scanPins[i], INPUT_PULLUP);
    lastState[i] = (uint8_t)digitalRead(scanPins[i]);
    stableState[i] = lastState[i];
    changeMs[i] = millis();
  }

  displayInit();
  displaySetSclkForSys(62.5f, 125000);
  drawWait();
  readyMs = millis();
}

void loop() {
  uint32_t now = millis();

  if (!ready) {
    if (now - readyMs >= 1000u) {
      for (uint8_t i = 0; i < N; i++) {
        stableState[i] = (uint8_t)digitalRead(scanPins[i]);
        lastState[i] = stableState[i];
        changeMs[i] = now;
      }
      ready = true;
      drawReady();
    }
    delay(2);
    return;
  }

  for (uint8_t i = 0; i < N; i++) {
    uint8_t s = (uint8_t)digitalRead(scanPins[i]);
    if (s != lastState[i]) {
      lastState[i] = s;
      changeMs[i] = now;
    }
    if (now - changeMs[i] >= 30 && s != stableState[i]) {
      stableState[i] = s;
      bool have = false;
      for (uint8_t k = 0; k < foundCount; k++)
        if (foundPin[k] == scanPins[i]) have = true;
      if (!have && foundCount < 8) {
        foundPin[foundCount] = scanPins[i];
        foundLevel[foundCount] = s;
        foundCount++;
        drawResult();
      }
    }
  }
  delay(2);
}
