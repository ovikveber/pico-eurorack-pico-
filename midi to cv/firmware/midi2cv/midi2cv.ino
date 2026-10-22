// SPDX-License-Identifier: GPL-3.0-or-later
// midi2cv.ino - RP2040 USB-MIDI -> 8x CV/Gate (DAC8568) with ST7789 channel UI.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <EEPROM.h>
#include <cmath>
#include <hardware/clocks.h>
#include <hardware/pll.h>
#include <hardware/vreg.h>
#include <hardware/watchdog.h>
#include "Display.h"

Adafruit_USBD_MIDI usb_midi;

// ---------------- clock ----------------
#define CLK_MAGIC 0x4F56434Du
static uint32_t gSysKhz = 125000u;

static bool clockOnPllSys() {
  uint32_t ctrl = clocks_hw->clk[clk_sys].ctrl;
  uint32_t sel  = clocks_hw->clk[clk_sys].selected;
  bool lock = (pll_sys_hw->cs & 0x80000000u) != 0;
  return (ctrl & 0x1u) && ((ctrl & 0xE0u) == 0) && (sel & 0x2u) && lock;
}

static void setupClock() {
  bool wd = watchdog_enable_caused_reboot();
  bool fallback = (watchdog_hw->scratch[0] == CLK_MAGIC && wd);
  watchdog_hw->scratch[0] = CLK_MAGIC;
  watchdog_hw->scratch[1] = 0;

  if (vreg_get_voltage() < VREG_VOLTAGE_1_30) {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    busy_wait_us_32(2000);
  }
  watchdog_start_tick(12);
  watchdog_enable(8000, false);

  bool ok = false;
  if (!fallback) {
    ok = set_sys_clock_khz(420000, false);
    busy_wait_us_32(2000);
    ok = ok && clockOnPllSys();
  }
  if (ok) {
    gSysKhz = 420000;
  } else {
    set_sys_clock_khz(125000, true);
    busy_wait_us_32(2000);
    clockOnPllSys();
    gSysKhz = 125000;
  }

  watchdog_hw->scratch[1] = gSysKhz;
  clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                  gSysKhz * 1000u, gSysKhz * 1000u);
}

// ---------------- pins / mapping ----------------
#define PIN_SCLK  28
#define PIN_DIN   27
#define PIN_SYNC  26
#define ENC_A     13
#define ENC_B     12
#define ENC_SW    11

static const uint8_t gatePin[8] = { 22, 21, 20, 19, 15, 16, 17, 18 };
static const uint8_t dacCh[8]   = {  1,  3,  5,  7,  0,  2,  4,  6 };

#define NVOICE         8
#define NOTE0          24
#define CV_FULLSCALE_V 5.0f
#define ENC_TRANS_PER_STEP 2

// ---------------- DAC8568 ----------------
static void dacBytes(uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4) {
  digitalWrite(PIN_SYNC, LOW);
  uint8_t buf[4] = { b1, b2, b3, b4 };
  for (int k = 0; k < 4; k++) {
    for (int i = 7; i >= 0; i--) {
      digitalWrite(PIN_DIN, (buf[k] >> i) & 1);
      digitalWrite(PIN_SCLK, HIGH);
      digitalWrite(PIN_SCLK, LOW);
    }
  }
  digitalWrite(PIN_SYNC, HIGH);
}
static inline void dacWriteUpdate(uint8_t ch, uint16_t data) {
  dacBytes(0x03, (uint8_t)((ch << 4) | (data >> 12)),
                 (uint8_t)(data >> 4),
                 (uint8_t)(data << 4));
}
static void dacInit() {
  pinMode(PIN_SCLK, OUTPUT); pinMode(PIN_DIN, OUTPUT); pinMode(PIN_SYNC, OUTPUT);
  digitalWrite(PIN_SCLK, LOW); digitalWrite(PIN_DIN, LOW); digitalWrite(PIN_SYNC, HIGH);
  dacBytes(0x07, 0x00, 0x00, 0x00);
  dacBytes(0x08, 0x00, 0x00, 0x01);
}

// ---------------- state ----------------
static uint8_t chan[NVOICE]   = { 1, 2, 3, 4, 5, 6, 7, 8 };
static uint8_t voctN     = 1;
static uint8_t gateInv   = 0;
static bool    gate[NVOICE]   = { false };
static uint8_t curNote[NVOICE]= { NOTE0 };
static uint8_t noteStack[NVOICE][16];
static uint8_t noteCnt[NVOICE] = { 0 };
static volatile bool gDirty = true;

// ---------------- settings ----------------
struct Cfg { uint8_t magic; uint8_t ch[NVOICE]; uint8_t voct; uint8_t ginv; };
static bool     cfgDirty = false;
static uint32_t cfgMs = 0;

