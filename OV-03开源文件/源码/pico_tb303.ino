/*
 * Pico TB-303 — 电路级仿真的 Roland TB-303 acid bass
 * 硬件: 新板 "rp2040 adc dac"（U1 = YD-RP2040），引脚与 pico_poly_vco 完全一致
 *
 *   I2S  -> PCM5102A   BCLK=GP13, WS=GP14, DIN=GP15   (SCK 不接)
 *   ADS8688 (SPI0)     SCLK=GP2, MOSI=GP3, MISO=GP4, CS=GP5
 *       ADS8688 AIN0..AIN7 = TUNE / CUTOFF / RES / ENV.MOD / DECAY / ACCENT / DIST / COLOR
 *       (来源: OV-303 飞针网表 U1 引脚网络顺序; 物理引脚固定, 不受 303paint 皮肤影响)
 *   RGB   : WS2812 GP23
 *   MIDI  : USB (Adafruit TinyUSB, FQBN 需带 usbstack=tinyusb)
 *
 * V1.2 板 (2026-10-01) 新增: SW1 WAVE=GP6, SW2 MORERES=GP7, SW3 BUTTON=GP8,
 *   AD5664(U8) 四路 DAC CV/Gate: SYNC#=GP9, SCLK=GP10, DIN=GP11 (经 74AHCT541 到 5V);
 *   输出 A=CVOUT B=GATEOUT C=ACCENTOUT D=FTENVOUT。pattern 默认不自动播, 按键/X1 才播。
 *
 * DSP 移植自 Open303 (Robin Schmidt, DSP 为 MIT 许可) 的电路级模型:
 *   BlendOscillator(saw/square) -> HP -> TeeBeeFilter(TB_303: 4级梯形+反馈HP)
 *   -> 抗混叠 -> AP/HP -> VCA。4x 过采样链路改为可配置 OS。
 * 电路性质: 锯齿 VCO + tanh 软削波方波 + 4级梯形低通(反馈支路高通, 非线性, 自激)
 *           + 一次性滤波包络(env mod) + amp VEG + accent(力度>95) + slide(60ms, legato; DAW 用重叠音符=滑音)
 */
#include <Arduino.h>
#include <SPI.h>
#include <I2S.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_TinyUSB.h>
#include "tusb.h"          // TinyUSB 底层 (非阻塞 CDC 写)
#include <pico/multicore.h>
#include <pico/bootrom.h>
#include <hardware/watchdog.h>
#include <hardware/vreg.h>
#include <hardware/clocks.h>
#include <hardware/flash.h>
#include <hardware/sync.h>
#include <string.h>
#include <hardware/pio.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/structs/qmi.h>
#include <hardware/structs/xip.h>
#include "lcd_spi.pio.h"
#include "font5x7.h"
#include "smiley_bmp.h"

// ================= 引脚 (不变) =================
#define ADS_SCLK      2
#define ADS_MOSI      3
#define ADS_MISO      4
#define ADS_CS        5
#define I2S_BCLK     13
#define I2S_DOUT     15
#define PIN_XSMT     27    // PCM5102 软静音 (低=静音, 高=正常)
// ---- V1.2 板新增 (2026-10-01): 2 拨动开关 + 按键 + AD5664 四路 CV/Gate DAC ----
#define PIN_WAVE      6    // V1.2 SW1 波形拨动开关 (公共端, 高/低 两档 -> 方波/锯波)
#define PIN_MORERES   7    // V1.2 SW2 更多共振/自激拨动开关 (公共端, 高/低 两档)
#define PIN_USER      8    // V1.2 SW3 用户按键 (-> GND, 低有效) 播放/暂停/双击回头
#define PIN_USER2    23    // 老板(16M) USER 键 KEY3->GND (低有效); 与 PIN_USER 等价, 两个键任一个都行
#define PIN_DAC_SYNC  9    // V1.2 U8(AD5664) SYNC# (经 U9 74AHCT541 电平转换到 5V)
#define PIN_DAC_SCLK 10    // V1.2 U8 SCLK
#define PIN_DAC_DIN  11    // V1.2 U8 DIN
#define NEOPIXEL_PIN 16    // 板载 WS2812 数据 (MuLuoxing 模组=GP16; 旧 YD-RP2040 为 GP23)
#define NEOPIXEL_NUM 4     // 板载 WS2812 数量 (MuLuoxing 模组 4 颗)

// ================= 配置 =================
#define SAMPLE_RATE       48000
#define OS                1                 // 振荡过采样倍数 (仅用于 cntOsc 计数)
volatile int gUsePots  = 1;                 // 1=用物理电位器 (默认); 0=忽略电位器走串口/MIDI CC (仅 PC 程序连接时用 p0 临时关, 退出程序/H0 自动恢复 1)
volatile int gAutoTest = 1;                 // 1=无 MIDI 时自动播 pattern (收到 MIDI 停; A0/A1 可关/开)
static volatile int gSeqPlay = 0;           // 1=内置音序器播放 (默认 0: 不连电脑不自动播, 按键/X1 才播; DAW 优先级见 dawTakeOver)
volatile int gNoFilter = 0;                 // 1=旁路滤波器(诊断)
volatile int gSineMode = 0;                 // 1=纯正弦直出(诊断 I2S/DAC)
volatile int gNoEnv = 0;                    // 1=旁路包络/调制(诊断: amp恒定, 截止固定)
volatile float gSlideMs = 60.0f;            // slide 时长 (0=不滑)
#define SLIDE_MS          60.0f             // slide 时长 (legato)

#define USE_USB_MIDI      1
#define MIDI_CHANNEL      1                 // 0=OMNI, 1..16 (固定 1: 不与软件冲突, 不做运行时设置)
#define ACCENT_VEL        95                // 重音判定: 力度 > 95 (DAW 与内置音序器共用同一 midiNoteOn; 内置注入 120=重音/64=普通)
#define DEBUG_SERIAL      0                                           

// 旋钮: ADS8688 AIN0..7 全部接入; gUsePots 默认开 -> 参数走物理电位器 (仅 PC 程序连接时用 p0 临时关)

// 参数范围 (参考视频实测+研究文档: 本体截止 全关≈200Hz / 半开≈400Hz / 全开≈2500Hz基频, 亮度靠env mod上扫)
#define CUT_MIN_HZ        20.0f        // Syrebas 官方: 截止下限 200Hz
#define CUT_MAX_HZ        4000.0f       // Syrebas 官方: 200*2^3.64385 ≈ 2.5kHz
#define DECAY_MIN_MS      200.0f        // TB-303 普通衰减范围 200..2000ms (Open303 文档)
#define DECAY_MAX_MS      2500.0f       // Syrebas: 200 -> ~2500ms
// TB-303 VCF 的“第二低频共振峰”(耦合电容造成): 用低频 peaking EQ 近似
// ⚠ 知识库 §10: 真 303 低频"不平直、别加低音增强", 此 +4.5dB@55Hz 方向可能相反 → 默认关 (也省 core1 CPU)
#define LOWPEAK_F0        8.0f      // 低频第二谐振峰 (Stinchcombe ~8Hz; 抬高低频)
#define LOWPEAK_Q         0.7f
#define LOWPEAK_DB        4.0f

// ---- 旋钮通道映射 (硬连线固定; 来自 OV-303 Gerber/飞针: ADS8688 AIN0..7) ----
//      303paint 里的 cellFn[] 只决定小屏幕每格显示什么, 不改这里的引脚 (引脚不可被皮肤改) ----
#define POT_CH_TUNE       0    // 第1个旋钮 -> AIN0  (TUNE)
#define POT_CH_CUTOFF     1    // 第2个旋钮 -> AIN1  (CUT OFF FREQ)
#define POT_CH_RES        2    // 第3个旋钮 -> AIN2  (RESONANCE)
#define POT_CH_ENVMOD     3    // 第4个旋钮 -> AIN3  (ENV MOD)
#define POT_CH_DECAY      4    // 第5个旋钮 -> AIN4  (DECAY)
#define POT_CH_ACCENT     5    // 第6个旋钮 -> AIN5  (ACCENT)
#define POT_CH_DIST       6    // 第7个旋钮 -> AIN6  (DIST 失真量, TD-3 DS-1 风格)
#define POT_CH_COLOR      7    // 第8个旋钮 -> AIN7  (COLOR 失真音色)
// 方向: 0=电压越高数值越大; 1=反向 (顺时针方向不对时翻转)
#define INV_CUTOFF   0
#define INV_RES      0
#define INV_DECAY    0
#define INV_ENVMOD   0
#define INV_ACCENT   0
#define INV_TUNE     0
#define INV_DIST     0
#define INV_COLOR    0
#define TUNE_RANGE_SEMIS 12.0f // TUNE 旋钮范围 ±1 个八度 (±12 半音)
#define TUNE_SNAP        0.06f  // TUNE 中点吸附区 (范围大点好卡住; 电位器无中点刻度)
#define POT_VREF          3.3f

// ADS8688
#define ADS_RANGE    0x06
#define ADS_FS_VOLTS (1.25f * 4.096f)

// 板级超频状态 (供串口命令/日志用; 逻辑见 overclockLadder)
volatile uint32_t gOcKHz   = 0;
volatile int      gOcIdx   = 0;
volatile uint32_t gOcFails = 0;

// 超频阶梯 + 看门狗 (统一定义在此, 命令处理与 overclockLadder 共用)
// scratch (掉电清、看门狗/软复位保留): [0]=magic [1]=档位 [2]=连续失败 [3]=实际主频kHz
//                                    [6]=看门狗开关(0/1) [7]=看门狗开关有效 magic
#define OC_SCRATCH_MAGIC   0x0C0FFEE1u
#define OC_WDT_MS          6000u
#define OC_FAILS_TO_DROP   2
// RP2350 档位: 频率 + DVDD(mV)。⚠️ 数据手册 DVDD 绝对最大 1.21V; 这里按用户要求放到 1.35V (超规, 风险自负)。
// 0=原版未动(150MHz@1.10V) ... 开关关时最高 4:375MHz@1.30V; 5..8 需"解锁超频"开关 (1.35V)
// 注: 300MHz 用 1.20V (<=abs max 1.21V, 手测稳定); 之前 1.25V 在音频+扫参数下会偶发复位(疑似电源/稳压)。
static const uint32_t OC_KHZ[] = { 150000, 250000, 300000, 360000, 375000, 396000, 408000, 432000, 456000 };
static const uint16_t OC_MV [] = { 1100,   1200,   1200,   1300,   1300,   1350,   1350,   1350,   1350   };
static const uint32_t OC_N = 9;
#define OC_LOCK_MAX_IDX 4      // 解锁开关关时最高档 = 375MHz@1.30V
#define OC_DEFAULT_IDX 0       // 出厂默认 150MHz@1.10V (用户要求; 用户自行超频)
volatile int gPauseCore1 = 0;   // 1=写 flash 前让 core1 音频 ISR 空转 (否则核心满载时 lockout IRQ 被饿死 -> 死锁)
static   int gCore1Started = 0; // core1 起来后才允许 lockout 写 flash (提前声明, 供各 flash 写函数用)
volatile int gOcUnlock = 0;               // 超频解锁开关 (0=最高 375MHz@1.30V; 1=可继续超)
// ⚠️ watchdog_enable 写 scratch[4], watchdog_reboot 写 scratch[4..7]! 故只用 scratch[0..3]:
//   [0]=magic [1]=idx|unlock<<8|filtOScode<<9|wdt<<11 [3]=fails
#define OC_FOSCODE(f)  (((f)==4)?0u:(((f)==12)?2u:(((f)==16)?3u:1u)))
#define OC_FOSFROMC(c) (((c)==0)?4:(((c)==2)?12:(((c)==3)?16:8)))
#define OC_PACK(i,u,fc,w) ((uint32_t)(i) | ((uint32_t)(u) << 8) | ((uint32_t)(fc) << 9) | ((uint32_t)(w) << 11))
// 黑盒: scratch[2] 的最高字节记 loop() 最后到达的阶段 (开机打印 bb= 可看卡在哪)
#define BB(st) do { watchdog_hw->scratch[2] = (watchdog_hw->scratch[2] & 0x00FFFFFFu) | ((uint32_t)((uint8_t)(st)) << 24); } while (0)
volatile int gFiltOS   = 8;               // 滤波器过采样倍数 (4/8/12/16; 开机固定, 走编译期常量 -> 快)
volatile int gSelfOsc  = 0;               // 自激开关: 0=干净不自激(真机行为,k封顶30); 1=允许自激(k到36)  (串口 $0/$1)
volatile int gNoteActive = 0;             // 1=有音符(env 未归零); 0=空闲 -> VCA 增益硬置 0 (去残余漏音)
volatile uint8_t gLastBB = 0;             // 上次运行 loop() 最后到达的阶段 (开机快照, 用于定位卡死)
volatile int gRstReason = 0;              // 上次复位原因位图 (0=上电/brownout,1=WDT,2=enable,4=liveness)
volatile int gWdtEnabled = 1;            // 看门狗开关 (掉电默认开; 串口 u0/u1 改)
volatile int gKeySimPress = 0;           // 测试钩子: 串口 '*' 模拟按下 USER 键 (供无手按键时验证单击/双击)
static uint32_t gKeySimReleaseMs = 0;
volatile int gAutoReport = 1;            // 1=每 2s 自动打印 CPU 占用 (串口 I0/I1 改)

static const float PI_F   = 3.14159265358979f;
static const float SQRT2F = 1.41421356237f;

// ================= 全局对象 =================
I2S i2s(OUTPUT, I2S_BCLK, I2S_DOUT);
Adafruit_NeoPixel rgb(NEOPIXEL_NUM, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_USBD_CDC AudioCDC;   // 第二路 CDC (独立 FIFO/端点): 专用音频 (裸 PCM, 与文本彻底分开)

// ================= ADS8688 =================
static inline uint8_t adsManCmd(uint8_t ch) { return 0xC0 + 4 * ch; }

static void adsWriteReg(uint8_t reg, uint8_t val) {
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE1));
  digitalWrite(ADS_CS, LOW);
  SPI.transfer((uint8_t)((reg << 1) | 0x01));
  SPI.transfer(val);
  SPI.transfer(0x00);
  digitalWrite(ADS_CS, HIGH);
  SPI.endTransaction();
}

static uint16_t adsCmd(uint8_t cmd) {
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE1));
  digitalWrite(ADS_CS, LOW);
  SPI.transfer(cmd);
  SPI.transfer(0x00);
  uint8_t msb = SPI.transfer(0x00);
  uint8_t lsb = SPI.transfer(0x00);
  digitalWrite(ADS_CS, HIGH);
  SPI.endTransaction();
  return ((uint16_t)msb << 8) | lsb;
}

static uint16_t adsReadCh(uint8_t ch) {
  uint8_t c = adsManCmd(ch);
  uint16_t v = 0;
  for (int i = 0; i < 3; i++) v = adsCmd(c);
  return v;
}

static void adsBegin() {
  adsCmd(0x85);
  delay(5);
  adsWriteReg(0x02, 0x00);
  adsWriteReg(0x03, 0x00);
  for (uint8_t ch = 0; ch < 8; ch++) adsWriteReg(0x05 + ch, ADS_RANGE);
}

static inline float adsVolts(uint16_t code) { return (float)code / 65536.0f * ADS_FS_VOLTS; }

// 电位器归一化 (0..1), invert=1 时反向
static inline float potNorm(uint16_t raw, int invert) {
  float v = adsVolts(raw) / POT_VREF;
  if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
  return invert ? (1.0f - v) : v;
}

// ================= 数学工具 =================
static inline float clampf(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}
// 死区: 变化小于 db 就保持旧值 (抑制 ADC 小幅抖动)
static inline float dbnd(float prev, float now, float db) {
  return (fabsf(now - prev) < db) ? prev : now;
}

// (A 锥 audioTaper 已删: 电位器全部换 B 线性, 不再做对数换算)
// Open303 的 waveshaper 非线性 (soft clip)
static inline float shape(float x) {
  x = clampf(x, -SQRT2F, SQRT2F);
  return x - (1.0f / 6.0f) * x * x * x;
}
// polyBLEP 抗混叠修正
static inline float polyBLEP(float t, float dt) {
  if (dt <= 0.0f) return 0.0f;
  if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
  return 0.0f;
}

