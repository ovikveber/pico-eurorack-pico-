#include <Arduino.h>

// ============================================================================
// test_final —— 8 路 CV 输出全部打满（DAC 5V -> 运放 x2 = 10V）
//
// 用途：万用表逐个量 8 个输出插孔，每路都应 ≈10V。
//
// 只驱动 DAC8568，不碰 TLC5947(LED)、ADS8688(Gate/Retrig)、knobbus。
// 命令 0b0011 = 写输入寄存器并立即更新对应 DAC 寄存器，不依赖 LDAC 脉冲。
// 每 bit / 每帧都有延时，慢速稳妥，排除时序问题。
//
// 注意：
//  - 需插 16pin 电源（USB 只供 Pico 3V3，不供 DAC 的 +5V 与运放 ±12V）。
//  - DAC8568 内部基准默认关闭，上电必须发 0x08000001 使能，否则无输出。
// ============================================================================

constexpr uint8_t PIN_DAC_LDAC = 17;
constexpr uint8_t PIN_DAC_SYNC = 18;
constexpr uint8_t PIN_DAC_SCLK = 19;
constexpr uint8_t PIN_DAC_DIN  = 20;

constexpr uint32_t CMD_SW_RESET  = 0x07000000u;  // 软件复位
constexpr uint32_t CMD_REF_ON    = 0x08000001u;  // 内部基准使能
constexpr uint8_t  CMD_WR_UPDATE = 0x3;          // 写输入寄存器 + 立即更新该通道
constexpr uint16_t FULL_SCALE    = 65535u;

static void dacSend(uint32_t w) {
  digitalWrite(PIN_DAC_SYNC, LOW);
  delayMicroseconds(2);
  for (int i = 31; i >= 0; --i) {
    digitalWrite(PIN_DAC_DIN, (w >> i) & 1u);
    delayMicroseconds(2);
    digitalWrite(PIN_DAC_SCLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(PIN_DAC_SCLK, LOW);
    delayMicroseconds(2);
  }
  delayMicroseconds(2);
  digitalWrite(PIN_DAC_SYNC, HIGH);
  delayMicroseconds(5);
}

static void dacWriteCh(uint8_t ch, uint16_t code) {
  dacSend(((uint32_t)CMD_WR_UPDATE << 24) | ((uint32_t)(ch & 7) << 20) | ((uint32_t)code << 4));
}

static void dacAllFull() {
  for (uint8_t c = 0; c < 8; ++c) dacWriteCh(c, FULL_SCALE);
}

void setup() {
  pinMode(PIN_DAC_LDAC, OUTPUT); digitalWrite(PIN_DAC_LDAC, HIGH);  // 靠命令自更新
  pinMode(PIN_DAC_SYNC, OUTPUT); digitalWrite(PIN_DAC_SYNC, HIGH);
  pinMode(PIN_DAC_SCLK, OUTPUT); digitalWrite(PIN_DAC_SCLK, LOW);
  pinMode(PIN_DAC_DIN,  OUTPUT); digitalWrite(PIN_DAC_DIN,  LOW);

  delay(20);
  dacSend(CMD_SW_RESET);   // 复位到已知状态
  delay(5);
  for (uint8_t c = 0; c < 8; ++c) dacWriteCh(c, 0);  // 先全 0，避免上电"噗"
  delay(5);
  dacSend(CMD_REF_ON);     // 使能内部 2.5V 基准
  delay(50);               // 等基准电容稳定
  dacAllFull();            // 8 路满值 -> DAC 5V -> 运放 10V
}

void loop() {
  static uint32_t tick = 0;
  tick++;
  dacAllFull();
  if ((tick & 7) == 0) dacSend(CMD_REF_ON);  // 周期性重申基准使能
  delay(200);
}
