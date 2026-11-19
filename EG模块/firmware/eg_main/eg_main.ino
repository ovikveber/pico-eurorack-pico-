#include <Arduino.h>
#include <math.h>
#include <hardware/gpio.h>
#include <hardware/structs/sio.h>
#include <pico/time.h>
#include <pico/platform.h>

constexpr uint8_t PIN_TLC_SIN   = 0;
constexpr uint8_t PIN_TLC_SCLK  = 1;
constexpr uint8_t PIN_TLC_BLANK = 2;
constexpr uint8_t PIN_TLC_XLAT  = 3;

constexpr uint8_t PIN_B_SDI  = 6;
constexpr uint8_t PIN_B_CS   = 7;
constexpr uint8_t PIN_B_SCLK = 8;
constexpr uint8_t PIN_B_SDO  = 9;

constexpr uint8_t PIN_A_SDI  = 10;
constexpr uint8_t PIN_A_CS   = 11;
constexpr uint8_t PIN_A_SCLK = 12;
constexpr uint8_t PIN_A_SDO  = 13;

constexpr uint8_t PIN_DAC_LDAC = 17;
constexpr uint8_t PIN_DAC_SYNC = 18;
constexpr uint8_t PIN_DAC_SCLK = 19;
constexpr uint8_t PIN_DAC_DIN  = 20;

constexpr uint8_t PIN_MUX_S0 = 26;
constexpr uint8_t PIN_MUX_S1 = 27;
constexpr uint8_t PIN_MUX_S2 = 21;
constexpr uint8_t PIN_MUX_S3 = 22;
constexpr uint8_t PIN_MUX_ADC = A2;

constexpr uint8_t NUM_ENV = 8;
constexpr uint8_t NUM_LED = 24;

constexpr uint8_t DAC_CMD_WR_UPDATE = 0x3;

#define EG_DEBUG 1

constexpr float TICK_HZ = 6000.0f;
constexpr int64_t TICK_US = (int64_t)(1000000.0f / TICK_HZ + 0.5f);

constexpr uint8_t ADS_CONV_US   = 4;
constexpr uint8_t ADS_SETTLE_US = 14;
constexpr uint8_t ADS_BIT_NOPS  = 18;

constexpr float INPUT_FS_V = 5.0f;
constexpr float GATE_ON_V  = 0.40f * INPUT_FS_V;
constexpr float GATE_OFF_V = 0.20f * INPUT_FS_V;
constexpr float ADC_FS_V   = 10.24f;
constexpr float ADC_LSB_V  = (2.0f * ADC_FS_V) / 65536.0f;

constexpr uint16_t LED_GATE_MS   = 50;
constexpr uint16_t LED_RETRIG_MS = 50;
constexpr uint16_t LED_PERIOD_MS = 10;

constexpr bool SLIDER_INVERT = false;

constexpr float MIN_TIME = 0.0001f;
constexpr float MAX_TIME = 10.0f;
constexpr float LAMBDA_BASE = MAX_TIME / MIN_TIME;
constexpr float ATT_TARGET = 1.2f;

enum EnvType : uint8_t { ENV_ADSR = 0, ENV_AR = 1, ENV_DECAY = 2 };
enum EnvPhase : uint8_t { PH_IDLE = 0, PH_ATTACK = 1, PH_DECAY = 2, PH_SUSTAIN = 3, PH_RELEASE = 4 };

struct AdsBus { uint8_t sdi, cs, sclk, sdo; };

struct EnvParam {
  volatile float attackAlpha;
  volatile float decayAlpha;
  volatile float releaseAlpha;
  volatile float sustain;
};

struct EnvRuntime {
  EnvPhase phase;
  float level;
  uint8_t prevGate;
  uint32_t lastRetrig;
};

static EnvParam gParam[NUM_ENV];
static EnvRuntime gEnv[NUM_ENV];
static const EnvType ENV_TYPE[NUM_ENV] = { ENV_ADSR, ENV_ADSR, ENV_AR, ENV_AR, ENV_DECAY, ENV_DECAY, ENV_DECAY, ENV_DECAY };

static volatile uint8_t  gGateLevel[NUM_ENV];
static volatile uint32_t gRetrigCount[NUM_ENV];

