// SPDX-License-Identifier: GPL-3.0-or-later
// midi2cv.ino - Pico (RP2040) USB-MIDI -> 8x CV/Gate, minimal single DAC8568 version
// FQBN: rp2040:rp2040:rpipico:usbstack=tinyusb
//
// Hardware: Pico + DAC8568 (8 channels, 16bit, internal 2.5V reference = 0-5V)
//          + 2x 74AHCT541 (U19 = SPI 3.3V->5V level shift, U20 = 8x gate buffer)
//          + EC11 rotary encoder + 1.3" ST7789 OLED (TFT1) + private digital bus connector (U17).
//          The old board's ADS8688/TM1638/ADR4550/second DAC/LED are all removed (new board has no LED).
//
// Pins (see the hardware docs folder, OV-MIDI wiring table):
//   DAC:  SCLK=GP28  DIN=GP27  SYNC(CHIPSELECT)=GP26
//   Gate: GP22 GP21 GP20 GP19 GP18 GP17 GP16 GP15  (through U20 Y1..Y8)
//   Enc : A=GP13  B=GP12  SW=GP11
//   Bus : GP14  (private digital bus, single wire, PIO UART still to be implemented)
//   OLED: SCL=GP10 SDA=GP9 RST=GP8 DC=GP7 CS=GP6 BL=GP5  (still to be implemented)
//
// DAC8568/AD5668 SPI frame = 32 bit, MSB first:
//   byte1 = cmd (low 4 bits) | byte2 = (addr<<4)|(data>>12) | byte3 = (data>>4)&0xFF | byte4 = (data<<4)&0xF0
//   cmd: 0x3=write input register and update  0x7=reset  0x8=internal reference setup (byte4=0x01 enable)  0x4=power up/down
//   addr: A..H = 0..7, All = 0xF
//   Power-on init: reset(cmd7) -> internal reference ON(cmd8, byte4=1).
//
// Bring-up mode: force MIDI ch1..8, disable encoder config, run a CV/gate self-test.
#define BRINGUP 0

// * Output mapping (group = one voice = 1 CV line + 1 Gate line; group number follows the physical jack pairs):
//   V1 CV=DAC B(1) Gate=GP22 | V2 CV=DAC D(3) Gate=GP21 | V3 CV=DAC F(5) Gate=GP20 | V4 CV=DAC H(7) Gate=GP19
//   V5 CV=DAC A(0) Gate=GP15 | V6 CV=DAC C(2) Gate=GP16 | V7 CV=DAC E(4) Gate=GP17 | V8 CV=DAC G(6) Gate=GP18
//   Note: DAC channel order does not match jack order (PCB routing), this table is the only reference.
//   Default MIDI channels 1..8, 1V/oct, Gate active high.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <EEPROM.h>
#include "Display.h"

Adafruit_USBD_MIDI usb_midi;

// ---------------- Pins ----------------
#define PIN_SCLK  28     // DAC SCLK  (level-shifted to 5V by U19)
#define PIN_DIN   27     // DAC DIN/SDI
#define PIN_SYNC  26     // DAC SYNC# (chip select, active low)
#define ENC_A     13     // encoder A  (internal pull-up, closes to GND)
#define ENC_B     12     // encoder B
#define ENC_SW    11     // encoder button (to GND)
// Hard-wired on the board: DAC ~LDAC -> GND, ~CLR -> 5V (high, inactive)

// Gate pin per voice (V1..V8), buffered by U20
static const uint8_t gatePin[8] = { 22, 21, 20, 19, 15, 16, 17, 18 };
// DAC channel for each CV (A..H = 0..7)
static const uint8_t dacCh[8]   = {  1,  3,  5,  7,  0,  2,  4,  6 };

// ---------------- Constants ----------------
#define NVOICE         8
#define NOTE0          24        // MIDI note that maps to 0V (C1)
#define CV_FULLSCALE_V 5.0f      // internal 2.5V reference x2 = 5.0V (exact, no calibration needed)
#define ENC_TRANS_PER_STEP 2     // encoder transitions per detent (15 pulses/30 detents=2; 20 pulses/20 detents=4)

// ---------------- AD5668/DAC8568 ----------------
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
static inline void dacWriteUpdate(uint8_t ch, uint16_t data) {   // cmd 0x3
  dacBytes(0x03, (uint8_t)((ch << 4) | (data >> 12)),
                 (uint8_t)(data >> 4),
                 (uint8_t)(data << 4));
}
static void dacInit() {
  pinMode(PIN_SCLK, OUTPUT); pinMode(PIN_DIN, OUTPUT); pinMode(PIN_SYNC, OUTPUT);
  digitalWrite(PIN_SCLK, LOW); digitalWrite(PIN_DIN, LOW); digitalWrite(PIN_SYNC, HIGH);
  dacBytes(0x07, 0x00, 0x00, 0x00);   // reset (power-on reset)
  dacBytes(0x08, 0x00, 0x00, 0x01);   // internal reference ON
}

// ---------------- State ----------------
static uint8_t chan[NVOICE]   = { 1, 2, 3, 4, 5, 6, 7, 8 };  // MIDI channel per voice
static uint8_t voctN     = 1;                          // 1/2/5/8 V/oct
static uint8_t gateInv   = 0;                          // 0=normal (high when note on) 1=inverted
static bool    gate[NVOICE]   = { false };
static uint8_t curNote[NVOICE]= { NOTE0 };
static uint8_t noteStack[NVOICE][16];
static uint8_t noteCnt[NVOICE] = { 0 };
static volatile bool gDirty = true;

