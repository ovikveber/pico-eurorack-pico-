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
#include "RandomWorld.h"

Adafruit_USBD_MIDI usb_midi;

// ---------------- core clock ----------------
#define CLK_MAGIC 0x4F56434Fu
#define TEST_125MHZ 0
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

#if TEST_125MHZ
  if (vreg_get_voltage() < VREG_VOLTAGE_1_10) {
    vreg_set_voltage(VREG_VOLTAGE_1_10);
    busy_wait_us_32(2000);
  }
#else
  if (vreg_get_voltage() < VREG_VOLTAGE_1_30) {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    busy_wait_us_32(2000);
  }
#endif
  watchdog_start_tick(12);
  watchdog_enable(8000, false);

#if TEST_125MHZ
  (void)fallback;
  set_sys_clock_khz(125000, true);
  busy_wait_us_32(2000);
  gSysKhz = 125000;
#else
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
#endif

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
#define BTN_RANDOM 23

static const uint8_t gatePin[8] = { 22, 21, 20, 19, 15, 16, 17, 18 };
static const uint8_t dacCh[8]   = {  1,  3,  5,  7,  0,  2,  4,  6 };

#define NVOICE 8
#define NOTE0 24
#define CV_FULLSCALE_V 5.0f
#define ENC_TRANS_PER_STEP 2

#define NCLK 8
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
static volatile bool clockRunning = false;
static volatile bool clockArmed = false;
static volatile uint16_t bpm = 120;
static const uint8_t DEF_DIV[NCLK] = { 9, 13, 15, 8, 11, 14, 4, 6 };
static volatile uint8_t divIdx[NCLK] = { 9, 13, 15, 8, 11, 14, 4, 6 };
static volatile uint32_t clkPhase[NCLK] = { 0 };
static volatile bool clkState[NCLK] = { false };
static repeating_timer_t clkTimer;
static bool clkTimerActive = false;

// ---------------- RandomSynth mode ----------------
enum RsBind : uint8_t {
  RB_FIELD, RB_BALL, RB_SPEED, RB_WAVE, RB_WSPD, RB_RANGE, RB_PITCH,
  RB_ADSR, RB_FXR, RB_SCALE, RB_VOCT, RB_GATE, RB_TRIG, RB_FXSEND,
  RB_SHAPE, RB_COUNT
};
static const char* RB_NAMES[RB_COUNT] = {
  "FIELD", "BALL", "SPEED", "WAVE", "W.SPD", "RANGE", "PITCH",
  "ADSR", "FX RATIO", "SCALE", "V/OCT", "GATE", "TRIG", "FX SEND", "SHAPE"
};
static const char* SHAPE_NAMES[4] = { "CIRCLE", "RECT", "ELLIPSE", "HEX" };
static const int SCALE_COUNT = (int)ScaleMode::Count;

static bool randomMode = false;
static bool randomSettings = false;
static bool prevClockMode = false;
static bool prevClockRunning = false;
static bool rsInited = false;
static bool rsGateOn = false;
static bool rsTrigOn = false;
static float rsGateRemain = 0.0f;
static float rsTrigRemain = 0.0f;
static float rsCvVolts = 0.0f;
static float rsFxVolts = 0.0f;
static float rsNote = 60.0f;
static uint32_t rsTrigSeenMs = 0;
static uint32_t rsLastSim = 0;
static uint32_t rsLastDraw = 0;
static bool randomDirty = true;
static World rsWorld;
static Snapshot rsSnap;

static uint8_t rsShape = 0;
static uint8_t rsBind = RB_FIELD;
static uint8_t rsVoct = 1;
static uint8_t rsGateInv = 0;
static uint8_t rsTrigInv = 0;
static uint8_t rsFxSend = 100;
static uint8_t rsScale = 0;
static uint8_t rsRangeFine = 0;
static float rsField = 1.0f;
static float rsBall = 0.55f;
static float rsSpeed = 1.0f;
static float rsWave = 0.5f;
static float rsWSPD = 0.65f;
static float rsRange = 4.0f;
static float rsPitch = 50.0f;
static float rsAdsr = 25.0f;
static float rsFxRatio = 25.0f;

// ---------------- settings ----------------
struct Cfg {
  uint8_t magic;
  uint8_t ch[NVOICE];
  uint8_t voct;
  uint8_t ginv;
  uint16_t bpm;
  uint8_t div[NCLK];
  uint8_t rsShape, rsBind, rsVoct, rsGate, rsTrig, rsFxSend, rsScale;
  uint8_t rsField, rsBall, rsSpeed, rsWave, rsWSPD, rsRange, rsPitch, rsAdsr, rsFx;
  uint8_t rsRangeFine;
  uint16_t rsRangeMilli;
};
static bool cfgDirty = false;
static uint32_t cfgMs = 0;