static uint16_t gTlc[NUM_LED];
static uint16_t gDacCode[NUM_ENV];
static volatile uint32_t gTlcWrites = 0;
static volatile uint8_t gPinHi = 0;
static volatile uint8_t gPinLo = 0;
static volatile uint8_t gPinBlk = 0;
static repeating_timer_t gTickTimer;
static volatile uint16_t gDbgA[NUM_ENV];
static volatile uint16_t gDbgB[NUM_ENV];

static inline void pinW(uint8_t p, bool v) { if (v) sio_hw->gpio_set = (1u << p); else sio_hw->gpio_clr = (1u << p); }
static inline bool pinR(uint8_t p) { return (sio_hw->gpio_in >> p) & 1u; }
static inline void tinyDelay() { for (volatile int k = 0; k < 4; ++k) __nop(); }

static inline void dacPace() { for (volatile int k = 0; k < 6; ++k) __nop(); }

static inline void dacFrame(uint32_t w) {
  sio_hw->gpio_clr = 1u << PIN_DAC_SYNC;
  dacPace();
  for (int i = 31; i >= 0; --i) {
    if ((w >> i) & 1u) sio_hw->gpio_set = 1u << PIN_DAC_DIN; else sio_hw->gpio_clr = 1u << PIN_DAC_DIN;
    dacPace();
    sio_hw->gpio_set = 1u << PIN_DAC_SCLK;
    dacPace();
    sio_hw->gpio_clr = 1u << PIN_DAC_SCLK;
    dacPace();
  }
  sio_hw->gpio_set = 1u << PIN_DAC_SYNC;
  dacPace();
}

static inline void dacBuffer(uint8_t ch, uint16_t code) {
  dacFrame(((uint32_t)DAC_CMD_WR_UPDATE << 24) | ((uint32_t)(ch & 7) << 20) | ((uint32_t)code << 4));
}

static void dacAll(uint16_t code) {
  for (uint8_t c = 0; c < NUM_ENV; ++c) dacBuffer(c, code);
}

static inline void dacWriteAll() {
  for (uint8_t c = 0; c < NUM_ENV; ++c) dacBuffer(c, gDacCode[c]);
}

static void tlcWrite() {
  gTlcWrites = gTlcWrites + 1;
  digitalWrite(PIN_TLC_BLANK, HIGH);
  digitalWrite(PIN_TLC_XLAT, LOW);
  for (int c = NUM_LED - 1; c >= 0; --c) {
    uint16_t v = gTlc[c] & 0x0FFF;
    for (int b = 11; b >= 0; --b) {
      digitalWrite(PIN_TLC_SCLK, LOW);
      if ((v >> b) & 1u) digitalWrite(PIN_TLC_SIN, HIGH); else digitalWrite(PIN_TLC_SIN, LOW);
      delayMicroseconds(1);
      digitalWrite(PIN_TLC_SCLK, HIGH);
      delayMicroseconds(1);
    }
  }
  digitalWrite(PIN_TLC_SCLK, LOW);
  digitalWrite(PIN_TLC_XLAT, HIGH);
  delayMicroseconds(2);
  digitalWrite(PIN_TLC_XLAT, LOW);
  delayMicroseconds(1);
  digitalWrite(PIN_TLC_BLANK, LOW);
  delayMicroseconds(1);
}

static AdsBus ADSA = { PIN_A_SDI, PIN_A_CS, PIN_A_SCLK, PIN_A_SDO };
static AdsBus ADSB = { PIN_B_SDI, PIN_B_CS, PIN_B_SCLK, PIN_B_SDO };

static inline uint16_t adsFrame(const AdsBus &b, uint16_t cmd) {
  sio_hw->gpio_clr = 1u << b.cs;
  delayMicroseconds(ADS_CONV_US);
  for (int i = 15; i >= 0; --i) {
    if ((cmd >> i) & 1u) sio_hw->gpio_set = 1u << b.sdi; else sio_hw->gpio_clr = 1u << b.sdi;
    sio_hw->gpio_set = 1u << b.sclk;
    for (volatile int k = 0; k < ADS_BIT_NOPS; ++k) __nop();
    sio_hw->gpio_clr = 1u << b.sclk;
    for (volatile int k = 0; k < ADS_BIT_NOPS; ++k) __nop();
  }
  sio_hw->gpio_clr = 1u << b.sdi;
  uint16_t rx = 0;
  for (int i = 0; i < 16; ++i) {
    sio_hw->gpio_set = 1u << b.sclk;
    for (volatile int k = 0; k < ADS_BIT_NOPS; ++k) __nop();
    rx = (uint16_t)((rx << 1) | (uint16_t)pinR(b.sdo));
    sio_hw->gpio_clr = 1u << b.sclk;
    for (volatile int k = 0; k < ADS_BIT_NOPS; ++k) __nop();
  }
  sio_hw->gpio_set = 1u << b.cs;
  return rx;
}

