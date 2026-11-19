#include <Arduino.h>

// DAC8568 诊断：8 路各输出不同已知档位，用万用表逐孔量，判断是
//  - 基准问题（全 0）
//  - 数据位错位（档位串/减半，例如满值只有 2.5V）
//  - 某几路硬件坏（某路恒 0 或不跟随）
// SCLK 放很慢(5us)，排除时序/速度因素。

constexpr uint8_t PIN_DAC_LDAC = 17;
constexpr uint8_t PIN_DAC_SYNC = 18;
constexpr uint8_t PIN_DAC_SCLK = 19;
constexpr uint8_t PIN_DAC_DIN  = 20;

constexpr uint32_t CMD_REF_ON    = 0x08000001u;
constexpr uint32_t CMD_WR_UPDATE = 0x3u;

// A,B,C,D,E,F,G,H 的已知代码（约 0, 满, 3/4, 1/2, 1/4, 1/8, 1/16, 1/32）
static const uint16_t CODES[8] = { 0, 65535, 49152, 32768, 16384, 8192, 4096, 2048 };

static void dacSend(uint32_t w) {
  digitalWrite(PIN_DAC_SYNC, LOW);
  delayMicroseconds(5);
  for (int i = 31; i >= 0; --i) {
    digitalWrite(PIN_DAC_DIN, (w >> i) & 1u);
    delayMicroseconds(5);
    digitalWrite(PIN_DAC_SCLK, HIGH);
    delayMicroseconds(5);
    digitalWrite(PIN_DAC_SCLK, LOW);
    delayMicroseconds(5);
  }
  delayMicroseconds(5);
  digitalWrite(PIN_DAC_SYNC, HIGH);
  delayMicroseconds(5);
}

static void dacWriteCh(uint8_t ch, uint16_t code) {
  dacSend((CMD_WR_UPDATE << 24) | ((uint32_t)(ch & 7) << 20) | ((uint32_t)code << 4));
}

static void dacAll() {
  for (uint8_t c = 0; c < 8; ++c) dacWriteCh(c, CODES[c]);
}

void setup() {
  pinMode(PIN_DAC_LDAC, OUTPUT); digitalWrite(PIN_DAC_LDAC, HIGH);
  pinMode(PIN_DAC_SYNC, OUTPUT); digitalWrite(PIN_DAC_SYNC, HIGH);
  pinMode(PIN_DAC_SCLK, OUTPUT); digitalWrite(PIN_DAC_SCLK, LOW);
  pinMode(PIN_DAC_DIN,  OUTPUT); digitalWrite(PIN_DAC_DIN,  LOW);
  delay(20);
  dacSend(CMD_REF_ON);
  delay(50);
  dacAll();
}

void loop() {
  dacAll();
  dacSend(CMD_REF_ON);
  delay(200);
}
