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
// * Output mapping (group = one voice = 1 CV line + 1 Gate line; group number follows the physical jack pairs):
//   V1 CV=DAC B(1) Gate=GP22 | V2 CV=DAC D(3) Gate=GP21 | V3 CV=DAC F(5) Gate=GP20 | V4 CV=DAC H(7) Gate=GP19
//   V5 CV=DAC A(0) Gate=GP15 | V6 CV=DAC C(2) Gate=GP16 | V7 CV=DAC E(4) Gate=GP17 | V8 CV=DAC G(6) Gate=GP18
//   Note: DAC channel order does not match jack order (PCB routing), this table is the only reference.
//   Default MIDI channels 1..8, 1V/oct, Gate active high.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <EEPROM.h>

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
#define MENU_ITEMS     10        // 8 channel items + V/oct + Gate polarity
#define ENC_TRANS_PER_STEP 2     // encoder transitions per detent (15 pulses/30 detents=2; 20 pulses/20 detents=4)

#define MODE_RUN   0
#define MODE_CFG   1

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
static uint8_t mode      = MODE_RUN;
static uint8_t uiItem    = 0;                          // current config item 0..9
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
  Cfg c; EEPROM.get(0, c);
  if (c.magic != 0x5A) {
    for (int v = 0; v < NVOICE; v++) c.ch[v] = v + 1;
    c.voct = 1; c.ginv = 0; c.magic = 0x5A;
  }
  for (int v = 0; v < NVOICE; v++) chan[v] = (c.ch[v] >= 1 && c.ch[v] <= 16) ? c.ch[v] : (v + 1);
  voctN   = (c.voct == 1 || c.voct == 2 || c.voct == 5 || c.voct == 8) ? c.voct : 1;
  gateInv = c.ginv ? 1 : 0;
}
static void cfgSave() {
  Cfg c; c.magic = 0x5A;
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
    for (int v = 0; v < NVOICE; v++) {
      if (chan[v] != midiCh) continue;
      if (type == 0x90 && pkt[3] > 0) midiNoteOn(v, pkt[2]);
      else if (type == 0x80 || (type == 0x90 && pkt[3] == 0)) midiNoteOff(v, pkt[2]);
    }
  }
}

// ---------------- Encoder ----------------
static int8_t encStep() {                        // rotation -> -1/+1
  static int8_t accum = 0;
  static const int8_t TBL[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
  };
  static uint8_t lastAB = 0;
  uint8_t ab = (uint8_t)((digitalRead(ENC_A) << 1) | digitalRead(ENC_B));
  if (ab != lastAB) {
    accum += TBL[(lastAB << 2) | ab];
    lastAB = ab;
    if (accum >= ENC_TRANS_PER_STEP)  { accum = 0; return 1; }
    if (accum <= -ENC_TRANS_PER_STEP) { accum = 0; return -1; }
  }
  return 0;
}
static void menuAdjust(int dir) {
  if (dir == 0) return;
  if (uiItem < NVOICE) {                              // channels 1..16
    int c = chan[uiItem] + dir;
    if (c < 1) c = 16; if (c > 16) c = 1;
    chan[uiItem] = (uint8_t)c;
  } else if (uiItem == NVOICE) {                      // V/oct 1/2/5/8
    static const uint8_t V[4] = { 1, 2, 5, 8 };
    int i = 0; while (V[i] != voctN && i < 3) i++;
    i = (i + dir + 4) % 4;
    voctN = V[i];
  } else {                                            // Gate polarity
    gateInv ^= 1;
  }
  cfgDirty = true; cfgMs = millis();
}
static void enterConfig() {
  for (int v = 0; v < NVOICE; v++) { noteCnt[v] = 0; gate[v] = false; }   // clear notes to avoid stuck gates
  mode = MODE_CFG; uiItem = 0; gDirty = true;
}
static void exitConfig() {
  mode = MODE_RUN; cfgSave(); cfgDirty = false; gDirty = true;
}
static void encTask() {
  static uint8_t sLast = 0, sStable = 0;
  static uint32_t tChange = 0, tPress = 0;
  static bool    longDone = false;
  uint32_t now = millis();

  int8_t step = encStep();
  if (step) menuAdjust(step);

  uint8_t s = (digitalRead(ENC_SW) == LOW) ? 1 : 0;   // pressed=1
  if (s != sLast) { sLast = s; tChange = now; }
  if ((now - tChange) >= 15 && s != sStable) {
    sStable = s;
    if (s == 1) { tPress = now; longDone = false; }
    else {                                            // released
      if (!longDone) {                                // short press
        if (mode == MODE_RUN) enterConfig();
        else uiItem = (uint8_t)((uiItem + 1) % MENU_ITEMS);
        gDirty = true;
      }
      longDone = false;
    }
  }
  if (sStable == 1 && !longDone && (now - tPress) >= 800) {   // long press: exit and save
    longDone = true;
    if (mode == MODE_CFG) exitConfig();
  }
}

// ---------------- UI (TODO: ST7789 OLED) ----------------
// The new board has no LED; the config display will be implemented once the OLED is connected.
// For now the serial port is used as temporary feedback.
static void uiTask() {
  static uint8_t lastMode = 0xFF, lastItem = 0xFF;
  if (mode != lastMode || uiItem != lastItem) {
    lastMode = mode; lastItem = uiItem;
    if (mode == MODE_CFG) {
      Serial.print(F("[CFG] item=")); Serial.print(uiItem);
      if (uiItem < NVOICE) { Serial.print(F(" ch=")); Serial.println(chan[uiItem]); }
      else if (uiItem == NVOICE) { Serial.print(F(" voct=")); Serial.println(voctN); }
      else { Serial.print(F(" gateInv=")); Serial.println(gateInv); }
    } else {
      Serial.println(F("[RUN]"));
    }
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
  for (int v = 0; v < NVOICE; v++) { pinMode(gatePin[v], OUTPUT); digitalWrite(gatePin[v], LOW); }

  dacInit();
  updateOutputs();
}

void loop() {
  midiTask();

  static uint32_t tPoll = 0;
  if (millis() - tPoll >= 1) { tPoll = millis(); encTask(); }

  if (gDirty) { gDirty = false; updateOutputs(); }
  uiTask();
  if (cfgDirty && millis() - cfgMs > 800) { cfgDirty = false; cfgSave(); }
}