static void adsScan(const AdsBus &b, uint16_t *out) {
  adsFrame(b, 0xC000);
  delayMicroseconds(ADS_SETTLE_US);
  for (uint8_t n = 0; n < 8; ++n) {
    uint16_t cmd = (uint16_t)(0xC000 | ((uint16_t)((n + 1) & 7) << 10));
    out[n] = adsFrame(b, cmd);
    delayMicroseconds(ADS_SETTLE_US);
  }
}

static inline float adsVolts(uint16_t code) {
  return (float)code * ADC_LSB_V - ADC_FS_V;
}

static void muxSelect(uint8_t ch) {
  pinW(PIN_MUX_S0, ch & 1);
  pinW(PIN_MUX_S1, (ch >> 1) & 1);
  pinW(PIN_MUX_S2, (ch >> 2) & 1);
  pinW(PIN_MUX_S3, (ch >> 3) & 1);
}

static uint16_t muxRead(uint8_t ch) {
  muxSelect(ch);
  delayMicroseconds(10);
  analogRead(PIN_MUX_ADC);
  delayMicroseconds(2);
  return (uint16_t)analogRead(PIN_MUX_ADC);
}

static inline float timeFromX(float x) { return MIN_TIME * powf(LAMBDA_BASE, x); }
static inline float alphaOf(float t) { return 1.0f - expf(-1.0f / (t * TICK_HZ)); }

static void applyControl(uint8_t idx, float x) {
  if (x < 0.0f) x = 0.0f;
  if (x > 1.0f) x = 1.0f;
  switch (idx) {
    case 0:  gParam[0].attackAlpha = alphaOf(timeFromX(x)); break;
    case 1:  gParam[0].decayAlpha  = alphaOf(timeFromX(x)); break;
    case 2:  gParam[0].sustain     = x; break;
    case 3:  gParam[0].releaseAlpha = alphaOf(timeFromX(x)); break;
    case 4:  gParam[1].attackAlpha = alphaOf(timeFromX(x)); break;
    case 5:  gParam[1].decayAlpha  = alphaOf(timeFromX(x)); break;
    case 6:  gParam[1].sustain     = x; break;
    case 7:  gParam[1].releaseAlpha = alphaOf(timeFromX(x)); break;
    case 8:  gParam[2].releaseAlpha = alphaOf(timeFromX(x)); break;
    case 9:  gParam[2].attackAlpha = alphaOf(timeFromX(x)); break;
    case 10: gParam[3].attackAlpha = alphaOf(timeFromX(x)); break;
    case 11: gParam[3].releaseAlpha = alphaOf(timeFromX(x)); break;
    case 12: gParam[4].decayAlpha  = alphaOf(timeFromX(x)); break;
    case 13: gParam[5].decayAlpha  = alphaOf(timeFromX(x)); break;
    case 14: gParam[6].decayAlpha  = alphaOf(timeFromX(x)); break;
    case 15: gParam[7].decayAlpha  = alphaOf(timeFromX(x)); break;
    default: break;
  }
}