// 二阶 peaking EQ (RBJ): 近似 TB-303 的“第二个低频共振峰”(耦合电容带来的低频隆起)
struct Biquad {
  float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
  void reset() { z1 = z2 = 0.0f; }
  void setPeaking(float f0, float Q, float gainDb, float fs) {
    float A = powf(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * PI_F * f0 / fs;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * Q);
    float a0 = 1.0f + alpha / A;
    b0 = (1.0f + alpha * A) / a0; b1 = (-2.0f * cw) / a0; b2 = (1.0f - alpha * A) / a0;
    a1 = (-2.0f * cw) / a0;       a2 = (1.0f - alpha / A) / a0;
  }
  void setLowpass(float f0, float Q, float fs) {          // RBJ 二阶低通
    float w0 = 2.0f * PI_F * f0 / fs, cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * Q), a0 = 1.0f + alpha;
    b0 = (1.0f - cw) * 0.5f / a0; b1 = (1.0f - cw) / a0; b2 = b0;
    a1 = (-2.0f * cw) / a0;       a2 = (1.0f - alpha) / a0;
  }
  void setHighpass(float f0, float Q, float fs) {         // RBJ 二阶高通
    float w0 = 2.0f * PI_F * f0 / fs, cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * Q), a0 = 1.0f + alpha;
    b0 = (1.0f + cw) * 0.5f / a0; b1 = -(1.0f + cw) / a0; b2 = b0;
    a1 = (-2.0f * cw) / a0;       a2 = (1.0f - alpha) / a0;
  }
  inline float process(float x) { float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

// ================= TD-3 内置失真 (Boss DS-1 架构, 定点) =================
// 依据(已存 参考资料\): Maffez《TD-3 Modifications Guide》P19
//   "The inbuilt distortion of the TD-3 is based on the Boss DS-1" (失真在 VCA 之后);
//   Gitec/Bernd Meiser《Effect-ive! Three degrees of Distortion: Boss DS-1》逐级元件值:
//   ① 输入缓冲 Q1 → ② 晶体管级 Q2: 输入 HP≈600Hz(47nF)、反馈 470k‖250pF(≈1.4kHz 滚降)、
//      软饱和(36dB, "fine harmonic distortion", 非硬削) → ③ 运放增益 1→×20 (DIST) →
//      ④ 反并联硅二极管削波 ±0.6V (modified-ln 软拐点→平顶) → ⑤ Big-Muff 音调(低通+高通交叉, ~中频凹陷)。
// 数字近似: HP600 → tanh软饱和(晶体管) → LP1.4k → ×DIST → tanh软削波(二极管) → LP+HP音调 → 干湿。
// 用 tanh 软饱和(而非硬削)大幅降低混叠(谐波滚降快), 避免"方波感"。
// DSP 版本选择: RP2350 有硬件 FPU -> 失真链用真浮点 tanh (无查表量化) + DIST_OS 倍过采样;
//              RP2040 无 FPU -> 保留 Q15 定点 + tanh 查表 (低混叠, 省 CPU)。
#define DIST_PREHP_HZ    600.0f     // 输入高通 (DS-1 47nF/550Ω → ~600Hz)
#define DIST_TRANS_LP_HZ 1400.0f    // 晶体管级高频滚降 (470k‖250pF → ~1.35kHz; DS-1/TD-3 原版固定高切)
#define DIST_TONE_LP_HZ  1600.0f    // Big-Muff 音调低通段 (原版)
#define DIST_TONE_HP_HZ  500.0f     // Big-Muff 音调高通段
#define DIST_GAIN_MAX    30.0f      // DIST 旋钮最大增益 (1→×31) — 更猛
#define DIST_MIN         0.02f      // 低于此值直接旁路 (旋钮到底 = 干净 303)

// 快速 tanh: Padé 有理近似 (纯 FPU 乘/除, 不调用 libm 超越函数, 快约 10x); |x|>3 饱和
static inline float fastTanh(float x) {
  if (x < -3.0f) return -1.0f;
  if (x >  3.0f) return  1.0f;
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
// 三次软削波 (斜率1@0, ±1.5->±1; 无除法, 便宜) —— 用于滤波反馈二极管饱和
static inline float softclip3(float x) {
  if (x > 1.5f) return 1.0f;
  if (x < -1.5f) return -1.0f;
  return x - (4.0f / 27.0f) * x * x * x;
}
// ---------- RP2350: 真浮点 DS-1 (FPU), 二极管削波 DIST_OS 倍过采样 ----------
#define DIST_OS          8          // 削波级过采样倍数 (浮点, 降混叠; RP2040 上是 2)
struct DS1Dist {
  float lpPre = 0, lpTrans = 0, lpToneL = 0, lpToneH = 0, dcx = 0, cprev = 0;
  float lpMid = 0, hpMid = 0;                                         // 中频 bandpass (Big-Muff scoop)
  float aPre = 0, aTrans = 0, aToneL = 0, aToneH = 0, aDC = 0, aMidL = 0, aMidH = 0;
  float g = 1.0f, c = 0.5f, m = 1.0f, tg = 1.5f, post = 1.0f;
  static float mkCoef(float f, float fs) { return 1.0f - expf(-2.0f * PI_F * f / fs); }
  void setSampleRate(float fs) {
    aPre   = mkCoef(DIST_PREHP_HZ,    fs);
    aTrans = mkCoef(DIST_TRANS_LP_HZ, fs);
    aToneL = mkCoef(DIST_TONE_LP_HZ,  fs);
    aToneH = mkCoef(DIST_TONE_HP_HZ,  fs);
    aDC    = mkCoef(20.0f,            fs);
    aMidH  = mkCoef(600.0f,  fs);                                       // 中频带通下沿
    aMidL  = mkCoef(1100.0f, fs);                                       // 中频带通上沿
  }
  void reset() { lpPre = lpTrans = lpToneL = lpToneH = dcx = cprev = lpMid = hpMid = 0; }
  // DIST = 失真量(驱动); COLOR = 音色(0=暗/低通↔1=亮/高通); 带输出补偿(驱动越大衰减越多)
  void setParams(float d, float color) {
    g = 1.0f + d * d * DIST_GAIN_MAX;             // 二次曲线: 低驱动几乎不推, 越推越大
    c = clampf(color, 0.0f, 1.0f);                // 0..1
    m = 1.0f / (1.0f + d * 0.4f);                 // 驱动补偿 (max ≈ -3dB) — 更猛
    tg = 1.5f + d * 12.0f;                        // 晶体管级增益也随驱动 (低驱动=接近干净)
    aPre = mkCoef(150.0f + 450.0f * d, (float)SAMPLE_RATE);   // 输入高通随驱动: 低驱动150Hz(少切低音) → 高驱动600Hz(DS-1)
    post = 0.5f + 0.5f * (1.0f - d);              // 输出电平: 低驱动≈unity, 高驱动=之前 -6dB (修低DIST音量猛掉)
  }
  static inline float satu(float x) { return fastTanh(x); }   // 快速 tanh (FPU)
  inline float process(float x) {
    lpPre  += aPre  * (x - lpPre);  float xp = x - lpPre;             // 输入高通 600Hz
    float s1 = satu(xp * tg);                                          // 晶体管级软饱和 (随驱动)
    lpTrans += aTrans * (s1 - lpTrans); float v1 = lpTrans;           // 1.4kHz 低通
    float v2 = v1 * g;                                                // ×DIST (失真量)
    float acc = satu(v2);                                             // 二极管削波: DIST_OS 倍过采样 (线性插值)
    for (int k = 1; k < DIST_OS; k++) { float t = (float)k / (float)DIST_OS; acc += satu(cprev + (v2 - cprev) * t); }
    float v3 = acc * (1.0f / (float)DIST_OS); cprev = v2;
    lpToneL += aToneL * (v3 - lpToneL); float tl = lpToneL;           // 音调 LP
    lpToneH += aToneH * (v3 - lpToneH); float th = v3 - lpToneH;      // 音调 HP
    hpMid += aMidH * (v3 - hpMid); float mh = v3 - hpMid;             // 中频带通 (600..1100Hz)
    lpMid += aMidL * (mh - lpMid); float mid = lpMid;
    float vt = tl + (th - tl) * c - 0.8f * mid;                       // COLOR 交叉 - 中频 scoop (Big-Muff 型; 原版)
    float o  = vt * m;                                                // 驱动补偿
    dcx += aDC * (o - dcx);                                           // DC 阻断 (~20Hz)
    return (o - dcx) * post;                                          // 输出电平 (低驱动≈unity, 高驱动 -6dB)
  }
};
// (RP2040 定点失真分支已移除: 现在只有 RP2350)

// 保真开关 (原版有、Open303 没有的东西; 用串口 h<掩码> 切换)
//   bit0 = 滤波反馈软饱和 (限制自激/加"咬")
//   bit1 = 级内软饱和 (贵)
//   bit2 = 动态反馈高通 (150->250Hz 随谐振)
//   bit3 = 输出/VCA 软饱和
//   bit4 = ENV MOD gimmick (ENV MOD 越大->基础截止越低; 服务手册 p8)
//   bit5 = ~8Hz 第二低频峰 (Stinchcombe)
// RP2350 有余量 -> 默认开 bit0。
volatile int gFidFB    = 1;
volatile int gFidStage = 0;   // bit1 级内饱和: 非原版, 会压死谐振/变闷 -> 默认关 (可用 h 开)
volatile int gFidFbHp  = 1;
volatile int gFidOut   = 0;   // bit3: 输出/VCA 软饱和 (非原版) -> 默认关
volatile int gFidEnvMod = 1;  // bit4: ENV MOD gimmick (默认开; 服务手册 p8)
volatile int gFidLowPeak = 0;   // it11: ~8Hz 下谐振峰 (原版耦合电容, Stinchcombe) // bit5: ~8Hz 第二低频峰 (Stinchcombe); 默认关 (REFERENCE §47: 原版低频是衰减不是提升)
volatile int gDistPost   = 1;   // 失真位置: 1=VCA之后(TD-3,默认); 0=VCA之前 (串口 /1 或 /0)
volatile int gResBassComp = 0; // bit6: 谐振低频补偿 (关=原版 res 逻辑; 开=按 res 抬低频)
volatile int gFidNoise  = 1;   // bit7: 极低模拟噪声使能 (总开关); 实际量由 gNoiseAmt 旋钮定
volatile float gNoiseAmt = 0.0f;   // §51 噪声量 0..1 (软件旋钮; 0=关)
volatile float gVarAmt   = 0.0f;   // §49 元件容差/个体差异 0..1 (0=理想)
volatile float gTempAmt  = 0.0f;   // §50 温度漂移 0..1 (0=关)
static inline void saveKnobsScratch() {   // 噪声/容差/温度 旋钮存 scratch[2] (跨软复位保留)
  uint32_t n7=(uint32_t)(gNoiseAmt*127.0f+0.5f)&0x7Fu, v7=(uint32_t)(gVarAmt*127.0f+0.5f)&0x7Fu, t7=(uint32_t)(gTempAmt*127.0f+0.5f)&0x7Fu;
  watchdog_hw->scratch[2] = n7 | (v7<<7) | (t7<<14) | (watchdog_hw->scratch[2] & 0xFF000000u);   // 保留黑盒阶段字节
}
volatile float gDetuneLin = 1.0f, gCutDriftLin = 1.0f, gGainDriftLin = 1.0f;  // §49/§50 实时应用到音频
static float gVarTuneC = 0, gVarCutOc = 0, gVarGainF;   // 容差随机定值
static float gDrT = 0, gDrC = 0, gDrG = 0;              // 温度随机游走
#define NOISE_MAX 2.0e-3f   // 噪声旋钮满量程幅度 (~ -54dB)
#define ENVGMOD_BASE_DROP 1.0f   // ENV MOD 满时基础截止降的八度数

// ================= 包络 / 积分器 =================
struct Leaky {                            // 泄漏积分 (rc1/rc2/pitch slew)
  float y = 0.0f, a = 1.0f;
  void setTauMs(float ms) {
    if (ms <= 0.0f) { a = 1.0f; return; }
    a = 1.0f - expf(-1.0f / ((ms / 1000.0f) * (float)SAMPLE_RATE));
  }
  void setState(float s) { y = s; }
  inline float process(float x) { y += a * (x - y); return y; }
  void reset() { y = 0.0f; }
};

// Open303 ampDeClicker 的轻量版: 一阶低通 (~200Hz), 作用在 amp 增益上, 抹掉 isNoteOn 硬门控的台阶
struct AmpDeClicker {
  float y = 0.0f, a = 0.0f;
  void setLowpass(float fc, float fs) {
    a = 1.0f - expf(-2.0f * PI_F * fc / fs);
  }
  inline float process(float x) { y += a * (x - y); return y; }
  void reset() { y = 0.0f; }
};

// ================= Syrebas 复刻引擎 (RP2350 专用; 电路级) =================
// 振荡器 (忠实版): 连续锯齿核 + polyBLEP; 方波由锯齿比较导出(占空比随音高 45..70%); 方波幅度低于锯齿
// (按 REFERENCE: 不加 14kHz LPF / 不加 x-0.05x² / 方波不专用 150Hz HPF; 用输出耦合 HP)
struct Osc303 {
  float ph = 0.0f, curFreq = 440.0f, tgtFreq = 440.0f;
  float cpA = 0.0f, cpX = 0.0f, cpY = 0.0f;     // 输出耦合高通 (~82Hz)
  float lpfSawState = 0.0f, lpfSawCoeff = 0.0f; // it6: 锯齿高频柔化
  void init() { float sr = (float)SAMPLE_RATE;
    cpA = expf(-2.0f * PI_F * 82.0f / sr);
    lpfSawCoeff = 1.0f - expf(-2.0f * PI_F * 14000.0f / sr); }
  void reset() { ph = 0.0f; cpX = 0.0f; cpY = 0.0f; lpfSawState = 0.0f; }
  void setFreq(float f) { tgtFreq = f; }
  __force_inline float process(float wave) {
    curFreq = tgtFreq;
    float inc = curFreq / (float)SAMPLE_RATE;
    ph += inc; if (ph >= 1.0f) ph -= 1.0f;
    float out;
    if (wave < 0.5f) {                                       // it6: 干净负向锯齿 + polyBLEP + 14kHz LPF
      float rawSaw = 1.0f - 2.0f * ph + polyBLEP(ph, inc);
      lpfSawState += lpfSawCoeff * (rawSaw - lpfSawState);
      out = lpfSawState;
    } else {                                                 // 方波 (占空比随音高) + polyBLEP
      float duty = 0.45f + 0.25f * expf(-curFreq / 180.0f);
      if (duty < 0.45f) duty = 0.45f; if (duty > 0.70f) duty = 0.70f;
      float sq = (ph < duty) ? 0.75f : -0.75f;
      sq += 0.75f * polyBLEP(ph, inc);
      float t2 = ph - duty; if (t2 < 0.0f) t2 += 1.0f;
      sq -= 0.75f * polyBLEP(t2, inc);
      out = sq;
    }
    float h = cpA * (cpY + out - cpX); cpX = out; cpY = h;   // osc->VCF 耦合高通 ~82Hz
    return h;
  }
};
// 4级耦合二极管梯形 (Syrebas): 过采样 RK2/中点, 非线性级间 tanh, 动态反馈 HP, k=res*33
// 过采样倍数 = 运行时 gFiltOS (4/8/12/16)
struct Filter303 {
  float v1 = 0, v2 = 0, v3 = 0, v4 = 0, hpX1 = 0, hpY1 = 0, prevIn = 0;
  float ic1X = 0, ic1Y = 0, ic2X = 0, ic2Y = 0, icA = 0, icA2 = 0;   // 输入耦合 HP
  float oc1X = 0, oc1Y = 0, oc2X = 0, oc2Y = 0, oc3X = 0, oc3Y = 0, ocA = 0, ocA2 = 0, ocA3 = 0;   // 输出耦合 HP (3级)
  void init() { reset(); float sr = (float)SAMPLE_RATE;
    icA = expf(-2.0f * PI_F * 16.0f / sr); icA2 = expf(-2.0f * PI_F * 6.0f / sr);     // 输入耦合 (手册实测 ~1.6-16Hz)
    ocA = expf(-2.0f * PI_F * 16.0f / sr); ocA2 = expf(-2.0f * PI_F * 3.4f / sr); ocA3 = expf(-2.0f * PI_F * 1.6f / sr); }  // 输出耦合
  void reset() { v1 = v2 = v3 = v4 = hpX1 = hpY1 = prevIn = 0.0f;
    ic1X = ic1Y = ic2X = ic2Y = 0.0f; oc1X = oc1Y = oc2X = oc2Y = oc3X = oc3Y = 0.0f; }
  // 编译期常量过采样内核 (NOS=4/8/12/16) -> 可完全内联展开, 不落 flash
  template<int NOS>
  __force_inline float kernel(float input, float cutoffHz, float res) {
    float iy1 = icA  * (ic1Y + input - ic1X); ic1X = input; ic1Y = iy1;
    float iy2 = icA2 * (ic2Y + iy1   - ic2X); ic2X = iy1;   ic2Y = iy2;
    input = iy2;
    const float dt = 1.0f / ((float)SAMPLE_RATE * (float)NOS);
    float fc = cutoffHz; if (fc < 20.0f) fc = 20.0f; if (fc > 18000.0f) fc = 18000.0f;
    float wc = 2.0f * PI_F * fc;
    float r = res; if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
    float hpfc = 150.0f + 100.0f * r;     // 反馈耦合 HP (Syrebas 官方: 150->250Hz)
    float hpA = 1.0f / (1.0f + 2.0f * PI_F * hpfc * dt);
    float kFb = r * (gSelfOsc ? 30.0f : 17.3f);   // 反馈量: 自激阈值实测≈16.5. 关=17.3(满程到达自激边缘,手感不变); 开=30(上限更高,自激更猛)
    const float Vt = 0.052f, Vti = 19.23f;
    float out = 0.0f;
    float pIn = prevIn; prevIn = input;
    for (int os = 0; os < NOS; os++) {
      float a = (float)(os + 1) / (float)NOS;
      float inS = (pIn + a * (input - pIn)) * 0.05f;
      float fb1 = hpA * (hpY1 + v4 - hpX1); hpX1 = v4; hpY1 = fb1;
      float fbCur = softclip3(fb1 * kFb);   // 反馈二极管软饱和
      float u = inS - fbCur;
      float h = dt;
      float t12 = fastTanh((v1 - v2) * Vti), t23 = fastTanh((v2 - v3) * Vti), t34 = fastTanh((v3 - v4) * Vti);
      float d1 = wc * Vt * (fastTanh((u - v1) * Vti) - t12);
      float d2 = wc * Vt * (t12 - t23);
      float d3 = wc * Vt * (t23 - t34);
      float d4 = 2.0f * wc * Vt * t34;
      float m1 = v1 + 0.5f * h * d1, m2 = v2 + 0.5f * h * d2, m3 = v3 + 0.5f * h * d3, m4 = v4 + 0.5f * h * d4;
      float fm1 = hpA * (hpY1 + m4 - hpX1);
      float fmCur = softclip3(fm1 * kFb);
      float uM = inS - fmCur;
      float q12 = fastTanh((m1 - m2) * Vti), q23 = fastTanh((m2 - m3) * Vti), q34 = fastTanh((m3 - m4) * Vti);
      float e1 = wc * Vt * (fastTanh((uM - m1) * Vti) - q12);
      float e2 = wc * Vt * (q12 - q23);
      float e3 = wc * Vt * (q23 - q34);
      float e4 = 2.0f * wc * Vt * q34;
      v1 += h * e1; v2 += h * e2; v3 += h * e3; v4 += h * e4;
      out += (v4 * 20.0f) * (1.0f / (float)NOS);   // (v4/0.05)/OS 平均降采
    }
    float oy1 = ocA  * (oc1Y + out - oc1X); oc1X = out; oc1Y = oy1;
    float oy2 = ocA2 * (oc2Y + oy1 - oc2X); oc2X = oy1; oc2Y = oy2;
    float oy3 = ocA3 * (oc3Y + oy2 - oc3X); oc3X = oy2; oc3Y = oy3;
    return oy3;
  }
  __force_inline float process(float input, float cutoffHz, float res) {
    switch (gFiltOS) {                     // 开机固定, 走编译期常量 -> 内联展开
      case 4:  return kernel<4>(input, cutoffHz, res);
      case 12: return kernel<12>(input, cutoffHz, res);
      case 16: return kernel<16>(input, cutoffHz, res);
      default: return kernel<8>(input, cutoffHz, res);
    }
  }
};
// 双包络 (Syrebas): VCF(MEG) 3.5ms/Decay(accent 推向 200ms); VCA(VEG) 3ms/gate-high 3.5s/gate-low 16ms;
// accent sweep 电容 47k+1µF(47ms, 跨音符连续) + accent->VCA RC 1.55ms; slide 不重触发
struct Env303 {
  bool gate = false, isAccent = false;
  float sr = 48000.0f;
  float vcfDecayTime = 0.2f;
  float vcfAttack = 0, vcfDecay = 0, vcaAttack = 0, vcaGateHigh = 0, vcaQuickDrain = 0;
  float accCharge = 0, accDischarge = 0, accVcaCoef = 0;
  float vcfEnv = 0, vcfTarget = 0, vcaEnv = 0, vcaTarget = 0, accentCap = 0, accentVca = 0;
  static constexpr float kAccentDecay = 0.20f;      // accent 强制最短 decay
  void setSampleRate(float s) { sr = s; update(); }
  void setDecayMs(float ms) { if (ms < 100.0f) ms = 100.0f; vcfDecayTime = ms * 0.001f; update(); }
  void update() {
    vcfAttack   = 1.0f - expf(-1.0f / (sr * 0.001f));    // it9: MEG attack 1ms (干脆)
    vcfDecay    = expf(-1.0f / (sr * (vcfDecayTime / 2.302585f)));
    vcaAttack   = 1.0f - expf(-1.0f / (sr * 0.003f));
    vcaGateHigh = expf(-1.0f / (sr * (3.5f / 2.302585f)));
    vcaQuickDrain = expf(-1.0f / (sr * (0.016f / 2.302585f)));
    accCharge   = 1.0f - expf(-1.0f / (sr * 0.047f));     // 47k+1µF
    accDischarge = expf(-1.0f / (sr * 0.047f));
    accVcaCoef  = 1.0f - expf(-1.0f / (sr * 0.0006f));    // it9: accent->VCA 更快 (更 punch)
  }
  void reset() { gate = false; isAccent = false; vcfEnv = vcfTarget = vcaEnv = vcaTarget = accentCap = accentVca = 0.0f; }
  void noteOn(bool accent, bool slide, float accentKnob) {
    gate = true; isAccent = accent; update();
    if (accent) { vcfDecay = expf(-1.0f / (sr * (kAccentDecay / 2.302585f))); }   // it9: accent MEG 衰减加长 (200ms->10%)   // 重音: 4066 短路 Decay 电位器 -> MEG 固定最短 ~200ms (服务手册/原理图; 与 Decay/Accent 旋钮无关)
    if (!slide) { vcfTarget = 1.0f; vcaTarget = 1.0f; }   // 重触发 (从当前电压起)
    else        { vcfTarget = 0.0f; vcaTarget = 0.0f; }   // slide: 不重触发, 继续衰减
  }
  void noteOff() { gate = false; vcaTarget = 0.0f; vcfTarget = 0.0f; }
  inline void process() {
    if (vcfTarget > vcfEnv) { vcfEnv += vcfAttack * (vcfTarget - vcfEnv); if (vcfEnv >= 0.99f) vcfTarget = 0.0f; }
    else vcfEnv *= vcfDecay;
    if (gate) {
      if (vcaTarget > vcaEnv) { vcaEnv += vcaAttack * (vcaTarget - vcaEnv); if (vcaEnv >= 0.99f) vcaTarget = 0.0f; }
      else vcaEnv *= vcaGateHigh;
    } else vcaEnv *= vcaQuickDrain;
    if (isAccent && gate) accentCap += accCharge * (vcfEnv - accentCap); else accentCap *= accDischarge;
    float avT = (isAccent && gate) ? vcfEnv : 0.0f;
    accentVca += accVcaCoef * (avT - accentVca);
  }
  bool active() { return gate || vcaEnv > 0.0001f || vcfEnv > 0.0001f; }
};

// ================= 音源状态 =================
static Filter303 gFilter303;
static Osc303    gOsc303;
static Env303    gEnv303;
static Leaky     gPitchSlew;
static AmpDeClicker gAmpDeClick;            // amp 增益去咔哒 (200Hz 二阶低通, Open303)
static Biquad    gLowPeak;                  // 第二低频共振峰 (近似)
static Biquad    gResBass;                  // 谐振低频补偿 (可选; 关=原版)
static DS1Dist   gDist;                     // TD-3 失真 (Boss DS-1 架构, post-VCA)

// 极低模拟噪声 (REFERENCE §51): 白噪 ~-88dB, 加在滤波器输入前 -> 经 VCA 门控, 空闲仍静音
static inline float analogNoise() {
  static uint32_t s = 0x9E3779B9u;
  s ^= s << 13; s ^= s >> 17; s ^= s << 5;
  return (float)(int32_t)s * (1.0f / 2147483648.0f);
}
// 慢变漂移/容差用随机源 (§49/§50)
static inline float driftRnd() {
  static uint32_t s = 0x00C0FFEEu;
  s ^= s << 13; s ^= s >> 17; s ^= s << 5;
  return (float)(int32_t)s * (1.0f / 2147483648.0f);
}

#define ENVMOD_DEPTH   1.0f                // env mod 深度 (Open303/TB-303 忠实值 = 1.0)
volatile float gMaster = 1.0f;            // 输出主增益 (串口 O<0..8> 实时调; 浮点滤波谐振增益大, 取 0.10 防瞬态削波)
volatile float gAccVcaGain = 1.5f;         // accent→VCA 强度 (串口 J<0..8> 实时调)

// 参数 (控制率, 主循环写; ISR 读)
volatile float pCutHz     = 2000.0f;
volatile float pResPct    = 60.0f;
volatile float pDecayMs   = 600.0f;
volatile float pAccent    = 0.5f;      // 0..1
volatile float pEnvModPct = 50.0f;
volatile float pWave      = 0.0f;      // 0=saw 1=square
volatile float pVolume    = 1.0f;
volatile float pTuneSemis = 0.0f;      // 总调音 (半音)
volatile float pDist      = 0.0f;      // 失真量 0..1 (第7旋钮 AIN6 / 串口 a / MIDI CC77)
volatile float pColor     = 0.5f;      // 失真音色 0..1 (暗↔亮; 串口 q / MIDI CC78; 暂无电位器, 用软件调)

// env mod 标定 (core0 算, core1 读)
volatile float envScaler = 1.0f, envOffset = 0.3f;
volatile float envModCutScale = 1.0f;   // ENV MOD gimmick: 基础截止倍率 (ENV MOD 越大越小)

// 音符状态 (仅 core1 音频 ISR 读写)
static volatile float oscFreq = 261.625565f;
static volatile float gCvFreq = 55.0f;      // 当前(经滑音)振荡频率, 供 CV 输出 (produceStage1 每采样更新)
volatile float accentGain = 0.0f;

// ---- core0 -> core1 无锁命令队列 ----
enum { CMD_NONE = 0, CMD_TRIG, CMD_SLIDE, CMD_RELEASE };
struct NoteCmd { uint8_t type; uint8_t accent; float freq; };
#define CMD_QSIZE 16
static volatile NoteCmd cmdQ[CMD_QSIZE];
static volatile uint8_t cmdHead = 0;   // core0 写
static volatile uint8_t cmdTail = 0;   // core1 写

static inline void postCmd(uint8_t type, float freq, uint8_t accent) {
  uint8_t h = cmdHead;
  uint8_t nh = (uint8_t)((h + 1) & (CMD_QSIZE - 1));
  if (nh == cmdTail) return;           // 满则丢
  cmdQ[h].type = type; cmdQ[h].freq = freq; cmdQ[h].accent = accent;
  __dmb();                             // 确保字段先于 cmdHead 对另一个核可见
  cmdHead = nh;                        // 最后发布
}

// 调试
volatile uint32_t isrUs = 0;
volatile uint32_t isrBlocks = 0;
volatile uint32_t cntOsc = 0;

// 示波器采样: core1 音频 ISR 写, core0 显示读
#define SCOPE_N   1024
volatile int16_t  gScope[SCOPE_N];
volatile uint16_t gScopeW = 0;
volatile int      gScopeDiv = 8;   // 每 8 个输出采样存 1 个 (本地屏用)
// PC 模式专用: 每采样都存 (发给电脑由电脑渲染/触发; 仅在 PC 模式采集, 512 点/50ms 以省 CPU/带宽)
#define FAST_N 512
static int16_t    gScopeFast[FAST_N];
volatile uint16_t gScopeFastW = 0;
volatile int      gPcDecim = 1;    // PC 示波器抽点 (1=每采样; N=每 N 个存 1 -> 512 点覆盖 N×10.7ms 窗口)
volatile int32_t  gOutPeak = 0;    // 输出峰值保持 (V 读取并清零)
volatile float    gPrePeak = 0;    // shape 之前的峰值
volatile int      gLcdReinit = 0;  // 串口 Y: 重新初始化屏幕
volatile int      gPcMode = 0;     // 1=PC 软件已连接: 停刷屏, 画笑脸, 上传示波器/参数
volatile int      gPcStream = 0;   // 1=PC 模式且正在上传示波器流 (S0/S1 可暂停, 写/读 pattern 时用)
volatile int      gDrawSmiley = 0; // 请求绘制笑脸
volatile int      gUiForce = 0;    // 1=强制整屏重画 (如从 PC 模式/重初始化返回)
volatile uint32_t gC0BusyUs = 0;   // core0 produceStage1 累计忙时间(µs)
volatile uint32_t gUiUs     = 0;   // 最近一帧 UI 耗时(µs)
volatile uint32_t gUiFrames = 0;   // drawUI 累计调用次数 (PC 模式下应保持不变)

// ================= core0 -> core1 音频流水线 =================
// stage1 (core0): 包络/滑音/截止  ->  stage2 (core1 ISR): 振荡器+HP+滤波器+VCA+输出
#define PIPE_BLKS 6                       // 缓冲块数 (每块 128 = I2S 缓冲; 6 块 ≈ 16ms 余量)
#define PIPE_BLK  128
static float pipeSig [PIPE_BLKS * PIPE_BLK];   // 振荡器经 HP 后的样本
static float pipeAmp [PIPE_BLKS * PIPE_BLK];
static float pipeCut [PIPE_BLKS * PIPE_BLK];
static volatile uint32_t blkProd = 0;     // core0 写
static volatile uint32_t blkCons = 0;     // core1 写

// ---------- USB 音频回传 (裸板无 DAC 时: 把输出 PCM 经 USB CDC 喷给电脑播放) ----------
// core1 ISR 写环形缓冲, core0 loop 批量写 Serial。48kHz/16bit/mono。
volatile int      gAudioLink = 0;         // j<0/1> 开关
volatile int      gUsbAudOn = 0;          // =gAudioLink 且 主机真开了音频口 (1路CDC已连); 否则 ISR 一行都不跑
#define USBAUD_N 8192                     // RP2350: 16KB 环形缓冲
static int16_t    gUsbAud[USBAUD_N];
static volatile uint32_t gUsbAudW = 0;    // core1 写
static volatile uint32_t gUsbAudR = 0;    // core0 读

#if USE_USB_MIDI
Adafruit_USBD_MIDI usb_midi;
bool     midiEverReceived = false;
bool     externalMidi = false;              // 收到过外部 USB MIDI (用于停掉自动测试)
volatile uint32_t gLastMidiMs = 0;          // 最近一次收到 USB MIDI 的时间
volatile uint32_t gMidiClockCnt = 0, gLastClockMs = 0;   // MIDI clock (0xF8) 计数/最近时间
uint8_t  noteStack[16];
uint8_t  noteStackCount = 0;
int8_t   noteStackVel[16];
#endif

// ================= env mod 标定 (Open303 实测映射) =================
static float expToLin(float x, float x0, float x1, float y0, float y1) {
  if (x <= 0.0f) x = 0.0001f;
  float lx = logf(x), l0 = logf(x0), l1 = logf(x1);
  return y0 + (y1 - y0) * (lx - l0) / (l1 - l0);
}
static inline float envModNorm() {   // ENV MOD 电位器已换 B 线性 -> 线性 (原 A 对数锥 audioTaper 不再用)
  return clampf(pEnvModPct, 0.0f, 100.0f) * 0.01f;
}
static void calcEnvModScalerOffset() {
  const float c0 = 3.138152786059267e+002f;
  const float c1 = 2.394411986817546e+003f;
  const float oF = 0.048292930943553f;
  const float oC = 0.294391201442418f;
  const float sLoF = 3.773996325111173f, sLoC = 0.736965594166206f;
  const float sHiF = 4.194548788411135f, sHiC = 0.864344900642434f;
  float e   = envModNorm();
  float c   = clampf(expToLin(pCutHz, c0, c1, 0.0f, 1.0f), 0.0f, 1.0f);
  float sLo = sLoF * e + sLoC;
  float sHi = sHiF * e + sHiC;
  envScaler = ((1.0f - c) * sLo + c * sHi) * ENVMOD_DEPTH;
  envOffset = oF * c + oC;
  // ENV MOD gimmick: ENV MOD 越大 -> 基础截止越低 (服务手册 p8: 改变 bias 压低 VCF 截止)
  envModCutScale = gFidEnvMod ? exp2f(-ENVGMOD_BASE_DROP * e) : 1.0f;
}

// ================= core1: 应用音符命令 (只在音频 ISR 里跑) =================
static void applyNoteCmd(uint8_t ctype, float cfreq, uint8_t cacc) {
  switch (ctype) {
    case CMD_TRIG:
      oscFreq = cfreq;
      gPitchSlew.setState(oscFreq);
      gEnv303.setDecayMs(pDecayMs);
      gEnv303.noteOn(cacc != 0, false, pAccent);
      break;
    case CMD_SLIDE:
      oscFreq = cfreq;
      gEnv303.setDecayMs(pDecayMs);
      gEnv303.noteOn(cacc != 0, true, pAccent);
      break;
    case CMD_RELEASE:
      gEnv303.noteOff();
      break;
    default: break;
  }
}

// ================= core0: stage1 (包络/滑音/截止 -> 流水线缓冲) =================
static void produceStage1() {
  uint32_t t0 = time_us_32();
  int budget = 3;                            // 每次最多补 3 块, 避免长时间占住 core0
  while (budget-- > 0 && (uint32_t)(blkProd - blkCons) < PIPE_BLKS) {
    while (cmdTail != cmdHead) {             // 处理音符命令 (与 stage1 同核 core0)
      uint8_t t = cmdTail;
      uint8_t ct = cmdQ[t].type; float cf = cmdQ[t].freq; uint8_t ca = cmdQ[t].accent;
      cmdTail = (uint8_t)((t + 1) & (CMD_QSIZE - 1));
      applyNoteCmd(ct, cf, ca);
    }
    uint32_t slot = (blkProd % PIPE_BLKS) * PIPE_BLK;
    for (int k = 0; k < PIPE_BLK; k++) {
      float instCut, ampOut;
      // Syrebas 电路级: 双包络 + 控制电流域求和
      gEnv303.process();
      float vcfE = gNoEnv ? 0.0f : gEnv303.vcfEnv;
      float vcaE = gNoEnv ? 1.0f : gEnv303.vcaEnv;
      float accCap = gEnv303.accentCap, accVca = gEnv303.accentVca;
      bool accNote = gEnv303.isAccent && gEnv303.gate;
      float res = clampf(pResPct * 0.01f, 0.0f, 1.0f);
      float envMod = envModNorm();                                  // ENV MOD = 50k A 对数电位器
      float cv_base = log2f(clampf(pCutHz, 20.0f, 20000.0f) / 200.0f);
      float cv_offset = gFidEnvMod ? (-ENVGMOD_BASE_DROP * envMod) : 0.0f;   // 服务手册 p8 gimmick: ENV MOD↑ -> 基础截止↓ (=CUTOFF 往 CCW), CV→电流反对数(exp2)
      float effEM = accNote ? (envMod + (1.0f - envMod) * pAccent * 0.25f) : envMod;   // it3: accent 推量减小
      float cv_envmod = effEM * vcfE * 4.5f;                        // MEG 上扫
      float accSweep = (1.0f - res) * vcfE + res * accCap;          // 谐振第二联: 低res=MEG直击, 高res=1µF平滑
      float cv_accent = accNote ? (pAccent * accSweep * 0.5f) : (pAccent * (res * accCap) * 0.25f);   // it3
      instCut = clampf(200.0f * exp2f(cv_base + cv_offset + cv_envmod + cv_accent) * (1.0f - 0.15f * res), 20.0f, 15000.0f);
      instCut *= gCutDriftLin;                                       // §49/§50 滤波个体偏移 + 温度漂移
      ampOut = clampf(vcaE + accVca * pAccent * 1.3f, 0.0f, 4.0f);   // accent 项始终平滑 (accVca 自身 1.55ms 衰减) -> 去咔哒
      gNoteActive = ((vcaE + accVca) > 0.001f) ? 1 : 0;             // 按 VCA 实际电平判断 (gate 高但包络已衰到 0 也算空闲)
      if (!gNoteActive) ampOut = 0.0f;
      ampOut = gAmpDeClick.process(ampOut);                          // 200Hz 去咔哒 (防音头/音尾跳变)
      ampOut *= gGainDriftLin;                                       // §49/§50 VCA 个体增益 + 温度漂移
      float f = gPitchSlew.process(oscFreq);
      gCvFreq = f;                                     // CV/Gate 输出用 (取滑音后的频率, 不含个体跑调)
      f *= gDetuneLin;                                 // §49/§50 个体跑调 + 温度漂移 (慢变)
      gOsc303.setFreq(f);
      float sig = gOsc303.process(pWave);              // Syrebas 振荡器 (锯齿核+polyBLEP / 方波比较导出)
      if (gFidNoise && gNoiseAmt > 0.0f) sig += gNoiseAmt * NOISE_MAX * analogNoise();   // §51 噪声 (软件旋钮; 经 VCA 门控)
      pipeSig [slot + k] = sig;
      pipeAmp [slot + k] = ampOut;
      pipeCut [slot + k] = instCut;
    }
    __dmb(); blkProd++; __dmb();
  }
  gC0BusyUs += time_us_32() - t0;
}

// ================= core1: stage2 音频 ISR (振荡器+HP+滤波器+VCA+输出) =================
static void __not_in_flash_func(onTransmit)() {
  if (gPauseCore1) { isrBlocks++; return; }    // 写 flash 期间空转 (让 lockout IRQ 能被服务)
  uint32_t t0 = time_us_32();
  float outGain = gMaster * pVolume;
  float dist = pDist;                    // 每 ISR 读一次 (避免每采样 volatile 读取)
  bool distOn = (dist >= DIST_MIN);
  if (distOn) gDist.setParams(dist, pColor);   // 驱动/音色/补偿系数每块算一次

  if (gSineMode) {                       // 纯正弦直出 (诊断 I2S/DAC, 绕过全部 DSP)
    static float sph = 0.0f;
    float sinc = 440.0f / (float)SAMPLE_RATE;
    while (i2s.availableForWrite() > 0) {
      int32_t to = (int32_t)(sinf(2.0f * PI_F * sph) * 2000.0f);
      sph += sinc; if (sph >= 1.0f) sph -= 1.0f;
      int16_t o16 = (int16_t)to;
      i2s.write16(o16, o16);
      if (gUsbAudOn) { uint32_t w = gUsbAudW; if ((uint32_t)(w - gUsbAudR) < USBAUD_N) { gUsbAud[w & (USBAUD_N - 1)] = o16; __dmb(); gUsbAudW = w + 1; } }
    }
    isrUs += time_us_32() - t0; isrBlocks++;
    return;
  }

  static uint32_t consIdx = 0;
  while (i2s.availableForWrite() > 0) {
    float sig, ampOut, instCut;
    uint32_t bp = blkProd; __dmb();
    if (blkCons < bp) {                  // core0 已备好的样本
      uint32_t slot = (blkCons % PIPE_BLKS) * PIPE_BLK + consIdx;
      sig = pipeSig[slot]; ampOut = pipeAmp[slot]; instCut = pipeCut[slot];
      if (++consIdx >= PIPE_BLK) { consIdx = 0; blkCons++; }
    } else {                             // 欠载 (core0 没跟上): 静音, 不推进计数
      sig = 0.0f; ampOut = 0.0f; instCut = pCutHz;
    }

    // Syrebas 滤波器每采样接收 cutoff/res, 无需系数重算
    float v = sig;
    if (!gNoFilter) {
      v = gFilter303.process(sig, instCut, pResPct * 0.01f);
      if (v != v || v > 1.0e6f || v < -1.0e6f) { gFilter303.reset(); v = 0.0f; }   // NaN/发散保护
    }
    if (gFidLowPeak && !gNoFilter) v = gLowPeak.process(v);   // ~8Hz 第二低频峰 (bit5, 默认关, 贵)
    if (gResBassComp && !gNoFilter) v = gResBass.process(v);  // 谐振低频补偿 (bit6, 默认关=原版)
    if (distOn && !gDistPost) v = gDist.process(v);            // 失真前置: VCF->失真->VCA
    { float cg = fastTanh(ampOut * 1.2f);   // BA662 控制电流 (随 ampOut 平滑; ampOut 空闲已门控去咔哒=0 -> cg=0 静音, 无硬切)
      float x = v * cg;
      v = (x > 0.0f) ? fastTanh(x * 1.1f) : fastTanh(x * 0.9f); }                 // VCA(BA662) 输出非对称饱和
    if (distOn && gDistPost) v = gDist.process(v);             // 失真后置: VCF->VCA->失真 (TD-3, 默认)
    v *= outGain;
    { float vp = v < 0 ? -v : v; if (vp > gPrePeak) gPrePeak = vp; }
    if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;

    int32_t out = (int32_t)(v * 32767.0f);
    if (out > 32767) out = 32767; else if (out < -32768) out = -32768;
    { int32_t a = out < 0 ? -out : out; if (a > gOutPeak) gOutPeak = a; }
    i2s.write16((int16_t)out, (int16_t)out);
    if (gUsbAudOn) {                                     // USB 音频回传 (主机开了音频口才进; 满则丢, 不阻塞 ISR)
      uint32_t w = gUsbAudW;
      if ((uint32_t)(w - gUsbAudR) < USBAUD_N) { gUsbAud[w & (USBAUD_N - 1)] = (int16_t)out; __dmb(); gUsbAudW = w + 1; }
    }
    { static int sc = 0; if (++sc >= gScopeDiv) { sc = 0; int16_t sv = (int16_t)out;
        gScope[gScopeW & (SCOPE_N-1)] = sv; gScopeW++; } }
    if (gPcMode) { static int dc = 0; if (++dc >= gPcDecim) { dc = 0; gScopeFast[gScopeFastW & (FAST_N-1)] = (int16_t)out; gScopeFastW++; } }  // PC 示波器 (抽点)
    cntOsc += OS;
  }
  isrUs += time_us_32() - t0;
  isrBlocks++;
}

// ================= 音符 (core0: 算频率并投递命令给 core1) =================
static inline float noteToFreq(uint8_t n) { return 440.0f * powf(2.0f, ((float)n - 69.0f + pTuneSemis) / 12.0f); }
static uint16_t gNoteLog[240]; static volatile uint8_t gNoteLogN = 0; static volatile uint32_t gNoteLogT0 = 0;
static void noteLogPush(uint8_t type, uint8_t note) {
  if (gNoteLogN == 0) gNoteLogT0 = millis();
  if (gNoteLogN < 240) { gNoteLog[gNoteLogN] = ((uint16_t)type << 8) | note; gNoteLogN++; }
}
static void triggerNote(uint8_t n, bool accent) { noteLogPush(0, n); postCmd(CMD_TRIG, noteToFreq(n), accent ? 1 : 0); }
static void slideToNote(uint8_t n, bool accent) { noteLogPush(1, n); postCmd(CMD_SLIDE, noteToFreq(n), accent ? 1 : 0); }
static void releaseNote() { postCmd(CMD_RELEASE, 0.0f, 0); }

#if USE_USB_MIDI
static uint8_t midiLog[600];
static volatile int midiLogLen = 0;
volatile int gMidiLogOn = 0;
static void midiNoteOn(uint8_t n, uint8_t vel) {
  bool accent = vel > ACCENT_VEL;   // 重音 = 力度 > 95 (DAW 与内置音序器一致; 内置注入 120=重音/64=普通)
  if (noteStackCount == 0) triggerNote(n, accent);
  else                     slideToNote(n, accent);
  // 去重后入栈
  for (uint8_t i = 0; i < noteStackCount; i++)
    if (noteStack[i] == n) { noteStack[i] = noteStack[--noteStackCount]; break; }
  if (noteStackCount < sizeof(noteStack)) { noteStack[noteStackCount] = n; noteStackVel[noteStackCount] = vel; noteStackCount++; }
  midiEverReceived = true;
}
static void midiNoteOff(uint8_t n) {
  bool found = false;
  for (uint8_t i = 0; i < noteStackCount; i++)
    if (noteStack[i] == n) { noteStack[i] = noteStack[--noteStackCount]; found = true; break; }
  if (!found) return;
    if (noteStackCount > 0) slideToNote(noteStack[noteStackCount - 1], noteStackVel[noteStackCount - 1] > ACCENT_VEL);
  else                    releaseNote();
}
static void handleCC(uint8_t cc, uint8_t val) {
  float v = val / 127.0f;
  switch (cc) {
    case 74: pCutHz = CUT_MIN_HZ * powf(CUT_MAX_HZ / CUT_MIN_HZ, v); calcEnvModScalerOffset(); break;
    case 71: pResPct = v * 100.0f; break;
    case 72: pEnvModPct = v * 100.0f; calcEnvModScalerOffset(); break;
    case 73: pDecayMs = DECAY_MIN_MS * powf(DECAY_MAX_MS / DECAY_MIN_MS, v); break;
    case 75: pAccent = v; break;
    case 76: pWave = v; break;
    case 77: pDist = v; break;                 // DIST 失真量
    case 78: pColor = v; break;                // COLOR 失真音色
    case 7:  pVolume = v; break;
    default: break;
  }
}
static void dawTakeOver() {   // DAW 绝对优先: 外部 MIDI 一发音符 -> 停掉内置音序器 (停后需手动再按播放/按键才恢复)
  if (gSeqPlay) { gSeqPlay = 0; noteStackCount = 0; releaseNote(); }
}
static void midiTask() {
  uint8_t pkt[4];
  while (usb_midi.readPacket(pkt)) {
    uint8_t status = pkt[1];
    if (status < 0x80) continue;
    if (gMidiLogOn && midiLogLen < 597) { midiLog[midiLogLen++] = status; midiLog[midiLogLen++] = pkt[2]; midiLog[midiLogLen++] = pkt[3]; }
    externalMidi = true;
    if (status == 0xF8) { gMidiClockCnt++; gLastClockMs = millis(); continue; }   // MIDI clock
    uint8_t type = status & 0xF0, chan = status & 0x0F;
    if (MIDI_CHANNEL && (chan + 1) != MIDI_CHANNEL) continue;
    switch (type) {
      case 0x90: gLastMidiMs = millis(); if (pkt[3] == 0) midiNoteOff(pkt[2]); else { dawTakeOver(); midiNoteOn(pkt[2], pkt[3]); } break;
      case 0x80: gLastMidiMs = millis(); midiNoteOff(pkt[2]); break;
      case 0xB0:
        if (pkt[2] == 120 || pkt[2] == 123) { noteStackCount = 0; releaseNote(); }
        else handleCC(pkt[2], pkt[3]);
        break;
      default: break;
    }
  }
}
#endif

// ================= 用户 pattern 存储 (QSPI flash, 掉电保存) =================
// 最多 4 个 pattern (A..D), 每个 8..16 步。每步: note(MIDI 0..127), flags:
//   bit0=accent  bit1=slide  bit2=rest  bit3=tie
// 上传后写 flash 末扇区; 开机读回。无外部 MIDI 时内置音序器循环播放 (空 pattern 跳过)。
#define PAT_MAX     4
#define STEPS_MAX   16
// ---- flash 布局: 运行时探测芯片容量 (RP2350 最大 16MB) ----
// ⚠️ 不能信 flash_devinfo_get_cs_size(0): 编译期一旦定义了 PICO_FLASH_SIZE_BYTES, 它会直接返回该值、
//    无视 OTP (见 pico-sdk flash.c)。故用 JEDEC RDID(0x9F) 读真实芯片容量作为首选。
static uint32_t gFlashBytesCache = 0;
static int      gFlashSrc = 0;              // 来源: 1=RDID 2=devinfo 3=编译期 9=串口强制
static uint8_t  gRdid[3] = { 0, 0, 0 };     // 原始 JEDEC RDID (厂商/类型/容量码), 供 T 回报核对
static uint32_t flashSizeBytes() {
  if (gFlashBytesCache) return gFlashBytesCache;
  uint32_t b = 0; int src = 0;
  { // 1) JEDEC RDID 0x9F: 需 1 命令 + 3 响应字节 (容量在 r[3], 厂商在 r[1]); size = 2^(r[3]&0x1F)
    uint8_t t[4] = { 0x9F, 0, 0, 0 }, r[4] = { 0, 0, 0, 0 };
    flash_do_cmd(t, r, 4);
    gRdid[0] = r[1]; gRdid[1] = r[2]; gRdid[2] = r[3];
    if (r[1] != 0x00 && r[1] != 0xFF) {
      uint32_t cap = (r[3] & 0x1Fu) ? (1u << (r[3] & 0x1Fu)) : 0;   // 0x18=16M 0x17=8M 0x16=4M 0x15=2M
      if (cap >= (1024u * 1024u) && cap <= (16u * 1024u * 1024u)) { b = cap; src = 1; }
    }
  }
  if (!b) {  // 2) 回退: flash_devinfo (编译期覆盖下 = 编译期值)
    uint32_t d = flash_devinfo_size_to_bytes(flash_devinfo_get_cs_size(0));
    if (d >= (1024u * 1024u) && d <= (16u * 1024u * 1024u)) { b = d; src = 2; }
  }
  if (!b) { b = PICO_FLASH_SIZE_BYTES; src = 3; }   // 3) 最后兜底: 编译期值
  if (b < (1024u * 1024u))  b = 1024u * 1024u;      // 夹取 [1MB, 16MB] (绝不越界到板外)
  if (b > (16u * 1024u * 1024u)) b = 16u * 1024u * 1024u;
  // ★ 关键: runtime FLASH_DEVINFO 被编译期 PICO_FLASH_SIZE_BYTES 覆盖(如 16MB), 而 bootrom 的
  //   flash 擦/写会据它选地址宽度 (16MB=4字节地址, 8MB=3字节) -> 与实际芯片不符 -> 擦除卡死。
  //   这里把 runtime devinfo 改回真实容量 (只影响本次运行的 RAM 副本, 不写 OTP)。
  if (b && ((b & (b - 1u)) == 0u)) flash_devinfo_set_cs_size(0, flash_devinfo_bytes_to_size(b));
  gFlashBytesCache = b; gFlashSrc = src;
  return b;
}
// ================= 自建 QMI flash 驱动 (flash 命令时钟与主频解耦; 任何板子/任何主频都能读写) =================
#define FLASH_CMD_MAX_KHZ 25000u     // flash 命令(擦/写)时钟上限 25MHz (任何 SPI NOR 都稳)
#define QTX_OE 0x00080000u           // QMI_DIRECT_TX.OE (输出使能)
static volatile uint8_t gFsr = 0;    // 最近一次状态寄存器 (诊断)
static void __not_in_flash_func(qmiSetDirectClk)(void) {   // direct 通道时钟 = clk_sys / CLKDIV
  uint32_t khz = gOcKHz ? gOcKHz : 150000u;
  uint32_t dc = (khz + FLASH_CMD_MAX_KHZ - 1u) / FLASH_CMD_MAX_KHZ;
  if (dc < 2u) dc = 2u; if (dc > 255u) dc = 255u;
  qmi_hw->direct_csr = (qmi_hw->direct_csr & ~QMI_DIRECT_CSR_CLKDIV_BITS) | ((dc << QMI_DIRECT_CSR_CLKDIV_LSB) & QMI_DIRECT_CSR_CLKDIV_BITS);
}
static void __not_in_flash_func(qmiXf)(const uint8_t* tx, uint8_t* rx, size_t n, uint32_t fl) {   // QMI direct 收发
  qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
  qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
  size_t tr = n, rr = n;
  while (tr || rr) {
    uint32_t f = qmi_hw->direct_csr;
    if (!(f & QMI_DIRECT_CSR_TXFULL_BITS) && tr) { qmi_hw->direct_tx = fl | (*tx++); tr--; }
    if (!(f & QMI_DIRECT_CSR_RXEMPTY_BITS) && rr) { *rx++ = (uint8_t)qmi_hw->direct_rx; rr--; }
  }
  qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;
  qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
}
static uint8_t __not_in_flash_func(qmiStatus)(void) { uint8_t s[2] = {0x05, 0}, r[2] = {0, 0}; qmiXf(s, r, 2, 0); return r[1]; }
static void __not_in_flash_func(qmiWaitDone)(void) {   // 轮询 WIP=0 (最长 1s; time_us_32 是内联寄存器读, XIP 关闭时安全)
  uint32_t t0 = time_us_32();
  while ((uint32_t)(time_us_32() - t0) < 1000000u) { gFsr = qmiStatus(); if (!(gFsr & 1)) return; }
}
static void __not_in_flash_func(myFlashErase)(uint32_t off) {   // 擦一个 4KB 扇区 (direct 0x20)
  uint32_t svT = qmi_hw->m[0].timing, svF = qmi_hw->m[0].rfmt, svC = qmi_hw->m[0].rcmd;   // 存快读配置
  rom_connect_internal_flash(); rom_flash_exit_xip();
  qmiSetDirectClk();
  { uint8_t w[1] = {0x06}, r[1]; qmiXf(w, r, 1, QTX_OE); }
  { uint8_t t[4] = {0x20, (uint8_t)(off >> 16), (uint8_t)(off >> 8), (uint8_t)off}, r[4]; qmiXf(t, r, 4, QTX_OE); }
  qmiWaitDone();
  rom_flash_flush_cache(); rom_flash_enter_cmd_xip();
  // ★ 恢复快读 XIP (rom_flash_enter_cmd_xip 会留成 03h 慢读, 高主频下不稳)
  qmi_hw->m[0].timing = svT; qmi_hw->m[0].rfmt = svF; qmi_hw->m[0].rcmd = svC;
}
static void __not_in_flash_func(myFlashProgram)(uint32_t off, const uint8_t* data, uint32_t n) {   // 页写 (自动按 256B 分页)
  uint32_t svT = qmi_hw->m[0].timing, svF = qmi_hw->m[0].rfmt, svC = qmi_hw->m[0].rcmd;   // 存快读配置
  for (uint32_t pg = 0; pg < n; pg += 256) {
    uint32_t m = (n - pg < 256u) ? (n - pg) : 256u;
    // 1) 写使能 0x06 (direct)
    rom_connect_internal_flash(); rom_flash_exit_xip();
    qmiSetDirectClk();
    { uint8_t w[1] = {0x06}, r[1]; qmiXf(w, r, 1, QTX_OE); }
    rom_flash_flush_cache(); rom_flash_enter_cmd_xip();
    qmi_hw->m[0].timing = svT; qmi_hw->m[0].rfmt = svF; qmi_hw->m[0].rcmd = svC;
    // 2) 写窗口: 使能 M0 可写, 数据写【无缓存别名 0x14000000】(带缓存的 0x10000000 只读会被吃掉)
    uint32_t savedTiming = qmi_hw->m[0].timing;
    qmi_hw->m[0].wfmt = QMI_M0_WFMT_RESET;    // 默认 = 单线 02h 页写, 24 位地址
    qmi_hw->m[0].wcmd = QMI_M0_WCMD_RESET;
    { uint32_t khz = gOcKHz ? gOcKHz : 150000u; uint32_t cd = (khz + FLASH_CMD_MAX_KHZ - 1u) / FLASH_CMD_MAX_KHZ; if (cd < 2u) cd = 2u; if (cd > 255u) cd = 255u;
      qmi_hw->m[0].timing = (qmi_hw->m[0].timing & ~0xFFu) | (cd & 0xFFu); }
    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M0_BITS;
    __dmb();
    volatile uint8_t* dst = (volatile uint8_t*)(XIP_NOCACHE_NOALLOC_BASE + off + pg);
    for (uint32_t i = 0; i < m; i++) dst[i] = data[pg + i];
    __dmb();
    xip_ctrl_hw->ctrl &= ~XIP_CTRL_WRITABLE_M0_BITS;
    qmi_hw->m[0].timing = savedTiming;
    // 3) 等完成
    rom_connect_internal_flash(); rom_flash_exit_xip(); qmiSetDirectClk(); qmiWaitDone();
    rom_flash_flush_cache(); rom_flash_enter_cmd_xip();
    qmi_hw->m[0].timing = svT; qmi_hw->m[0].rfmt = svF; qmi_hw->m[0].rcmd = svC;   // ★ 恢复快读 XIP
  }
}
static void flashTestCmd() {   // 上板验证: 擦专用扇区 -> 写 512B(2页) -> 读回
  uint32_t off = flashSizeBytes() - 0x4000u;
  static uint8_t page[512];
  for (int i = 0; i < 512; i++) page[i] = (uint8_t)(0x5Au ^ (i * 7) ^ 0x33);
  uint32_t khz = clock_get_hz(clk_sys) / 1000u;
  const uint8_t* p = (const uint8_t*)(XIP_BASE + off);
  Serial.print("#FTEST A init:"); for (int i = 0; i < 8; i++) { Serial.print(' '); Serial.print(p[i], HEX); } Serial.println();
  gPauseCore1 = 1; if (gCore1Started) multicore_lockout_start_blocking();
  myFlashErase(off);
  if (gCore1Started) multicore_lockout_end_blocking(); gPauseCore1 = 0;
  Serial.print("#FTEST B erased:"); for (int i = 0; i < 8; i++) { Serial.print(' '); Serial.print(p[i], HEX); } Serial.println();
  gPauseCore1 = 1; if (gCore1Started) multicore_lockout_start_blocking();
  myFlashProgram(off, page, 512);
  if (gCore1Started) multicore_lockout_end_blocking(); gPauseCore1 = 0;
  int bad = -1; for (int i = 0; i < 512; i++) if (p[i] != page[i]) { bad = i; break; }
  Serial.print("#FTEST C prog:"); for (int i = 0; i < 8; i++) { Serial.print(' '); Serial.print(p[i], HEX); }
  Serial.print("  exp:"); for (int i = 0; i < 8; i++) { Serial.print(' '); Serial.print(page[i], HEX); }
  Serial.print("  bad="); Serial.print(bad); Serial.print(" khz="); Serial.println(khz);
}
#define IMG_CAP_BYTES     ((uint32_t)((flashSizeBytes() > (0x00100000u + 0x1000u + 0x3000u)) ? (flashSizeBytes() - 0x3000u - 0x00100000u - 0x1000u) : 0u))
#define PAT_FLASH_OFFSET  (flashSizeBytes() - 0x1000u)        // 16MB flash 的最后 4KB 扇区
#define PAT_MAGIC         0x50303332u        // "P032" (v2: 增加 playMask 字段)
struct PatStep { uint8_t note; uint8_t flags; };
struct PatStore {
  uint32_t magic;
  uint16_t bpm10;                 // BPM ×10
  uint8_t  nPat;                  // 非空 pattern 数 (0..4)
  uint8_t  playMask;              // 内置音序器播放掩码: bit i = pattern i 是否参与播放
  uint8_t  len[PAT_MAX];          // 每个 pattern 步数 (0=空/跳过, 1..16)
  PatStep  st[PAT_MAX][STEPS_MAX];
  uint32_t chk;
};
static PatStore gPat;
static uint8_t  seqPat = 0, seqStep = 0;
static uint8_t  seqShowPat = 0, seqShowStep = 0; // 最近播放的步 (发给 PC 做播放头)
static bool     seqPrevSlide = false;           // TD-3/303: 上一步有 SLIDE -> 本步滑入 (不重触发)
static bool     seqWantGateOff = false;         // 本步是普通音 -> 步内提前关 gate (303 gate≈0.87步)
static bool     seqGatePending = false;
static uint32_t seqGateOffMs = 0;
static uint32_t seqStepLenMs = 0, seqLastStepMs = 0;
static uint32_t seqNextMs = 0;
static uint32_t seqClockLast = 0;
#define GATE_FRAC  0.50f                        // 303 普通 gate = 3/6 时钟脉冲 (实测 49.96~55.8%, 标称 50%)

static uint32_t patChk() {
  const uint8_t* b = (const uint8_t*)&gPat; uint32_t s = 2166136261u;
  for (size_t i = 0; i < offsetof(PatStore, chk); i++) { s ^= b[i]; s *= 16777619u; }
  return s;
}
static void patRecount() {
  uint8_t n = 0;
  for (int i = 0; i < PAT_MAX; i++) if (gPat.len[i] >= 1 && gPat.len[i] <= STEPS_MAX) n++;
  gPat.nPat = n;
}
static bool patEnabled(int p) { return (p >= 0 && p < PAT_MAX) && ((gPat.playMask >> p) & 1); }
static bool patPlayable(int p) { return patEnabled(p) && gPat.len[p] >= 1 && gPat.len[p] <= STEPS_MAX; }
static bool patAnyPlayable() { for (int p = 0; p < PAT_MAX; p++) if (patPlayable(p)) return true; return false; }
static void patDefaults() {
  memset(&gPat, 0, sizeof(gPat));
  gPat.magic = PAT_MAGIC; gPat.bpm10 = 1206; gPat.nPat = 0;
  gPat.playMask = 0x0F;                 // 默认 4 个 pattern 全部参与播放 (空 pattern 仍跳过)
}
static void patLoad() {
  memcpy(&gPat, (const void*)(XIP_BASE + PAT_FLASH_OFFSET), sizeof(gPat));
  if (gPat.magic != PAT_MAGIC || gPat.nPat > PAT_MAX || gPat.chk != patChk()) patDefaults();
}
static void patSave() {
  gPat.magic = PAT_MAGIC; gPat.chk = patChk();
  static uint8_t page[256];
  const uint8_t* src = (const uint8_t*)&gPat; uint32_t n = sizeof(gPat);
  // 必须停 core1: onTransmit 虽在 RAM, 但 I2S 库的 DMA 中断处理在 flash, XIP 关闭期间会崩。
  // (不改这条 -> 写 flash 后看门狗复位; 见日志 §42.12)
  gPauseCore1 = 1;                              // core1 音频 ISR 先空转 (否则 lockout IRQ 被饿死 -> flash 写死锁)
  if (gCore1Started) multicore_lockout_start_blocking();   // 暂停 core1 (它从 flash 取指)
  uint32_t ints = save_and_disable_interrupts(); // core0 中断也可能从 flash 取指 -> 必须关
  myFlashErase(PAT_FLASH_OFFSET);
  for (uint32_t off = 0; off < n; off += 256) {
    uint32_t m = (n - off < 256) ? (n - off) : 256;
    memset(page, 0xFF, 256); memcpy(page, src + off, m);
    myFlashProgram(PAT_FLASH_OFFSET + off, page, 256);
  }
  restore_interrupts(ints);
  if (gCore1Started) multicore_lockout_end_blocking();
  gPauseCore1 = 0;
}

// ================= 持久设置 (存 flash: 超频档/解锁/过采样/看门狗 + 三旋钮) =================
// 与 pattern 分开的独立 4KB 扇区 (16MB flash 倒数第 2 个扇区)。上电读取 -> 生效; 改设置时写回。
// 这样"掉电也保留"(之前存 scratch 只跨软复位, 一掉电就回到默认)。
#define SET_FLASH_OFFSET  (flashSizeBytes() - 0x2000u)        // 倒数第 2 个 4KB 扇区 (pattern 在 0x00FFF000)
#define SET_MAGIC         0x50305345u        // "P0SE"
struct SetStore {
  uint32_t magic;
  uint8_t  ocIdx, unlock, filtOS, wdt;   // 超频档 / 解锁 / 过采样 / 看门狗
  uint8_t  noise7, var7, temp7;          // 三旋钮 (0..127)
  uint8_t  rsv[5];
  uint32_t chk;
};
static SetStore gSet;
static volatile int      gSetDirty = 0;      // 有变化待写 flash (去抖)
static volatile uint32_t gSetChangeMs = 0;

static uint32_t setChk() {
  const uint8_t* b = (const uint8_t*)&gSet; uint32_t s = 2166136261u;
  for (size_t i = 0; i < offsetof(SetStore, chk); i++) { s ^= b[i]; s *= 16777619u; }
  return s;
}
static void setDefaults() {
  memset(&gSet, 0, sizeof(gSet));
  gSet.magic = SET_MAGIC;
  gSet.ocIdx = (uint8_t)OC_DEFAULT_IDX; gSet.unlock = 0; gSet.filtOS = 8; gSet.wdt = 1;
  gSet.chk = setChk();
}
static bool setLoad() {
  memcpy(&gSet, (const void*)(XIP_BASE + SET_FLASH_OFFSET), sizeof(gSet));
  if (gSet.magic != SET_MAGIC || gSet.chk != setChk()) { setDefaults(); return false; }
  return true;
}
static void setSave() {                       // 写 flash (短暂停 core1 ~数十 ms)
  gSet.magic = SET_MAGIC; gSet.chk = setChk();
  static uint8_t page[256];
  gPauseCore1 = 1;                            // core1 音频 ISR 先空转, 否则满载时 lockout IRQ 被饿死 -> flash 写死锁
  if (gCore1Started) multicore_lockout_start_blocking();
  uint32_t ints = save_and_disable_interrupts();
  myFlashErase(SET_FLASH_OFFSET);
  memset(page, 0xFF, 256); memcpy(page, &gSet, sizeof(gSet));
  myFlashProgram(SET_FLASH_OFFSET, page, 256);
  restore_interrupts(ints);
  if (gCore1Started) multicore_lockout_end_blocking();
  gPauseCore1 = 0;
}
static void setTouched() { gSetDirty = 1; gSetChangeMs = millis(); }   // 去抖写 (拖旋钮时别狂写)
static void setSyncFromGlobals() {
  gSet.ocIdx = (uint8_t)gOcIdx; gSet.unlock = (uint8_t)gOcUnlock;
  gSet.filtOS = (uint8_t)gFiltOS; gSet.wdt = (uint8_t)gWdtEnabled;
  gSet.noise7 = (uint8_t)(gNoiseAmt * 127.0f + 0.5f);
  gSet.var7   = (uint8_t)(gVarAmt   * 127.0f + 0.5f);
  gSet.temp7  = (uint8_t)(gTempAmt  * 127.0f + 0.5f);
}

// 内置音序器: 播放一个"启用且非空"的 pattern 的一步 (未启用的 pattern 直接跳过)
static void seqStepPlay() {
  for (int guard = 0; guard < PAT_MAX && !patPlayable(seqPat); guard++) { seqStep = 0; seqPat = (seqPat + 1) % PAT_MAX; }
  if (!patPlayable(seqPat)) return;
  PatStep sp = gPat.st[seqPat][seqStep];
  seqShowPat = seqPat; seqShowStep = seqStep;     // 播放头位置 (发给 PC)
  uint8_t fl = sp.flags;
  bool isRest = (fl & 4) != 0, isTie = (fl & 8) != 0, isSlide = (fl & 2) != 0;
  // 参照 Behringer TD-3 (subatomicglue/behringer-td3-editor td3.js 的 Sequencer.update):
  //   非滑音步: 先关掉上一步 -> 再触发本步 (重触发, gate = 一步)
  //   上一步是滑音: 不关, 本步滑入 (legato, 重叠 -> glide)
  bool glide = seqPrevSlide;
  if (!glide && !isTie) { noteStackCount = 0; releaseNote(); }   // TIE 步不关 (保持上一步)
  if (isRest) { noteStackCount = 0; releaseNote(); }   // rest
  else if (isTie) { /* TIE: 保持上一步 (不重触发、音高不变) —— 实测原版 303: tie 不改变 pitch CV */ }
  else {
    if (!glide) noteStackCount = 0;                    // 非滑入 -> 重触发 (滑入时留栈 -> glide)
    midiNoteOn(sp.note, (fl & 1) ? 120 : 64);
  }
  seqPrevSlide = isSlide && !isRest;                   // 本步 SLIDE -> 下一步滑入
  // 若“下一步”是 TIE, 本步不要提前关 gate (否则音在 TIE 前就断了)
  bool nextTie = false;
  if (seqStep + 1 < gPat.len[seqPat]) nextTie = (gPat.st[seqPat][seqStep + 1].flags & 8) != 0;
  else { int np = seqPat; for (int g = 0; g < PAT_MAX; g++) { np = (np + 1) % PAT_MAX; if (patPlayable(np)) break; }
         nextTie = patPlayable(np) && (gPat.st[np][0].flags & 8) != 0; }
  seqWantGateOff = (!isRest && !isTie && !isSlide && !nextTie);   // 普通音 -> 步内提前关 gate (staccato)
  if (++seqStep >= gPat.len[seqPat]) {
    seqStep = 0;                                              // 步进到尾 -> 找下一个启用的 pattern
    for (int guard = 0; guard < PAT_MAX; guard++) { seqPat = (seqPat + 1) % PAT_MAX; if (patPlayable(seqPat)) break; }
  }
}
static void seqTask() {
  if (!gSeqPlay || !patAnyPlayable()) return;
  if (millis() - gLastMidiMs < 1000) return;                      // DAW 在放音符 -> 让位
  // 步内提前关 gate (303 普通 gate ≈ 0.87 步 -> staccato)
  if (seqGatePending && (int32_t)(millis() - seqGateOffMs) >= 0) { releaseNote(); seqGatePending = false; }
  bool useClock = (millis() - gLastClockMs < 500);
  if (useClock) {
    uint32_t c = gMidiClockCnt;
    if (c - seqClockLast >= 6) { seqClockLast += 6; seqStepPlay();          // 24ppqn / 6 = 16 分
      uint32_t now = millis(); if (seqLastStepMs) seqStepLenMs = now - seqLastStepMs; seqLastStepMs = now;
      if (seqWantGateOff && seqStepLenMs) { seqGatePending = true; seqGateOffMs = now + (uint32_t)(seqStepLenMs * GATE_FRAC); }
      else seqGatePending = false;
    }
  } else {
    if (millis() >= seqNextMs) {
      uint32_t stepMs = (uint32_t)(60000.0f / (gPat.bpm10 / 10.0f) / 4.0f);
      seqNextMs = millis() + stepMs;
      seqStepLenMs = stepMs;
      seqStepPlay();
      if (seqWantGateOff) { seqGatePending = true; seqGateOffMs = millis() + (uint32_t)(stepMs * GATE_FRAC); }
      else seqGatePending = false;
    }
  }
}


// ================= 串口命令 (调试/调参) =================
// 每条命令以换行结束: c<Hz> r<%> d<ms> e<%> x<0..127> w<0..127> v<0..127>
//                      n<note> 普通音  N<note> 重音  f 松音  m<0|1|2> 测试模式  s 状态
// CPU 占用报告 (Q 命令 / 周期自动打印 I1 共用)
static void reportCpu() {
  static uint32_t tPrev = 0, isrPrev = 0, c0Prev = 0;
  uint32_t now = time_us_32();
  uint32_t dt = now - tPrev; tPrev = now;
  uint32_t isr = isrUs;    uint32_t disr = isr - isrPrev; isrPrev = isr;
  uint32_t c0 = gC0BusyUs; uint32_t dc0 = c0 - c0Prev; c0Prev = c0;
  Serial.print("core1="); if (dt) Serial.print(100.0f*disr/dt, 1); else Serial.print(0.0f,1);
  Serial.print("% core0="); if (dt) Serial.print(100.0f*dc0/dt, 1); else Serial.print(0.0f,1);
  Serial.print("% ui="); Serial.print(gUiUs);
  Serial.print("us uiN="); Serial.print(gUiFrames);
  Serial.print(" MHz="); Serial.print(clock_get_hz(clk_sys) / 1000000.0f, 1);
  Serial.print(" temp="); Serial.print(analogReadTemp(3.3f), 1);
  Serial.print(" dt="); Serial.print(dt); Serial.println("us");
}
static void seqCmd(int m) {                        // X0/X1/X2 播放控制 (软件 串口 与 板载 USER 键 共用)
  if (m == 0) { gSeqPlay = 0; noteStackCount = 0; releaseNote(); seqGatePending = false; }   // 暂停: 停在当前步
  else if (m == 1) {                               // 从"当前步"(最近播放的步)开始
    seqPat = (uint8_t)(seqShowPat % PAT_MAX); seqStep = seqShowStep;
    if (seqStep >= gPat.len[seqPat]) seqStep = 0;
    for (int g=0; g<PAT_MAX && !patPlayable(seqPat); g++) { seqStep = 0; seqPat = (seqPat+1)%PAT_MAX; }
    gSeqPlay = 1;
  } else {                                         // X2: 回到开头
    gSeqPlay = 0; seqStep = 0; seqPat = 0;
    for (int g=0; g<PAT_MAX && !patPlayable(seqPat); g++) seqPat = (seqPat+1)%PAT_MAX;
    seqShowPat = seqPat; seqShowStep = 0;
  }
}
// 板载 USER 键: 去抖; 单击=播放/暂停(从暂停那步继续); 连续两下=回到开头
static void userKeyTask() {
  static int lastRaw = 1, stable = 1; static uint32_t tChg = 0;
  static bool pending = false; static uint32_t tPress = 0;
  uint32_t now = millis();
  if (gKeySimPress && gKeySimReleaseMs && (int32_t)(now - gKeySimReleaseMs) >= 0) gKeySimPress = 0;   // 测试钩子自动松开
  int raw = gKeySimPress ? 0 : ((gpio_get(PIN_USER) && gpio_get(PIN_USER2)) ? 1 : 0);   // 1=松开, 0=按下 (GP8 或 GP23 任一按下即算)
  if (raw != lastRaw) { lastRaw = raw; tChg = now; }
  if (now - tChg >= 25 && stable != raw) {          // ~25ms 去抖
    stable = raw;
    if (stable == 0) {                              // 按下沿
      if (pending && (now - tPress) < 350) { pending = false; seqCmd(2); }   // 双击 -> 回头
      else { pending = true; tPress = now; }        // 记下, 等判定单击
    }
  }
  if (pending && (now - tPress) >= 350) {           // 单击 -> 切换 播放/暂停
    pending = false;
    if (gSeqPlay) seqCmd(0); else seqCmd(1);
  }
}
// ================= V1.2: AD5664 (U8) 四路 CV/Gate 输出 =================
// 输出: 0=A CVOUT / 1=B GATEOUT / 2=C ACCENTOUT / 3=D FTENVOUT ; VREF=5V -> code 0..65535 = 0..5V
volatile int   gDacEnable = 1;        // 1=启用 CV/Gate 输出 (软开关, 便于诊断)
volatile float gCvBaseHz  = 55.0f;    // CV 0V 对应频率 (1V/oct; 默认 A1=55Hz, 可调)
static void dacWrite(uint8_t ch, uint16_t code) {   // AD5664 24bit: cmd=011(写并更新), 地址 ch, 16bit 数据
  uint32_t w = ((uint32_t)0x3u << 19) | ((uint32_t)(ch & 7u) << 16) | (uint32_t)code;
  digitalWrite(PIN_DAC_SYNC, LOW);
  for (int i = 23; i >= 0; i--) {
    digitalWrite(PIN_DAC_DIN, (w >> i) & 1u);
    digitalWrite(PIN_DAC_SCLK, HIGH);
    digitalWrite(PIN_DAC_SCLK, LOW);
  }
  digitalWrite(PIN_DAC_SYNC, HIGH);
}
static void dacInit() {
  pinMode(PIN_DAC_SYNC, OUTPUT); digitalWrite(PIN_DAC_SYNC, HIGH);
  pinMode(PIN_DAC_SCLK, OUTPUT); digitalWrite(PIN_DAC_SCLK, LOW);
  pinMode(PIN_DAC_DIN,  OUTPUT); digitalWrite(PIN_DAC_DIN,  LOW);
  for (int ch = 0; ch < 4; ch++) dacWrite((uint8_t)ch, 0);
}
static inline uint16_t voltsToCode(float v) { return (uint16_t)clampf(v * (65535.0f / 5.0f), 0.0f, 65535.0f); }
static void dacTask() {               // core0 loop 调用 (~1kHz)
  if (!gDacEnable) return;
  float cv = log2f(clampf(gCvFreq, 20.0f, 20000.0f) / gCvBaseHz);   // 1V/oct (0V @ gCvBaseHz)
  dacWrite(0, voltsToCode(cv));                                     // CVOUT: 音高 CV
  dacWrite(1, gEnv303.gate ? 65535 : 0);                            // GATEOUT: 有 gate = 5V
  dacWrite(2, voltsToCode((gEnv303.isAccent && gEnv303.gate) ? pAccent * 5.0f : 0.0f));  // ACCENTOUT
  dacWrite(3, voltsToCode(clampf(gEnv303.vcfEnv, 0.0f, 1.0f) * 5.0f));                   // FTENVOUT: 滤波包络
}
// ================= V1.2: 面板拨动开关 =================
static void switchTask() {            // 连软件时由软件控制(忽略物理开关); 不连时以物理开关为准 (同电位器)
  static int lastW = -1, lastR = -1;
  if (gPcMode) { lastW = -1; lastR = -1; return; }   // PC 模式: 软件控制波形/自激, 忽略物理开关
  int w = gpio_get(PIN_WAVE) ? 1 : 0;
  int r = gpio_get(PIN_MORERES) ? 1 : 0;
  if (w != lastW) { lastW = w; pWave = w ? 1.0f : 0.0f; }   // SW1: 高=方波 / 低=锯波 (方向可在实物上对调)
  if (r != lastR) { lastR = r; gSelfOsc = r ? 1 : 0; }      // SW2: 高=自激开 / 低=关
}
static void lcdReadId();      // 前向声明 (LCD 诊断, 定义在屏幕驱动段)
static void lcdColorTest();   // 前向声明 (LCD 全屏颜色测试)
static void lcdBbTest();      // 前向声明 (LCD 纯位敲颜色测试)
static void imgStart(const char* s);   // 前向声明 (图片上传开始)
static void imgClear();                // 前向声明 (图片清除)
// 小屏皮肤 (结构/全局在前, 供 handleCmd 用; 实现见 LCD 段)
#define SCR_FLASH_OFFSET  (flashSizeBytes() - 0x3000u)
#define SCR_MAGIC         0x53433031u
struct ScreenSkin {
  uint32_t magic;
  uint8_t  ver, scopeH, cellCols, cellRows, cellW, cellH, knobR, rsv;
  uint8_t  cellFn[8];                            // 每个参数格显示的功能: 0=TUNE 1=CUT 2=RES 3=DIST 4=ENV 5=DEC 6=ACC 7=WAVE
  uint16_t cScopeBg, cScopeGrid, cScopeTrace, cDivider, cCellBg, cRing, cKnobFill, cPtr, cLabel, cValue;
  uint32_t chk;
};
static ScreenSkin gSkin;
static uint8_t gLcdMadctl = 0x60;              // 小屏旋转: 默认 0x60=90°向右 (0x00=0° 0xA0=90°向左 0xC0=180°; ST7789 MADCTL MX/MY/MV)
static int hexNib(char c) { if (c>='0'&&c<='9') return c-'0'; if (c>='a'&&c<='f') return c-'a'+10; if (c>='A'&&c<='F') return c-'A'+10; return -1; }
static void scrSave();
static void scrLoad();
static void scrDefaults();
static uint32_t imgUsedBytes();
static void imgReadBack();

static void handleCmd(char *s) {
  if (!s || !s[0]) return;
  char c = s[0];
  float v = (float)atof(s + 1);
  switch (c) {
    case 'c': pCutHz = clampf(v, CUT_MIN_HZ, CUT_MAX_HZ); calcEnvModScalerOffset(); break;
    case 'r': pResPct = clampf(v, 0, 100); break;
    case 'd': pDecayMs = clampf(v, DECAY_MIN_MS, DECAY_MAX_MS); break;
    case 'e': pEnvModPct = clampf(v, 0, 100); calcEnvModScalerOffset(); break;
    case 'x': pAccent = clampf(v, 0, 127) / 127.0f; break;
    case 'a': pDist = clampf(v, 0, 127) / 127.0f; break;   // a<0..127> DIST 失真量
    case 'q': pColor = clampf(v, 0, 127) / 127.0f; break;  // q<0..127> COLOR 失真音色
    case 'o': pTuneSemis = clampf(v, -12.0f, 12.0f); break; // o<-12..12> 软件调音 (半音; 电位器关时生效)
    case 'R': Serial.println("factory reset settings + reboot"); setDefaults(); setSave(); delay(20); watchdog_hw->scratch[0] = 0; watchdog_reboot(0, 0, 0); break;
    case 'w': pWave = clampf(v, 0, 127) / 127.0f; break;
    case '=': {                                   // =<hex> 写入小屏皮肤(掉电保存): ver,scopeH,cols,rows,cellW,cellH,knobR,rsv, cellFn[8], 10×RGB565(LE)
      const char* h = s + 1; uint8_t bb[40]; int n = 0;
      while (h[0] && h[1] && n < (int)sizeof(bb)) { int hi = hexNib(h[0]), lo = hexNib(h[1]); if (hi < 0 || lo < 0) break; bb[n++] = (uint8_t)((hi << 4) | lo); h += 2; }
      if (n >= 36) {
        gSkin.ver = bb[0]; gSkin.scopeH = bb[1]; gSkin.cellCols = bb[2]; gSkin.cellRows = bb[3];
        gSkin.cellW = bb[4]; gSkin.cellH = bb[5]; gSkin.knobR = bb[6]; gSkin.rsv = bb[7];
        for (int i = 0; i < 8; i++) gSkin.cellFn[i] = bb[8 + i] & 7;
        uint16_t* cc = &gSkin.cScopeBg;
        for (int i = 0; i < 10; i++) cc[i] = (uint16_t)(bb[16 + i*2] | (bb[17 + i*2] << 8));
        scrSave(); gUiForce = 1;
        Serial.print("#SCREEN "); Serial.print(n); Serial.println("B saved");
      } else Serial.println("#SCREEN bad");
    } break;
    case ';': imgReadBack(); break;                // 回读媒体 (图/视频帧) 到 PC
    case '?': {                                   // 擦除绘制器已有数据(其余不动): ?1=只擦图片 ?2=只擦小屏皮肤 ?其它=都擦
      int w = (int)v;
      if (w == 1) { imgClear(); Serial.println("#ERASED img"); }
      else if (w == 2) { scrDefaults(); scrSave(); gUiForce = 1; Serial.println("#ERASED skin"); }
      else { imgClear(); scrDefaults(); scrSave(); gUiForce = 1; Serial.println("#ERASED all"); }
    } break;
    case 'v': pVolume = clampf(v, 0, 127) / 127.0f; break;
    case 'p': gUsePots = (v != 0.0f) ? 1 : 0; break;
    case 'j': gAudioLink = (v != 0.0f) ? 1 : 0; if (gAudioLink) { gAutoReport = 0; gUsbAudR = gUsbAudW; } break;  // j0/j1 USB 音频回传 (48k/16bit mono)
    case 'A': gAutoTest = (v != 0.0f) ? 1 : 0; break;
    // ---- 用户 pattern 上传 / 读回 / 存盘 ----
    case 'P': {                                  // P<pat>,<step>,<note>,<flags>
        int a=0,b=0,c=0,d=0;
        if (sscanf(s+1, "%d,%d,%d,%d", &a,&b,&c,&d)==4 && a>=0 && a<PAT_MAX && b>=0 && b<STEPS_MAX) {
          gPat.st[a][b].note=(uint8_t)c; gPat.st[a][b].flags=(uint8_t)d;
        }
      } break;
    case 'l': {                                  // l<pat>,<len>  (len 0 或 8..16)
        int a=0,b=0;
        if (sscanf(s+1, "%d,%d", &a,&b)==2 && a>=0 && a<PAT_MAX && b>=0 && b<=STEPS_MAX) {
          gPat.len[a]=(uint8_t)b; patRecount();
        }
      } break;
    case 'G': { float bp=atof(s+1); if (bp>=20.0f && bp<=300.0f) gPat.bpm10=(uint16_t)(bp*10.0f+0.5f); } break;  // G<bpm>
    case 'm': { int mk=(int)v; if (mk>=0 && mk<=15) gPat.playMask=(uint8_t)mk; } break;   // m<0..15> 播放掩码
    case 'S': gPcStream = (atof(s+1) != 0.0f) ? 1 : 0; break;              // S0/S1 暂停/恢复 PC 示波器流
    case 'W': patSave(); Serial.println("#SAVED"); break;                 // 写 flash
    case 'C': patDefaults(); patRecount(); Serial.println("#CLEARED"); break; // 清空 (仅 RAM; 需 W 才存盘)
    case 'X': seqCmd((int)atof(s+1)); break;                               // X0=暂停 X1=从当前步播 X2=回到开头
    case '*': gKeySimPress = 1; gKeySimReleaseMs = millis() + 60; Serial.println("#KEY"); break;  // 测试: 模拟 USER 键按下 60ms
    case 'H': gPcMode = (atof(s+1) != 0.0f) ? 1 : 0; if (!gPcMode) gUsePots = 1;   // H0/H1 PC 模式; 退出程序(H0)默认恢复物理电位器
              if (gPcMode) { gDrawSmiley = 1; gPcStream = 1; } else { gLcdReinit = 1; gPcStream = 0; }
              break;
    case 'U': {                                  // 读回 flash 里的 pattern (文本)
        Serial.print("#303PAT bpm="); Serial.print(gPat.bpm10/10.0f,1); Serial.print(" n="); Serial.print(gPat.nPat);
        Serial.print(" mask="); Serial.println(gPat.playMask);
        for (int pi=0; pi<PAT_MAX; pi++) {
          Serial.print("P "); Serial.print(pi); Serial.print(' '); Serial.println(gPat.len[pi]);
          for (int si=0; si<STEPS_MAX; si++) {
            Serial.print("S "); Serial.print(pi); Serial.print(' '); Serial.print(si); Serial.print(' ');
            Serial.print(gPat.st[pi][si].note); Serial.print(' '); Serial.println(gPat.st[pi][si].flags);
          }
        }
        Serial.println("#END");
      } break;
    case 'g': gSlideMs = clampf(v, 0.0f, 1000.0f); gPitchSlew.setTauMs(gSlideMs); break;   // 滑音 τ(ms), 原版≈60
    case 'F': gNoFilter = (v != 0.0f) ? 1 : 0; break;
    case 'y': gSineMode = (v != 0.0f) ? 1 : 0; break;
    case 'i': gNoEnv = (v != 0.0f) ? 1 : 0; break;
    case 'M': gMidiLogOn = (v != 0.0f) ? 1 : 0; midiLogLen = 0; break;
    case 'L': Serial.print("MIDI:"); for (int q = 0; q < midiLogLen; q++) { Serial.print(' '); Serial.print(midiLog[q], HEX); } Serial.println(); midiLogLen = 0; break;
    case 'D': {
        Serial.print("ADS:");
        for (int ch = 0; ch < 8; ch++) {
          uint16_t r = adsReadCh(ch);
          Serial.print(" A"); Serial.print(ch); Serial.print("="); Serial.print(r);
          Serial.print("("); Serial.print(adsVolts(r), 2); Serial.print("V)");
        }
        Serial.println();
      } break;
    case 'E': {
        uint32_t us = isrUs; uint32_t bl = isrBlocks;
        Serial.print("ISR us="); Serial.print(us);
        Serial.print(" blocks="); Serial.print(bl);
        Serial.print(" uspb=");
        if (bl) Serial.print((float)us / (float)bl, 2); else Serial.print(0.0f, 2);
        Serial.println();
      } break;
    // 'u' (USB 重枚举) 已移除: 避免 Windows 累积重复 MIDI 设备
    case 'T': {
        Serial.print("temp="); Serial.print(analogReadTemp(3.3f), 1);
        Serial.print("C cpuMHz="); Serial.print(clock_get_hz(clk_sys) / 1000000.0f, 1);
        Serial.print(" ocIdx="); Serial.print(gOcIdx);
        Serial.print(" fails="); Serial.print(gOcFails);
        Serial.print(" wdt="); Serial.print(gWdtEnabled);
        Serial.print(" oc=");
        for (uint32_t i = 0; i < OC_N; i++) { if (i) Serial.print(','); Serial.print(OC_KHZ[i] / 1000); Serial.print('/'); Serial.print(OC_MV[i]); }
        Serial.print(" ocunlock="); Serial.print(gOcUnlock);
        Serial.print(" ocmax="); Serial.print(OC_LOCK_MAX_IDX);
        Serial.print(" filtOS="); Serial.print(gFiltOS);
        Serial.print(" madctl="); Serial.print(gLcdMadctl, HEX);
        Serial.print(" up="); Serial.print(millis());   // 运行毫秒 (软件可据此检测 Pico 复位)
        Serial.print(" bb="); Serial.print(gLastBB);   // 黑盒: 上次运行最后阶段 (卡死定位)
        Serial.print(" rr="); Serial.print(gRstReason); // 上次复位原因 (0=上电/brownout 1=WDT 2=enable 4=liveness)
        Serial.print(" al="); Serial.print(gAudioLink);  // USB 音频开关 (排查"没声音")
        Serial.print(" ps="); Serial.print(gPcStream);   // 示波器文本流开关 (排查"示波器不动")
        Serial.print(" noise="); Serial.print(gNoiseAmt, 3);
        Serial.print(" var="); Serial.print(gVarAmt, 3);
        Serial.print(" tempk="); Serial.print(gTempAmt, 3);
        Serial.print(" vreg="); Serial.print((int)vreg_get_voltage());
        Serial.print(" imgUsed="); Serial.print(imgUsedBytes());
        Serial.print(" flash="); Serial.print(flashSizeBytes());
        Serial.print(" flashSrc="); Serial.print(gFlashSrc);
        Serial.print(" rdid="); Serial.print(gRdid[0], HEX); Serial.print(gRdid[1], HEX); Serial.print(gRdid[2], HEX);
        Serial.print(" m0rcmd="); Serial.print(qmi_hw->m[0].rcmd, HEX);   // XIP 读命令 (诊断: 修好后应恢复快读, 不是 3)
        Serial.print(" m0rfmt="); Serial.print(qmi_hw->m[0].rfmt, HEX);
        Serial.print(" m0tmg="); Serial.print(qmi_hw->m[0].timing, HEX);
        Serial.print(" imgCap="); Serial.println(IMG_CAP_BYTES);
      } break;
    case 'b': Serial.println("reboot->BOOTSEL"); delay(50); reset_usb_boot(0, 0); break;
    case 'z': Serial.println("reboot"); delay(20); watchdog_reboot(0, 0, 0); break;   // 软复位 (保留 flash pattern, 供验证)
    case '>': lcdReadId(); break;                                                      // 诊断: 读屏幕 ST7789 ID (检查接线)
    case '^': if (s[1] == 'X' || s[1] == 'x') imgClear(); else imgStart(s + 1); break;  // 图片: ^<mode>,<len> 上传; ^X 清除
    case '~': lcdColorTest(); break;                                                   // 诊断: 全屏颜色测试 (白/红/绿/蓝)
    case '<': lcdBbTest(); break;                                                      // 诊断: 纯位敲全屏颜色测试 (不用 PIO/DMA)
    case 'K': {                                                        // K<idx> 超频档位 -> 存 flash 并复位
        int oi = (int)clampf(v, 0.0f, (float)(OC_N - 1));
        if (!gOcUnlock && oi > OC_LOCK_MAX_IDX) oi = OC_LOCK_MAX_IDX;   // 未解锁: 封顶 375
        setSyncFromGlobals(); gSet.ocIdx = (uint8_t)oi; setSave();      // 存 flash (掉电保留)
        watchdog_hw->scratch[0] = OC_SCRATCH_MAGIC;
        watchdog_hw->scratch[1] = OC_PACK(oi, gOcUnlock, OC_FOSCODE(gFiltOS), gWdtEnabled);
        watchdog_hw->scratch[3] = 0;                                   // fails 清零
        Serial.print("#OC "); Serial.print(OC_KHZ[oi] / 1000); Serial.print("MHz");
        Serial.print("@"); Serial.print(OC_MV[oi]); Serial.print("mV");
        Serial.println(" reboot");
        delay(50); watchdog_reboot(0, 0, 0);
      } break;
    case 'u': {                                                        // u0/u1 看门狗开关
        int on = (v != 0.0f) ? 1 : 0;
        gWdtEnabled = on;
        setSyncFromGlobals(); setSave();                               // 存 flash
        watchdog_hw->scratch[0] = OC_SCRATCH_MAGIC;
        watchdog_hw->scratch[1] = OC_PACK(gOcIdx, gOcUnlock, OC_FOSCODE(gFiltOS), on);
        if (on) { watchdog_start_tick(12); watchdog_enable(OC_WDT_MS, false); }
        else    { watchdog_disable(); }
        Serial.print("#WDT "); Serial.println(on ? "on" : "off");
      } break;
    case 'Y': if (s[1] == 'T' || s[1] == 't') flashTestCmd(); else gLcdReinit = 1; break;
    case 'Z': {                                                   // Z<0..3> 小屏旋转: 0=0° 1=90° 2=180° 3=270°
        static const uint8_t rot[4] = { 0x00, 0x60, 0xC0, 0xA0 };
        int ri = (int)clampf(v, 0.0f, 3.0f);
        gLcdMadctl = rot[ri];
        Serial.print("#ROT "); Serial.print(ri); Serial.print(" madctl=0x"); Serial.println(gLcdMadctl, HEX);
        gLcdReinit = 1;                                           // 立即重发初始化, 无需重启
      } break;
    case '#': { Serial.print("NOTELOG "); Serial.print(gNoteLogT0); Serial.print(" :"); for (uint8_t q = 0; q < gNoteLogN; q++) { Serial.print(' '); Serial.print((gNoteLog[q] >> 8) ? 'S' : 'T'); Serial.print(gNoteLog[q] & 0xFF); } Serial.println(); gNoteLogN = 0; } break;  // dump 音符事件日志 (T=TRIG S=SLIDE)
    case 'V': {
        const uint16_t M = SCOPE_N - 1;
        int w = gScopeW, s0 = w - 240, mn = 32767, mx = -32768; long sum = 0;
        if (s0 < 0) s0 = 0;
        for (int i = s0; i < w; i++) { int16_t a = gScope[i & M]; if (a < mn) mn = a; if (a > mx) mx = a; sum += a; }
        Serial.print("scope min="); Serial.print(mn);
        Serial.print(" max="); Serial.print(mx);
        Serial.print(" mean="); Serial.print((int)(sum / (w - s0)));
        Serial.print(" outPeak="); Serial.print(gOutPeak); gOutPeak = 0;
        Serial.print(" prePeak="); Serial.println(gPrePeak, 3); gPrePeak = 0;
      } break;
    case 'B': gScopeDiv = (int)clampf(v, 1.0f, 2000.0f); Serial.print("scopeDiv="); Serial.println(gScopeDiv); break;
    case 't': gPcDecim = (int)clampf(v, 1.0f, 64.0f); Serial.print("pcDecim="); Serial.println(gPcDecim); break;  // PC 示波器抽点
    case 'h': { int m=(int)v; gFidFB=(m&1)?1:0; gFidStage=(m&2)?1:0; gFidFbHp=(m&4)?1:0; gFidOut=(m&8)?1:0;
                gFidEnvMod=(m&16)?1:0; gFidLowPeak=(m&32)?1:0; gResBassComp=(m&64)?1:0; gFidNoise=(m&128)?1:0;
                calcEnvModScalerOffset();
                Serial.print("fid="); Serial.println(gFidFB|(gFidStage<<1)|(gFidFbHp<<2)|(gFidOut<<3)|(gFidEnvMod<<4)|(gFidLowPeak<<5)|(gResBassComp<<6)|(gFidNoise<<7)); } break;  // 保真开关 掩码
    case '!': gNoiseAmt = clampf(v, 0, 127) / 127.0f; saveKnobsScratch(); setSyncFromGlobals(); setTouched(); Serial.print("#noise "); Serial.println(gNoiseAmt, 3); break;   // §51 噪声量旋钮
    case '%': gVarAmt   = clampf(v, 0, 127) / 127.0f; saveKnobsScratch(); setSyncFromGlobals(); setTouched(); Serial.print("#var ");   Serial.println(gVarAmt, 3); break;   // §49 元件容差旋钮
    case '&': gTempAmt  = clampf(v, 0, 127) / 127.0f; saveKnobsScratch(); setSyncFromGlobals(); setTouched(); Serial.print("#temp ");  Serial.println(gTempAmt, 3); break;   // §50 温度漂移旋钮
    case '@': { int n=(int)clampf(v, 4.0f, 16.0f);
                if (n!=4 && n!=8 && n!=12 && n!=16) n=8;
                gFiltOS = n;
                setSyncFromGlobals(); setSave();                            // 存 flash
                watchdog_hw->scratch[0] = OC_SCRATCH_MAGIC;                 // 存 scratch + 复位后按此过采样运行
                watchdog_hw->scratch[1] = OC_PACK(gOcIdx, gOcUnlock, OC_FOSCODE(n), gWdtEnabled);
                Serial.print("#filtOS "); Serial.print(gFiltOS); Serial.println(" reboot");
                delay(50); watchdog_reboot(0, 0, 0);
              } break;
    case '+': gOcUnlock = (v != 0.0f) ? 1 : 0;
              setSyncFromGlobals(); setSave();                          // 存 flash
              watchdog_hw->scratch[0] = OC_SCRATCH_MAGIC;
              watchdog_hw->scratch[1] = OC_PACK(gOcIdx, gOcUnlock, OC_FOSCODE(gFiltOS), gWdtEnabled);
              Serial.print("#ocUnlock "); Serial.println(gOcUnlock); break;   // 超频解锁开关
    case 'O': gMaster = clampf(v, 0.0f, 8.0f); Serial.print("master="); Serial.println(gMaster, 2); break;
    case 'J': gAccVcaGain = clampf(v, 0.0f, 8.0f); Serial.print("accVca="); Serial.println(gAccVcaGain, 2); break;
    case '$': gSelfOsc = (v != 0.0f) ? 1 : 0; Serial.print("#selfOsc "); Serial.println(gSelfOsc ? "on" : "off"); break;   // 自激开关 (字母用尽, 用 $)
    case '/': gDistPost = (v != 0.0f) ? 1 : 0; Serial.print("#distPos "); Serial.println(gDistPost ? "post" : "pre"); break;   // 失真位置: 0=VCA前, 1=VCA后(TD-3)
    case 'Q': reportCpu(); break;
    case 'I': gAutoReport = (v != 0.0f) ? 1 : 0; Serial.print("#autoReport "); Serial.println(gAutoReport ? "on" : "off"); break;
#if USE_USB_MIDI
    case 'n': Serial.print("note "); Serial.println((int)v); midiNoteOn((uint8_t)v, 64); break;
    case 'N': Serial.print("noteA "); Serial.println((int)v); midiNoteOn((uint8_t)v, 110); break;
    case 'f': noteStackCount = 0; releaseNote(); break;
#endif
    case 'k': Serial.print("freq "); Serial.println(v, 1); postCmd(CMD_TRIG, v, 0); break;
    case 's':
      Serial.print("cut="); Serial.print(pCutHz, 0);
      Serial.print(" res="); Serial.print(pResPct, 0);
      Serial.print(" dec="); Serial.print(pDecayMs, 0);
      Serial.print(" env="); Serial.print(pEnvModPct, 0);
      Serial.print(" acc="); Serial.print(pAccent, 2);
      Serial.print(" dist="); Serial.print(pDist, 2);
      Serial.print(" color="); Serial.print(pColor, 2);
      Serial.print(" wave="); Serial.print(pWave, 2);
      Serial.print(" distpos="); Serial.print(gDistPost);
      Serial.print(" tune="); Serial.print(pTuneSemis, 2);
      Serial.print(" fid="); Serial.print(gFidFB | (gFidStage << 1) | (gFidFbHp << 2) | (gFidOut << 3) | (gFidEnvMod << 4) | (gFidLowPeak << 5) | (gResBassComp << 6));
      Serial.println();
      break;
  }
}

// ================= core1: 音频 =================
static void core1_entry() {
  multicore_lockout_victim_init();      // 允许 core0 在写 flash 时暂停本核
  i2s.setBitsPerSample(16);
  i2s.setBuffers(8, 128);
  i2s.onTransmit(onTransmit);
  i2s.begin(SAMPLE_RATE);
  sleep_ms(50);                        // 等 I2S 时钟稳定
  gpio_put(PIN_XSMT, 1);               // 解除静音 (消上电爆音)
  for (;;) tight_loop_contents();
}

// ================= 运行时超频阶梯 + 看门狗（板级优化） =================
// 实测: 纯负载下芯片上限 416MHz; 但 303 固件(I2S+DMA+ADS+MIDI+屏+双核音频)负载下
//       400MHz 会出现卡死/噪声(不稳), 故 303 首选 375MHz, 死机自动降档复位, 永不砖。
// (超频阶梯/scratch 布局定义见文件开头 gOcKHz 附近)

static void overclockLadder() {
  // ---- 复位原因快照 (在任何 watchdog_* 调用前读取) ----
  // bit0=看门狗定时器超时 bit1=watchdog_enable 触发的复位 bit2=我们的 liveness 主动复位
  gRstReason = 0;
  if (watchdog_hw->reason)          gRstReason |= 1;
  if (watchdog_enable_caused_reboot()) gRstReason |= 2;
  if ((watchdog_hw->scratch[1] >> 12) & 1u) gRstReason |= 4;
  // ---- 从 flash 读取持久设置 (权威) -> 播种 scratch, 让下面的既有逻辑沿用 ----
  setLoad();
  watchdog_hw->scratch[0] = OC_SCRATCH_MAGIC;
  watchdog_hw->scratch[1] = OC_PACK(gSet.ocIdx, gSet.unlock, OC_FOSCODE(gSet.filtOS), gSet.wdt);
  watchdog_hw->scratch[2] = (watchdog_hw->scratch[2] & 0xFF000000u)   // 保留 BB 黑盒高字节
      | ((uint32_t)(gSet.noise7 & 0x7Fu)) | ((uint32_t)(gSet.var7 & 0x7Fu) << 7) | ((uint32_t)(gSet.temp7 & 0x7Fu) << 14);
  // RP2350 超频阶梯 (启动时在配好外设前设定; 运行时改频会破坏 I2S/PIO -> 故用复位档位)。
  // ⚠️ DVDD 数据手册绝对最大 1.21V。默认档 300MHz@1.20V; 350/400MHz 电压超规, 风险自负。
  // 超频档/解锁/过采样/wdt 存 scratch[1] (跨软复位保留; 避开被 SDK 占用的 [4..7]); fails 存 scratch[3]
  uint32_t idx = OC_DEFAULT_IDX, fails = 0, wdt = 1;
  gOcUnlock = 0; gFiltOS = 8;
  if (watchdog_hw->scratch[0] == OC_SCRATCH_MAGIC) {
    uint32_t st = watchdog_hw->scratch[1];
    idx = st & 0xFFu; gOcUnlock = (int)((st >> 8) & 1u);
    gFiltOS = OC_FOSFROMC((st >> 9) & 3u); wdt = (st >> 11) & 1u;
    fails = watchdog_hw->scratch[3];
    { uint32_t kn = watchdog_hw->scratch[2]; gNoiseAmt=(kn&0x7Fu)/127.0f; gVarAmt=((kn>>7)&0x7Fu)/127.0f; gTempAmt=((kn>>14)&0x7Fu)/127.0f; }   // 三个旋钮
    if (gRstReason & 2) {                              // 看门狗超时 = 该档不可靠
      if (idx > OC_LOCK_MAX_IDX) { idx = OC_LOCK_MAX_IDX; fails = 0; gOcUnlock = 0; }   // 解锁的高频档崩了 -> 直接回安全档(375)+上锁
      else if (++fails >= OC_FAILS_TO_DROP) { fails = 0; if (idx > 0) idx--; }          // 降一档 (往更安全/更低主频)
    } else fails = 0;
  }
  if (idx == 0) idx = OC_DEFAULT_IDX;                              // 迁移: 旧的"原版 150MHz"会让 core1 满载, 改用默认
  if (idx >= OC_N) idx = OC_N - 1;
  if (!gOcUnlock && idx > OC_LOCK_MAX_IDX) idx = OC_LOCK_MAX_IDX;   // 未解锁: 最高档
  gWdtEnabled = (int)wdt;
  watchdog_hw->scratch[0] = OC_SCRATCH_MAGIC;
  watchdog_hw->scratch[3] = fails;
  watchdog_hw->scratch[1] = OC_PACK(idx, gOcUnlock, OC_FOSCODE(gFiltOS), wdt);
  if (OC_MV[idx] > 1300) vreg_disable_voltage_limit();                  // >1.30V 必须先解除电压上限(否则被钳到 1.30V)
  vreg_set_voltage((enum vreg_voltage)((OC_MV[idx] - 550) / 50));   // 先升压
  busy_wait_us_32(2000);
  watchdog_start_tick(12);
  if (wdt) watchdog_enable(OC_WDT_MS, false); else watchdog_disable();
  // 升主频前先加大 flash QMI 分频 (SCK = clk_sys / CLKDIV, 保持 <= ~100MHz), 并插 dummy 访问
  { uint32_t khz = OC_KHZ[idx];
    uint32_t cd = (khz + 99999u) / 100000u;              // ceil(kHz / 100MHz)
    if (cd < 2u) cd = 2u; if (cd > 255u) cd = 255u;
    uint32_t cur = qmi_hw->m[0].timing & 0xFFu;
    if (cd > cur) {
      qmi_hw->m[0].timing = (qmi_hw->m[0].timing & ~0xFFu) | cd;
      __dmb();
      volatile uint32_t dummy = *(volatile uint32_t*)0x10000000u; (void)dummy;   // dummy XIP 访问
      __dmb();
    }
  }
  set_sys_clock_khz(OC_KHZ[idx], false);                // 再升频
  gOcIdx   = (int)idx;
  gOcFails = fails;
  gOcKHz   = clock_get_hz(clk_sys) / 1000u;
  // 看门狗降档等改变了设置 -> 同步并去抖写回 flash (正常开机不会触发)
  {
    SetStore before = gSet;
    setSyncFromGlobals();
    if (gSet.ocIdx != before.ocIdx || gSet.unlock != before.unlock || gSet.filtOS != before.filtOS ||
        gSet.wdt != before.wdt || gSet.noise7 != before.noise7 || gSet.var7 != before.var7 || gSet.temp7 != before.temp7)
      setTouched();
  }
  if (gRstReason & 1) *((volatile uint32_t*)&watchdog_hw->reason) = 3u;   // W1C 清粘滞的看门狗原因 -> 只对本次复位判一次 (免得每开机都算 WDT 一路降档)
}

// ================= 屏幕驱动 (ST7789 240x240, PIO+DMA) + UI =================
static void periFromSys() {
  clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                  clock_get_hz(clk_sys), clock_get_hz(clk_sys));
}
#define PIN_BL    26    // 背光 (原16)
#define PIN_SCK   18
#define PIN_MOSI  19
#define PIN_CS    20
#define PIN_DC    21
#define PIN_RST   22
#define PIN_SDO   16    // 屏幕 MISO (读字库)
#define PIN_CSF   17    // 字库芯片片选 (CSF)
#define LCD_W 240
#define LCD_H 240
static int      lcdYoff = 0;
static float    lcdClkDiv = 1.5f;                 // SCK = clk_sys/(4*div) -> 50MHz @300MHz (原始值; 面包板若不稳再降)
static uint16_t fb[LCD_W * LCD_H];
static PIO      lcdPio = pio0;
static int      lcdSm  = -1;
static uint     lcdOff = 0;
static int      dmaTx  = -1;

static inline void putPix(int i, uint16_t c) { fb[i ^ 1] = c; }
static inline uint16_t C(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static void bbByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    gpio_put(PIN_MOSI, (b >> i) & 1); busy_wait_us_32(1);
    gpio_put(PIN_SCK, 1);             busy_wait_us_32(1);
    gpio_put(PIN_SCK, 0);             busy_wait_us_32(1);
  }
}
static void lcdCmdBB(uint8_t c) { gpio_put(PIN_DC, 0); gpio_put(PIN_CS, 0); bbByte(c); gpio_put(PIN_CS, 1); }
static void lcdDataBB(const uint8_t* d, size_t n) {
  gpio_put(PIN_DC, 1); gpio_put(PIN_CS, 0); while (n--) bbByte(*d++); gpio_put(PIN_CS, 1);
}
static void lcdCmdDataBB(uint8_t c, const uint8_t* d, size_t n) { lcdCmdBB(c); if (n) lcdDataBB(d, n); }
static void bbModeBegin() {
  pio_sm_set_enabled(lcdPio, lcdSm, false);
  gpio_set_function(PIN_SCK,  GPIO_FUNC_SIO);
  gpio_set_function(PIN_MOSI, GPIO_FUNC_SIO);
  gpio_set_dir(PIN_SCK, OUTPUT); gpio_set_dir(PIN_MOSI, OUTPUT);
  gpio_put(PIN_SCK, 0); gpio_put(PIN_MOSI, 0);
}
static void bbModeEnd() {
  pio_gpio_init(lcdPio, PIN_SCK);
  pio_gpio_init(lcdPio, PIN_MOSI);
  pio_sm_set_consecutive_pindirs(lcdPio, lcdSm, PIN_SCK, 1, true);
  pio_sm_set_consecutive_pindirs(lcdPio, lcdSm, PIN_MOSI, 1, true);
  pio_sm_set_enabled(lcdPio, lcdSm, true);
}
static uint8_t bbRead8() {                    // 位敲读 MISO(GP16), 采样 SCK 上升沿 (读受限 ~6.75MHz, 故 1us/半周期)
  uint8_t v = 0;
  for (int i = 7; i >= 0; i--) {
    gpio_put(PIN_SCK, 1); busy_wait_us_32(1);
    v = (uint8_t)((v << 1) | (gpio_get(PIN_SDO) ? 1u : 0u));
    gpio_put(PIN_SCK, 0); busy_wait_us_32(1);
  }
  return v;
}
static void lcdReadId() {                     // 诊断: 读 ST7789 ID/状态 -> 串口 #LCDID
  if (dma_channel_is_busy(dmaTx)) { dma_channel_wait_for_finish_blocking(dmaTx); }
  while (!pio_sm_is_tx_fifo_empty(lcdPio, lcdSm)) {}
  gpio_put(PIN_CS, 1);
  pio_sm_set_enabled(lcdPio, lcdSm, false);
  gpio_set_function(PIN_SCK, GPIO_FUNC_SIO); gpio_set_function(PIN_MOSI, GPIO_FUNC_SIO);
  gpio_set_dir(PIN_SCK, OUTPUT); gpio_set_dir(PIN_MOSI, OUTPUT);
  gpio_put(PIN_SCK, 0); gpio_put(PIN_MOSI, 0);
  gpio_set_function(PIN_SDO, GPIO_FUNC_SIO); gpio_set_dir(PIN_SDO, INPUT);
  uint8_t id[4], m[2], cm[2];
  gpio_put(PIN_CS, 0); gpio_put(PIN_DC, 0); bbByte(0x04); gpio_put(PIN_DC, 1);   // RDDID
  for (int i = 0; i < 4; i++) id[i] = bbRead8();
  gpio_put(PIN_CS, 1);
  gpio_put(PIN_CS, 0); gpio_put(PIN_DC, 0); bbByte(0x0B); gpio_put(PIN_DC, 1);   // RDDMADCTL
  m[0] = bbRead8(); m[1] = bbRead8();
  gpio_put(PIN_CS, 1);
  gpio_put(PIN_CS, 0); gpio_put(PIN_DC, 0); bbByte(0x0C); gpio_put(PIN_DC, 1);   // RDDCOLMOD (我们设成 0x55)
  cm[0] = bbRead8(); cm[1] = bbRead8();
  gpio_put(PIN_CS, 1);
  rgb.begin();                                // 恢复 GP16 给板载 WS2812
  bbModeEnd();
  Serial.print("#LCDID id"); for (int i = 0; i < 4; i++) { Serial.print(' '); Serial.print(id[i], HEX); }
  Serial.print(" madctl"); for (int i = 0; i < 2; i++) { Serial.print(' '); Serial.print(m[i], HEX); }
  Serial.print(" colmod"); for (int i = 0; i < 2; i++) { Serial.print(' '); Serial.print(cm[i], HEX); }
  Serial.print(" BL="); Serial.print(gpio_get(PIN_BL));
  Serial.print(" pc="); Serial.println(gPcMode);
}
static void audioPump() {                      // 把 USB 音频环形缓冲尽量搬进 CDC1; 主循环 + 等 LCD DMA 时都调用
  if (!tud_cdc_n_connected(1)) { gUsbAudOn = 0; gAudioLink = 0; return; }   // 音频口断开(关软件/崩溃) -> 自动清音频开关, al 不残留
  if (!gAudioLink) { gUsbAudOn = 0; return; }
  gUsbAudOn = 1;
  for (int f = 0; f < 8; f++) {
    uint32_t avail = tud_cdc_n_write_available(1);
    if (avail < 2 || gUsbAudR == gUsbAudW) break;
    uint8_t fr[128]; int n = 0;
    while (n + 2 <= (int)avail && n + 2 <= 128 && gUsbAudR != gUsbAudW) {
      int16_t s = gUsbAud[gUsbAudR & (USBAUD_N - 1)]; gUsbAudR++;
      fr[n++] = (uint8_t)(s & 0xFF); fr[n++] = (uint8_t)((s >> 8) & 0xFF);
    }
    if (n) { uint32_t irqSave = save_and_disable_interrupts(); tud_cdc_n_write(1, fr, n); tud_cdc_n_write_flush(1); restore_interrupts(irqSave); }
  }
}
static volatile bool lcdDmaBusy = false;
static void lcdWaitIdle() {                    // 推屏/改窗口前: 确保上一次 DMA 已完成, 否则关 PIO 会让它永久 busy -> 死等卡死
  while (dma_channel_is_busy(dmaTx)) audioPump();   // 等 DMA 时也搬音频 (双保险)
  while (!pio_sm_is_tx_fifo_empty(lcdPio, lcdSm)) {}
  if (lcdDmaBusy) { busy_wait_us_32((uint32_t)(32.0f / (clock_get_hz(clk_sys) / (4.0f * lcdClkDiv)) * 1e6f) + 3); gpio_put(PIN_CS, 1); lcdDmaBusy = false; }
}
static void lcdSetWindow(int x0, int y0, int x1, int y1) {
  lcdWaitIdle();
  int Y0 = y0 + lcdYoff, Y1 = y1 + lcdYoff;
  uint8_t d[4];
  bbModeBegin();
  lcdCmdBB(0x2A); d[0]=x0>>8; d[1]=x0; d[2]=x1>>8; d[3]=x1; lcdDataBB(d, 4);
  lcdCmdBB(0x2B); d[0]=Y0>>8; d[1]=Y0; d[2]=Y1>>8; d[3]=Y1; lcdDataBB(d, 4);
  lcdCmdBB(0x2C);
  bbModeEnd();
}
static void dmaStart(const void* src, uint32_t words) {
  while (dma_channel_is_busy(dmaTx)) audioPump();
  dma_channel_set_read_addr(dmaTx, src, false);
  dma_channel_set_trans_count(dmaTx, words, true);
}
static void lcdPushFb() {                      // 非阻塞: 启动 DMA 后立即返回, 由 lcdWaitIdle()/lcdPushTask() 收尾 (core0 不再被 ~18ms 卡住 -> USB 音频不断流)
  lcdSetWindow(0, 0, LCD_W - 1, LCD_H - 1);
  gpio_put(PIN_DC, 1); gpio_put(PIN_CS, 0);
  dmaStart(fb, sizeof(fb) / 4);
  lcdDmaBusy = true;
}
// 非阻塞推屏: 启动 DMA 后立即返回, 由 lcdPushTask() 收尾并启动下一块 (core0 不再被 8.7ms 等待卡住)
static volatile int  uiPend = 0;             // bit0=示波器区(0..140), bit1=参数区(141..239)
static void lcdStartRegion(int y0, int y1) {
  lcdSetWindow(0, y0, LCD_W - 1, y1);
  gpio_put(PIN_DC, 1); gpio_put(PIN_CS, 0);
  dma_channel_set_read_addr(dmaTx, &fb[y0 * LCD_W], false);
  dma_channel_set_trans_count(dmaTx, (uint32_t)((y1 - y0 + 1) * LCD_W) / 2, true);
  lcdDmaBusy = true;
}
static void lcdPushTask() {
  if (lcdDmaBusy) {
    if (dma_channel_is_busy(dmaTx)) return;          // DMA 后台仍在跑 -> 立即返回, 不阻塞
    while (!pio_sm_is_tx_fifo_empty(lcdPio, lcdSm)) {}
    busy_wait_us_32((uint32_t)(32.0f / (clock_get_hz(clk_sys) / (4.0f * lcdClkDiv)) * 1e6f) + 3);
    gpio_put(PIN_CS, 1); lcdDmaBusy = false;
  }
  if (uiPend) {
    if (uiPend & 2) { uiPend &= ~2; lcdStartRegion(141, 239); }   // 参数区优先 (变化少)
    else            { uiPend &= ~1; lcdStartRegion(0, 140); }
  }
}
static void lcdFill(uint16_t c) {
  for (int i = 0; i < LCD_W*LCD_H; i++) fb[i] = c;
  lcdPushFb();
}
static void lcdColorTest() {                  // 诊断: 全屏 白/红/绿/蓝/黑 各 0.4s -> 看得见=数据通, 看不见=供电/接线
  const uint16_t cols[4] = { 0xFFFF, 0xF800, 0x07E0, 0x001F };
  for (int i = 0; i < 4; i++) { lcdFill(cols[i]); delay(400); }
  lcdFill(0x0000);
}
static void lcdBbFill(uint16_t c) {            // 纯位敲全屏填充 (不用 PIO/DMA) -> 隔离"接线 vs PIO"
  pio_sm_set_enabled(lcdPio, lcdSm, false);
  gpio_set_function(PIN_SCK, GPIO_FUNC_SIO); gpio_set_function(PIN_MOSI, GPIO_FUNC_SIO);
  gpio_set_dir(PIN_SCK, OUTPUT); gpio_set_dir(PIN_MOSI, OUTPUT);
  gpio_put(PIN_SCK, 0); gpio_put(PIN_MOSI, 0);
  uint8_t d[4];
  lcdCmdBB(0x11); delay(120);
  d[0] = 0x55; lcdCmdDataBB(0x3A, d, 1);
  d[0] = gLcdMadctl; lcdCmdDataBB(0x36, d, 1);
  lcdCmdBB(0x21); lcdCmdBB(0x13); lcdCmdBB(0x29); delay(50);
  lcdCmdBB(0x2A); d[0]=0; d[1]=0; d[2]=0; d[3]=239; lcdDataBB(d, 4);
  lcdCmdBB(0x2B); d[0]=0; d[1]=0; d[2]=0; d[3]=239; lcdDataBB(d, 4);
  lcdCmdBB(0x2C);
  gpio_put(PIN_DC, 1); gpio_put(PIN_CS, 0);
  uint8_t hi = (uint8_t)(c >> 8), lo = (uint8_t)(c & 0xFF);
  for (int i = 0; i < LCD_W * LCD_H; i++) { bbByte(hi); bbByte(lo); if ((i & 0x3FF) == 0) watchdog_update(); }
  gpio_put(PIN_CS, 1);
  bbModeEnd();
}
static void lcdBbTest() {                      // 诊断: 纯位敲 白/红/绿/蓝 -> 串口命令 '<'
  const uint16_t cols[4] = { 0xFFFF, 0xF800, 0x07E0, 0x001F };
  for (int i = 0; i < 4; i++) { lcdBbFill(cols[i]); delay(600); watchdog_update(); }
  lcdBbFill(0x0000);
}
static void lcdInitSeq() {  gpio_set_function(PIN_SCK, GPIO_FUNC_SIO);
  gpio_set_function(PIN_MOSI, GPIO_FUNC_SIO);
  gpio_set_dir(PIN_SCK, OUTPUT); gpio_set_dir(PIN_MOSI, OUTPUT);
  gpio_put(PIN_SCK, 0); gpio_put(PIN_MOSI, 0);
  gpio_put(PIN_RST, 0); delay(30); gpio_put(PIN_RST, 1); delay(150);
  lcdCmdBB(0x01); delay(150);
  lcdCmdBB(0x11); delay(120);
  uint8_t d[1];
  d[0]=0x55;  lcdCmdDataBB(0x3A, d, 1);
  d[0]=gLcdMadctl;  lcdCmdDataBB(0x36, d, 1);
  const uint8_t b2[5]={0x0C,0x0C,0x00,0x33,0x33}; lcdCmdDataBB(0xB2,b2,5);
  d[0]=0x35; lcdCmdDataBB(0xB7,d,1);
  d[0]=0x19; lcdCmdDataBB(0xBB,d,1);
  d[0]=0x2C; lcdCmdDataBB(0xC0,d,1);
  d[0]=0x01; lcdCmdDataBB(0xC2,d,1);
  d[0]=0x12; lcdCmdDataBB(0xC3,d,1);
  d[0]=0x20; lcdCmdDataBB(0xC4,d,1);
  d[0]=0x0F; lcdCmdDataBB(0xC6,d,1);
  const uint8_t dd[2]={0xA4,0xA1}; lcdCmdDataBB(0xD0,dd,2);
  lcdCmdBB(0x21);
  const uint8_t e0[14]={0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,0x54,0x4C,0x18,0x0D,0x0B,0x1F,0x23};
  lcdCmdDataBB(0xE0,e0,14);
  const uint8_t e1[14]={0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,0x44,0x51,0x2F,0x1F,0x1F,0x20,0x23};
  lcdCmdDataBB(0xE1,e1,14);
  lcdCmdBB(0x13); delay(10);
  lcdCmdBB(0x29); delay(100);
  gpio_put(PIN_CS, 1);
}
// 屏幕心跳: 重发初始化寄存器 (不含长时间 delay), 排线抖动导致黑屏可自动恢复
static void lcdRegRefresh() {
  bbModeBegin();
  uint8_t d[1];
  lcdCmdBB(0x11);                                   // SLPOUT
  d[0]=0x55; lcdCmdDataBB(0x3A,d,1);                // COLMOD
  d[0]=gLcdMadctl; lcdCmdDataBB(0x36,d,1);          // MADCTL (旋转)
  const uint8_t b2[5]={0x0C,0x0C,0x00,0x33,0x33}; lcdCmdDataBB(0xB2,b2,5);
  d[0]=0x35; lcdCmdDataBB(0xB7,d,1);
  d[0]=0x19; lcdCmdDataBB(0xBB,d,1);
  d[0]=0x2C; lcdCmdDataBB(0xC0,d,1);
  d[0]=0x01; lcdCmdDataBB(0xC2,d,1);
  d[0]=0x12; lcdCmdDataBB(0xC3,d,1);
  d[0]=0x20; lcdCmdDataBB(0xC4,d,1);
  d[0]=0x0F; lcdCmdDataBB(0xC6,d,1);
  const uint8_t dd[2]={0xA4,0xA1}; lcdCmdDataBB(0xD0,dd,2);
  lcdCmdBB(0x21);
  const uint8_t e0[14]={0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,0x54,0x4C,0x18,0x0D,0x0B,0x1F,0x23};
  lcdCmdDataBB(0xE0,e0,14);
  const uint8_t e1[14]={0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,0x44,0x51,0x2F,0x1F,0x1F,0x20,0x23};
  lcdCmdDataBB(0xE1,e1,14);
  lcdCmdBB(0x13);
  lcdCmdBB(0x29);
  bbModeEnd();
}
static void lcdPioBegin() {
  lcdSm = pio_claim_unused_sm(pio0, false);
  lcdPio = pio0;
  if (lcdSm < 0) { lcdPio = pio1; lcdSm = pio_claim_unused_sm(pio1, true); }
  lcdOff = pio_add_program(lcdPio, &lcd_spi_program);
  pio_sm_config c = lcd_spi_program_get_default_config(lcdOff);
  sm_config_set_sideset_pins(&c, PIN_SCK);
  sm_config_set_out_pins(&c, PIN_MOSI, 1);
  sm_config_set_out_shift(&c, false, true, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  { uint32_t sys = clock_get_hz(clk_sys);            // 屏 SPI 与主频解耦
    int n = (int)((sys + 249999999u) / 250000000u);  // 关键: 用【整数】分频 N (ceil(clk_sys/250MHz))
    if (n < 1) n = 1;
    lcdClkDiv = (float)n;                            // SCK = clk_sys/(4*N) <= 62.5MHz
    // 说明: 之前用小数分频 (clk_sys/250MHz=1.5/1.58/1.73) 让 PIO 钟在 clk_sys/1 与 /2 间跳动 -> 62.5MHz 的 SCK 边沿抖动大 -> 高主频下 ST7789 锁存错 -> 花屏。
    //       整数分频后 PIO 钟 = clk_sys/N (完全均匀, 无抖动), SCK 干净且 <=62.5MHz。
  }
  sm_config_set_clkdiv(&c, lcdClkDiv);
  pio_gpio_init(lcdPio, PIN_SCK);
  pio_gpio_init(lcdPio, PIN_MOSI);
  pio_sm_set_consecutive_pindirs(lcdPio, lcdSm, PIN_SCK, 1, true);
  pio_sm_set_consecutive_pindirs(lcdPio, lcdSm, PIN_MOSI, 1, true);
  pio_sm_init(lcdPio, lcdSm, lcdOff, &c);
  pio_sm_set_enabled(lcdPio, lcdSm, true);
  dmaTx = dma_claim_unused_channel(true);
  dma_channel_config dc = dma_channel_get_default_config(dmaTx);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_dreq(&dc, pio_get_dreq(lcdPio, lcdSm, true));
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  dma_channel_configure(dmaTx, &dc, &lcdPio->txf[lcdSm], NULL, 0, false);
}

// ================= 小屏皮肤 (结构/gSkin/hexNib 已在 handleCmd 之前声明; 这里是实现) =================
static uint32_t scrChk() {
  const uint8_t* b = (const uint8_t*)&gSkin; uint32_t s = 2166136261u;
  for (size_t i = 0; i < offsetof(ScreenSkin, chk); i++) { s ^= b[i]; s *= 16777619u; }
  return s;
}
static void scrDefaults() {
  memset(&gSkin, 0, sizeof(gSkin));
  gSkin.magic = SCR_MAGIC; gSkin.ver = 1;
  gSkin.scopeH = 141; gSkin.cellCols = 4; gSkin.cellRows = 2; gSkin.cellW = 60; gSkin.cellH = 49; gSkin.knobR = 10;
  for (int i = 0; i < 8; i++) gSkin.cellFn[i] = i;   // 默认 8 格 = TUNE/CUT/RES/DIST/ENV/DEC/ACC/WAVE (不能省, 否则全 TUNE)
  gSkin.cScopeBg = C(0,0,35); gSkin.cScopeGrid = C(0,45,0); gSkin.cScopeTrace = C(0,255,120); gSkin.cDivider = C(70,70,70);
  gSkin.cCellBg = C(8,8,14); gSkin.cRing = C(0,170,255); gSkin.cKnobFill = C(20,24,34); gSkin.cPtr = C(255,220,0);
  gSkin.cLabel = C(150,150,150); gSkin.cValue = C(255,255,255);
  gSkin.chk = scrChk();
}
static void scrLoad() {
  memcpy(&gSkin, (const void*)(XIP_BASE + SCR_FLASH_OFFSET), sizeof(gSkin));
  if (gSkin.magic != SCR_MAGIC || gSkin.chk != scrChk()) scrDefaults();
}
static void scrSave() {
  gSkin.magic = SCR_MAGIC; gSkin.chk = scrChk();
  static uint8_t page[256];
  gPauseCore1 = 1;
  if (gCore1Started) multicore_lockout_start_blocking();
  uint32_t ints = save_and_disable_interrupts();
  myFlashErase(SCR_FLASH_OFFSET);
  memset(page, 0xFF, 256); memcpy(page, &gSkin, sizeof(gSkin));
  myFlashProgram(SCR_FLASH_OFFSET, page, 256);
  restore_interrupts(ints);
  if (gCore1Started) multicore_lockout_end_blocking();
  gPauseCore1 = 0;
}

// ---- 绘图 ----
static void drawChar(int x, int y, char ch, uint16_t c, int sc) {
  if (ch < 32 || ch > 90) ch = 32;
  const uint8_t* g = font5x7[ch - 32];
  for (int cx = 0; cx < 5; cx++) {
    uint8_t bits = g[cx];
    for (int cy = 0; cy < 7; cy++) if ((bits >> cy) & 1)
      for (int sy = 0; sy < sc; sy++) for (int sx = 0; sx < sc; sx++) {
        int px = x + cx*sc + sx, py = y + cy*sc + sy;
        if (px >= 0 && px < LCD_W && py >= 0 && py < LCD_H) putPix(py*LCD_W + px, c);
      }
  }
}
static int textW(const char* s, int sc) { int n = 0; while (*s++) n++; return n*6*sc - sc; }
static void drawText(int x, int y, const char* s, uint16_t c, int sc) {
  for (; *s; s++) { drawChar(x, y, *s, c, sc); x += 6*sc; }
}
static inline uint16_t lerp565(uint16_t a, uint16_t b, float t) {   // RGB565 线性插值 (旋钮抗锯齿)
  if (t <= 0.0f) return a; if (t >= 1.0f) return b;
  int ar=(a>>11)&31, ag=(a>>5)&63, ab=a&31, br=(b>>11)&31, bg=(b>>5)&63, bb=b&31;
  int rr=ar+(int)((br-ar)*t+0.5f), gg=ag+(int)((bg-ag)*t+0.5f), bl=ab+(int)((bb-ab)*t+0.5f);
  return (uint16_t)((rr<<11)|(gg<<5)|bl);
}
static void drawKnob(int cx, int cy, int r, float v, uint16_t ring) {
  const uint16_t fill = gSkin.cKnobFill;          // 旋钮底色 (皮肤)
  const uint16_t bg   = gSkin.cCellBg;            // 圈外=格子底 (做外缘抗锯齿)
  int ro = r + 2;
  for (int dy = -ro; dy <= ro; dy++) {            // 逐像素按距离画 + 边沿抗锯齿 (消尖刺/锯齿)
    for (int dx = -ro; dx <= ro; dx++) {
      float d = sqrtf((float)(dx*dx + dy*dy));
      if (d > (float)r + 1.5f) continue;
      int x = cx + dx, y = cy + dy;
      if (x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) continue;
      uint16_t base = (d < (float)r) ? fill : bg;              // 圈内=底, 圈外=格子底
      float a = 1.0f - (fabsf(d - (float)r) - 0.5f);           // 环带: r±0.5 内=1, r±1.5 外=0
      if (a < 0.0f) a = 0.0f; if (a > 1.0f) a = 1.0f;
      putPix(y*LCD_W + x, lerp565(base, ring, a));
    }
  }
  if (v < 0) v = 0; if (v > 1) v = 1;
  float ang = (135.0f + 270.0f * v) * 3.14159265f / 180.0f;   // 0=左下, 1=右下, 中间朝上
  float ca = cosf(ang), sa = sinf(ang);
  int ir = r - 2;
  for (int t = 0; t <= ir; t++) {
    int x = cx + (int)(t * ca + (ca >= 0 ? 0.5f : -0.5f));
    int y = cy + (int)(t * sa + (sa >= 0 ? 0.5f : -0.5f));
    for (int o = 0; o < 2; o++) {                  // 指针加粗到 2px
      int qx = x + o;
      if (qx >= 0 && qx < LCD_W && y >= 0 && y < LCD_H) putPix(y*LCD_W + qx, gSkin.cPtr);
    }
  }
}
static void drawCell(int col, int row, const char* label, float v, const char* val) {
  int CW = gSkin.cellW, x = col*CW, y = gSkin.scopeH + row*gSkin.cellH;
  drawKnob(x+CW/2, y+14, gSkin.knobR, v, gSkin.cRing);
  drawText(x + (CW - textW(label,1))/2, y+28, label, gSkin.cLabel, 1);
  drawText(x + (CW - textW(val,1))/2,  y+38, val,   gSkin.cValue, 1);
}
static bool drawScope(bool force) {
  static int16_t prevY[LCD_W];
  static bool first = true;
  int SH = gSkin.scopeH; if (SH < 40) SH = 40; if (SH > LCD_H) SH = LCD_H;
  int mid = SH / 2;                                     // 纵向映满量程
  float og = gMaster > 0.02f ? gMaster : 0.02f;         // gMaster 只是"防削波的固定衰减"(默认0.10); 示波器按未衰减前的电平显示, 真实反映(含用户音量)且最大到顶
  int amp = (int)(mid / og); if (amp < mid) amp = mid; if (amp > mid * 16) amp = mid * 16;
  int y[LCD_W];
  uint16_t w = gScopeW;
  if (w < 4) {
    for (int x = 0; x < LCD_W; x++) y[x] = mid;
  } else {
    const uint16_t M = SCOPE_N - 1;
    int base = (int)w - 240;
    int s0 = base - 400; if (s0 < 1) s0 = 1;
    int s1 = base + 240;
    long sum = 0; int mx = 0;
    for (int i = s0; i < s1; i++) { int16_t a = gScope[i & M]; sum += a; int aa = a < 0 ? -a : a; if (aa > mx) mx = aa; }
    int dc = (int)(sum / (s1 - s0));
    int hyst = mx / 4 + 30;
    int start = base;
    for (int i = base; i > s0 && i > 1; i--) {           // 去直流后的上升过零 + 自适应迟滞
      int16_t a = gScope[(i-1) & M] - dc, b = gScope[i & M] - dc;
      if (a < -hyst && b >= hyst) { start = i; break; }
    }
    for (int x = 0; x < LCD_W; x++) {
      int sv = (int)gScope[(start + x) & M] - dc;
      int yy = mid - (sv * amp) / 32768;                 // 固定满量程映射 (非自动缩放): 幅度真实反映声音大小, 最大到顶
      if (yy < 0) yy = 0; if (yy > SH-2) yy = SH-2;
      y[x] = yy;
    }
  }
  bool same = true;
  for (int x = 0; x < LCD_W; x++) if (y[x] != prevY[x]) { same = false; break; }
  if (!force && !first && same) return false;          // 画面没变则不重画不刷屏
  first = false;
  for (int x = 0; x < LCD_W; x++) prevY[x] = (int16_t)y[x];
  for (int i = 0; i < LCD_W*SH; i++) fb[i] = gSkin.cScopeBg;
  for (int x = 0; x < LCD_W; x += 30) for (int yy = 0; yy < SH; yy++) putPix(yy*LCD_W+x, gSkin.cScopeGrid);
  for (int yy = 0; yy < SH; yy += 35) for (int x = 0; x < LCD_W; x++) putPix(yy*LCD_W+x, gSkin.cScopeGrid);
  int prev = mid;
  for (int x = 0; x < LCD_W; x++) {
    int yy = y[x];
    int a = prev < yy ? prev : yy, b = prev < yy ? yy : prev;
    for (int t = a; t <= b; t++) putPix(t*LCD_W + x, gSkin.cScopeTrace);
    prev = yy;
  }
  for (int x = 0; x < LCD_W; x++) putPix((SH-1)*LCD_W + x, gSkin.cDivider);
  return true;
}
static void drawUI() {
  gUiFrames++;
  bool force = gUiForce; if (force) gUiForce = 0;
  bool scopeChanged = drawScope(force) || force;   // 波形没变则 false; force 时强制
  static int lastP[8] = { -99999,-99999,-99999,-99999,-99999,-99999,-99999,-99999 };
  int p[8];
  p[0] = (int)(pTuneSemis * 10.0f);
  p[1] = (int)pCutHz;
  p[2] = (int)pResPct;
  p[3] = (int)pEnvModPct;
  p[4] = (int)pDecayMs;
  p[5] = (int)(pAccent * 255.0f);
  p[6] = (int)(pDist * 255.0f);
  p[7] = (int)(pWave * 100.0f);
  static const int thr[8] = { 2, 5, 2, 2, 5, 2, 2, 2 };
  bool changed = force;
  for (int i = 0; i < 8; i++) { int d = p[i] - lastP[i]; if (d < 0) d = -d; if (d > thr[i]) changed = true; }
  if (changed) {                                 // 参数区仅在真正变化时重画 (无节流)
      for (int i = LCD_W*gSkin.scopeH; i < LCD_W*LCD_H; i++) fb[i] = gSkin.cCellBg;
      char b[12];
      float uCutLog = logf(pCutHz / CUT_MIN_HZ) / logf(CUT_MAX_HZ / CUT_MIN_HZ);
      float uDcyLog = (pDecayMs - DECAY_MIN_MS) / (DECAY_MAX_MS - DECAY_MIN_MS);   // 线性 (原版 1M lin)
      float uTune   = (pTuneSemis / TUNE_RANGE_SEMIS + 1.0f) * 0.5f;
      int nCells = gSkin.cellCols * gSkin.cellRows; if (nCells < 1) nCells = 1; if (nCells > 8) nCells = 8;
      for (int i = 0; i < nCells; i++) {
        int fn = gSkin.cellFn[i] & 7; const char* lb; float kv = 0.0f;
        switch (fn) {
          case 0: lb = "TUNE"; kv = uTune; snprintf(b, sizeof b, "%+.1fST", pTuneSemis); break;
          case 1: lb = "CUT"; kv = uCutLog; if (pCutHz >= 1000) snprintf(b, sizeof b, "%.1fK", pCutHz/1000.0f); else snprintf(b, sizeof b, "%dHZ", (int)pCutHz); break;
          case 2: lb = "RES"; kv = pResPct/100.0f; snprintf(b, sizeof b, "%d", (int)(pResPct*2.55f+0.5f)); break;
          case 3: lb = "DIST"; kv = pDist; snprintf(b, sizeof b, "%d", (int)(pDist*255.0f+0.5f)); break;
          case 4: lb = "ENV"; kv = pEnvModPct/100.0f; snprintf(b, sizeof b, "%d", (int)(pEnvModPct*2.55f+0.5f)); break;
          case 5: lb = "DEC"; kv = uDcyLog; if (pDecayMs >= 1000) snprintf(b, sizeof b, "%.1fS", pDecayMs/1000.0f); else snprintf(b, sizeof b, "%dMS", (int)pDecayMs); break;
          case 6: lb = "ACC"; kv = pAccent; snprintf(b, sizeof b, "%d", (int)(pAccent*255.0f+0.5f)); break;
          default: lb = "WAVE"; kv = pWave; snprintf(b, sizeof b, "%s", pWave < 0.5f ? "SAW" : "SQR"); break;
        }
        drawCell(i % gSkin.cellCols, i / gSkin.cellCols, lb, kv, b);
      }
      for (int i = 0; i < 8; i++) lastP[i] = p[i];
  }
  if (scopeChanged) uiPend |= 1;
  if (changed)      uiPend |= 2;
}

// ================= PC 模式: 停刷屏 + 黄色笑脸 + 示波器/参数上传 =================
static void drawSmiley() {                             // PC 模式笑脸: 点阵图直接画 (无文字, 一次推屏)
  lcdWaitIdle();
  const uint16_t BG = C(0, 0, 0), FG = C(255, 205, 0);
  for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = BG;
  const int SC = LCD_W / SMILEY_W;                     // 整数倍放大 (80->3x)
  const int OX = (LCD_W - SMILEY_W * SC) / 2, OY = (LCD_H - SMILEY_W * SC) / 2;
  for (int y = 0; y < SMILEY_W; y++) {
    for (int x = 0; x < SMILEY_W; x++) {
      if (!(smiley_bmp[y * SMILEY_BPR + (x >> 3)] & (0x80 >> (x & 7)))) continue;
      int px = OX + x * SC, py = OY + y * SC;
      for (int dy = 0; dy < SC; dy++) for (int dx = 0; dx < SC; dx++) {
        int qx = px + dx, qy = py + dy;
        if (qx >= 0 && qx < LCD_W && qy >= 0 && qy < LCD_H) putPix(qy * LCD_W + qx, FG);
      }
    }
  }
  lcdPushFb();
}

// ================= 用户图片 (存 flash; PC 模式显示) =================
// flash 布局: program 低段 / 用户图片 @0x00100000 (头 1 扇区 + 数据) / 小屏皮肤 0x00FFD000 / 设置 0x00FFE000 / pattern 0x00FFF000
// 图片区开放: 1MB..0xFFD000 (≈15.9MB); 绘制器按 芯片-0.3MB 上限控制
#define IMG_FLASH_OFFSET  0x00100000u
#define IMG_DATA_OFFSET   (IMG_FLASH_OFFSET + 0x1000u)
#define IMG_MAGIC         0x494D4731u                 // "IMG1"
#define IMG_MAX_LEN       IMG_CAP_BYTES
#define IMG_CHUNK         4096u
#define IMG_BPR           30                          // 1-bit 240 宽 -> 30 B/行
struct ImgHeader { uint32_t magic; uint16_t mode, w, h; uint32_t len, chk; };
static ImgHeader gImg;
static volatile int gHasImg = 0;
static volatile int gImgUpActive = 0;
static volatile uint32_t gImgLastRx = 0;      // 上传中最近一次收到数据的时间 (超时自动放弃)
static uint16_t gImgUpMode = 1;
static uint32_t gImgUpLen = 0, gImgUpGot = 0, gImgUpChk = 2166136261u, gImgUpOff = 0, gImgUpBufLen = 0;
static uint8_t gImgUpBuf[IMG_CHUNK];
// ---- 多媒体: 帧序列 (多图/GIF/视频抽帧) ----
#define IMG_MAGIC2       0x494D4732u                 // "IMG2" 多媒体
#define IMG_MAX_FRAMES   256
struct FrameEnt { uint32_t off; uint32_t len; uint16_t delay; uint16_t rsv; };
static int       gImgMulti = 0;
static int       gFrameCount = 0;
static FrameEnt  gFrames[IMG_MAX_FRAMES];
static int       gFrameIdx = 0;
static uint32_t  gFrameNextMs = 0;
static void imgParseMulti();
static void imgDrawFrame(int idx);

static uint32_t imgChk(const uint8_t* p, uint32_t n, uint32_t s) { for (uint32_t i = 0; i < n; i++) { s ^= p[i]; s *= 16777619u; } return s; }
static void imgErase(uint32_t bytes) {                // 擦除 [IMG_FLASH_OFFSET, +bytes)
  uint32_t n = (bytes + 0xFFFu) & ~0xFFFu;
  gPauseCore1 = 1;
  if (gCore1Started) multicore_lockout_start_blocking();
  for (uint32_t off = 0; off < n; off += 4096) {
    uint32_t ints = save_and_disable_interrupts();
    myFlashErase(IMG_FLASH_OFFSET + off);
    restore_interrupts(ints);
    watchdog_update();
  }
  if (gCore1Started) multicore_lockout_end_blocking();
  gPauseCore1 = 0;
}
static void imgProg(uint32_t off, const uint8_t* p, uint32_t n) {
  gPauseCore1 = 1;
  if (gCore1Started) multicore_lockout_start_blocking();
  uint32_t ints = save_and_disable_interrupts();
  myFlashProgram(IMG_DATA_OFFSET + off, p, n);
  restore_interrupts(ints);
  if (gCore1Started) multicore_lockout_end_blocking();
  gPauseCore1 = 0;
}
static void imgEraseRange(uint32_t off, uint32_t n) {   // 边写边擦: 擦掉该块覆盖的扇区
  uint32_t a = IMG_DATA_OFFSET + off, b = a + n;
  gPauseCore1 = 1;
  if (gCore1Started) multicore_lockout_start_blocking();
  for (uint32_t s = a & ~0xFFFu; s < b; s += 4096) {
    uint32_t ints = save_and_disable_interrupts();
    myFlashErase(s);
    restore_interrupts(ints);
    watchdog_update();
  }
  if (gCore1Started) multicore_lockout_end_blocking();
  gPauseCore1 = 0;
}
static void imgLoad() {
  gImgMulti = 0; gFrameCount = 0;
  memcpy(&gImg, (const void*)(XIP_BASE + IMG_FLASH_OFFSET), sizeof(gImg));
  if ((gImg.magic != IMG_MAGIC && gImg.magic != IMG_MAGIC2) || gImg.len == 0 || gImg.len > IMG_MAX_LEN) { gHasImg = 0; return; }
  gHasImg = (imgChk((const uint8_t*)(XIP_BASE + IMG_DATA_OFFSET), gImg.len, 2166136261u) == gImg.chk) ? 1 : 0;
  if (gHasImg && gImg.magic == IMG_MAGIC2) imgParseMulti();
}
static uint32_t imgUsedBytes() { return gHasImg ? (0x1000u + ((gImg.len + 0xFFFu) & ~0xFFFu)) : 0; }
static void imgClear() {
  ImgHeader h; memcpy(&h, (const void*)(XIP_BASE + IMG_FLASH_OFFSET), sizeof(h));   // 读头, 连数据区一起擦
  uint32_t bytes = 0x1000;
  if (h.magic == IMG_MAGIC && h.len > 0 && h.len <= IMG_MAX_LEN) bytes = 0x1000 + ((h.len + 0xFFFu) & ~0xFFFu);
  imgErase(bytes);
  gHasImg = 0; gImgMulti = 0; gFrameCount = 0; Serial.println("#IMGCLEAR");
  if (gPcMode) { lcdWaitIdle(); lcdInitSeq(); bbModeEnd(); drawSmiley(); }
}
static void imgStart(const char* s) {                 // ^<mode>,<len>  (mode 0=1bit, 1=RGB565)
  BB(21);
  int md = 0; long ln = 0;
  if (sscanf(s, "%d,%ld", &md, &ln) == 2 && ln > 0 && ln <= (long)IMG_MAX_LEN) {
    gImgUpMode = (md == 2) ? 2 : ((md != 0) ? 1 : 0);
    gImgUpLen = (uint32_t)ln; gImgUpGot = 0; gImgUpOff = 0; gImgUpBufLen = 0; gImgUpChk = 2166136261u;
    BB(22);
    gImgUpActive = 1; gImgLastRx = millis(); Serial.println("#IMGRDY"); BB(24);   // 不再整块预擦: 分块时边擦边写(快很多, 不会超时)
  } else Serial.println("#IMGBAD");
}
static void imgDraw() {
  if (gImgMulti) { imgDrawFrame(gFrameIdx); return; }
  if (!gHasImg) { drawSmiley(); return; }
  lcdWaitIdle();                                       // 上一帧 DMA 完成后才能重写 fb
  const uint8_t* src = (const uint8_t*)(XIP_BASE + IMG_DATA_OFFSET);
  if (gImg.mode == 0) {                               // 1-bit 240x240 (黄线黑底)
    for (int y = 0; y < LCD_H; y++)
      for (int x = 0; x < LCD_W; x++)
        fb[(y * LCD_W + x) ^ 1] = (src[y * IMG_BPR + (x >> 3)] & (0x80 >> (x & 7))) ? C(255, 205, 0) : C(0, 0, 0);
  } else {                                            // RGB565
    const uint16_t* p = (const uint16_t*)src;
    for (int i = 0; i < LCD_W * LCD_H; i++) fb[i ^ 1] = p[i];
  }
  lcdPushFb();
}
static void imgParseMulti() {                          // 解析数据区开头的帧表: [u16 count][count×{u16 delay, u32 len}] + 帧数据
  gImgMulti = 0; gFrameCount = 0;
  if (gImg.magic != IMG_MAGIC2) return;
  uint16_t cnt = 0, fw = LCD_W, fh = LCD_H;
  memcpy(&cnt, (const void*)(XIP_BASE + IMG_DATA_OFFSET), 2);
  memcpy(&fw, (const void*)(XIP_BASE + IMG_DATA_OFFSET + 2), 2);
  memcpy(&fh, (const void*)(XIP_BASE + IMG_DATA_OFFSET + 4), 2);
  if (cnt == 0 || cnt > IMG_MAX_FRAMES) return;
  if (fw < 8 || fw > LCD_W) fw = LCD_W; if (fh < 8 || fh > LCD_H) fh = LCD_H;
  gImg.w = fw; gImg.h = fh;
  uint32_t tabOff = 6, cum = 6u + (uint32_t)cnt * 6u;
  for (int i = 0; i < cnt; i++) {
    uint16_t dl = 0; uint32_t ln = 0;
    memcpy(&dl, (const void*)(XIP_BASE + IMG_DATA_OFFSET + tabOff), 2);
    memcpy(&ln, (const void*)(XIP_BASE + IMG_DATA_OFFSET + tabOff + 2), 4);
    tabOff += 6;
    if (ln == 0 || ln > IMG_MAX_LEN || cum > IMG_MAX_LEN - ln) break;
    gFrames[i].off = cum; gFrames[i].len = ln; gFrames[i].delay = dl ? dl : 100; gFrames[i].rsv = 0;
    cum += ln; gFrameCount = i + 1;
  }
  if (gFrameCount > 0) { gImgMulti = 1; gFrameIdx = 0; gFrameNextMs = millis(); }
}
static void imgDrawFrame(int idx) {
  if (idx < 0 || idx >= gFrameCount) return;
  lcdWaitIdle();                                       // 上一帧 DMA 完成后才能重写 fb (非阻塞推屏)
  const uint16_t* p = (const uint16_t*)(XIP_BASE + IMG_DATA_OFFSET + gFrames[idx].off);
  int fw = gImg.w, fh = gImg.h;
  if (fw < 1) fw = LCD_W; if (fh < 1) fh = LCD_H;
  int avail = (int)(gFrames[idx].len / 2);
  if (fw == LCD_W && fh == LCD_H) {                    // 全分辨率: 直接拷
    int n = avail; if (n > LCD_W * LCD_H) n = LCD_W * LCD_H;
    for (int i = 0; i < n; i++) fb[i ^ 1] = p[i];
  } else {                                             // 小分辨率: 最近邻放大铺满
    for (int dy = 0; dy < LCD_H; dy++) {
      int sy = dy * fh / LCD_H; if (sy >= fh) sy = fh - 1;
      for (int dx = 0; dx < LCD_W; dx++) {
        int sx = dx * fw / LCD_W; if (sx >= fw) sx = fw - 1;
        int si = sy * fw + sx;
        fb[(dy * LCD_W + dx) ^ 1] = (si < avail) ? p[si] : 0;
      }
    }
  }
  lcdPushFb();
}
static void imgReadBack() {                            // 回读媒体数据到 PC: "#IMGRD <mode> <w> <h> <len> <chk>" + len 字节 + "#IMGRDE"
  if (!gHasImg) { Serial.println("#IMGRD 0"); return; }
  Serial.print("#IMGRD "); Serial.print(gImg.mode); Serial.print(' '); Serial.print(gImg.w); Serial.print(' ');
  Serial.print(gImg.h); Serial.print(' '); Serial.print(gImg.len); Serial.print(' '); Serial.println(gImg.chk);
  const uint8_t* p = (const uint8_t*)(XIP_BASE + IMG_DATA_OFFSET);
  for (uint32_t i = 0; i < gImg.len; i += 512) {
    uint32_t n = gImg.len - i; if (n > 512) n = 512;
    uint32_t t0 = millis(); while (Serial.availableForWrite() < (int)n) { watchdog_update(); if (millis() - t0 > 2000) break; }
    Serial.write(p + i, n);
    watchdog_update();
  }
  Serial.flush(); Serial.println(); Serial.println("#IMGRDE");
}
static void imgPlayTick() {                            // PC 模式下按帧延时循环播放
  if (!gImgMulti || gFrameCount <= 1 || !gPcMode) return;
  if ((int32_t)(millis() - gFrameNextMs) >= 0) {
    gFrameIdx = (gFrameIdx + 1) % gFrameCount;
    imgDrawFrame(gFrameIdx);
    int dl = gFrames[gFrameIdx].delay; if (dl < 33) dl = 33;   // 节流: 最快 30fps, 别让整帧推屏把 core0 占满
    gFrameNextMs = millis() + dl;
  }
}
static void imgUploadTask() {                          // 收满一 chunk(4KB) 或收完 -> 写 flash -> 回 ACK
  if (Serial.available()) gImgLastRx = millis();
  while (Serial.available() && gImgUpBufLen < IMG_CHUNK) { gImgUpBuf[gImgUpBufLen++] = (uint8_t)Serial.read(); gImgUpGot++; }
  bool done = (gImgUpGot >= gImgUpLen);
  if (gImgUpBufLen == IMG_CHUNK || (done && gImgUpBufLen)) {
    uint32_t real = gImgUpBufLen, n = (real + 255u) & ~255u;
    if (n > real) memset(gImgUpBuf + real, 0xFF, n - real);
    gImgUpChk = imgChk(gImgUpBuf, real, gImgUpChk);
    imgEraseRange(gImgUpOff, n);
    imgProg(gImgUpOff, gImgUpBuf, n);
    gImgUpOff += real; gImgUpBufLen = 0;
    if (done) {
      gImg.magic = (gImgUpMode == 2) ? IMG_MAGIC2 : IMG_MAGIC; gImg.mode = gImgUpMode; gImg.w = LCD_W; gImg.h = LCD_H; gImg.len = gImgUpLen; gImg.chk = gImgUpChk;
      gImgUpActive = 0;
      { uint8_t page[256]; memset(page, 0xFF, 256); memcpy(page, &gImg, sizeof(gImg));
        gPauseCore1 = 1;
        if (gCore1Started) multicore_lockout_start_blocking();
        uint32_t ints = save_and_disable_interrupts();
        myFlashErase(IMG_FLASH_OFFSET);
        myFlashProgram(IMG_FLASH_OFFSET, page, 256);
        restore_interrupts(ints);
        if (gCore1Started) multicore_lockout_end_blocking(); gPauseCore1 = 0; }
      gHasImg = 1;
      Serial.println("#IMGDONE");
      imgLoad();                                    // 从 flash 回读校验 (chk 不符会自动清 gHasImg)
      lcdWaitIdle(); lcdInitSeq(); bbModeEnd(); imgDraw();
      Serial.println(gHasImg ? "#IMGOK" : "#IMGBADCHK");
    } else { Serial.print("#IMGC "); Serial.println(gImgUpOff); }
  }
}
static void pcStreamTask() {                            // 50ms: 参数 K... + 示波器 W... (W 分块非阻塞发, 防阻塞 core0)
  static uint32_t nextMs = 0;
  static int16_t wbuf[FAST_N]; static bool wPending = false; static uint16_t wIdx = 0;
  if (!gPcMode || !gPcStream) return;                   // 文本走第0路 CDC, 音频走第1路 -> 可共存, 互不挤
  if (wPending) {                                       // 继续发上次没发完的 W
    while (wIdx < FAST_N) { if (Serial.availableForWrite() < 12) return; Serial.print(','); Serial.print(wbuf[wIdx++]); }
    Serial.print('\n'); wPending = false; return;
  }
  if (millis() < nextMs) return;
  if (Serial.availableForWrite() < 80) return;          // K 行(~60B)放得下才发, 否则等下次
  nextMs = millis() + 50;
  Serial.print("K");                                    // K 行 (短, 不会阻塞)
  Serial.print(pCutHz, 0); Serial.print(','); Serial.print(pResPct, 0); Serial.print(',');
  Serial.print(pEnvModPct, 0); Serial.print(','); Serial.print(pDecayMs, 0); Serial.print(',');
  Serial.print((int)(pAccent * 255.0f)); Serial.print(','); Serial.print(pTuneSemis, 1);
  Serial.print(','); Serial.print(seqShowPat); Serial.print(','); Serial.print(seqShowStep);
  Serial.print(','); Serial.print(gSeqPlay);
  Serial.print(','); Serial.print((int)(pDist * 255.0f));
  Serial.print(','); Serial.print((int)(pColor * 255.0f));
  Serial.print(','); Serial.print((int)(pWave * 255.0f));     // 第12字段: 波形 (0=锯齿 255=方波)
  Serial.print(','); Serial.println(gSelfOsc);                // 第13字段: 自激 0/1 (含 V1.2 SW2 物理开关状态)
  uint16_t M = FAST_N - 1; int w = gScopeFastW;         // 快照 W 到缓冲
  for (int i = 0; i < FAST_N; i++) wbuf[i] = gScopeFast[(w - FAST_N + i) & M];
  wIdx = 0; wPending = true;
  Serial.print("W");
  while (wIdx < FAST_N) { if (Serial.availableForWrite() < 12) return; Serial.print(','); Serial.print(wbuf[wIdx++]); }
  Serial.print('\n'); wPending = false;
}

// ================= setup / loop =================
void setup() {
  (void)flashSizeBytes();     // 先以默认时钟探测真实 flash 容量 (JEDEC RDID), 免得超频影响探测
  overclockLadder();          // 板级超频 + 看门狗 (必须最先做)
  gLastBB = (uint8_t)((watchdog_hw->scratch[2] >> 24) & 0xFFu);   // 快照上次运行最后阶段(黑盒), 再清掉
  watchdog_hw->scratch[2] &= 0x00FFFFFFu;
  { uint32_t fpscr; __asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);                 // FZ|DN
    __asm volatile("vmsr fpscr, %0" :: "r"(fpscr)); } // 非规格化数刷零 (防滤波状态衰减时 FPU 变慢/卡)
  periFromSys();              // clk_peri = clk_sys (屏幕 SPI 才能高速)
  Serial.begin(115200);
  AudioCDC.setStringDescriptor("TB303Audio");   // 第二路 CDC: 音频 (需在 USB 枚举前注册)
  AudioCDC.begin(115200);
#if USE_USB_MIDI
  usb_midi.setStringDescriptor("Pico MIDI");    // MIDI 接口也必须在 USB 首次枚举前注册 (否则不进复合设备, DAW 看不到)
  usb_midi.begin();
#endif

  pinMode(ADS_CS, OUTPUT); digitalWrite(ADS_CS, HIGH);
  pinMode(PIN_USER, INPUT_PULLUP);   // V1.2 SW3 用户按键 (播放/暂停/双击回头)
  pinMode(PIN_USER2, INPUT_PULLUP);  // 老板 USER 键 GP23 (与 SW3 等价)
  pinMode(PIN_WAVE, INPUT_PULLDOWN);     // V1.2 SW1 波形拨动开关
  pinMode(PIN_MORERES, INPUT_PULLDOWN);  // V1.2 SW2 更多共振/自激拨动开关
  dacInit();                             // V1.2 AD5664 四路 CV/Gate 输出
  SPI.setSCK(ADS_SCLK);
  SPI.setTX(ADS_MOSI);
  SPI.setRX(ADS_MISO);
  SPI.begin();
  adsBegin();

  // 初始化 DSP
  patLoad();                     // 从 flash 读回用户 pattern
  imgLoad();                     // 读回用户图片 (若有)
  scrLoad();                     // 读回小屏皮肤 (无则用默认)
  gFilter303.init();
  gOsc303.init();
  gEnv303.setSampleRate((float)SAMPLE_RATE);
  gEnv303.setDecayMs(pDecayMs);
  gLowPeak.setPeaking(LOWPEAK_F0, LOWPEAK_Q, LOWPEAK_DB, (float)SAMPLE_RATE);   // 第二低频共振峰(近似)
  gResBass.setPeaking(150.0f, 0.8f, 0.0f, (float)SAMPLE_RATE);                  // 谐振低频补偿 (增益随 res 变)
  gDist.setSampleRate((float)SAMPLE_RATE); gDist.reset();                        // TD-3 失真
  gPitchSlew.setTauMs(SLIDE_MS);                                                 // 原版滑音 τ≈60ms (一阶滞后)
  gAmpDeClick.setLowpass(200.0f, (float)SAMPLE_RATE);   // Open303 ampDeClicker
  calcEnvModScalerOffset();

  rgb.begin(); rgb.setBrightness(30); rgb.show();

  // 屏幕 ST7789 (PIO+DMA)
  gpio_init(PIN_CS);  gpio_set_dir(PIN_CS, OUTPUT);  gpio_put(PIN_CS, 1);
  gpio_init(PIN_DC);  gpio_set_dir(PIN_DC, OUTPUT);  gpio_put(PIN_DC, 1);
  gpio_init(PIN_RST); gpio_set_dir(PIN_RST, OUTPUT); gpio_put(PIN_RST, 1);
  gpio_init(PIN_BL);  gpio_set_dir(PIN_BL, OUTPUT);  gpio_put(PIN_BL, 0);      // 背光先关: 初始化好并推完首帧再点亮 (不会看到面板残留乱色)
  gpio_init(PIN_XSMT); gpio_set_dir(PIN_XSMT, OUTPUT); gpio_put(PIN_XSMT, 0);   // 先静音, 等 I2S 起好再放开
  gpio_init(PIN_CSF);  gpio_set_dir(PIN_CSF, OUTPUT);  gpio_put(PIN_CSF, 1);    // 字库片选拉高=不选中 (GP16 与屏幕 SDO 共用, 防字库驱动总线)
  lcdPioBegin();                 // 必须先建 PIO: 否则 lcdInitSeq 里 bbModeBegin 用 lcdSm=-1 (越界写 PIO 寄存器)
  lcdInitSeq();
  for (int i = 0; i < LCD_W*LCD_H; i++) fb[i] = C(0,0,0);
  lcdPushFb();
  gpio_put(PIN_BL, 1);           // 首帧(黑)推完 -> 开背光

#if USE_USB_MIDI
  // (USB 接口已在上方 Serial.begin 处一并注册; 不再开机 detach/attach, 免得 Windows 累积重复 MIDI 设备)
  // 等 USB 枚举完成再启动音频 (最多等 1.5s)
  { uint32_t t0 = millis(); while (!TinyUSBDevice.mounted() && millis() - t0 < 1500) delay(5); }
#endif

  // 音频放到 core1, core0 专职 USB/控制。给 core1 一个较大的栈 (默认栈太小, 重 ISR 会溢出)
  static uint32_t core1_stack[16384];   // 64KB
  multicore_launch_core1_with_stack(core1_entry, core1_stack, sizeof(core1_stack));
  gCore1Started = 1;                    // 之后才允许 lockout 写 flash

  delay(100);
  Serial.print("PICO_TB303: ready. CPU="); Serial.print(gOcKHz / 1000.0f, 1);
  Serial.print("MHz ocIdx="); Serial.print(gOcIdx);
  Serial.print(" fails="); Serial.print(gOcFails);
  Serial.print(" bb="); Serial.println(gLastBB);   // 上次 loop 最后阶段 (卡死定位)
}

