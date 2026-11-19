#include <Arduino.h>

// ============================================================================
// led_follow_test —— 一个通道的「输出 LED + Gate(输入) LED」跟随该通道
//                     推子/电位器阻值（ADC 0..4095 -> TLC 12 位亮度）
//
// 另外每 10 秒打印一次 16 路控制的 min..max 扫幅，用来判断每个电位器
// 是否真的从头到尾（0..4095）都能动、底端是否接地。
//
// 默认：TEST_ENV=0（第 1 路 ADSR），TEST_MUX_CH=0（第 1 个推子 I0）。
// 改 TEST_MUX_CH 可换：0-7 推子，8-11 AR 旋钮，12-15 Decay 旋钮。
// ============================================================================

constexpr uint8_t PIN_TLC_SIN   = 0;
constexpr uint8_t PIN_TLC_SCLK  = 1;
constexpr uint8_t PIN_TLC_BLANK = 2;
constexpr uint8_t PIN_TLC_XLAT  = 3;

constexpr uint8_t PIN_MUX_S0 = 26;
constexpr uint8_t PIN_MUX_S1 = 27;
constexpr uint8_t PIN_MUX_S2 = 21;
constexpr uint8_t PIN_MUX_S3 = 22;
constexpr uint8_t PIN_MUX_ADC = A2;

constexpr uint8_t TEST_ENV    = 0;
constexpr uint8_t TEST_MUX_CH = 0;

constexpr uint8_t NUM_LED = 24;

static uint16_t gTlc[NUM_LED];
static uint16_t gRaw[16];
static uint16_t gMin[16];
static uint16_t gMax[16];

static void tlcWrite() {
  digitalWrite(PIN_TLC_BLANK, HIGH);
  digitalWrite(PIN_TLC_XLAT, LOW);
  for (int c = NUM_LED - 1; c >= 0; --c) {
    uint16_t v = gTlc[c] & 0x0FFF;
    for (int b = 11; b >= 0; --b) {
      digitalWrite(PIN_TLC_SCLK, LOW);
      digitalWrite(PIN_TLC_SIN, ((v >> b) & 1u) ? HIGH : LOW);
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

static void muxSelect(uint8_t ch) {
  digitalWrite(PIN_MUX_S0, ch & 1);
  digitalWrite(PIN_MUX_S1, (ch >> 1) & 1);
  digitalWrite(PIN_MUX_S2, (ch >> 2) & 1);
  digitalWrite(PIN_MUX_S3, (ch >> 3) & 1);
}

static uint16_t muxRead(uint8_t ch) {
  muxSelect(ch);
  delayMicroseconds(200);
  analogRead(PIN_MUX_ADC);
  delayMicroseconds(20);
  return (uint16_t)analogRead(PIN_MUX_ADC);
}

void setup() {
  pinMode(PIN_TLC_SIN, OUTPUT);   digitalWrite(PIN_TLC_SIN, LOW);
  pinMode(PIN_TLC_SCLK, OUTPUT);  digitalWrite(PIN_TLC_SCLK, LOW);
  pinMode(PIN_TLC_XLAT, OUTPUT);  digitalWrite(PIN_TLC_XLAT, LOW);
  pinMode(PIN_TLC_BLANK, OUTPUT); digitalWrite(PIN_TLC_BLANK, HIGH);

  pinMode(PIN_MUX_S0, OUTPUT);
  pinMode(PIN_MUX_S1, OUTPUT);
  pinMode(PIN_MUX_S2, OUTPUT);
  pinMode(PIN_MUX_S3, OUTPUT);
  pinMode(PIN_MUX_ADC, INPUT);
  analogReadResolution(12);

  for (uint8_t i = 0; i < NUM_LED; ++i) gTlc[i] = 0;
  for (uint8_t i = 0; i < 16; ++i) { gRaw[i] = 0; gMin[i] = 4095; gMax[i] = 0; }
  tlcWrite();

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) {}
  Serial.print("LED-FOLLOW: env="); Serial.print(TEST_ENV);
  Serial.print(" LEDs="); Serial.print(TEST_ENV + 1);
  Serial.print("&"); Serial.print(8 + TEST_ENV + 1);
  Serial.print(" muxCh="); Serial.println(TEST_MUX_CH);
  Serial.println("Sweep each control min->max; MM line every 10s shows min..max.");
}

void loop() {
  static uint32_t lastScan = 0, lastPrint = 0, lastMm = 0;
  uint32_t now = millis();

  if (now - lastScan >= 50) {
    lastScan = now;
    for (uint8_t ch = 0; ch < 16; ++ch) {
      uint16_t v = muxRead(ch);
      gRaw[ch] = v;
      if (v < gMin[ch]) gMin[ch] = v;
      if (v > gMax[ch]) gMax[ch] = v;
    }

    uint16_t b = gRaw[TEST_MUX_CH] & 0x0FFF;
    for (uint8_t i = 0; i < NUM_LED; ++i) gTlc[i] = 0;
    gTlc[TEST_ENV] = b;
    gTlc[8 + TEST_ENV] = b;
    tlcWrite();
  }

  if (now - lastPrint >= 500) {
    lastPrint = now;
    Serial.print("raw="); Serial.print(gRaw[TEST_MUX_CH]);
    Serial.print(" LED"); Serial.print(TEST_ENV + 1);
    Serial.print("&"); Serial.print(8 + TEST_ENV + 1);
    Serial.print("="); Serial.println(gRaw[TEST_MUX_CH] & 0x0FFF);
  }

  if (now - lastMm >= 10000) {
    lastMm = now;
    Serial.print("MM:");
    for (uint8_t ch = 0; ch < 16; ++ch) {
      Serial.print(' '); Serial.print(gMin[ch]);
      Serial.print('-'); Serial.print(gMax[ch]);
      gMin[ch] = 4095; gMax[ch] = 0;
    }
    Serial.println();
  }
}