static void __not_in_flash_func(envStep)(uint8_t i, bool gate, bool retrig) {
  EnvRuntime &e = gEnv[i];
  const EnvParam &p = gParam[i];
  EnvType t = ENV_TYPE[i];
  bool gateRise = (gate && !e.prevGate);
  e.prevGate = gate ? 1 : 0;

  if (t == ENV_DECAY) {
    if (gateRise || retrig) { e.level = 1.0f; e.phase = PH_DECAY; }
    if (e.phase == PH_DECAY) {
      e.level += (0.0f - e.level) * p.decayAlpha;
      if (e.level < 0.001f) { e.level = 0.0f; e.phase = PH_IDLE; }
    } else {
      e.level = 0.0f;
    }
    return;
  }

  if (t == ENV_AR) {
    if (retrig || gateRise) e.phase = PH_ATTACK;
    switch (e.phase) {
      case PH_IDLE: e.level = 0.0f; break;
      case PH_ATTACK:
        e.level += (ATT_TARGET - e.level) * p.attackAlpha;
        if (e.level >= 1.0f) { e.level = 1.0f; e.phase = PH_SUSTAIN; }
        break;
      case PH_SUSTAIN:
        e.level = 1.0f;
        if (!gate) e.phase = PH_RELEASE;
        break;
      case PH_RELEASE:
        e.level += (0.0f - e.level) * p.releaseAlpha;
        if (e.level < 0.001f) { e.level = 0.0f; e.phase = PH_IDLE; }
        break;
      default: e.phase = PH_IDLE; break;
    }
    return;
  }

  if (retrig || gateRise) e.phase = PH_ATTACK;
  switch (e.phase) {
    case PH_IDLE: e.level = 0.0f; break;
    case PH_ATTACK:
      e.level += (ATT_TARGET - e.level) * p.attackAlpha;
      if (e.level >= 1.0f) { e.level = 1.0f; e.phase = PH_DECAY; }
      break;
    case PH_DECAY:
      e.level += (p.sustain - e.level) * p.decayAlpha;
      if (e.level <= p.sustain + 0.001f) { e.level = p.sustain; e.phase = gate ? PH_SUSTAIN : PH_RELEASE; }
      break;
    case PH_SUSTAIN:
      e.level = p.sustain;
      if (!gate) e.phase = PH_RELEASE;
      break;
    case PH_RELEASE:
      e.level += (0.0f - e.level) * p.releaseAlpha;
      if (e.level < 0.001f) { e.level = 0.0f; e.phase = PH_IDLE; }
      break;
    default: e.phase = PH_IDLE; break;
  }
}

static void __not_in_flash_func(envTick)() {
  for (uint8_t i = 0; i < NUM_ENV; ++i) {
    uint32_t rc = gRetrigCount[i];
    bool retrig = (rc != gEnv[i].lastRetrig);
    if (retrig) gEnv[i].lastRetrig = rc;
    envStep(i, gGateLevel[i] != 0, retrig);
    float lv = gEnv[i].level;
    if (lv < 0.0f) lv = 0.0f;
    if (lv > 1.0f) lv = 1.0f;
    gDacCode[i] = (uint16_t)(lv * 65535.0f + 0.5f);
  }
  dacWriteAll();
}

static bool __not_in_flash_func(onTick)(repeating_timer_t *) {
  envTick();
  return true;
}

void setup() {
  pinMode(PIN_TLC_SIN, OUTPUT);   digitalWrite(PIN_TLC_SIN, LOW);
  pinMode(PIN_TLC_SCLK, OUTPUT);  digitalWrite(PIN_TLC_SCLK, LOW);
  pinMode(PIN_TLC_XLAT, OUTPUT);  digitalWrite(PIN_TLC_XLAT, LOW);
  pinMode(PIN_TLC_BLANK, OUTPUT); digitalWrite(PIN_TLC_BLANK, HIGH);

  digitalWrite(PIN_TLC_SIN, HIGH); digitalWrite(PIN_TLC_SCLK, HIGH); digitalWrite(PIN_TLC_XLAT, HIGH);
  delayMicroseconds(20);
  gPinHi = (uint8_t)((gpio_get(PIN_TLC_SIN) ? 1 : 0) | (gpio_get(PIN_TLC_SCLK) ? 2 : 0) | (gpio_get(PIN_TLC_XLAT) ? 4 : 0) | (gpio_get(PIN_TLC_BLANK) ? 8 : 0));
  digitalWrite(PIN_TLC_SIN, LOW); digitalWrite(PIN_TLC_SCLK, LOW); digitalWrite(PIN_TLC_XLAT, LOW);
  delayMicroseconds(20);
  gPinLo = (uint8_t)((gpio_get(PIN_TLC_SIN) ? 1 : 0) | (gpio_get(PIN_TLC_SCLK) ? 2 : 0) | (gpio_get(PIN_TLC_XLAT) ? 4 : 0) | (gpio_get(PIN_TLC_BLANK) ? 8 : 0));
  digitalWrite(PIN_TLC_BLANK, LOW);
  delayMicroseconds(20);
  gPinBlk = (uint8_t)(gpio_get(PIN_TLC_BLANK) ? 1 : 0);

  pinMode(PIN_DAC_LDAC, OUTPUT); digitalWrite(PIN_DAC_LDAC, HIGH);
  pinMode(PIN_DAC_SYNC, OUTPUT); digitalWrite(PIN_DAC_SYNC, HIGH);
  pinMode(PIN_DAC_SCLK, OUTPUT); digitalWrite(PIN_DAC_SCLK, LOW);
  pinMode(PIN_DAC_DIN, OUTPUT);  digitalWrite(PIN_DAC_DIN, LOW);

  for (uint8_t i = 0; i < NUM_ENV; ++i) {
    gParam[i].attackAlpha = alphaOf(0.005f);
    gParam[i].decayAlpha = alphaOf(0.2f);
    gParam[i].releaseAlpha = alphaOf(0.2f);
    gParam[i].sustain = 0.7f;
    gEnv[i].phase = PH_IDLE;
    gEnv[i].level = 0.0f;
    gEnv[i].prevGate = 0;
    gEnv[i].lastRetrig = 0;
    gGateLevel[i] = 0;
    gRetrigCount[i] = 0;
    gDacCode[i] = 0;
  }

  delay(20);
  dacFrame(0x08000001u);
  delay(10);
  dacAll(0);

  for (uint8_t i = 0; i < NUM_LED; ++i) gTlc[i] = 0;
  tlcWrite();
  digitalWrite(PIN_TLC_BLANK, LOW);

  add_repeating_timer_us(-TICK_US, onTick, nullptr, &gTickTimer);

#if EG_DEBUG
  Serial.begin(115200);
#endif
}