void loop() {
  watchdog_update();          // 喂狗: 主循环存活证明 (死机 -> 6s 后自动复位降档)
  BB(1);
  if (gPcMode) pcStreamTask();        // PC 文本流优先于音频块, 免得被音频挤满 FIFO 发不出去
  imgPlayTick();                      // PC 模式: 多媒体(GIF/视频抽帧)按帧延时循环播放
  // ---- USB 音频回传: 把 64 点/帧(128B)搬进 CDC1; 实现见 audioPump() (原 4 帧批量等价) ----
  BB(11);
  audioPump();                        // 与"等 LCD DMA 时"共用同一实现
  static bool ocStable = false;
    if (!ocStable && millis() > 4000) { ocStable = true; watchdog_hw->scratch[3] = 0; gOcFails = 0; }
  BB(2);
#if USE_USB_MIDI
  midiTask();
#endif

  produceStage1();            // 音频 stage1 (包络/滑音/截止) -> 流水线交给 core1
  userKeyTask();              // V1.2 SW3: 单击 播放/暂停, 双击 回头
  switchTask();               // V1.2 SW1/SW2: 波形 / 自激 拨动开关
  { static uint32_t nextDac = 0; if (millis() >= nextDac) { nextDac = millis() + 1; dacTask(); } }  // V1.2 DAC 更新 ~1kHz
  { // core1 音频 liveness: ISR 停 >2.5s -> 复位自恢复 (防"没声音/示波器不动"死住; 阈值放宽防误触发)
    static uint32_t lastBlk = 0, lastMs = 0;
    if (isrBlocks != lastBlk) { lastBlk = isrBlocks; lastMs = millis(); }
    else if (lastMs && (millis() - lastMs) > 2500) { watchdog_hw->scratch[1] |= (1u << 12); watchdog_reboot(0, 0, 0); }
  }
  BB(3);

  // ---- 串口命令 ----
  if (gImgUpActive) {                       // 上传中: 收原始二进制写 flash, 不解析文本
    if (millis() - gImgLastRx > 3000) { gImgUpActive = 0; gImgUpGot = 0; gImgUpBufLen = 0; gHasImg = 0; gImgMulti = 0; if (gPcMode) gDrawSmiley = 1; Serial.println("#IMGABORT"); }   // 断线自动放弃 + 作废旧图(防花屏)
    else imgUploadTask();
  } else {
    static char cmdBuf[128]; static uint8_t cmdLen = 0;
    while (Serial.available()) {
      char ch = (char)Serial.read();
      if (ch == '\n' || ch == '\r') { cmdBuf[cmdLen] = 0; if (cmdLen) handleCmd(cmdBuf); cmdLen = 0; }
      else if (cmdLen < sizeof(cmdBuf) - 1) cmdBuf[cmdLen++] = ch;
    }
  }
  BB(4);

  // ---- 旋钮 (原版 TB-303 面板 6 旋钮; 串口 p0 可暂停改用串口/MIDI CC) ----
  if (gUsePots) {
    static float fTune = 0, fAcc = 0, fCut = 2048, fRes = 0, fDcy = 2048, fEnv = 0, fDst = 0, fCol = 0;
    fTune += ((float)adsReadCh(POT_CH_TUNE)   - fTune) * 0.25f;
    fAcc  += ((float)adsReadCh(POT_CH_ACCENT) - fAcc)  * 0.25f;
    fCut  += ((float)adsReadCh(POT_CH_CUTOFF) - fCut)  * 0.25f;
    fRes  += ((float)adsReadCh(POT_CH_RES)    - fRes)  * 0.25f;
    fDcy  += ((float)adsReadCh(POT_CH_DECAY)  - fDcy)  * 0.25f;
    fEnv  += ((float)adsReadCh(POT_CH_ENVMOD) - fEnv)  * 0.25f;
    fDst  += ((float)adsReadCh(POT_CH_DIST)   - fDst)  * 0.25f;
    fCol  += ((float)adsReadCh(POT_CH_COLOR)  - fCol)  * 0.25f;

    float uTune = potNorm((uint16_t)fTune, INV_TUNE);
    float uAcc  = potNorm((uint16_t)fAcc,  INV_ACCENT);
    float uCut  = potNorm((uint16_t)fCut,  INV_CUTOFF);
    float uRes  = potNorm((uint16_t)fRes,  INV_RES);
    float uDcy  = potNorm((uint16_t)fDcy,  INV_DECAY);
    float uEnv  = potNorm((uint16_t)fEnv,  INV_ENVMOD);
    float uDst  = potNorm((uint16_t)fDst,  INV_DIST);
    float uCol  = potNorm((uint16_t)fCol,  INV_COLOR);

    // 死区 (抑制 ADC 小幅乱跳); 每路独立
    static float pT = 0.5f, pA = 0, pC = 0, pR = 0, pD = 0, pE = 0, pI = 0, pO = 0.5f;
    const float DB = 0.006f;
    uTune = dbnd(pT, uTune, DB); pT = uTune;
    uAcc  = dbnd(pA, uAcc,  DB); pA = uAcc;
    uCut  = dbnd(pC, uCut,  DB); pC = uCut;
    uRes  = dbnd(pR, uRes,  DB); pR = uRes;
    uDcy  = dbnd(pD, uDcy,  DB); pD = uDcy;
    uEnv  = dbnd(pE, uEnv,  DB); pE = uEnv;
    uDst  = dbnd(pI, uDst,  DB); pI = uDst;
    uCol  = dbnd(pO, uCol,  DB); pO = uCol;
    if (fabsf(uTune - 0.5f) < TUNE_SNAP) uTune = 0.5f;   // TUNE 中点吸附

    // 电位器全部换 B 线性 -> 不再做 A 锥换算 (CUT/ENV/DEC 均为线性位置)
    // CUT 参数轴仍按频率指数(标准滤波手感); DECAY 改回线性 ms

    pTuneSemis = (uTune * 2.0f - 1.0f) * TUNE_RANGE_SEMIS;
    pAccent    = uAcc;
    pCutHz     = CUT_MIN_HZ * powf(CUT_MAX_HZ / CUT_MIN_HZ, uCut);
    pResPct    = uRes * 100.0f;
    pDecayMs   = DECAY_MIN_MS + (DECAY_MAX_MS - DECAY_MIN_MS) * uDcy;   // B 线性电位器 -> 衰减时长线性
    pEnvModPct = uEnv * 100.0f;
    pDist      = uDst;
    pColor     = uCol;
  }  // gUsePots
  { static float lastRB = -1.0f;                 // 谐振低频补偿: 增益随 res (0..+9dB), 只在变化时重算
    if (gResBassComp && pResPct != lastRB) { lastRB = pResPct; gResBass.setPeaking(150.0f, 0.8f, pResPct * 0.09f, (float)SAMPLE_RATE); } }
  calcEnvModScalerOffset();

  // ---- §49 元件容差 / §50 温度漂移 (软件旋钮 0=关; 极慢) ----
  {
    static bool driftInit = false;
    static uint32_t lastDrift = 0;
    if (!driftInit) { driftInit = true; gVarTuneC = driftRnd()*10.0f; gVarCutOc = driftRnd()*0.08f; gVarGainF = driftRnd()*0.08f; }
    if (millis() - lastDrift >= 100) {
      lastDrift = millis();
      gDrT = clampf(gDrT * 0.98f + driftRnd() * 0.03f, -1.0f, 1.0f);
      gDrC = clampf(gDrC * 0.98f + driftRnd() * 0.03f, -1.0f, 1.0f);
      gDrG = clampf(gDrG * 0.98f + driftRnd() * 0.03f, -1.0f, 1.0f);
    }
    gDetuneLin    = exp2f((gVarTuneC * gVarAmt + gDrT * 20.0f * gTempAmt) / 1200.0f);
    gCutDriftLin  = exp2f(gVarCutOc * gVarAmt + gDrC * 0.15f * gTempAmt);
    gGainDriftLin = 1.0f + gVarGainF * gVarAmt + gDrG * 0.15f * gTempAmt;
  }

  // ---- 内置音序器 (未收到 MIDI 音符时播放 flash 里的 pattern) ----
  BB(5);
  if (gAutoTest) seqTask();

  // ---- 屏幕 UI (core0) ----
  if (gPcMode) {                          // PC 软件已连接: 完全停刷屏; 仅画一次笑脸 + 上传示波器/参数
    if (gDrawSmiley) { gDrawSmiley = 0; lcdWaitIdle(); lcdInitSeq(); bbModeEnd(); if (gHasImg) imgDraw(); else drawSmiley(); }
    BB(6);
  } else {
    static uint32_t nextUi = 0, nextKick = 0;
    if (gLcdReinit) { gLcdReinit = 0; lcdWaitIdle(); lcdInitSeq(); bbModeEnd(); lcdFill(C(0,0,0)); gUiForce = 1; }
    if (millis() >= nextKick && !lcdDmaBusy) { nextKick = millis() + 3000; lcdRegRefresh(); }  // 屏心跳 (DMA 空闲时才发)
    if (millis() >= nextUi) { nextUi = millis() + 42; uint32_t t = time_us_32(); drawUI(); gUiUs = time_us_32() - t; }  // 24fps
    lcdPushTask();          // 非阻塞收尾/启动下一块 (不阻塞 core0)
  }

  // ---- RGB (res 指示) ----
  static uint32_t nextRGB = 0;
  if (millis() >= nextRGB) {
    nextRGB = millis() + 50;
    float t = clampf(pResPct / 100.0f, 0.0f, 1.0f);
    uint32_t col = rgb.Color((uint8_t)(50 + 205 * t), (uint8_t)(255 * (1 - t)), 0);
    for (int i = 0; i < NEOPIXEL_NUM; i++) rgb.setPixelColor(i, col);
    rgb.show();
  }

  // ---- 持久设置: 去抖写 flash (拖旋钮停 1.5s 后才写, 避免狂写磨损) ----
  if (gSetDirty && millis() - gSetChangeMs > 1500) { gSetDirty = 0; setSave(); }

  // ---- 周期 CPU 占用自动报告 (RP2350 裸板测试: 串口 I0 关 / I1 开) ----
  static uint32_t nextReport = 0;
  if (gAutoReport && millis() >= nextReport) { nextReport = millis() + 2000; reportCpu(); }

  // ---- 调试 ----
#if DEBUG_SERIAL
  static uint32_t nextPrint = 0, lastPrint = 0;
  if (Serial && millis() >= nextPrint) {
    uint32_t now = millis();
    uint32_t dt = now - lastPrint; lastPrint = now;
    nextPrint = now + 100;
    uint32_t us = isrUs; isrUs = 0;
    uint32_t bl = isrBlocks; isrBlocks = 0;
    Serial.print(" res="); Serial.print(pResPct, 0);
    Serial.print(" dec="); Serial.print(pDecayMs, 0);
    Serial.print(" acc="); Serial.print(pAccent, 2);
    Serial.print(" env="); Serial.print(pEnvModPct, 0);
    Serial.print(" wave="); Serial.print(pWave, 2);
    Serial.print("voct="); Serial.print(0.0f, 3);
    Serial.print(" of="); Serial.print(oscFreq, 2);
    Serial.print(" cpu=");
    if (dt > 0) Serial.print((float)us * 100.0f / ((float)dt * 1000.0f), 1);
    Serial.print("% dt="); Serial.print(dt);
    Serial.print(" oscN="); Serial.print(cntOsc);
    cntOsc = 0;
    Serial.print(" blk="); Serial.println(bl);
  }
#endif
}

