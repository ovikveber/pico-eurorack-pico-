#include <Arduino.h>
#include <hardware/gpio.h>
#include <hardware/structs/sio.h>

// ============================================================================
// ads_comm_test —— 只测两片 ADS8688 的 SPI 通信，不碰 LED/电位器/推子/插座
//
// 判断依据（强弱递进）：
//   1) RST 后读 AUTO_SEQ_EN(addr 0x01)，默认应为 0xFF。读到 FF 说明 SDO 会动，
//      但对“SDO 被拉高”无区分力。
//   2) 写 AUTO_SEQ_EN = 0x5A，再读回，必须是 0x5A —— 只有双向通信正常才会出现，
//      能排除 SDO 恒 0 / 恒 1 / 虚焊。这是关键判据。
//   3) 恢复 0xFF，然后 MAN 方式读 8 个通道 + AUX（AUX 硬件接 GND -> 应 ≈0x8000）。
//
// 注意：ADS8688 的 AVDD(+5V) 来自 16pin 电源，DVDD(+3V3) 来自 Pico。
//       不插 16pin 时数字接口可能还能读寄存器，但转换值一定不对。
// ============================================================================

struct AdsBus { uint8_t sdi, cs, sclk, sdo; const char *name; };

static const AdsBus ADSA = { 10, 11, 12, 13, "ADCA/Gate" };
static const AdsBus ADSB = {  6,  7,  8,  9, "ADCB/Retrig" };

static inline void pinW(uint8_t p, bool v) {
  if (v) sio_hw->gpio_set = (1u << p); else sio_hw->gpio_clr = (1u << p);
}
static inline bool pinR(uint8_t p) { return (sio_hw->gpio_in >> p) & 1u; }

static void busInit(const AdsBus &b) {
  pinMode(b.sdi, OUTPUT);  digitalWrite(b.sdi, LOW);
  pinMode(b.sclk, OUTPUT); digitalWrite(b.sclk, LOW);
  pinMode(b.cs, OUTPUT);   digitalWrite(b.cs, HIGH);
  pinMode(b.sdo, INPUT);
}

// 32 时钟帧：前 16 位发命令，后 16 位收数据（数据是“上一条命令”的转换结果）
static uint16_t convFrame(const AdsBus &b, uint16_t cmd) {
  pinW(b.cs, false);
  delayMicroseconds(2);
  for (int i = 15; i >= 0; --i) {
    pinW(b.sdi, (cmd >> i) & 1u);
    delayMicroseconds(1);
    pinW(b.sclk, true);
    delayMicroseconds(1);
    pinW(b.sclk, false);
    delayMicroseconds(1);
  }
  pinW(b.sdi, false);
  uint16_t rx = 0;
  for (int i = 0; i < 16; ++i) {
    pinW(b.sclk, true);
    delayMicroseconds(1);
    rx = (uint16_t)((rx << 1) | (pinR(b.sdo) ? 1u : 0u));
    pinW(b.sclk, false);
    delayMicroseconds(1);
  }
  pinW(b.cs, true);
  delayMicroseconds(2);
  return rx;
}

// 24 时钟帧：程序寄存器读/写。命令字 = (addr<<9) | (写?0x100:0) | data
// 寄存器数据在最后 8 个时钟出现在 SDO（MSB first）。返回 24 位原始接收。
static uint32_t regFrame(const AdsBus &b, uint16_t cmd) {
  pinW(b.cs, false);
  delayMicroseconds(2);
  uint32_t rx = 0;
  for (int i = 15; i >= 0; --i) {
    pinW(b.sdi, (cmd >> i) & 1u);
    delayMicroseconds(1);
    pinW(b.sclk, true);
    delayMicroseconds(1);
    rx = (rx << 1) | (pinR(b.sdo) ? 1u : 0u);
    pinW(b.sclk, false);
    delayMicroseconds(1);
  }
  pinW(b.sdi, false);
  for (int i = 0; i < 8; ++i) {
    pinW(b.sclk, true);
    delayMicroseconds(1);
    rx = (rx << 1) | (pinR(b.sdo) ? 1u : 0u);
    pinW(b.sclk, false);
    delayMicroseconds(1);
  }
  pinW(b.cs, true);
  delayMicroseconds(2);
  return rx & 0xFFFFFFu;
}

static uint8_t regRead(const AdsBus &b, uint8_t addr) {
  uint32_t r = regFrame(b, (uint16_t)((uint16_t)addr << 9));  // bit8=0 -> 读
  return (uint8_t)(r & 0xFF);  // 最后 8 位
}

