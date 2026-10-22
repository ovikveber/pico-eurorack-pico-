// SPDX-License-Identifier: GPL-3.0-or-later
// midi2cv.ino - RP2040 USB-MIDI -> 8x CV/Gate (DAC8568) + ST7789 UI + clock mode.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <EEPROM.h>
#include <cmath>
#include <hardware/clocks.h>
#include <hardware/pll.h>
#include <hardware/vreg.h>
#include <hardware/watchdog.h>
#include <pico/time.h>
#include "Display.h"

Adafruit_USBD_MIDI usb_midi;

// ---------------- core clock ----------------
#define CLK_MAGIC 0x4F56434Fu
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
#define BTN_MODE  24

static const uint8_t gatePin[8] = { 22, 21, 20, 19, 15, 16, 17, 18 };
static const uint8_t dacCh[8]   = {  1,  3,  5,  7,  0,  2,  4,  6 };

#define NVOICE 8
#define NOTE0 24
#define CV_FULLSCALE_V 5.0f
#define ENC_TRANS_PER_STEP 2

#define NCLK 6
static const char* CLK_LABELS[] = {
  "16/1", "8/1", "4/1", "2/1", "1/1", "3/4", "1/2", "3/8",
  "1/3", "1/4", "3/16", "1/6", "1/8", "3/32", "1/12", "1/16",
  "3/64", "1/24", "1/32", "3/128", "1/48", "1/64", "1/96", "1/128"
};
static const float CLK_TICKS[] = {
  64.0f, 32.0f, 16.0f, 8.0f, 4.0f, 3.0f, 2.0f, 1.5f,
  4.0f / 3.0f, 1.0f, 0.75f, 4.0f / 6.0f, 0.5f, 0.375f, 1.0f / 3.0f, 0.25f,
  0.1875f, 1.0f / 6.0f, 0.125f, 0.09375f, 1.0f / 12.0f, 0.0625f, 1.0f / 24.0f, 0.03125f
};
static const uint8_t DIV_N = sizeof(CLK_LABELS) / sizeof(CLK_LABELS[0]);

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
static inline void dacWriteUpdateRaw(uint8_t ch, uint16_t data) {
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
static uint8_t voctN = 1;
static uint8_t gateInv = 0;
static bool    gate[NVOICE] = { false };
static uint8_t curNote[NVOICE] = { NOTE0 };
static uint8_t noteStack[NVOICE][16];
static uint8_t noteCnt[NVOICE] = { 0 };
static volatile bool gDirty = true;

static volatile bool clockMode = false;
static volatile bool runMode = 0;       // 0=HOLD, 1=TRIG
static volatile uint16_t bpm = 120;
static const uint8_t DEF_DIV[NCLK] = { 9, 13, 15, 8, 11, 14 };
static volatile uint8_t divIdx[NCLK] = { 9, 13, 15, 8, 11, 14 };
static volatile uint32_t clkPhase[NCLK] = { 0 };
static volatile bool clkState[NCLK] = { false };
static volatile bool runOut = false;
static volatile bool resetOut = false;
static volatile uint32_t resetUs = 0;
static volatile uint32_t runPulseUs = 0;
static repeating_timer_t clkTimer;

// ---------------- settings ----------------
struct Cfg {
  uint8_t magic;
  uint8_t ch[NVOICE];
  uint8_t voct;
  uint8_t ginv;
  uint16_t bpm;
  uint8_t div[NCLK];
  uint8_t runMode;
};
static bool cfgDirty = false;
static uint32_t cfgMs = 0;

static void cfgLoad() {
  Cfg c; EEPROM.get(0, c);
  if (c.magic != 0x5D) {
    for (int v = 0; v < NVOICE; v++) c.ch[v] = v + 1;
    c.voct = 1; c.ginv = 0; c.bpm = 120;
    for (int i = 0; i < NCLK; i++) c.div[i] = DEF_DIV[i];
    c.runMode = 0;
    c.magic = 0x5D;
  }
  for (int v = 0; v < NVOICE; v++) chan[v] = (c.ch[v] >= 1 && c.ch[v] <= 16) ? c.ch[v] : (v + 1);
  voctN = (c.voct == 1 || c.voct == 2 || c.voct == 5) ? c.voct : 1;
  gateInv = c.ginv ? 1 : 0;
  bpm = (c.bpm >= 20 && c.bpm <= 300) ? c.bpm : 120;
  for (int i = 0; i < NCLK; i++) divIdx[i] = (c.div[i] < DIV_N) ? c.div[i] : DEF_DIV[i];
  runMode = c.runMode ? 1 : 0;
}
static void cfgSave() {
  Cfg c; c.magic = 0x5D;
  for (int v = 0; v < NVOICE; v++) c.ch[v] = chan[v];
  c.voct = voctN; c.ginv = gateInv;
  c.bpm = bpm;
  for (int i = 0; i < NCLK; i++) c.div[i] = divIdx[i];
  c.runMode = runMode;
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
  noInterrupts();
  int start = clockMode ? 4 : 0;
  for (int v = start; v < NVOICE; v++) {
    dacWriteUpdateRaw(dacCh[v], cvCode(curNote[v]));
    digitalWrite(gatePin[v], gate[v] ? (gateInv ? LOW : HIGH) : (gateInv ? HIGH : LOW));
  }
  interrupts();
}

// ---------------- clock generation ----------------
static void setClockOutput(uint8_t i, bool on) {
  uint8_t v = 1 + i / 2;
  if (i % 2 == 0) digitalWrite(gatePin[v], on ? HIGH : LOW);
  else dacWriteUpdateRaw(dacCh[v], on ? 65535 : 0);
}

static bool clockTimerCB(repeating_timer_t*) {
  if (!clockMode) return true;

  if (resetUs) {
    resetUs = (resetUs > 1000) ? (resetUs - 1000) : 0;
    bool on = (resetUs > 0);
    if (on != resetOut) {
      resetOut = on;
      dacWriteUpdateRaw(dacCh[0], on ? 65535 : 0);
    }
  }

  bool runNow;
  if (runMode == 0) {
    runNow = true;
  } else {
    if (runPulseUs > 0) runPulseUs = (runPulseUs > 1000) ? (runPulseUs - 1000) : 0;
    runNow = (runPulseUs > 0);
  }
  if (runNow != runOut) {
    runOut = runNow;
    digitalWrite(gatePin[0], runNow ? HIGH : LOW);
  }

  for (int i = 0; i < NCLK; i++) {
    uint32_t period = (uint32_t)(60000000.0f / (float)bpm * CLK_TICKS[divIdx[i]]);
    clkPhase[i] += 1000u;
    if (clkPhase[i] >= period) clkPhase[i] %= period;
    bool on = (clkPhase[i] < period / 2u);
    if (on != clkState[i]) {
      clkState[i] = on;
      setClockOutput(i, on);
    }
  }
  return true;
}

static void clockStart() {
  for (int i = 0; i < NCLK; i++) { clkPhase[i] = 0; clkState[i] = false; }
  for (int v = 0; v < 4; v++) { noteCnt[v] = 0; gate[v] = false; }
  resetUs = 100000;
  resetOut = false;
  runPulseUs = (runMode ? 100000 : 0);
  runOut = false;
  clockMode = true;
  updateOutputs();
}

static void clockStop() {
  clockMode = false;
  updateOutputs();
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
      if (clockMode && v < 4) continue;
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

// ---------------- single-column UI ----------------
enum UiType : uint8_t { UI_CH, UI_CLK, UI_TEMPO, UI_GATE, UI_VOCT, UI_RUN, UI_RUNSTAT };
struct UiItem { uint8_t type; uint8_t index; };

static UiItem  uiItems[16];
static uint8_t uiCount = 0;
static uint8_t uiFirst = 0;
static uint8_t uiSel = 0;
static bool    uiEdit = false;
static bool    uiDirty = true;
static bool    uiPainted = false;
static float   uiSelY = 17.0f;
static float   uiVelY = 0.0f;
static bool    uiAnimating = false;
static uint8_t uiLedCache[16];

#define UI_VIS   10
#define UI_ROW_H 22
#define UI_ROW_Y 18
#define UI_X     2
#define UI_W     236
#define DOT_SIZE 12

static bool uiSelectable(uint8_t i) { return uiItems[i].type != UI_RUNSTAT; }

static void uiClampWindow() {
  if (uiSel < uiFirst) uiFirst = uiSel;
  else if (uiSel >= uiFirst + UI_VIS) uiFirst = uiSel - UI_VIS + 1;
  if (uiFirst + UI_VIS > uiCount) uiFirst = (uiCount > UI_VIS) ? (uiCount - UI_VIS) : 0;
}

static void uiBuildItems() {
  uiCount = 0;
  if (!clockMode) {
    for (int i = 0; i < 8; i++) uiItems[uiCount++] = { UI_CH, (uint8_t)i };
    uiItems[uiCount++] = { UI_GATE, 0 };
    uiItems[uiCount++] = { UI_VOCT, 0 };
  } else {
    for (int i = 0; i < NCLK; i++) uiItems[uiCount++] = { UI_CLK, (uint8_t)i };
    for (int i = 4; i < 8; i++) uiItems[uiCount++] = { UI_CH, (uint8_t)i };
    uiItems[uiCount++] = { UI_RUNSTAT, 0 };
    uiItems[uiCount++] = { UI_TEMPO, 0 };
    uiItems[uiCount++] = { UI_GATE, 0 };
    uiItems[uiCount++] = { UI_VOCT, 0 };
    uiItems[uiCount++] = { UI_RUN, 0 };
  }
  uiSel = 0;
  uiFirst = 0;
  uiEdit = false;
  uiSelY = (float)(UI_ROW_Y - 1);
  uiVelY = 0;
  uiAnimating = false;
  for (int i = 0; i < 16; i++) uiLedCache[i] = 0xFF;
  uiDirty = true;
}

static void uiSyncLedCache() {
  for (uint8_t i = 0; i < uiCount; i++) {
    uint8_t v = 0;
    switch (uiItems[i].type) {
      case UI_CH: v = gate[uiItems[i].index] ? 1 : 0; break;
      case UI_CLK: v = clkState[uiItems[i].index] ? 1 : 0; break;
      case UI_RUNSTAT: v = (runOut ? 1 : 0) | (resetOut ? 2 : 0); break;
      default: break;
    }
    uiLedCache[i] = v;
  }
}

static int uiSelTargetY() {
  return UI_ROW_Y + (int)(uiSel - uiFirst) * UI_ROW_H - 1;
}

static void uiSnapSel() {
  uiSelY = (float)uiSelTargetY();
  uiVelY = 0;
  uiAnimating = false;
}

static void uiMove(int dir) {
  if (uiCount == 0) return;
  do {
    uiSel = (uint8_t)((uiSel + dir + uiCount) % uiCount);
  } while (!uiSelectable(uiSel));
  uiClampWindow();
  uiAnimating = true;
  if (!uiPainted) uiDirty = true;
}

static void uiHandleRotation(int16_t delta) {
  if (delta == 0) return;
  if (!uiEdit) {
    int n = delta > 0 ? delta : -delta;
    for (int i = 0; i < n; i++) uiMove(delta > 0 ? 1 : -1);
    return;
  }

  UiItem& it = uiItems[uiSel];
  switch (it.type) {
    case UI_CH: {
      int c = (int)chan[it.index] + delta;
      while (c < 1) c += 16;
      while (c > 16) c -= 16;
      chan[it.index] = (uint8_t)c;
      break;
    }
    case UI_CLK: {
      int d = (int)divIdx[it.index] + delta;
      while (d < 0) d += DIV_N;
      while (d >= DIV_N) d -= DIV_N;
      divIdx[it.index] = (uint8_t)d;
      break;
    }
    case UI_TEMPO: {
      int m = (int)bpm + delta;
      if (m < 20) m = 20;
      if (m > 300) m = 300;
      bpm = (uint16_t)m;
      break;
    }
    case UI_GATE:
      gateInv ^= 1;
      gDirty = true;
      break;
    case UI_VOCT: {
      static const uint8_t V[3] = { 1, 2, 5 };
      int i = 0; while (i < 2 && V[i] != voctN) i++;
      i = (i + (delta > 0 ? 1 : 2)) % 3;
      voctN = V[i];
      gDirty = true;
      break;
    }
    case UI_RUN:
      runMode ^= 1;
      runPulseUs = runMode ? 100000 : 0;
      break;
    default:
      break;
  }
  cfgDirty = true; cfgMs = millis();
  uiDirty = true;
}

static void uiHandlePress() {
  uiEdit = !uiEdit;
  if (!uiEdit) {
    cfgSave();
    cfgDirty = false;
  }
  uiSnapSel();
  uiDirty = true;
}

static void uiDrawDotFb(int x, int y, bool on, uint16_t color) {
  displayFillRect(x, y, DOT_SIZE, DOT_SIZE, on ? color : rgb565(38, 44, 54));
}

static void uiRender() {
  const uint16_t bg     = rgb565(6, 8, 12);
  const uint16_t fg     = rgb565(235, 238, 242);
  const uint16_t dim    = rgb565(120, 128, 140);
  const uint16_t accent = rgb565(255, 164, 58);
  const uint16_t selCol = rgb565(70, 180, 255);

  displayClear(bg);
  displayText(6, 4, "MIDI TO CV", accent, 1);
  displayText(180, 4, clockMode ? "CLOCK" : "MIDI", clockMode ? accent : dim, 1);
  displayFillRect(0, 15, 240, 1, rgb565(40, 46, 56));

  int by = (int)lroundf(uiSelY);
  displayFillRect(UI_X + 1, by + 1, UI_W - 2, UI_ROW_H - 3,
                  uiEdit ? rgb565(22, 36, 54) : rgb565(14, 24, 38));

  char buf[24];
  for (uint8_t i = uiFirst; i < uiFirst + UI_VIS && i < uiCount; i++) {
    const UiItem& it = uiItems[i];
    int row = i - uiFirst;
    int y = UI_ROW_Y + row * UI_ROW_H;
    int ty = y + 7;
    bool sel = (i == uiSel);

    switch (it.type) {
      case UI_CH: {
        int v = it.index;
        snprintf(buf, sizeof(buf), "V%d", v + 1);
        displayText(UI_X + 8, ty, buf, sel ? fg : dim, 1);

        int bx = UI_X + 58, bw = 100;
        displayFillRect(bx, y + 8, bw, 6, rgb565(38, 44, 54));
        int fw = (int)chan[v] * bw / 16;
        displayFillRect(bx, y + 8, fw, 6, selCol);

        snprintf(buf, sizeof(buf), "CH %d", chan[v]);
        displayText(UI_X + 162, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        uiDrawDotFb(UI_X + 220, y + 5, gate[v], selCol);
        break;
      }
      case UI_CLK: {
        int k = it.index;
        snprintf(buf, sizeof(buf), "C%d  %s", k + 1, CLK_LABELS[divIdx[k]]);
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        uiDrawDotFb(UI_X + 220, y + 5, clkState[k], accent);
        break;
      }
      case UI_RUNSTAT:
        displayText(UI_X + 12, ty, "RUN", dim, 1);
        uiDrawDotFb(UI_X + 48, y + 5, runOut, accent);
        displayText(UI_X + 78, ty, "RST", dim, 1);
        uiDrawDotFb(UI_X + 112, y + 5, resetOut, accent);
        break;
      case UI_TEMPO:
        snprintf(buf, sizeof(buf), "BPM  %d", bpm);
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_GATE:
        displayText(UI_X + 8, ty, gateInv ? "GATE  -" : "GATE  +",
                    sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_VOCT:
        snprintf(buf, sizeof(buf), "V/OCT  %d", voctN);
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RUN:
        displayText(UI_X + 8, ty, runMode ? "RUN  TRIG" : "RUN  HOLD",
                    sel ? fg : rgb565(180, 186, 196), 1);
        break;
    }
    displayFillRect(UI_X, y + UI_ROW_H - 1, UI_W, 1, rgb565(40, 46, 56));
  }

  displayDrawRect(UI_X, by, UI_W, UI_ROW_H - 1, uiEdit ? accent : selCol, 1);
}

static void uiFlashDots() {
  const uint16_t accent = rgb565(255, 164, 58);
  const uint16_t selCol = rgb565(70, 180, 255);
  for (uint8_t i = uiFirst; i < uiFirst + UI_VIS && i < uiCount; i++) {
    const UiItem& it = uiItems[i];
    int y = UI_ROW_Y + (i - uiFirst) * UI_ROW_H;
    switch (it.type) {
      case UI_CH: {
        uint8_t cur = gate[it.index] ? 1 : 0;
        if ((uiLedCache[i] & 1) != cur) {
          displayFillSquare(UI_X + 220, y + 5, DOT_SIZE,
                            cur ? selCol : rgb565(38, 44, 54));
          uiLedCache[i] = (uiLedCache[i] & ~1u) | cur;
        }
        break;
      }
      case UI_CLK: {
        uint8_t cur = clkState[it.index] ? 1 : 0;
        if ((uiLedCache[i] & 1) != cur) {
          displayFillSquare(UI_X + 220, y + 5, DOT_SIZE,
                            cur ? accent : rgb565(38, 44, 54));
          uiLedCache[i] = (uiLedCache[i] & ~1u) | cur;
        }
        break;
      }
      case UI_RUNSTAT: {
        uint8_t cur = (runOut ? 1 : 0) | (resetOut ? 2 : 0);
        if ((uiLedCache[i] & 3) != cur) {
          displayFillSquare(UI_X + 48, y + 5, DOT_SIZE,
                            (cur & 1) ? accent : rgb565(38, 44, 54));
          displayFillSquare(UI_X + 112, y + 5, DOT_SIZE,
                            (cur & 2) ? accent : rgb565(38, 44, 54));
          uiLedCache[i] = (uiLedCache[i] & ~3u) | cur;
        }
        break;
      }
      default:
        break;
    }
  }
}

static void toggleClockMode() {
  if (clockMode) clockStop(); else clockStart();
  uiBuildItems();
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

  static uint8_t mLast = 1, mStable = 1;
  static uint32_t mChange = 0;
  uint8_t m = (digitalRead(BTN_MODE) == LOW) ? 0 : 1;
  if (m != mLast) { mLast = m; mChange = millis(); }
  if (millis() - mChange >= 20 && m != mStable) {
    mStable = m;
    if (m == 0) toggleClockMode();
  }

  static uint32_t tAnim = 0;
  static uint32_t tFlash = 0;
  uint32_t now = millis();

  if (uiDirty) {
    uiDirty = false;
    uiSnapSel();
    uiRender();
    displayFlush();
    uiSyncLedCache();
    uiPainted = true;
    tFlash = now;
    return;
  }

  if (uiAnimating && (now - tAnim >= 16)) {
    tAnim = now;
    float ty = (float)uiSelTargetY();
    uiVelY += (ty - uiSelY) * 0.30f;
    uiVelY *= 0.55f;
    uiSelY += uiVelY;
    if (fabsf(ty - uiSelY) < 0.2f && fabsf(uiVelY) < 0.2f) {
      uiSelY = ty; uiVelY = 0; uiAnimating = false;
    }
    uiRender();
    displayFlush();
    uiSyncLedCache();
    tFlash = now;
    return;
  }

  if (now - tFlash >= 33) {
    tFlash = now;
    uiFlashDots();
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
  pinMode(BTN_MODE, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encISR, CHANGE);
  for (int v = 0; v < NVOICE; v++) { pinMode(gatePin[v], OUTPUT); digitalWrite(gatePin[v], LOW); }

  dacInit();
  updateOutputs();
  displayInit();
  displaySetSclkForSys(62.5f, gSysKhz);

  uiBuildItems();
  add_repeating_timer_us(-1000, clockTimerCB, nullptr, &clkTimer);
}

void loop() {
  watchdog_update();
  midiTask();

  if (gDirty) { gDirty = false; updateOutputs(); }
  uiProcess();
  if (cfgDirty && millis() - cfgMs > 800) { cfgDirty = false; cfgSave(); }
}