void loop() {
#if EG_DEBUG
  static uint32_t dbgT = 0;
  if (millis() - dbgT >= 500) {
    dbgT = millis();
    Serial.print("A:");
    for (uint8_t i = 0; i < NUM_ENV; ++i) { Serial.print(gDbgA[i]); Serial.print(','); }
    Serial.print(" B:");
    for (uint8_t i = 0; i < NUM_ENV; ++i) { Serial.print(gDbgB[i]); Serial.print(','); }
    Serial.print(" G:");
    for (uint8_t i = 0; i < NUM_ENV; ++i) { Serial.print(gGateLevel[i]); Serial.print(','); }
    Serial.print(" R:");
    for (uint8_t i = 0; i < NUM_ENV; ++i) { Serial.print(gRetrigCount[i]); Serial.print(','); }
    Serial.print(" W:");
    Serial.print(gTlcWrites);
    Serial.print(" PIN:");
    Serial.print(gPinHi);
    Serial.print('/');
    Serial.print(gPinLo);
    Serial.print(" BLK:");
    Serial.print(gPinBlk);
    Serial.print(" T0:");
    Serial.print(gTlc[0]);
    Serial.print(" T8:");
    Serial.print(gTlc[8]);
    Serial.print(" T16:");
    Serial.print(gTlc[16]);
    Serial.print(" E0:");
    Serial.print(gEnv[0].level);
    Serial.print(" S:");
    Serial.print((uint8_t)gpio_get(PIN_TLC_SIN));
    Serial.print((uint8_t)gpio_get(PIN_TLC_SCLK));
    Serial.print((uint8_t)gpio_get(PIN_TLC_BLANK));
    Serial.print((uint8_t)gpio_get(PIN_TLC_XLAT));
    Serial.println();
  }
  delay(50);
#else
  delay(1000);
#endif
}

static bool gGateOn[NUM_ENV];
static bool gRetrigOn[NUM_ENV];
static uint16_t gGateMs[NUM_ENV];
static uint16_t gRetrigMs[NUM_ENV];
static uint16_t gLastRaw[16];

