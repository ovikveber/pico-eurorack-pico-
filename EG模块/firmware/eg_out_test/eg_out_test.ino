#include <Arduino.h>

// 只测试 DAC8568 -> 运放 -> 8 路音频输出（满值 = DAC 5V -> 运放 x2 = 10V）。
// 不驱动 TLC5947/LED，不驱动 ADS8688/Gate，不驱动 knobbus。
//
// 关键：用命令 0b0011 = "写输入寄存器并立即更新对应 DAC 寄存器"，
// 不依赖 LDAC 脉冲（原来 LDAC 脉宽太短导致部分通道不更新）。
// 每个 SCLK 边沿加延时，保证帧间/位间满足 DAC8568 时序。

constexpr uint8_t PIN_DAC_LDAC = 17;
constexpr uint8_t PIN_DAC_SYNC = 18;
constexpr uint8_t PIN_DAC_SCLK = 19;
constexpr uint8_t PIN_DAC_DIN  = 20;

constexpr uint32_t CMD_SW_RESET   = 0x07000000u;  // 软件复位
constexpr uint32_t CMD_REF_ON     = 0x08000001u;  // 内部基准使能（静态模式）
constexpr uint32_t CMD_WR_UPDATE  = 0x3u;         // 写输入寄存器并更新该通道
constexpr uint16_t FULL_SCALE     = 65535u;       // 16 位满值

static void dacSend(uint32_t w) {
  digitalWrite(PIN_DAC_SYNC, LOW);
  delayMicroseconds(1);
  for (int i = 31; i >= 0; --i) {
    digitalWrite(PIN_DAC_DIN, (w >> i) & 1u);
    delayMicroseconds(1);
    digitalWrite(PIN_DAC_SCLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(PIN_DAC_SCLK, LOW);
    delayMicroseconds(1);
  }
  delayMicroseconds(1);
  digitalWrite(PIN_DAC_SYNC, HIGH);
  delayMicroseconds(3);
}

static void dacWriteCh(uint8_t ch, uint16_t code) {
  dacSend((CMD_WR_UPDATE << 24) | ((uint32_t)(ch & 7) << 20) | ((uint32_t)code << 4));
}

static void dacAll(uint16_t code) {
  for (uint8_t c = 0; c < 8; ++c) dacWriteCh(c, code);
}

void setup() {
  pinMode(PIN_DAC_LDAC, OUTPUT); digitalWrite(PIN_DAC_LDAC, HIGH); // 保持高，靠命令自更新
  pinMode(PIN_DAC_SYNC, OUTPUT); digitalWrite(PIN_DAC_SYNC, HIGH);
  pinMode(PIN_DAC_SCLK, OUTPUT); digitalWrite(PIN_DAC_SCLK, LOW);
  pinMode(PIN_DAC_DIN,  OUTPUT); digitalWrite(PIN_DAC_DIN,  LOW);

  delay(20);
  dacSend(CMD_SW_RESET);   // 复位到已知状态
  delay(5);
  dacAll(0);               // 先全 0，避免上电"噗"
  delay(5);
  dacSend(CMD_REF_ON);     // 使能内部 2.5V 基准
  delay(50);               // 等基准电容稳定
  dacAll(FULL_SCALE);      // 8 路满值 -> DAC 5V -> 运放 10V
  dacSend(CMD_REF_ON);     // 再使能一次，确保生效
}

void loop() {
  static uint32_t tick = 0;
  tick++;
  dacAll(FULL_SCALE);
  if ((tick & 3) == 0) dacSend(CMD_REF_ON); // 周期性重新使能基准，防止被意外关断
  delay(100);
}