static uint8_t regWrite(const AdsBus &b, uint8_t addr, uint8_t val) {
  uint32_t r = regFrame(b, (uint16_t)(((uint16_t)addr << 9) | 0x100u | val));
  return (uint8_t)(r & 0xFF);  // 写入时最后 8 位回显写入值
}

static uint16_t adsRead(const AdsBus &b, uint8_t ch) {
  uint16_t cmd = (uint16_t)(0xC000u | ((uint16_t)ch << 10));
  convFrame(b, cmd);
  delayMicroseconds(5);
  return convFrame(b, cmd);
}

static uint16_t adsReadAux(const AdsBus &b) {
  convFrame(b, 0xE000u);
  delayMicroseconds(5);
  return convFrame(b, 0xE000u);
}

static void regDiag(const AdsBus &b) {
  uint8_t idle = pinR(b.sdo) ? 1 : 0;
  uint8_t a1 = regRead(b, 0x01);
  uint8_t wr = regWrite(b, 0x01, 0x5A);
  uint8_t rb = regRead(b, 0x01);
  regWrite(b, 0x01, 0xFF);
  Serial.print(b.name);
  Serial.print(": SDO_idle="); Serial.print(idle);
  Serial.print(" AUTO_SEQ=0x"); Serial.print(a1, HEX);
  Serial.print(" echo=0x"); Serial.print(wr, HEX);
  Serial.print(" rb=0x"); Serial.print(rb, HEX);
  Serial.println((rb == 0x5A) ? F("  REG-PASS") : F("  REG-FAIL"));
}

static void testBus(const AdsBus &b) {
  Serial.println();
  Serial.print("=== "); Serial.print(b.name);
  Serial.print("  SDI=GP"); Serial.print(b.sdi);
  Serial.print(" CS=GP"); Serial.print(b.cs);
  Serial.print(" SCLK=GP"); Serial.print(b.sclk);
  Serial.print(" SDO=GP"); Serial.println(b.sdo);

  convFrame(b, 0x8500u);      // RST
  delay(5);

  uint8_t a1 = regRead(b, 0x01);
  Serial.print("AUTO_SEQ_EN default = 0x"); Serial.println(a1, HEX);

  uint8_t wr = regWrite(b, 0x01, 0x5A);
  uint8_t rb = regRead(b, 0x01);
  Serial.print("write 0x5A -> echo 0x"); Serial.print(wr, HEX);
  Serial.print(", readback 0x"); Serial.print(rb, HEX);
  Serial.println((rb == 0x5A) ? "  [PASS]" : "  [FAIL]");

  regWrite(b, 0x01, 0xFF);    // 恢复默认
  delay(1);
  uint8_t a2 = regRead(b, 0x01);
  Serial.print("restored = 0x"); Serial.println(a2, HEX);

  uint8_t fs = regRead(b, 0x03);  // Feature Select，默认 0x00
  Serial.print("Feature Select = 0x"); Serial.println(fs, HEX);

  Serial.print("CH0..7:");
  for (uint8_t n = 0; n < 8; ++n) {
    Serial.print(' '); Serial.print(adsRead(b, n));
  }
  Serial.println();
  Serial.print("AUX (硬件接GND, 期望≈32768): "); Serial.println(adsReadAux(b));
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) {}
  Serial.println();
  Serial.println("ADS-COMM-TEST v1");
  Serial.println("Need 16pin PSU (+5V -> ADS AVDD).");

  busInit(ADSA);
  busInit(ADSB);
  delay(10);
  convFrame(ADSA, 0x8500u);
  convFrame(ADSB, 0x8500u);
  delay(5);
  testBus(ADSA);
  testBus(ADSB);
}

void loop() {
  static uint32_t last = 0;
  static uint32_t lastReg = 0;
  if (millis() - last < 1000) return;
  last = millis();

  if (millis() - lastReg >= 5000) {
    lastReg = millis();
    regDiag(ADSA);
    regDiag(ADSB);
  }

  uint16_t a[8], b2[8];
  for (uint8_t n = 0; n < 8; ++n) a[n] = adsRead(ADSA, n);
  for (uint8_t n = 0; n < 8; ++n) b2[n] = adsRead(ADSB, n);
  uint16_t ax = adsReadAux(ADSA);
  uint16_t bx = adsReadAux(ADSB);

  Serial.print("A:"); for (uint8_t n = 0; n < 8; ++n) { Serial.print(a[n]); Serial.print(','); }
  Serial.print(" aux="); Serial.print(ax);
  Serial.print("  B:"); for (uint8_t n = 0; n < 8; ++n) { Serial.print(b2[n]); Serial.print(','); }
  Serial.print(" aux="); Serial.println(bx);
}
