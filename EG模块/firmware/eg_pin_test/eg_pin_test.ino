#include <Arduino.h>

// ============================================================================
// EG 模块 Pico 焊点自检（万用表版）
//
// 目的：把 Pico 上所有接了东西的 GPIO 输出高电平并保持，
//       用万用表在「Pico 引脚」和「目标芯片引脚」两端各量一次，
//       两边都 ≈3.3V 才说明这个引脚焊透了、走线通。
//
// 安全说明（后级都能扛 3.3V 高，不会烧）：
//   GP0-3   TLC5947 SIN/SCLK/BLANK/XLAT：BLANK 高 = LED 关，安全
//   GP6-13  ADS8688 SDI/CS#/SCLK/SDO：CS# 先拉高 -> SDO 高阻，不会有冲突
//   GP17-20 DAC8568 LDAC#/SYNC#/SCLK/DIN：SYNC# 高 = 不选中，安全
//   GP21/22/26/27 CD74HC4067 S0-S3：纯输入，安全
//
// 例外：GP28 不输出高（它是 ADC2，经 R51 接 mux COMMON 模拟节点），
//       只用内部上拉，高阻 3.3V。
//
// 如果之前怀疑某脚对地短路：先断电用万用表电阻档量各脚对 GND / 3V3，
// 确认没有 <1Ω 短路再上电，否则输出高会持续灌电流（约 60mA/脚）。
//
// 板子供电：Pico 的 USB 只给 3V3；要量 DAC/运放那边的引脚，
// 需要同时插 16pin 电源。只量 Pico 引脚则 USB 即可。
// ============================================================================

constexpr bool SAFE_PULLUP = false;  // true = 只用内部上拉(最安全, ~50k)；false = 输出高(低阻, 更好测)

// 接了东西、需要量的 GPIO（不含 GP28）
constexpr uint8_t TEST_PINS[] = {
  0, 1, 2, 3,          // TLC5947
  4, 5,                // 备用
  6, 7, 8, 9,          // ADCB (Retrig)
  10, 11, 12, 13,      // ADCA (Gate)
  14, 15, 16,          // 备用
  17, 18, 19, 20,      // DAC8568
  21, 22,              // mux S2/S3
  26, 27               // mux S0/S1
};
constexpr uint8_t N_PINS = sizeof(TEST_PINS) / sizeof(TEST_PINS[0]);

constexpr uint8_t PIN_MUX_COMMON = 28;  // ADC2，只上拉

// Pico 板载 LED
constexpr uint8_t PIN_STATUS_LED = 25;

static void driveTestPins() {
  // 1) 先把 CS#（GP7 / GP11）拉高，让两片 ADS8688 的 SDO 进入高阻
  pinMode(7, OUTPUT);  digitalWrite(7, HIGH);
  pinMode(11, OUTPUT); digitalWrite(11, HIGH);

  // 2) 其它引脚设为输出高
  for (uint8_t i = 0; i < N_PINS; ++i) {
    uint8_t p = TEST_PINS[i];
    if (p == 7 || p == 11) continue;  // 已处理
    if (SAFE_PULLUP) {
      pinMode(p, INPUT_PULLUP);
    } else {
      pinMode(p, OUTPUT);
      digitalWrite(p, HIGH);
    }
  }

  // 3) GP28 永远只上拉
  pinMode(PIN_MUX_COMMON, INPUT_PULLUP);
}

// 读回「焊盘真实电平」必须先把脚切成 INPUT 再 digitalRead，
// 直接读 OUTPUT 脚拿到的是输出锁存值，没意义。
static uint8_t readPad(uint8_t p) {
  pinMode(p, INPUT);          // 不要 PULLUP/PULLDOWN，否则会掩盖外部电平
  delayMicroseconds(50);
  uint8_t v = digitalRead(p);
  return v;
}

static void printTable() {
  Serial.print(F("pad: "));
  for (uint8_t i = 0; i < N_PINS; ++i) {
    Serial.print(F("GP"));
    Serial.print(TEST_PINS[i]);
    Serial.print('=');
    Serial.print(readPad(TEST_PINS[i]));
    Serial.print(' ');
  }
  Serial.print(F("GP28="));
  Serial.println(readPad(PIN_MUX_COMMON));

  // 读完后恢复驱动状态
  driveTestPins();
}

void setup() {
  pinMode(PIN_STATUS_LED, OUTPUT); digitalWrite(PIN_STATUS_LED, LOW);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) { /* 等 USB 串口，最多 1.5s */ }

  Serial.println(F("EG pin test: all test GPIO = HIGH (steady)."));
  Serial.println(F("Measure with multimeter at Pico pin AND target IC pin."));
  Serial.println(F("SAFE_PULLUP="));
  Serial.println(SAFE_PULLUP ? F("1 (internal pull-up ~50k)") : F("0 (push-pull HIGH)"));

  driveTestPins();
  delay(200);  // 等节点稳定

  // 注意：readPad 会把脚临时切成 INPUT，跑完会重新 drive，
  // 所以万用表读数以「两次打印之间」的稳定高为准。
  printTable();
}

void loop() {
  // 心跳：板载 LED 慢闪，证明固件在跑
  static uint32_t last = 0;
  static bool on = false;
  if (millis() - last >= 500) {
    last = millis();
    on = !on;
    digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
  }

  // 每 2 秒打印一次，方便边量边看串口对照。
  // readPad 期间会有几十 us 的低阻->高阻切换，万用表看不出来。
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint >= 2000) {
    lastPrint = millis();
    printTable();
  }
}

