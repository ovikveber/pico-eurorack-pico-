#include <Arduino.h>
#include <hardware/gpio.h>

// ============================================================================
// mux_diag —— 判断 MUX/电位器节点到底是「悬空」还是「没接对」
//
// 思路：悬空的高阻节点，读数会随「切换顺序」和「等待时间」变化；
//       而真正由一个电位器驱动的节点（低阻源）不会。
// 所以打印三组：
//   FWD  正序扫（各等 200us）
//   REV  反序扫（各等 200us）
//   SLOW 正序扫（各等 5ms）
// 如果 FWD/REV/SLOW 对同一通道差别很大 => 节点悬空/串扰，不是代码问题。
// 另外打印 A2 宏的真实值、各选择脚的 GPIO 回读，排除引脚映射错误。
// ============================================================================

constexpr uint8_t PIN_MUX_S0 = 26;
constexpr uint8_t PIN_MUX_S1 = 27;
constexpr uint8_t PIN_MUX_S2 = 21;
constexpr uint8_t PIN_MUX_S3 = 22;
constexpr uint8_t PIN_ADC    = A2;

static void muxSelect(uint8_t ch) {
  digitalWrite(PIN_MUX_S0, ch & 1);
  digitalWrite(PIN_MUX_S1, (ch >> 1) & 1);
  digitalWrite(PIN_MUX_S2, (ch >> 2) & 1);
  digitalWrite(PIN_MUX_S3, (ch >> 3) & 1);
}

static uint16_t readCh(uint8_t ch, uint16_t settleUs) {
  muxSelect(ch);
  delayMicroseconds(settleUs);
  analogRead(PIN_ADC);
  delayMicroseconds(settleUs);
  return (uint16_t)analogRead(PIN_ADC);
}

static void printScan(const char *tag, bool rev, uint16_t settleUs) {
  Serial.print(tag);
  Serial.print(":");
  if (!rev) {
    for (uint8_t ch = 0; ch < 16; ++ch) { Serial.print(' '); Serial.print(readCh(ch, settleUs)); }
  } else {
    for (int ch = 15; ch >= 0; --ch) { Serial.print(' '); Serial.print(readCh((uint8_t)ch, settleUs)); }
  }
  Serial.println();
}

void setup() {
  pinMode(PIN_MUX_S0, OUTPUT);
  pinMode(PIN_MUX_S1, OUTPUT);
  pinMode(PIN_MUX_S2, OUTPUT);
  pinMode(PIN_MUX_S3, OUTPUT);
  pinMode(PIN_ADC, INPUT);
  analogReadResolution(12);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) {}

  Serial.print("MUX-DIAG v1  A2="); Serial.println((int)A2);
  Serial.print("S pins: S0=GP"); Serial.print(PIN_MUX_S0);
  Serial.print(" S1=GP"); Serial.print(PIN_MUX_S1);
  Serial.print(" S2=GP"); Serial.print(PIN_MUX_S2);
  Serial.print(" S3=GP"); Serial.print(PIN_MUX_S3);
  Serial.print("  ADC=GP"); Serial.println((int)PIN_ADC);

  // 回读选择脚：确认 Pico 自己有没有在驱动
  muxSelect(0);
  delayMicroseconds(50);
  Serial.print("readback ch0: ");
  Serial.print(gpio_get(PIN_MUX_S0)); Serial.print(gpio_get(PIN_MUX_S1));
  Serial.print(gpio_get(PIN_MUX_S2)); Serial.print(gpio_get(PIN_MUX_S3));
  Serial.println("  (expect 0000)");
  muxSelect(15);
  delayMicroseconds(50);
  Serial.print("readback ch15: ");
  Serial.print(gpio_get(PIN_MUX_S0)); Serial.print(gpio_get(PIN_MUX_S1));
  Serial.print(gpio_get(PIN_MUX_S2)); Serial.print(gpio_get(PIN_MUX_S3));
  Serial.println("  (expect 1111)");
}

void loop() {
  printScan("FWD ", false, 200);
  printScan("REV ", true, 200);
  printScan("SLOW", false, 5000);
  Serial.println();
  delay(1500);
}