// ---- Settings persistence (EEPROM/flash emulation) ----
struct Cfg { uint8_t magic; uint8_t ch[NVOICE]; uint8_t voct; uint8_t ginv; };
static bool     cfgDirty = false;
static uint32_t cfgMs = 0;
static void cfgLoad() {
#if BRINGUP
  for (int v = 0; v < NVOICE; v++) chan[v] = v + 1;
  voctN = 1;
  gateInv = 0;
  return;
#endif
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

// ---------------- Output ----------------
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
#if BRINGUP
    if (Serial.availableForWrite() >= 48) {
      Serial.print("MIDI st="); Serial.print(status, HEX);
      Serial.print(" d1="); Serial.print(pkt[2]);
      Serial.print(" d2="); Serial.println(pkt[3]);
    }
#endif
    for (int v = 0; v < NVOICE; v++) {
      if (chan[v] != midiCh) continue;
      if (type == 0x90 && pkt[3] > 0) midiNoteOn(v, pkt[2]);
      else if (type == 0x80 || (type == 0x90 && pkt[3] == 0)) midiNoteOff(v, pkt[2]);
    }
  }
}

// ---------------- Encoder (interrupt driven) ----------------
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
static bool uiEdit = false;
static int  uiSel  = 0;
static bool uiDirty = true;

static void uiHandleRotation(int16_t delta) {
  if (delta == 0) return;
  if (!uiEdit) {
    uiSel = (uiSel + delta) % NVOICE;
    if (uiSel < 0) uiSel += NVOICE;
  } else {
    int c = (int)chan[uiSel] + delta;
    while (c < 1) c += 16;
    while (c > 16) c -= 16;
    chan[uiSel] = (uint8_t)c;
    cfgDirty = true; cfgMs = millis();
  }
  uiDirty = true;
}

static void uiHandlePress() {
  if (!uiEdit) {
    uiEdit = true;
  } else {
    uiEdit = false;
    cfgSave();
    cfgDirty = false;
  }
  uiDirty = true;
}

static void uiDraw() {
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

  for (int i = 0; i < NVOICE; i++) {
    int y = 18 + i * 27;
    bool selected = (i == uiSel);
    if (selected) {
      if (uiEdit) displayFillRect(4, y - 1, 232, 25, rgb565(22, 36, 54));
      displayDrawRect(3, y - 1, 234, 25, uiEdit ? accent : selCol, 2);
    }
    snprintf(buf, sizeof(buf), "V%d", i + 1);
    displayText(12, y + 7, buf, selected ? fg : dim, 1);
    snprintf(buf, sizeof(buf), "CH %d", chan[i]);
    displayText(60, y + 5, buf, selected ? fg : rgb565(180, 186, 196), 2);
    int barW = (int)chan[i] * 6;
    displayFillRect(232 - barW, y + 9, barW, 6,
                    selected ? (uiEdit ? accent : selCol) : rgb565(50, 58, 72));
  }
  displayFlush();
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

  if (uiDirty) {
    uiDirty = false;
    uiDraw();
  }
}

// ---------------- setup / loop ----------------
void setup() {
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);
  TinyUSBDevice.setManufacturerDescriptor("OV");
  TinyUSBDevice.setProductDescriptor("OV-MIDI");
  TinyUSBDevice.setSerialDescriptor("OVMIDI0001");   // fixed serial number to avoid ghost devices on Windows/FL
  usb_midi.setStringDescriptor("OV-MIDI");
  usb_midi.begin();
  { uint32_t t0 = millis(); while (!TinyUSBDevice.mounted() && millis() - t0 < 1500) delay(5); }

  Serial.begin(115200);
  EEPROM.begin(sizeof(Cfg)); cfgLoad();

  pinMode(ENC_A, INPUT_PULLUP); pinMode(ENC_B, INPUT_PULLUP); pinMode(ENC_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encISR, CHANGE);
  for (int v = 0; v < NVOICE; v++) { pinMode(gatePin[v], OUTPUT); digitalWrite(gatePin[v], LOW); }

  dacInit();
  updateOutputs();
  displayInit();

#if BRINGUP
  // Groups 1..4 -> 1V, 2V, 3V, 4V and all four gates high for 8 s.
  for (int v = 0; v < 4; v++) {
    curNote[v] = NOTE0 + 12 * (v + 1);
    gate[v] = true;
  }
  updateOutputs();
  if (Serial.availableForWrite() >= 100) {
    Serial.print("SELFTEST V1=1V V2=2V V3=3V V4=4V gates HIGH 8s; ch=");
    for (int v = 0; v < NVOICE; v++) { Serial.print(chan[v]); Serial.print(' '); }
    Serial.println();
  }
  delay(8000);
  for (int v = 0; v < 4; v++) {
    curNote[v] = NOTE0;
    gate[v] = false;
  }
  updateOutputs();
#endif
}

void loop() {
  midiTask();

  if (gDirty) { gDirty = false; updateOutputs(); }
  uiProcess();
  if (cfgDirty && millis() - cfgMs > 800) { cfgDirty = false; cfgSave(); }
}