static void cfgLoad() {
  Cfg c; EEPROM.get(0, c);
  if (c.magic != 0x5B) {
    for (int v = 0; v < NVOICE; v++) c.ch[v] = v + 1;
    c.voct = 1; c.ginv = 0; c.magic = 0x5B;
  }
  for (int v = 0; v < NVOICE; v++) chan[v] = (c.ch[v] >= 1 && c.ch[v] <= 16) ? c.ch[v] : (v + 1);
  voctN   = (c.voct == 1 || c.voct == 2 || c.voct == 5 || c.voct == 8) ? c.voct : 1;
  gateInv = c.ginv ? 1 : 0;
}
static void cfgSave() {
  Cfg c; c.magic = 0x5B;
  for (int v = 0; v < NVOICE; v++) c.ch[v] = chan[v];
  c.voct = voctN; c.ginv = gateInv;
  EEPROM.put(0, c); EEPROM.commit();
}

// ---------------- outputs ----------------
static uint16_t cvCode(uint8_t note) {
  if (note < NOTE0) note = NOTE0;
  float v = (note - NOTE0) / 12.0f * (float)voctN;
  float code = v / CV_FULLSCALE_V * 65535.0f;
  if (code < 0) code = 0;
  if (code > 65535.0f) code = 65535.0f;
  return (uint16_t)(code + 0.5f);
}
static void updateOutputs() {
  for (int v = 0; v < NVOICE; v++) {
    dacWriteUpdate(dacCh[v], cvCode(curNote[v]));
    digitalWrite(gatePin[v], gate[v] ? (gateInv ? LOW : HIGH) : (gateInv ? HIGH : LOW));
  }
}

// ---------------- MIDI ----------------
static void midiNoteOn(uint8_t v, uint8_t n) {
  if (noteCnt[v] < 16) noteStack[v][noteCnt[v]++] = n;
  curNote[v] = n; gate[v] = true; gDirty = true;
}
static void midiNoteOff(uint8_t v, uint8_t n) {
  for (int i = 0; i < noteCnt[v]; i++) if (noteStack[v][i] == n) {
    for (int j = i; j < noteCnt[v] - 1; j++) noteStack[v][j] = noteStack[v][j + 1];
    noteCnt[v]--; break;
  }
  if (noteCnt[v] > 0) curNote[v] = noteStack[v][noteCnt[v] - 1];
  else gate[v] = false;
  gDirty = true;
}
static void midiTask() {
  uint8_t pkt[4];
  while (usb_midi.readPacket(pkt)) {
    uint8_t status = pkt[1];
    if (status < 0x80) continue;
    uint8_t type = status & 0xF0, midiCh = (status & 0x0F) + 1;
    for (int v = 0; v < NVOICE; v++) {
      if (chan[v] != midiCh) continue;
      if (type == 0x90 && pkt[3] > 0) midiNoteOn(v, pkt[2]);
      else if (type == 0x80 || (type == 0x90 && pkt[3] == 0)) midiNoteOff(v, pkt[2]);
    }
  }
}

// ---------------- encoder ----------------
static const int8_t ENC_TBL[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};
static volatile int16_t encDelta = 0;

static void encISR() {
  static int8_t accum = 0;
  static uint8_t lastAB = 0;
  uint8_t ab = (uint8_t)((digitalRead(ENC_A) << 1) | digitalRead(ENC_B));
  if (ab != lastAB) {
    accum += ENC_TBL[(lastAB << 2) | ab];
    lastAB = ab;
    if (accum >= ENC_TRANS_PER_STEP)  { accum = 0; encDelta++; }
    if (accum <= -ENC_TRANS_PER_STEP) { accum = 0; encDelta--; }
  }
}

// ---------------- UI ----------------
static bool  uiEdit = false;
static int   uiSel = 0;
static bool  uiDirty = true;
static bool  uiPainted = false;
static float uiSelY = 18.0f;
static float uiSelVel = 0.0f;
static bool  uiAnimating = false;

static int uiTargetY() { return 18 + uiSel * 27; }

static void uiHandleRotation(int16_t delta) {
  if (delta == 0) return;
  if (!uiEdit) {
    uiSel = (uiSel + delta) % NVOICE;
    if (uiSel < 0) uiSel += NVOICE;
    uiAnimating = true;
    if (!uiPainted) uiDirty = true;
  } else {
    int c = (int)chan[uiSel] + delta;
    while (c < 1) c += 16;
    while (c > 16) c -= 16;
    chan[uiSel] = (uint8_t)c;
    cfgDirty = true; cfgMs = millis();
    uiSelY = (float)uiTargetY();
    uiSelVel = 0;
    uiAnimating = false;
    uiDirty = true;
  }
}