void setup1() {
  pinMode(PIN_B_SDI, OUTPUT);  digitalWrite(PIN_B_SDI, LOW);
  pinMode(PIN_B_SCLK, OUTPUT); digitalWrite(PIN_B_SCLK, LOW);
  pinMode(PIN_B_CS, OUTPUT);   digitalWrite(PIN_B_CS, HIGH);
  pinMode(PIN_B_SDO, INPUT);

  pinMode(PIN_A_SDI, OUTPUT);  digitalWrite(PIN_A_SDI, LOW);
  pinMode(PIN_A_SCLK, OUTPUT); digitalWrite(PIN_A_SCLK, LOW);
  pinMode(PIN_A_CS, OUTPUT);   digitalWrite(PIN_A_CS, HIGH);
  pinMode(PIN_A_SDO, INPUT);

  pinMode(PIN_MUX_S0, OUTPUT);
  pinMode(PIN_MUX_S1, OUTPUT);
  pinMode(PIN_MUX_S2, OUTPUT);
  pinMode(PIN_MUX_S3, OUTPUT);
  pinMode(PIN_MUX_ADC, INPUT);
  analogReadResolution(12);

  for (uint8_t i = 0; i < NUM_ENV; ++i) {
    gGateOn[i] = false;
    gRetrigOn[i] = false;
    gGateMs[i] = 0;
    gRetrigMs[i] = 0;
  }
  for (uint8_t i = 0; i < 16; ++i) gLastRaw[i] = 0xFFFF;

  delay(5);
  adsFrame(ADSA, 0x8500u);
  adsFrame(ADSB, 0x8500u);
  delayMicroseconds(100);
}

void loop1() {
  static uint8_t muxCh = 0;
  static uint32_t lastLed = 0;
  static bool selfTestDone = false;
  uint16_t ra[NUM_ENV];
  uint16_t rb[NUM_ENV];

  if (!selfTestDone) {
    delay(200);
    for (int k = 0; k < 3; ++k) {
      for (uint8_t i = 0; i < NUM_LED; ++i) gTlc[i] = 4095;
      tlcWrite();
      delay(250);
      for (uint8_t i = 0; i < NUM_LED; ++i) gTlc[i] = 0;
      tlcWrite();
      delay(250);
    }
    for (uint8_t i = 0; i < NUM_LED; ++i) gTlc[i] = 0;
    tlcWrite();
    selfTestDone = true;
    return;
  }

  adsScan(ADSA, ra);
  adsScan(ADSB, rb);
  for (uint8_t i = 0; i < NUM_ENV; ++i) { gDbgA[i] = ra[i]; gDbgB[i] = rb[i]; }

  for (uint8_t i = 0; i < NUM_ENV; ++i) {
    float va = adsVolts(ra[i]);
    bool onA = gGateOn[i] ? (va > GATE_OFF_V) : (va > GATE_ON_V);
    if (onA && !gGateOn[i]) gGateMs[i] = LED_GATE_MS;
    gGateOn[i] = onA;
    gGateLevel[i] = onA ? 1 : 0;

    float vb = adsVolts(rb[i]);
    bool onB = gRetrigOn[i] ? (vb > GATE_OFF_V) : (vb > GATE_ON_V);
    if (onB && !gRetrigOn[i]) {
      gRetrigCount[i] = gRetrigCount[i] + 1;
      gRetrigMs[i] = LED_RETRIG_MS;
    }
    gRetrigOn[i] = onB;
  }

  uint16_t raw = muxRead(muxCh);
  int diff = (int)raw - (int)gLastRaw[muxCh];
  if (diff < 0) diff = -diff;
  if (diff > 6 || gLastRaw[muxCh] == 0xFFFF) {
    float x = (float)raw / 4095.0f;
    if (muxCh < 8 && SLIDER_INVERT) x = 1.0f - x;
    applyControl(muxCh, x);
    gLastRaw[muxCh] = raw;
  }
  muxCh = (muxCh + 1) & 15;

  uint32_t now = millis();
  if (now - lastLed >= LED_PERIOD_MS) {
    lastLed = now;
    for (uint8_t i = 0; i < NUM_ENV; ++i) {
      float lv = gEnv[i].level;
      if (lv < 0.0f) lv = 0.0f;
      if (lv > 1.0f) lv = 1.0f;
      gTlc[i] = (uint16_t)(lv * lv * 4095.0f + 0.5f);

      if (gGateMs[i] >= LED_PERIOD_MS) gGateMs[i] -= LED_PERIOD_MS; else gGateMs[i] = 0;
      if (gRetrigMs[i] >= LED_PERIOD_MS) gRetrigMs[i] -= LED_PERIOD_MS; else gRetrigMs[i] = 0;

      gTlc[8 + i] = (gGateOn[i] || gGateMs[i] > 0) ? 4095 : 0;
      gTlc[16 + i] = (gRetrigMs[i] > 0) ? 4095 : 0;
    }
    tlcWrite();
  }
}
