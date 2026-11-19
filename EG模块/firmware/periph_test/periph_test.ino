#include <Arduino.h>
#include <hardware/gpio.h>
#include <hardware/structs/sio.h>

// ============================================================================
// periph_test —— 其余外设排查（TLC5947 / CD74HC4067），不碰 DAC / ADS
//
// TLC5947（写-only，无法回读，只能静态/间接判断）：
//   - 每 200ms 写一次 24 通道灰度（跑马+阶梯），XLAT 锁存，BLANK 保持使能(低)。
//   - 需要你万用表确认：pin32 VCC=3.3V、pin1/EP=0V、pin31 IREF≈1.2V、pin2 BLANK≈0V。
//   - 数据帧正确与否要等 LED 焊上才能肉眼验证；这里只保证时序/引脚动作。
//
// CD74HC4067（可验证）：
//   - 每 1s 扫 16 通道，打印 GP28(A2) 的 12 位 ADC 读数。
//   - 你拿一根 3V3（或 5V）的线点某个通道的输入焊盘（推子/旋钮的 wiper 脚 Ix），
//     对应通道读数应明显升高；松开回落 => 选通 + COMMON 链路正常。
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

constexpr uint8_t NUM_LED = 24;

static uint16_t gTlc[NUM_LED];

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
  digitalWrite(PIN_TLC_BLANK, LOW);  // 低 = 输出使能
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

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) {}

  Serial.println();
  Serial.println("PERIPH-TEST v1");
  Serial.println("TLC: check VCC=3.3V, IREF=1.2V, BLANK=0V with meter.");
  Serial.println("MUX: touch 3V3 wire to an Ix pad, watch that channel rise.");
  Serial.println("ch: raw12 (0..4095)");
}

void loop() {
  static uint32_t lastTlc = 0;
  static uint32_t lastMux = 0;
  static uint8_t step = 0;

  uint32_t now = millis();

  if (now - lastTlc >= 200) {
    lastTlc = now;
    step = (uint8_t)((step + 1) % NUM_LED);
    for (uint8_t i = 0; i < NUM_LED; ++i) {
      int d = (int)i - (int)step;
      if (d < 0) d += NUM_LED;
      gTlc[i] = (uint16_t)(d * 170);   // 0..3910 阶梯，随 step 移动
    }
    tlcWrite();
  }

  if (now - lastMux >= 1000) {
    lastMux = now;
    Serial.print("MUX:");
    for (uint8_t ch = 0; ch < 16; ++ch) {
      Serial.print(' ');
      Serial.print(muxRead(ch));
    }
    Serial.println();
  }
}