static void uiHandlePress() {
  uiEdit = !uiEdit;
  if (!uiEdit) {
    cfgSave();
    cfgDirty = false;
  }
  uiSelY = (float)uiTargetY();
  uiSelVel = 0;
  uiAnimating = false;
  uiDirty = true;
}

static void uiRender() {
  const uint16_t bg     = rgb565(6, 8, 12);
  const uint16_t fg     = rgb565(235, 238, 242);
  const uint16_t dim    = rgb565(120, 128, 140);
  const uint16_t accent = rgb565(255, 164, 58);
  const uint16_t selCol = rgb565(70, 180, 255);

  displayClear(bg);
  char buf[20];
  displayText(6, 4, "MIDI TO CV", accent, 1);
  displayText(180, 4, uiEdit ? "EDIT" : "SELECT", uiEdit ? accent : dim, 1);
  displayFillRect(0, 15, 240, 1, rgb565(40, 46, 56));

  int by = (int)lroundf(uiSelY);
  int active = (int)lroundf((uiSelY - 18.0f) / 27.0f);
  if (active < 0) active = 0;
  if (active > NVOICE - 1) active = NVOICE - 1;

  displayFillRect(4, by - 1, 232, 25,
                  uiEdit ? rgb565(22, 36, 54) : rgb565(14, 24, 38));

  for (int i = 0; i < NVOICE; i++) {
    int y = 18 + i * 27;
    bool act = (i == active);
    snprintf(buf, sizeof(buf), "V%d", i + 1);
    displayText(12, y + 7, buf, act ? fg : dim, 1);
    snprintf(buf, sizeof(buf), "CH %d", chan[i]);
    displayText(60, y + 5, buf, act ? fg : rgb565(180, 186, 196), 2);
    int barW = (int)chan[i] * 6;
    displayFillRect(232 - barW, y + 9, barW, 6,
                    act ? (uiEdit ? accent : selCol) : rgb565(50, 58, 72));
  }

  displayDrawRect(3, by - 1, 234, 25, uiEdit ? accent : selCol, 2);
}

static void uiProcess() {
  int16_t delta;
  noInterrupts();
  delta = encDelta;
  encDelta = 0;
  interrupts();
  if (delta) uiHandleRotation(delta);

  static uint8_t sLast = 0, sStable = 0;
  static uint32_t tChange = 0;
  uint8_t s = (digitalRead(ENC_SW) == LOW) ? 1 : 0;
  if (s != sLast) { sLast = s; tChange = millis(); }
  if (millis() - tChange >= 15 && s != sStable) {
    sStable = s;
    if (s == 0) uiHandlePress();
  }

  static uint32_t tAnim = 0;
  uint32_t now = millis();
  if (uiAnimating && (now - tAnim >= 16)) {
    tAnim = now;
    float target = (float)uiTargetY();
    uiSelVel += (target - uiSelY) * 0.30f;
    uiSelVel *= 0.55f;
    uiSelY += uiSelVel;
    if (fabsf(target - uiSelY) < 0.2f && fabsf(uiSelVel) < 0.2f) {
      uiSelY = target;
      uiSelVel = 0;
      uiAnimating = false;
    }
    uiRender();
    displayFlush();
  }

  if (uiDirty) {
    uiDirty = false;
    uiSelY = (float)uiTargetY();
    uiSelVel = 0;
    uiAnimating = false;
    uiRender();
    displayFlush();
    uiPainted = true;
  }
}

// ---------------- setup / loop ----------------
void setup() {
  setupClock();

  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);
  TinyUSBDevice.setManufacturerDescriptor("OV");
  TinyUSBDevice.setProductDescriptor("OV-MIDI");
  TinyUSBDevice.setSerialDescriptor("OVMIDI0001");
  usb_midi.setStringDescriptor("OV-MIDI");
  usb_midi.begin();
  { uint32_t t0 = millis(); while (!TinyUSBDevice.mounted() && millis() - t0 < 1500) delay(5); }

  EEPROM.begin(sizeof(Cfg)); cfgLoad();

  pinMode(ENC_A, INPUT_PULLUP); pinMode(ENC_B, INPUT_PULLUP); pinMode(ENC_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encISR, CHANGE);
  for (int v = 0; v < NVOICE; v++) { pinMode(gatePin[v], OUTPUT); digitalWrite(gatePin[v], LOW); }

  dacInit();
  updateOutputs();
  displayInit();
  displaySetSclkForSys(62.5f, gSysKhz);
}

void loop() {
  watchdog_update();
  midiTask();

  if (gDirty) { gDirty = false; updateOutputs(); }
  uiProcess();
  if (cfgDirty && millis() - cfgMs > 800) { cfgDirty = false; cfgSave(); }
}