static void cfgLoad() {
  Cfg c; EEPROM.get(0, c);
  if (c.magic != 0x5F) {
    for (int v = 0; v < NVOICE; v++) c.ch[v] = v + 1;
    c.voct = 1; c.ginv = 0; c.bpm = 120;
    for (int i = 0; i < NCLK; i++) c.div[i] = DEF_DIV[i];
    c.rsShape = 0; c.rsBind = RB_FIELD; c.rsVoct = 1;
    c.rsGate = 0; c.rsTrig = 0; c.rsFxSend = 100; c.rsScale = 0;
    c.rsField = 100; c.rsBall = 55; c.rsSpeed = 100; c.rsWave = 50;
    c.rsWSPD = 65; c.rsRange = 8; c.rsPitch = 50; c.rsAdsr = 25; c.rsFx = 25;
    c.rsRangeFine = 0; c.rsRangeMilli = 4000;
    c.magic = 0x5F;
  }
  for (int v = 0; v < NVOICE; v++) chan[v] = (c.ch[v] >= 1 && c.ch[v] <= 16) ? c.ch[v] : (v + 1);
  voctN = (c.voct == 1 || c.voct == 2 || c.voct == 5) ? c.voct : 1;
  gateInv = c.ginv ? 1 : 0;
  bpm = (c.bpm >= 20 && c.bpm <= 300) ? c.bpm : 120;
  for (int i = 0; i < NCLK; i++) divIdx[i] = (c.div[i] < DIV_N) ? c.div[i] : DEF_DIV[i];

  rsShape = (c.rsShape <= 3) ? c.rsShape : 0;
  rsBind = (c.rsBind < RB_COUNT) ? c.rsBind : RB_FIELD;
  rsVoct = (c.rsVoct == 1 || c.rsVoct == 2 || c.rsVoct == 5) ? c.rsVoct : 1;
  rsGateInv = c.rsGate ? 1 : 0;
  rsTrigInv = c.rsTrig ? 1 : 0;
  rsFxSend = (c.rsFxSend <= 100) ? c.rsFxSend : 100;
  rsScale = (c.rsScale < SCALE_COUNT) ? c.rsScale : 0;
  rsField = constrain(c.rsField, 50, 100) / 100.0f;
  rsBall = constrain(c.rsBall, 30, 160) / 100.0f;
  rsSpeed = constrain(c.rsSpeed, 10, 300) / 100.0f;
  rsWave = constrain(c.rsWave, 20, 150) / 100.0f;
  rsWSPD = constrain(c.rsWSPD, 20, 200) / 100.0f;
  rsRangeFine = (c.rsRangeFine == 1) ? 1 : 0;
  if (c.rsRangeMilli >= 500 && c.rsRangeMilli <= 8000)
    rsRange = c.rsRangeMilli / 1000.0f;
  else
    rsRange = constrain(c.rsRange, 1, 16) * 0.5f;
  rsPitch = constrain(c.rsPitch, 0, 100);
  rsAdsr = constrain(c.rsAdsr, 0, 100);
  rsFxRatio = constrain(c.rsFx, 0, 100);
}
static void cfgSave() {
  Cfg c; c.magic = 0x5F;
  for (int v = 0; v < NVOICE; v++) c.ch[v] = chan[v];
  c.voct = voctN; c.ginv = gateInv;
  c.bpm = bpm;
  for (int i = 0; i < NCLK; i++) c.div[i] = divIdx[i];
  c.rsShape = rsShape; c.rsBind = rsBind; c.rsVoct = rsVoct;
  c.rsGate = rsGateInv; c.rsTrig = rsTrigInv; c.rsFxSend = rsFxSend;
  c.rsScale = rsScale;
  c.rsField = (uint8_t)constrain((int)lroundf(rsField * 100.0f), 50, 100);
  c.rsBall = (uint8_t)constrain((int)lroundf(rsBall * 100.0f), 30, 160);
  c.rsSpeed = (uint8_t)constrain((int)lroundf(rsSpeed * 100.0f), 10, 300);
  c.rsWave = (uint8_t)constrain((int)lroundf(rsWave * 100.0f), 20, 150);
  c.rsWSPD = (uint8_t)constrain((int)lroundf(rsWSPD * 100.0f), 20, 200);
  c.rsRange = (uint8_t)constrain((int)lroundf(rsRange * 2.0f), 1, 16);
  c.rsPitch = (uint8_t)constrain((int)lroundf(rsPitch), 0, 100);
  c.rsAdsr = (uint8_t)constrain((int)lroundf(rsAdsr), 0, 100);
  c.rsFx = (uint8_t)constrain((int)lroundf(rsFxRatio), 0, 100);
  c.rsRangeFine = rsRangeFine ? 1 : 0;
  c.rsRangeMilli = (uint16_t)constrain((int)lroundf(rsRange * 1000.0f), 500, 8000);
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
  uint8_t v = i / 2;
  if (i % 2 == 0) digitalWrite(gatePin[v], on ? HIGH : LOW);
  else dacWriteUpdateRaw(dacCh[v], on ? 65535 : 0);
}

static bool clockTimerCB(repeating_timer_t*) {
  if (!clockMode) return true;
  if (!clockRunning) return true;

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

static void clockTimerAdd() {
  if (!clkTimerActive)
    clkTimerActive = add_repeating_timer_us(-1000, clockTimerCB, nullptr, &clkTimer);
}

static void clockTimerCancel() {
  if (clkTimerActive) {
    cancel_repeating_timer(&clkTimer);
    clkTimerActive = false;
  }
}

static void clockOutputsLow() {
  noInterrupts();
  for (int i = 0; i < NCLK; i++) {
    clkState[i] = false;
    setClockOutput(i, false);
  }
  interrupts();
}

static void clockStart() {
  clockTimerCancel();
  clockMode = true;
  clockRunning = false;
  clockArmed = true;
  for (int i = 0; i < NCLK; i++) { clkPhase[i] = 0; clkState[i] = false; }
  for (int v = 0; v < 4; v++) { noteCnt[v] = 0; gate[v] = false; }
  clockOutputsLow();
  updateOutputs();
}

static void clockStop() {
  clockTimerCancel();
  clockRunning = false;
  clockMode = false;
  clockArmed = false;
  updateOutputs();
}

static void clockSetRunning(bool on) {
  if (on) {
    if (clockArmed) {
      for (int i = 0; i < NCLK; i++) clkPhase[i] = 0;
      clockArmed = false;
    }
    noInterrupts();
    clockRunning = true;
    interrupts();
    clockTimerAdd();
  } else {
    clockTimerCancel();
    noInterrupts();
    clockRunning = false;
    for (int i = 0; i < NCLK; i++) {
      clkState[i] = false;
      setClockOutput(i, false);
    }
    interrupts();
  }
}

// ---------------- RandomSynth outputs ----------------
static inline uint16_t voltsToCode(float v) {
  if (v < 0.0f) v = 0.0f;
  if (v > CV_FULLSCALE_V) v = CV_FULLSCALE_V;
  return (uint16_t)(v / CV_FULLSCALE_V * 65535.0f + 0.5f);
}

static void rsWriteOutputs() {
  dacWriteUpdateRaw(dacCh[0], voltsToCode(rsCvVolts));
  dacWriteUpdateRaw(dacCh[1], voltsToCode(rsFxVolts));
  digitalWrite(gatePin[0], rsGateOn ? (rsGateInv ? LOW : HIGH) : (rsGateInv ? HIGH : LOW));
  digitalWrite(gatePin[1], rsTrigOn ? (rsTrigInv ? LOW : HIGH) : (rsTrigInv ? HIGH : LOW));
}

static void rsApplySettings() {
  WorldSettings s;
  s.ballScale = rsBall;
  s.ballSpeed = rsSpeed;
  s.waveRange = rsWave;
  s.waveSpeed = rsWSPD;
  s.pitchRange = rsRange;
  s.scaleMode = rsScale;
  s.ratio = { rsPitch / 100.0f, rsAdsr / 100.0f, rsFxRatio / 100.0f };
  rsWorld.setSettings(s);
  rsWorld.setShape(rsShape);
  rsWorld.setFieldScale(rsField);
  rsWorld.setPitchRange(rsRange);
  rsWorld.setScale(rsScale);
  rsWorld.setBallScale(rsBall);
}

static void rsInit() {
  if (rsInited) return;
  rsApplySettings();
  rsWorld.reset();
  rsInited = true;
}

static void rsClearOutputs() {
  rsGateOn = false;
  rsTrigOn = false;
  rsGateRemain = 0.0f;
  rsTrigRemain = 0.0f;
  rsCvVolts = 0.0f;
  rsFxVolts = 0.0f;
  digitalWrite(gatePin[0], rsGateInv ? HIGH : LOW);
  digitalWrite(gatePin[1], rsTrigInv ? HIGH : LOW);
  dacWriteUpdateRaw(dacCh[0], 0);
  dacWriteUpdateRaw(dacCh[1], 0);
}

static void rsTick() {
  if (!randomMode) return;
  uint32_t now = millis();
  if (now - rsLastSim < 16u) return;
  rsLastSim = now;

  constexpr float dt = 1.0f / 60.0f;
  rsWorld.update(dt);

  if (rsGateRemain > 0.0f) {
    rsGateRemain -= dt;
    if (rsGateRemain <= 0.0f) {
      rsGateRemain = 0.0f;
      rsGateOn = false;
      rsFxVolts = 0.0f;
    }
  }
  if (rsTrigRemain > 0.0f) {
    rsTrigRemain -= dt;
    if (rsTrigRemain <= 0.0f) {
      rsTrigRemain = 0.0f;
      rsTrigOn = false;
    }
  }

  for (const auto& ev : rsWorld.getEvents()) {
    rsCvVolts = (ev.note - 60.0f) / 12.0f * (float)rsVoct;
    rsFxVolts = ev.send * 5.0f * ((float)rsFxSend / 100.0f);
    rsNote = ev.note;
    rsGateOn = true;
    rsTrigOn = true;
    rsTrigSeenMs = millis();
    rsGateRemain = 0.05f + ev.release * (0.5f + ((float)random(0, 1000) / 1000.0f) * 1.4f);
    rsTrigRemain = 0.02f;
    if (rsGateRemain > 2.5f) rsGateRemain = 2.5f;
  }

  rsWriteOutputs();
}

static int wrapIndex(int v, int n) {
  while (v < 0) v += n;
  while (v >= n) v -= n;
  return v;
}

static float rsNorm(float v, float lo, float hi) {
  if (hi <= lo) return 0.0f;
  v = fminf(hi, fmaxf(lo, v));
  return (v - lo) / (hi - lo);
}

static void rsAdjustNorm(float& v, float lo, float hi, int delta, float step = 0.01f) {
  float n = rsNorm(v, lo, hi) + delta * step;
  n = fminf(1.0f, fmaxf(0.0f, n));
  v = lo + n * (hi - lo);
}

static void rsAdjustRange(int delta) {
  if (rsRangeFine) {
    rsRange = fminf(8.0f, fmaxf(0.5f, rsRange + delta * 0.01f));
  } else {
    rsRange = roundf((rsRange + delta * 0.5f) * 2.0f) * 0.5f;
    rsRange = fminf(8.0f, fmaxf(0.5f, rsRange));
  }
}

static void rsAdjustBound(int delta) {
  if (delta == 0) return;
  const int d = delta;
  switch (rsBind) {
    case RB_FIELD: rsAdjustNorm(rsField, 0.5f, 1.0f, d); break;
    case RB_BALL: rsAdjustNorm(rsBall, 0.3f, 1.6f, d); break;
    case RB_SPEED: rsAdjustNorm(rsSpeed, 0.1f, 3.0f, d); break;
    case RB_WAVE: rsAdjustNorm(rsWave, 0.2f, 1.5f, d); break;
    case RB_WSPD: rsAdjustNorm(rsWSPD, 0.2f, 2.0f, d); break;
    case RB_RANGE: rsAdjustRange(d); break;
    case RB_PITCH: rsAdjustNorm(rsPitch, 0.0f, 100.0f, d); break;
    case RB_ADSR: rsAdjustNorm(rsAdsr, 0.0f, 100.0f, d); break;
    case RB_FXR: rsAdjustNorm(rsFxRatio, 0.0f, 100.0f, d); break;
    case RB_SCALE: rsScale = (uint8_t)wrapIndex((int)rsScale + d, SCALE_COUNT); break;
    case RB_VOCT: {
      static const uint8_t V[3] = { 1, 2, 5 };
      int i = 0; while (i < 2 && V[i] != rsVoct) i++;
      rsVoct = V[wrapIndex(i + (d > 0 ? 1 : 2), 3)];
      break;
    }
    case RB_GATE: rsGateInv ^= 1; break;
    case RB_TRIG: rsTrigInv ^= 1; break;
    case RB_FXSEND: {
      float f = rsFxSend;
      rsAdjustNorm(f, 0.0f, 100.0f, d);
      rsFxSend = (uint8_t)lroundf(f);
      break;
    }
    case RB_SHAPE: rsShape = (uint8_t)wrapIndex((int)rsShape + d, 4); break;
    default: break;
  }
  rsApplySettings();
  rsWriteOutputs();
  cfgDirty = true; cfgMs = millis();
  randomDirty = true;
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
    if (randomMode) continue;

    if (status == 0xFA) {              // Start
      if (clockMode) { clockArmed = true; clockSetRunning(true); }
      continue;
    }
    if (status == 0xFB) {              // Continue
      if (clockMode) clockSetRunning(true);
      continue;
    }
    if (status == 0xFC) {              // Stop
      if (clockMode) clockSetRunning(false);
      continue;
    }

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
enum UiType : uint8_t {
  UI_CH, UI_CLK, UI_TEMPO, UI_GATE, UI_VOCT, UI_PLAY,
  UI_RS_BACK, UI_RS_SHAPE, UI_RS_FIELD, UI_RS_BALL, UI_RS_SPEED,
  UI_RS_WAVE, UI_RS_WSPD, UI_RS_PITCH, UI_RS_ADSR, UI_RS_FXR,
  UI_RS_RANGE, UI_RS_RANGEFINE, UI_RS_SCALE, UI_RS_VOCT, UI_RS_GATE, UI_RS_TRIG,
  UI_RS_FXSEND, UI_RS_BALLS, UI_RS_BOOST, UI_RS_RESET, UI_RS_BIND
};
struct UiItem { uint8_t type; uint8_t index; };

static UiItem  uiItems[32];
static uint8_t uiCount = 0;
static uint8_t uiFirst = 0;
static uint8_t uiSel = 0;
static bool    uiEdit = false;
static bool    uiDirty = true;
static bool    uiPainted = false;
static float   uiSelY = 17.0f;
static float   uiVelY = 0.0f;
static bool    uiAnimating = false;
static uint8_t uiLedCache[32];

#define UI_VIS   10
#define UI_ROW_H 22
#define UI_ROW_Y 18
#define UI_X     2
#define UI_W     236
#define DOT_SIZE 12

static bool uiSelectable(uint8_t) { return true; }

static void uiClampWindow() {
  if (uiSel < uiFirst) uiFirst = uiSel;
  else if (uiSel >= uiFirst + UI_VIS) uiFirst = uiSel - UI_VIS + 1;
  if (uiFirst + UI_VIS > uiCount) uiFirst = (uiCount > UI_VIS) ? (uiCount - UI_VIS) : 0;
}

static void uiBuildItems() {
  uiCount = 0;
  if (randomMode) {
    uiItems[uiCount++] = { UI_RS_BACK, 0 };
    uiItems[uiCount++] = { UI_RS_SHAPE, 0 };
    uiItems[uiCount++] = { UI_RS_FIELD, 0 };
    uiItems[uiCount++] = { UI_RS_BALL, 0 };
    uiItems[uiCount++] = { UI_RS_SPEED, 0 };
    uiItems[uiCount++] = { UI_RS_WAVE, 0 };
    uiItems[uiCount++] = { UI_RS_WSPD, 0 };
    uiItems[uiCount++] = { UI_RS_PITCH, 0 };
    uiItems[uiCount++] = { UI_RS_ADSR, 0 };
    uiItems[uiCount++] = { UI_RS_FXR, 0 };
    uiItems[uiCount++] = { UI_RS_RANGEFINE, 0 };
    uiItems[uiCount++] = { UI_RS_RANGE, 0 };
    uiItems[uiCount++] = { UI_RS_SCALE, 0 };
    uiItems[uiCount++] = { UI_RS_VOCT, 0 };
    uiItems[uiCount++] = { UI_RS_GATE, 0 };
    uiItems[uiCount++] = { UI_RS_TRIG, 0 };
    uiItems[uiCount++] = { UI_RS_FXSEND, 0 };
    uiItems[uiCount++] = { UI_RS_BALLS, 0 };
    uiItems[uiCount++] = { UI_RS_BOOST, 0 };
    uiItems[uiCount++] = { UI_RS_RESET, 0 };
    uiItems[uiCount++] = { UI_RS_BIND, 0 };
  } else if (!clockMode) {
    for (int i = 0; i < 8; i++) uiItems[uiCount++] = { UI_CH, (uint8_t)i };
    uiItems[uiCount++] = { UI_GATE, 0 };
    uiItems[uiCount++] = { UI_VOCT, 0 };
  } else {
    uiItems[uiCount++] = { UI_PLAY, 0 };
    for (int i = 0; i < NCLK; i++) uiItems[uiCount++] = { UI_CLK, (uint8_t)i };
    for (int i = 4; i < 8; i++) uiItems[uiCount++] = { UI_CH, (uint8_t)i };
    uiItems[uiCount++] = { UI_TEMPO, 0 };
    uiItems[uiCount++] = { UI_GATE, 0 };
    uiItems[uiCount++] = { UI_VOCT, 0 };
    
  }
  uiSel = 0;
  uiFirst = 0;
  uiEdit = false;
  uiSelY = (float)(UI_ROW_Y - 1);
  uiVelY = 0;
  uiAnimating = false;
  for (int i = 0; i < 32; i++) uiLedCache[i] = 0xFF;
  uiDirty = true;
}

static void uiSyncLedCache() {
  for (uint8_t i = 0; i < uiCount; i++) {
    uint8_t v = 0;
    switch (uiItems[i].type) {
      case UI_CH: v = gate[uiItems[i].index] ? 1 : 0; break;
      case UI_CLK: v = clkState[uiItems[i].index] ? 1 : 0; break;
      case UI_PLAY: v = clockRunning ? 1 : 0; break;
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
    case UI_RS_SHAPE: rsShape = (uint8_t)wrapIndex((int)rsShape + delta, 4); break;
    case UI_RS_FIELD: rsAdjustNorm(rsField, 0.5f, 1.0f, delta); break;
    case UI_RS_BALL: rsAdjustNorm(rsBall, 0.3f, 1.6f, delta); break;
    case UI_RS_SPEED: rsAdjustNorm(rsSpeed, 0.1f, 3.0f, delta); break;
    case UI_RS_WAVE: rsAdjustNorm(rsWave, 0.2f, 1.5f, delta); break;
    case UI_RS_WSPD: rsAdjustNorm(rsWSPD, 0.2f, 2.0f, delta); break;
    case UI_RS_PITCH: rsAdjustNorm(rsPitch, 0.0f, 100.0f, delta); break;
    case UI_RS_ADSR: rsAdjustNorm(rsAdsr, 0.0f, 100.0f, delta); break;
    case UI_RS_FXR: rsAdjustNorm(rsFxRatio, 0.0f, 100.0f, delta); break;
    case UI_RS_RANGE: rsAdjustRange(delta); break;
    case UI_RS_SCALE: rsScale = (uint8_t)wrapIndex((int)rsScale + delta, SCALE_COUNT); break;
    case UI_RS_VOCT: {
      static const uint8_t V[3] = { 1, 2, 5 };
      int i = 0; while (i < 2 && V[i] != rsVoct) i++;
      rsVoct = V[wrapIndex(i + (delta > 0 ? 1 : 2), 3)];
      break;
    }
    case UI_RS_GATE: rsGateInv ^= 1; break;
    case UI_RS_TRIG: rsTrigInv ^= 1; break;
    case UI_RS_FXSEND: {
      float f = rsFxSend;
      rsAdjustNorm(f, 0.0f, 100.0f, delta);
      rsFxSend = (uint8_t)lroundf(f);
      break;
    }
    case UI_RS_BIND: rsBind = (uint8_t)wrapIndex((int)rsBind + delta, RB_COUNT); break;
    default:
      break;
  }
  if (randomMode) {
    rsApplySettings();
    rsWriteOutputs();
    randomDirty = true;
  }
  cfgDirty = true; cfgMs = millis();
  uiDirty = true;
}

static void uiHandlePress() {
  if (randomMode) {
    const UiType t = (UiType)uiItems[uiSel].type;
    if (t == UI_RS_BACK) {
      randomSettings = false;
      uiEdit = false;
      randomDirty = true;
      return;
    }
    if (t == UI_RS_BALLS) {
      rsWorld.addBall();
      uiDirty = true;
      randomDirty = true;
      return;
    }
    if (t == UI_RS_BOOST) {
      rsWorld.addBooster();
      uiDirty = true;
      randomDirty = true;
      return;
    }
    if (t == UI_RS_RESET) {
      rsWorld.reset();
      rsClearOutputs();
      rsWriteOutputs();
      uiDirty = true;
      randomDirty = true;
      return;
    }
    if (t == UI_RS_RANGEFINE) {
      rsRangeFine ^= 1;
      cfgDirty = true;
      cfgMs = millis();
      uiDirty = true;
      return;
    }
    uiEdit = !uiEdit;
    if (!uiEdit) {
      cfgSave();
      cfgDirty = false;
    }
    uiSnapSel();
    uiDirty = true;
    return;
  }

  if (uiItems[uiSel].type == UI_PLAY) {
    clockSetRunning(!clockRunning);
    return;
  }
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
  displayText(6, 4, randomMode ? "OV-RANDOM" : "OV-MIDI", accent, 1);
  const char* mode = randomMode ? "SET" : (clockMode ? "CLOCK" : "MIDI");
  displayText(237 - (int)strlen(mode) * 6, 4, mode, (randomMode || clockMode) ? accent : dim, 1);
  displayText(69, 4, "MADE BY OVIKVEBER", dim, 1);
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
      case UI_PLAY:
        displayText(UI_X + 12, ty, "PLAY", dim, 1);
        uiDrawDotFb(UI_X + 220, y + 5, clockRunning, rgb565(80, 220, 120));
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
      case UI_RS_BACK:
        displayText(UI_X + 12, ty, "< BACK", sel ? fg : dim, 1);
        break;
      case UI_RS_SHAPE:
        snprintf(buf, sizeof(buf), "SHAPE  %s", SHAPE_NAMES[rsShape]);
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_FIELD:
        snprintf(buf, sizeof(buf), "FIELD  %.2f", rsNorm(rsField, 0.5f, 1.0f));
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_BALL:
        snprintf(buf, sizeof(buf), "BALL   %.2f", rsNorm(rsBall, 0.3f, 1.6f));
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_SPEED:
        snprintf(buf, sizeof(buf), "SPEED  %.2f", rsNorm(rsSpeed, 0.1f, 3.0f));
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_WAVE:
        snprintf(buf, sizeof(buf), "WAVE   %.2f", rsNorm(rsWave, 0.2f, 1.5f));
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_WSPD:
        snprintf(buf, sizeof(buf), "W.SPD  %.2f", rsNorm(rsWSPD, 0.2f, 2.0f));
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_PITCH:
        snprintf(buf, sizeof(buf), "PITCH  %.2f", rsNorm(rsPitch, 0.0f, 100.0f));
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_ADSR:
        snprintf(buf, sizeof(buf), "ADSR   %.2f", rsNorm(rsAdsr, 0.0f, 100.0f));
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_FXR:
        snprintf(buf, sizeof(buf), "FX RAT %.2f", rsNorm(rsFxRatio, 0.0f, 100.0f));
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_RANGE:
        snprintf(buf, sizeof(buf), "RANGE  %.2f", rsRange);
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_RANGEFINE:
        snprintf(buf, sizeof(buf), "FINE   %s", rsRangeFine ? "ON" : "OFF");
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_SCALE:
        snprintf(buf, sizeof(buf), "SCALE  %s", scaleName(rsScale));
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_VOCT:
        snprintf(buf, sizeof(buf), "V/OCT  %d", rsVoct);
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_GATE:
        displayText(UI_X + 8, ty, rsGateInv ? "GATE   -" : "GATE   +",
                    sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_TRIG:
        displayText(UI_X + 8, ty, rsTrigInv ? "TRIG   -" : "TRIG   +",
                    sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_FXSEND:
        snprintf(buf, sizeof(buf), "FX SEND %.2f", rsNorm((float)rsFxSend, 0.0f, 100.0f));
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_BALLS:
        snprintf(buf, sizeof(buf), "+ BALL   %d", rsWorld.getBallCount());
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_BOOST:
        snprintf(buf, sizeof(buf), "+ BOOST  %d", rsWorld.getBoosterCount());
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
        break;
      case UI_RS_RESET:
        displayText(UI_X + 12, ty, "RESET", sel ? fg : rgb565(220, 120, 120), 1);
        break;
      case UI_RS_BIND:
        snprintf(buf, sizeof(buf), "KNOB   %s", RB_NAMES[rsBind]);
        displayText(UI_X + 8, ty, buf, sel ? fg : rgb565(180, 186, 196), 1);
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
      case UI_PLAY: {
        uint8_t cur = clockRunning ? 1 : 0;
        if ((uiLedCache[i] & 1) != cur) {
          displayFillSquare(UI_X + 220, y + 5, DOT_SIZE,
                            cur ? rgb565(80, 220, 120) : rgb565(38, 44, 54));
          uiLedCache[i] = (uiLedCache[i] & ~1u) | cur;
        }
        break;
      }
      default:
        break;
    }
  }
}

static uint16_t rsKindColor(int kind) {
  if (kind == 0) return rgb565(255, 158, 64);
  if (kind == 1) return rgb565(76, 214, 198);
  return rgb565(206, 108, 255);
}

static uint16_t rsMixColor(uint16_t a, uint16_t b, float t) {
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r = (int)lroundf(ar + (br - ar) * t);
  int g = (int)lroundf(ag + (bg - ag) * t);
  int bl = (int)lroundf(ab + (bb - ab) * t);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

static uint16_t rsScaleColor(uint16_t c, float f) {
  if (f < 0.0f) f = 0.0f;
  if (f > 1.0f) f = 1.0f;
  int r = (int)lroundf(((c >> 11) & 31) * f);
  int g = (int)lroundf(((c >> 5) & 63) * f);
  int b = (int)lroundf((c & 31) * f);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void rsNoteText(char* out, size_t n) {
  static const char* names[] = { "C", "C#", "D", "D#", "E", "F",
                                 "F#", "G", "G#", "A", "A#", "B" };
  int midi = (int)lroundf(rsNote);
  if (midi < 0) midi = 0;
  if (midi > 127) midi = 127;
  int cents = (int)lroundf((rsNote - (float)midi) * 100.0f);
  if (cents != 0)
    snprintf(out, n, "%s%d%+dc", names[midi % 12], midi / 12 - 1, cents);
  else
    snprintf(out, n, "%s%d", names[midi % 12], midi / 12 - 1);
}

static void rsRenderMain() {
  const uint16_t bg     = rgb565(4, 5, 8);
  const uint16_t dim    = rgb565(128, 136, 148);
  const uint16_t accent = rgb565(255, 164, 58);
  const uint16_t wall   = rgb565(52, 61, 76);

  rsWorld.fillSnapshot(rsSnap);
  displayClear(bg);

  for (int i = 0; i < rsSnap.fieldCount; i++) {
    const auto& f = rsSnap.fields[(size_t)i];
    uint16_t c = rsKindColor(f.kind);
    displayFillCircleAlpha((int)lroundf(f.pos.x), (int)lroundf(f.pos.y),
                           (int)lroundf(f.radius), c,
                           (uint8_t)constrain((int)(f.strength * 55.0f), 0, 90));
  }

  for (int i = 0; i < rsSnap.waveCount; i++) {
    const auto& w = rsSnap.waves[(size_t)i];
    uint16_t c = rsScaleColor(rsKindColor(w.kind), w.amp * 0.75f);
    displayDrawCircle((int)lroundf(w.pos.x), (int)lroundf(w.pos.y),
                      (int)lroundf(w.radius), c, 1);
  }

  for (int i = 0; i < rsSnap.linkCount; i++) {
    const auto& l = rsSnap.links[(size_t)i];
    displayDrawLine((int)lroundf(l.a.x), (int)lroundf(l.a.y),
                    (int)lroundf(l.b.x), (int)lroundf(l.b.y),
                    rsScaleColor(rgb565(150, 210, 255), 0.25f + 0.5f * l.amount), 1);
  }

  for (int i = 0; i < rsSnap.segmentCount; i++) {
    const auto& s = rsSnap.segments[(size_t)i];
    uint16_t c = wall;
    if (s.hasProp && s.strength > 0.02f)
      c = rsMixColor(wall, rsKindColor(s.kind), s.strength);
    displayDrawLine((int)lroundf(s.a.x), (int)lroundf(s.a.y),
                    (int)lroundf(s.b.x), (int)lroundf(s.b.y), c, 1);
  }

  for (int i = 0; i < rsSnap.boosterCount; i++) {
    const auto& b = rsSnap.boosters[(size_t)i];
    float pulse = 0.5f + 0.5f * sinf(rsSnap.elapsed * 4.0f + b.phase);
    uint16_t c = b.cooldown > 0.0f ? rgb565(150, 140, 105) : rgb565(255, 220, 90);
    int br = (int)lroundf(b.radius);
    displayDrawCircle((int)lroundf(b.pos.x), (int)lroundf(b.pos.y),
                      br + 1 + (int)(pulse * 2.0f), c, 1);
    for (int k = 0; k < 4; k++) {
      float ang = rsSnap.elapsed * 0.8f + (float)k * 1.5707963f;
      int x0 = (int)lroundf(b.pos.x + cosf(ang) * (b.radius + 4.0f));
      int y0 = (int)lroundf(b.pos.y + sinf(ang) * (b.radius + 4.0f));
      int x1 = (int)lroundf(b.pos.x + cosf(ang) * (b.radius + 10.0f + pulse * 3.0f));
      int y1 = (int)lroundf(b.pos.y + sinf(ang) * (b.radius + 10.0f + pulse * 3.0f));
      displayDrawLine(x0, y0, x1, y1, c, 1);
    }
  }

  for (int i = 0; i < rsSnap.ballCount; i++) {
    const auto& b = rsSnap.balls[(size_t)i];
    uint16_t c = rsKindColor(b.kind);
    int br = (int)lroundf(b.radius);
    if (br < 1) br = 1;
    float flash = fminf(1.0f, fmaxf(0.0f, b.flash / 0.22f));
    int cx = (int)lroundf(b.pos.x);
    int cy = (int)lroundf(b.pos.y);
    int glowR = (int)(b.radius * (1.35f + 0.25f * flash));
    uint8_t glowA = (uint8_t)(40.0f + 110.0f * flash);
    displayFillCircleAlpha(cx, cy, glowR, c, glowA);
    displayFillCircle(cx, cy, br,
                      b.flash > 0.0f ? rsMixColor(c, rgb565(255, 255, 255), 0.35f + 0.45f * flash) : c);
    displayDrawCircle(cx, cy, br, rgb565(230, 235, 240), 1);
  }

  const int bottomY = (int)World::fieldH;
  displayFillRect(0, bottomY, 240, 240 - bottomY, rgb565(5, 7, 10));
  displayFillRect(0, bottomY, 240, 1, rgb565(38, 44, 54));

  char l[40];
  snprintf(l, sizeof(l), "KNOB %s", RB_NAMES[rsBind]);
  displayText(2, bottomY + 9, l, accent, 1);

  auto rightText = [](int y, const char* s, uint16_t c) {
    int x = 238 - (int)strlen(s) * 6;
    if (x < 2) x = 2;
    displayText(x, y, s, c, 1);
  };

  char note[12];
  rsNoteText(note, sizeof(note));
  snprintf(l, sizeof(l), "OUT %s", note);
  rightText(bottomY + 1, l, accent);

  bool trigShow = rsTrigOn || (rsTrigSeenMs != 0 && millis() - rsTrigSeenMs < 120u);
  char g[10], t[10];
  snprintf(g, sizeof(g), "GATE %s", rsGateOn ? "ON" : "OFF");
  snprintf(t, sizeof(t), "TRIG %s", trigShow ? "ON" : "OFF");
  int gw = (int)strlen(g) * 6, tw = (int)strlen(t) * 6, gap = 6;
  int gx = 238 - gw - gap - tw;
  if (gx < 2) gx = 2;
  displayText(gx, bottomY + 9, g, rsGateOn ? rgb565(80, 220, 120) : dim, 1);
  displayText(gx + gw + gap, bottomY + 9, t, trigShow ? rgb565(255, 164, 58) : dim, 1);

  displayFlush();
}

static void toggleClockMode() {
  if (clockMode) clockStop(); else clockStart();
  uiBuildItems();
}

static void toggleRandomMode() {
  if (!randomMode) {
    prevClockMode = clockMode;
    prevClockRunning = clockRunning;
    if (clockMode) clockStop();
    for (int v = 0; v < NVOICE; v++) {
      noteCnt[v] = 0;
      gate[v] = false;
      dacWriteUpdateRaw(dacCh[v], 0);
      digitalWrite(gatePin[v], LOW);
    }
    rsClearOutputs();
    randomMode = true;
    randomSettings = false;
    rsInit();
    randomDirty = true;
  } else {
    randomMode = false;
    randomSettings = false;
    rsClearOutputs();
    for (int v = 0; v < NVOICE; v++) {
      noteCnt[v] = 0;
      gate[v] = false;
      dacWriteUpdateRaw(dacCh[v], 0);
      digitalWrite(gatePin[v], LOW);
    }
    if (prevClockMode) {
      clockStart();
      if (prevClockRunning) clockSetRunning(true);
    } else {
      clockStop();
    }
    uiBuildItems();
    uiDirty = true;
  }
}

static void uiListFrame() {
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

static bool uiReadPress() {
  static uint8_t sLast = 0, sStable = 0;
  static uint32_t tChange = 0;
  uint8_t s = (digitalRead(ENC_SW) == LOW) ? 1 : 0;
  if (s != sLast) { sLast = s; tChange = millis(); }
  if (millis() - tChange >= 15 && s != sStable) {
    sStable = s;
    if (s == 0) return true;
  }
  return false;
}

static bool uiReadButton(uint8_t pin) {
  static uint8_t last[32] = { 1 };
  static uint32_t change[32] = { 0 };
  static uint8_t stable[32] = { 1 };
  if (pin >= 32) return false;
  uint8_t m = (digitalRead(pin) == LOW) ? 0 : 1;
  if (m != last[pin]) { last[pin] = m; change[pin] = millis(); }
  if (millis() - change[pin] >= 20 && m != stable[pin]) {
    stable[pin] = m;
    if (m == 0) return true;
  }
  return false;
}

// IO24: short press = MIDI/CLOCK, long press (>=800ms) = RANDOM.
// IO23 still works as the dedicated RANDOM button when the pad is connected.
static uint8_t uiReadModeButton() {
  static uint8_t last = 1, stable = 1;
  static uint32_t changeMs = 0, downMs = 0;
  static bool down = false;
  uint8_t raw = (digitalRead(BTN_MODE) == LOW) ? 0 : 1;
  if (raw != last) { last = raw; changeMs = millis(); }
  if (millis() - changeMs < 20 || raw == stable) return 0;
  stable = raw;
  if (stable == 0) {
    down = true;
    downMs = millis();
    return 0;
  }
  if (down) {
    down = false;
    return (millis() - downMs >= 800u) ? 2 : 1;
  }
  return 0;
}

static void uiProcess() {
  int16_t delta;
  noInterrupts();
  delta = encDelta;
  encDelta = 0;
  interrupts();

  bool pressed = uiReadPress();
  uint8_t modeEvent = uiReadModeButton();
  bool randomPressed = uiReadButton(BTN_RANDOM);

  if (randomPressed || modeEvent == 2) {
    toggleRandomMode();
    return;
  }
  if (modeEvent == 1 && !randomMode) {
    toggleClockMode();
    return;
  }

  if (randomMode) {
    if (randomSettings) {
      if (delta) uiHandleRotation(delta);
      if (pressed) uiHandlePress();
      uiListFrame();
    } else {
      if (delta) rsAdjustBound(delta);
      if (pressed) {
        randomSettings = true;
        uiEdit = false;
        uiBuildItems();
        uiDirty = true;
      }
    }
    return;
  }

  if (delta) uiHandleRotation(delta);
  if (pressed) uiHandlePress();
  uiListFrame();
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
  rsInit();

  pinMode(ENC_A, INPUT_PULLUP); pinMode(ENC_B, INPUT_PULLUP); pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BTN_RANDOM, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encISR, CHANGE);
  for (int v = 0; v < NVOICE; v++) { pinMode(gatePin[v], OUTPUT); digitalWrite(gatePin[v], LOW); }

  dacInit();
  updateOutputs();
  displayInit();
  displaySetSclkForSys(gSysKhz >= 400000u ? 52.5f : 62.5f, gSysKhz);

  uiBuildItems();
}

void loop() {
  watchdog_update();
  midiTask();
  uiProcess();

  if (randomMode) {
    rsTick();
    if (!randomSettings) {
      if (randomDirty || millis() - rsLastDraw >= 33u) {
        rsLastDraw = millis();
        randomDirty = false;
        rsRenderMain();
      }
    }
  } else if (gDirty) {
    gDirty = false;
    updateOutputs();
  }

  if (cfgDirty && millis() - cfgMs > 800) { cfgDirty = false; cfgSave(); }
}
























