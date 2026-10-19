/*
 * OV-JP8000 — 用 OV-303 的硬件做一台 JP-8000 风格合成器 (不是 303)
 *
 * 硬件完全沿用 OV-303 V1.2/V1.3:
 *   PCM5102A  I2S : BCK=GP13, LRCK=GP14, DIN=GP15, XSMT=GP27
 *   ADS8688   SPI : SCLK=GP2, MOSI=GP3, MISO=GP4, CS=GP5
 *   AD5664    CV  : SYNC#=GP9, SCLK=GP10, DIN=GP11
 *   ST7789    屏  : 暂不驱动 (背光关闭), 参数用串口/物理旋钮
 *   SW1=GP6, SW2=GP7, SW3=GP8(按键), 板载 RGB=GP16 (本版不用)
 *
 * 8 个旋钮的新映射:
 *   AIN0 TUNE     -> VCF Cutoff
 *   AIN1 CUTOFF   -> VCF Resonance
 *   AIN2 RES      -> ADSR A/B Decay
 *   AIN3 ENV.MOD  -> ADSR A/B Sustain
 *   AIN4 DECAY    -> ADSR A/B Release
 *   AIN5 ACCENT   -> Filter Env Depth
 *   AIN6 DIST     -> SuperSaw Detune
 *   AIN7 COLOR    -> SuperSaw Mix
 *
 * 开关:
 *   SW1 (GP6)  -> 旋钮编辑对象: 低=AMP ADSR, 高=FILTER ADSR (两套同时生效)
 *   SW2 (GP7)  -> VCF 斜率: 低=12dB, 高=24dB
 *   SW3 (GP8)  -> VCF 模式循环: LP -> BP -> HP (按键)
 *
 * 声音结构 (每声部, 最多 8 声部):
 *   SuperSaw 7 锯齿 (带限 mipmap 表, detune/mix, 随机相位, 立体声 pan)
 *   -> 每声部固定 20Hz 次声 HPF (不再跟随 f0)
 *   -> JP-8000 风格多模式 SVF (LP/BP/HP, 12/24dB, 共振)
 *   -> ADSR (AMP + FILTER 两套同时生效; SW1 只切换旋钮编辑对象)
 *   -> 立体声混音
 *
 * 参考:
 *   - Roland JP-8000 是 8 声部虚拟模拟; VCF = 12/24dB LP/BP/HP, 两个 ADSR
 *   - Adam Szabo《How to Emulate the Super Saw》(7 saw / 11 阶 detune / mix)
 *   - 本机 OV-03 的 ADS8688/I2S/AD5664 初始化代码 (硬件不变)
 */

#include <Arduino.h>
#include <SPI.h>
#include <I2S.h>
#include <Adafruit_TinyUSB.h>
#include <pico/multicore.h>
#include <pico/bootrom.h>
#include <hardware/vreg.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/sync.h>
#include <hardware/watchdog.h>
#include <hardware/structs/qmi.h>
#include <hardware/structs/xip.h>
#include <hardware/pio.h>
#include <hardware/dma.h>
#include "font5x7.h"
#include "lcd_spi.pio.h"

// ================= 引脚 =================
#define ADS_SCLK      2
#define ADS_MOSI      3
#define ADS_MISO      4
#define ADS_CS        5
#define I2S_BCLK     13
#define I2S_DOUT     15
#define PIN_XSMT     27
#define PIN_SW_ENV    6    // SW1: ADSR 目标 (低 AMP / 高 FILTER)
#define PIN_SW_SLOPE  7    // SW2: 滤波斜率 (低 12dB / 高 24dB)
#define PIN_BTN_MODE  8    // SW3: VCF 模式 LP->BP->HP
#define PIN_DAC_SYNC  9
#define PIN_DAC_SCLK 10
#define PIN_DAC_DIN  11
#define PIN_SCK      18    // ST7789 SPI1 SCK
#define PIN_MOSI     19    // ST7789 SPI1 MOSI
#define PIN_BL       26    // 屏幕背光
#define PIN_CS       20
#define PIN_DC       21
#define PIN_RST      22

// ================= 配置 =================
#define SAMPLE_RATE    48000
#define CPU_KHZ        375000
#define NUM_VOICES     8
#define NUM_SAWS       7
#define USE_USB_MIDI   1
#define MIDI_CHANNEL   1
#define POT_VREF       3.3f
#define ADS_RANGE      0x06
#define ADS_FS_VOLTS   (1.25f * 4.096f)
#define PI_F           3.14159265358979f

// 前向声明 (Arduino 会自动生成函数原型, 需要让 ADSR/Voice 先可见)
struct ADSRCoefs;
struct ADSR;
struct Voice;

// ================= 参数 =================
// ADSR 不再共用一个：AMP 和 FILTER 各存一套 D/S/R，SW1 只切换"当前编辑哪一套"
struct ADSRParams { float decay, sustain, release; };
volatile ADSRParams ampP  = {0.30f, 0.70f, 0.30f};
volatile ADSRParams filtP = {0.50f, 0.20f, 0.40f};
#define FIXED_ATTACK_S 0.001f        // Attack 固定 1ms (用户不需要 Attack 旋钮)
volatile float pCutoff  = 2500.0f;  // Hz
volatile float pRes     = 0.30f;    // 0..1
volatile float pEnvDepth = 0.0f;    // 滤波包络深度 -1..+1 -> -6..+6 oct (中心=0, JP-8000 可反向)
volatile float pDetune  = 0.35f;    // 0..1
volatile float pMix     = 0.70f;    // 0..1

volatile int gEditTarget = 0;       // 0=旋钮编辑 AMP ADSR, 1=旋钮编辑 FILTER ADSR (两套同时生效)
volatile int gSlope24   = 0;        // 0=12dB, 1=24dB
volatile int gFilterMode = 0;       // 0=LP, 1=BP, 2=HP
volatile float gBend = 1.0f;        // pitch bend ratio
volatile int   gUsePots = 1;        // 1=读物理旋钮; 0=串口/调试控制 (p0/p1)
volatile int   gScreenOn = 1;       // 1=小屏刷新; 0=停屏 (诊断 bitcrush/CPU)
volatile int   gSineTest = 0;       // 1=1kHz 纯正弦直出, 绕过全部 DSP/限幅
volatile int   gBypassFilter = 0;   // 1=跳过 VCF
volatile int   gLimiter = 1;        // 0=关闭输出软限幅
volatile int   gBypassHPF = 0;      // 1=跳过每声部基频高通
volatile int   gCtrlFast = 0;       // 1=滤波系数每采样更新 (诊断 3kHz 阶梯)

I2S i2s(OUTPUT, I2S_BCLK, I2S_DOUT);
volatile uint32_t gIsrUs = 0, gIsrBlocks = 0;

// ================= 数学 =================
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float fastTanh(float x) {
  if (x < -3.0f) return -1.0f;
  if (x >  3.0f) return  1.0f;
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
static inline float potNorm(uint16_t raw, int invert) {
  float v = (float)raw / 65536.0f * ADS_FS_VOLTS / POT_VREF;
  v = clampf(v, 0.0f, 1.0f);
  return invert ? (1.0f - v) : v;
}

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

// ================= AD5664 =================
static void dacWrite(uint8_t ch, uint16_t code) {
  uint32_t w = ((uint32_t)0x3u << 19) | ((uint32_t)(ch & 7u) << 16) | (uint32_t)code;
  digitalWrite(PIN_DAC_SYNC, LOW);
  for (int i = 23; i >= 0; i--) {
    digitalWrite(PIN_DAC_DIN, (w >> i) & 1u);
    digitalWrite(PIN_DAC_SCLK, HIGH);
    digitalWrite(PIN_DAC_SCLK, LOW);
  }
  digitalWrite(PIN_DAC_SYNC, HIGH);
}
static inline uint16_t voltsToCode(float v) {
  if (v < 0.0f) v = 0.0f; if (v > 5.0f) v = 5.0f;
  return (uint16_t)(v / 5.0f * 65535.0f + 0.5f);
}
static void dacInit() {
  pinMode(PIN_DAC_SYNC, OUTPUT); digitalWrite(PIN_DAC_SYNC, HIGH);
  pinMode(PIN_DAC_SCLK, OUTPUT); digitalWrite(PIN_DAC_SCLK, LOW);
  pinMode(PIN_DAC_DIN,  OUTPUT); digitalWrite(PIN_DAC_DIN,  LOW);
  for (int ch = 0; ch < 4; ch++) dacWrite((uint8_t)ch, 0);
}

// ================= SuperSaw 带限表 (来自 JP8000 项目) =================
#define SS_TABLE_SIZE   2048
#define SS_TABLE_MASK   (SS_TABLE_SIZE - 1)
#define SS_TAB_MAX_MIPS 140
#define SS_TAB_POOL_PTS 86000
#define SS_TAPER_START  0.90f
#define SS_TAB_TOP_H    640
static const float SS_DET_OFF[NUM_SAWS] = {
  -0.110023f, -0.062884f, -0.019524f, 0.0f, 0.019912f, 0.062165f, 0.107452f
};
static uint16_t ssMipHarm[SS_TAB_MAX_MIPS];
static uint8_t  ssMipShift[SS_TAB_MAX_MIPS];
static uint32_t ssMipOff[SS_TAB_MAX_MIPS];
static int      ssNumMips = 0;
static int16_t  ssTabPool[SS_TAB_POOL_PTS];
static float    ssSineTab[SS_TABLE_SIZE];
static float    ssSawFloat[SS_TABLE_SIZE];
static int      ssMipLevel = 0;
static float    ssGainC = 1.0f, ssGainS = 0.0f;

static float ssDetuneCurve(float x) {
  x = clampf(x, 0.0f, 1.0f);
  float y = 10028.7312891634f;
  y = y * x - 50818.8652045924f;
  y = y * x + 111363.4808729368f;
  y = y * x - 138150.6761080548f;
  y = y * x + 106649.6679158292f;
  y = y * x - 53046.9642751875f;
  y = y * x + 17019.9518580080f;
  y = y * x - 3425.0836591318f;
  y = y * x + 404.2703938388f;
  y = y * x - 24.1878824391f;
  y = y * x + 0.6717417634f;
  y = y * x + 0.0030115596f;
  return clampf(y, 0.0f, 1.0f);
}
static void ssSetMix(float m) {
  m = clampf(m, 0.0f, 1.0f);
  float c = -0.55366f * m + 0.99785f;
  float s = -0.73764f * m * m + 1.2841f * m;
  if (c < 0.0f) c = 0.0f; if (s < 0.0f) s = 0.0f;
  float rms = sqrtf(c * c + 6.0f * s * s);
  if (rms < 1e-6f) rms = 1e-6f;
  ssGainC = c / rms; ssGainS = s / rms;
}
static void ssBuildTables() {
  for (int i = 0; i < SS_TABLE_SIZE; i++)
    ssSineTab[i] = sinf(2.0f * PI_F * (float)i / (float)SS_TABLE_SIZE);
  ssNumMips = 0;
  uint32_t off = 0;
  int H = SS_TAB_TOP_H;
  while (H >= 1 && ssNumMips < SS_TAB_MAX_MIPS) {
    int N = 1;
    while (N < 2 * H + 1 && N < SS_TABLE_SIZE) N <<= 1;
    if (N < 512) N = 512;          // 小表线性插值会有镜像/bitcrush: 最小 512 点
    uint8_t b = 0; { int t = N; while (t > 1) { t >>= 1; b++; } }
    ssMipHarm[ssNumMips]  = (uint16_t)H;
    ssMipShift[ssNumMips] = 32 - b;
    int step = SS_TABLE_SIZE / N;
    static float hw[1024];
    for (int h = 1; h <= H; h++) {
      float x = (float)h / (float)H;
      hw[h] = (x > SS_TAPER_START) ? 0.5f * (1.0f + cosf(PI_F * (x - SS_TAPER_START) / (1.0f - SS_TAPER_START))) : 1.0f;
    }
    float maxv = 0.0f;
    for (int n = 0; n < N; n++) {
      float v = 0.0f;
      for (int h = 1; h <= H; h++) v += ssSineTab[(h * n * step) & SS_TABLE_MASK] / (float)h * hw[h];
      ssSawFloat[n] = v;
      if (fabsf(v) > maxv) maxv = fabsf(v);
    }
    float scale = 32000.0f / maxv;
    ssTabPool[off] = (int16_t)(ssSawFloat[N - 1] * scale);
    for (int n = 0; n < N; n++) ssTabPool[off + 1 + n] = (int16_t)(ssSawFloat[n] * scale);
    ssTabPool[off + 1 + N]     = ssTabPool[off + 1];
    ssTabPool[off + 1 + N + 1] = ssTabPool[off + 2];
    ssMipOff[ssNumMips] = off + 1;
    off += N + 3;
    ssNumMips++;
    H = (H * 33) / 34;
  }
}

// ================= 滤波器系数表 =================
#define GTAB_N 1024
static float gTanTab[GTAB_N];
static const float LOG2_MIN = 4.321928f;    // log2(20)
static const float LOG2_MAX = 14.135709f;   // log2(18000)
static void buildGTab() {
  for (int i = 0; i < GTAB_N; i++) {
    float l2 = LOG2_MIN + (LOG2_MAX - LOG2_MIN) * (float)i / (float)(GTAB_N - 1);
    float fc = exp2f(l2);
    float x = PI_F * fc / (float)SAMPLE_RATE;
    if (x > 1.50f) x = 1.50f;
    gTanTab[i] = tanf(x);
  }
}
static inline float getG(float log2fc) {
  if (log2fc < LOG2_MIN) log2fc = LOG2_MIN;
  if (log2fc > LOG2_MAX) log2fc = LOG2_MAX;
  float fi = (log2fc - LOG2_MIN) * ((float)(GTAB_N - 1) / (LOG2_MAX - LOG2_MIN));
  int i = (int)fi;
  if (i >= GTAB_N - 1) return gTanTab[GTAB_N - 1];
  float f = fi - (float)i;
  return gTanTab[i] + f * (gTanTab[i + 1] - gTanTab[i]);
}

// ================= 每声部 =================
struct ADSRCoefs { float atk, dec, rel, sus; };
struct ADSR {
  float level;
  float pos;        // 0..1 within current stage (Vital-style position)
  float start;      // value at stage start (retrigger / release)
  uint8_t stage;    // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
  void init() { level = 0.0f; pos = 0.0f; start = 0.0f; stage = 0; }
  void noteOn() { start = level; pos = 0.0f; stage = 1; }
  void noteOff() { if (stage != 0) { start = level; pos = 0.0f; stage = 4; } }
};
struct Voice {
  bool active, gate;
  uint8_t note;
  uint32_t age;
  float freq, keyMul, velGain, log2base;
  float ph[NUM_SAWS];
  float inc[NUM_SAWS], gL[NUM_SAWS], gR[NUM_SAWS];   // 每块预计算, 省每采样乘法
  ADSR amp, filt;     // 两套独立包络
  float hpA, hpXL, hpYL, hpX2L, hpY2L, hpXR, hpYR, hpX2R, hpY2R;
  float ic1L, ic2L, ic1R, ic2R;
  float s1L, s2L, s1R, s2R;   // 24dB 第二级
  float g, a1, b1;            // 滤波系数 (每 16 采样更新一次)
};
static Voice voices[NUM_VOICES];
static uint32_t voiceAge = 0;

// 每块计算的 ADSR / filter 系数
static ADSRCoefs ampC, filtC;
static float gK = 1.0f;
static float gDetuneY = 0.0f;
static volatile float gMaster = 0.65f;

static void calcAdsrCoefs(float decay, float sus, float rel, ADSRCoefs &c) {
  float sr = (float)SAMPLE_RATE;
  c.atk = 1.0f / (FIXED_ATTACK_S * sr);                  // 1ms
  c.dec = 1.0f / (clampf(decay, 0.0005f, 20.0f) * sr);   // decay = 到达 sustain 的时间
  c.rel = 1.0f / (clampf(rel,   0.0005f, 20.0f) * sr);   // release = 从 noteOff 到 0 的时间
  c.sus = clampf(sus, 0.0f, 1.0f);
}
static void updateADSRCoefs() {
  float ad = ampP.decay, as = ampP.sustain, ar = ampP.release;
  float fd = filtP.decay, fs = filtP.sustain, fr = filtP.release;
  calcAdsrCoefs(ad, as, ar, ampC);
  calcAdsrCoefs(fd, fs, fr, filtC);
}
// Vital 默认 powers: attack=0 (linear), decay=-2, release=-2.
// powerScale(p, power) = (exp(power*p)-1)/(exp(power)-1), power=-2:
#define ENV_CURVE_N 512
static float envCurve[ENV_CURVE_N + 1];
static void buildEnvCurve() {
  const float k = expf(-2.0f) - 1.0f;
  for (int i = 0; i <= ENV_CURVE_N; i++) {
    float p = (float)i / (float)ENV_CURVE_N;
    envCurve[i] = (expf(-2.0f * p) - 1.0f) / k;
  }
}
static inline float envPowerScale(float p) {
  if (p <= 0.0f) return 0.0f;
  if (p >= 1.0f) return 1.0f;
  float x = p * (float)ENV_CURVE_N;
  int i = (int)x;
  float f = x - (float)i;
  return envCurve[i] + f * (envCurve[i + 1] - envCurve[i]);
}
static inline float adsrStep(ADSR &a, const ADSRCoefs &c) {
  switch (a.stage) {
    case 1: {
      a.pos += c.atk;
      if (a.pos >= 1.0f) { a.pos = 0.0f; a.level = 1.0f; a.stage = 2; }
      else a.level = a.start + (1.0f - a.start) * a.pos;   // attack power = 0
    } break;
    case 2: {
      a.pos += c.dec;
      if (a.pos >= 1.0f) { a.pos = 0.0f; a.level = c.sus; a.stage = 3; }
      else a.level = 1.0f - (1.0f - c.sus) * envPowerScale(a.pos);
    } break;
    case 3:
      a.level = c.sus;
      break;
    case 4: {
      a.pos += c.rel;
      if (a.pos >= 1.0f) { a.pos = 0.0f; a.level = 0.0f; a.stage = 0; }
      else a.level = a.start * (1.0f - envPowerScale(a.pos));
    } break;
    default: a.level = 0.0f; break;
  }
  return a.level;
}

// 每声部 2 阶次声高通 (固定 20Hz; 原来跟随 f0 会把低频基波削掉 -> 低频发薄/发炸)
static inline void hpStep(Voice &v, float &vL, float &vR) {
  float yL1 = v.hpA * (v.hpYL + vL - v.hpXL); v.hpXL = vL; v.hpYL = yL1;
  float yL2 = v.hpA * (v.hpY2L + yL1 - v.hpX2L); v.hpX2L = yL1; v.hpY2L = yL2;
  float yR1 = v.hpA * (v.hpYR + vR - v.hpXR); v.hpXR = vR; v.hpYR = yR1;
  float yR2 = v.hpA * (v.hpY2R + yR1 - v.hpX2R); v.hpX2R = yR1; v.hpY2R = yR2;
  vL = yL2; vR = yR2;
}

// TPT SVF: 返回 LP/BP/HP
static inline float satState(float x) {     // normal signal untouched; keep as identity
  return x;
}
static inline float svfA(float in, float &ic1, float &ic2, float g, float k, float a1, int mode) {
  float v1 = a1 * (ic1 + g * (in - ic2));
  v1 = satState(v1);
  float v2 = ic2 + g * v1;
  ic1 = 2.0f * v1 - ic1;
  ic2 = 2.0f * v2 - ic2;
  return (mode == 1) ? v1 : ((mode == 2) ? (in - k * v1 - v2) : v2);
}

// ================= 音符事件队列 (core0 -> core1) =================
struct Evt { uint8_t type; uint8_t note; uint8_t vel; };
#define EVT_QSIZE 64
static volatile Evt evtQ[EVT_QSIZE];
static volatile uint8_t evtHead = 0, evtTail = 0;
static inline void postEvt(uint8_t type, uint8_t note, uint8_t vel) {
  uint8_t h = evtHead;
  uint8_t nh = (uint8_t)((h + 1) & (EVT_QSIZE - 1));
  if (nh == evtTail) return;
  evtQ[h].type = type; evtQ[h].note = note; evtQ[h].vel = vel;
  __dmb(); evtHead = nh;
}

// 供 CV DAC / 状态显示
volatile float gLastFreq = 55.0f;
volatile int   gAnyGate = 0;
volatile float gLastFilterEnv = 0.0f;
volatile float gLastAmpEnv = 0.0f;

// ================= 声部分配 =================
static void voiceOn(uint8_t note, uint8_t vel) {
  int idx = -1;
  for (int i = 0; i < NUM_VOICES; i++) if (voices[i].active && voices[i].note == note) { idx = i; break; }
  if (idx < 0) for (int i = 0; i < NUM_VOICES; i++) if (!voices[i].active) { idx = i; break; }
  if (idx < 0) { idx = 0; for (int i = 1; i < NUM_VOICES; i++) if (voices[i].age < voices[idx].age) idx = i; }
  Voice &v = voices[idx];
  v.active = true; v.gate = true; v.note = note; v.age = ++voiceAge;
  v.freq = 440.0f * exp2f(((float)note - 69.0f) / 12.0f);
  v.keyMul = exp2f(((float)note - 60.0f) * (0.5f / 12.0f));   // 50% key follow
  v.velGain = 0.40f + 0.60f * (float)vel / 127.0f;
  for (int k = 0; k < NUM_SAWS; k++) v.ph[k] = (float)(random(0x7FFFFFFF) >> 8) * (1.0f / 16777216.0f);
  v.amp.noteOn();
  v.filt.noteOn();
  v.hpA = 1.0f / (1.0f + 2.0f * PI_F * 20.0f / (float)SAMPLE_RATE);
  v.log2base = log2f(clampf(pCutoff * v.keyMul, 20.0f, 18000.0f));
  v.g = getG(v.log2base);
  v.a1 = 1.0f / (1.0f + v.g * (v.g + gK));
  v.b1 = 1.0f / (1.0f + v.g * (v.g + 1.5f));
  v.hpXL = v.hpYL = v.hpX2L = v.hpY2L = 0.0f;
  v.hpXR = v.hpYR = v.hpX2R = v.hpY2R = 0.0f;
  v.ic1L = v.ic2L = v.ic1R = v.ic2R = 0.0f;
  v.s1L = v.s2L = v.s1R = v.s2R = 0.0f;
}
static void voiceOff(uint8_t note) {
  for (int i = 0; i < NUM_VOICES; i++)
    if (voices[i].active && voices[i].note == note) { voices[i].gate = false; voices[i].amp.noteOff(); voices[i].filt.noteOff(); }
}
static void allOff() {
  for (int i = 0; i < NUM_VOICES; i++) { voices[i].gate = false; voices[i].amp.noteOff(); voices[i].filt.noteOff(); }
}

// ================= 音频 ISR =================
static void __not_in_flash_func(onTransmit)() {
  uint32_t t0 = time_us_32();
  { uint32_t fpscr; __asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);
    __asm volatile("vmsr fpscr, %0" :: "r"(fpscr)); }
  // 处理音符事件
  while (evtTail != evtHead) {
    uint8_t t = evtTail;
    uint8_t type = evtQ[t].type;
    uint8_t note = evtQ[t].note, vel = evtQ[t].vel;
    evtTail = (uint8_t)((t + 1) & (EVT_QSIZE - 1));
    if (type == 1) voiceOn(note, vel);
    else if (type == 2) voiceOff(note);
    else if (type == 3) allOff();
  }

  // 诊断: 1kHz 纯正弦直出 (不经过 VCO/VCF/ADSR/限幅)
  if (gSineTest) {
    static float sph = 0.0f;
    while (i2s.availableForWrite() > 0) {
      sph += 1000.0f / (float)SAMPLE_RATE;
      if (sph >= 1.0f) sph -= 1.0f;
      int32_t q = (int32_t)(sinf(2.0f * PI_F * sph) * 0.2f * 32767.0f);
      i2s.write16((int16_t)q, (int16_t)q);
    }
    gIsrUs += time_us_32() - t0;
    gIsrBlocks++;
    return;
  }

  // 控制率参数 (每次回调更新)
  updateADSRCoefs();
  ssSetMix(pMix);
  gDetuneY = ssDetuneCurve(pDetune);
  float res = clampf(pRes, 0.0f, 1.0f);
  gK = 2.0f - 1.95f * res;                 // 线性 SVF, 不做内部饱和 (bitcrush 来源)
  if (gK < 0.10f) gK = 0.10f;
  ssMipLevel = 0;   // 下面按最高音重算
  float fmax = 1.0f;
  for (int i = 0; i < NUM_VOICES; i++)
    if (voices[i].active) { float f = voices[i].freq * gBend * 1.11f; if (f > fmax) fmax = f; }
  ssMipLevel = ssNumMips - 1;
  for (int k = 0; k < ssNumMips; k++)
    if ((float)ssMipHarm[k] * fmax < (float)SAMPLE_RATE * 0.5f) { ssMipLevel = k; break; }
  const int16_t *tab = &ssTabPool[ssMipOff[ssMipLevel]];
  int sh = ssMipShift[ssMipLevel];
  int tabN = 1 << (32 - sh);

  int activeN = 0;
  for (int i = 0; i < NUM_VOICES; i++) {
    Voice &v = voices[i];
    if (!v.active) continue;
    activeN++;
    v.log2base = log2f(clampf(pCutoff * v.keyMul, 20.0f, 18000.0f));
    float fBend = v.freq * gBend;
    v.hpA = 1.0f / (1.0f + 2.0f * PI_F * 20.0f / (float)SAMPLE_RATE);
    for (int k = 0; k < NUM_SAWS; k++) {
      float f = fBend * (1.0f + SS_DET_OFF[k] * gDetuneY);
      f = clampf(f, 1.0f, 16000.0f);
      v.inc[k] = f / (float)SAMPLE_RATE;
      float pan = (float)(k - 3) * (1.0f / 3.0f);
      float mix = (k == 3) ? ssGainC : ssGainS;
      v.gL[k] = (1.0f - pan * 0.70f) * mix;
      v.gR[k] = (1.0f + pan * 0.70f) * mix;
    }
  }
  float voiceNorm = 1.0f / sqrtf((float)(activeN > 0 ? activeN : 1));

  static uint32_t smpCnt = 0;
  while (i2s.availableForWrite() > 0) {
    bool ctrl = gCtrlFast || ((smpCnt & 15u) == 0u);
    float outL = 0.0f, outR = 0.0f;
    float lastEnv = 0.0f, lastVca = 0.0f;
    int anyGate = 0;
    for (int i = 0; i < NUM_VOICES; i++) {
      Voice &v = voices[i];
      if (!v.active) continue;
      float aenv = adsrStep(v.amp, ampC);   // AMP ADSR -> VCA
      float fenv = adsrStep(v.filt, filtC); // FILTER ADSR -> VCF cutoff
      if (v.amp.stage == 0 && !v.gate) {
        v.active = false;
        v.ic1L = v.ic2L = v.ic1R = v.ic2R = 0.0f;
        v.s1L = v.s2L = v.s1R = v.s2R = 0.0f;
        v.hpXL = v.hpYL = v.hpX2L = v.hpY2L = 0.0f;
        v.hpXR = v.hpYR = v.hpX2R = v.hpY2R = 0.0f;
        continue;
      }
      if (v.gate) anyGate = 1;

      // 7 锯齿 (inc/pan/mix 已在每块预计算)
      float vL = 0.0f, vR = 0.0f;
      for (int k = 0; k < NUM_SAWS; k++) {
        float ph = v.ph[k] + v.inc[k]; if (ph >= 1.0f) ph -= 1.0f;
        v.ph[k] = ph;
        float pos = ph * (float)tabN;
        int idx = (int)pos; if (idx >= tabN) idx = tabN - 1;
        float frac = pos - (float)idx;
        float y0 = (float)tab[idx]     * (1.0f / 32768.0f);
        float y1 = (float)tab[idx + 1] * (1.0f / 32768.0f);
        float saw = y0 + (y1 - y0) * frac;   // 表已最小 512 点 -> 线性也够干净, CPU 低
        vL += saw * v.gL[k]; vR += saw * v.gR[k];
      }
      if (!gBypassHPF) hpStep(v, vL, vR);

      // VCF: 每声部 log2 cutoff + ADSR (FILTER 目标) 调制
      if (ctrl) {
        float log2cut = v.log2base + fenv * pEnvDepth * 6.0f;   // FILTER 包络深度 -6..+6 oct
        v.g = getG(log2cut);
        v.a1 = 1.0f / (1.0f + v.g * (v.g + gK));
        v.b1 = 1.0f / (1.0f + v.g * (v.g + 1.5f));
      }
      float g = v.g;
      int mode = gFilterMode;
      float yl, yr;
      if (gBypassFilter) {
        yl = vL; yr = vR;
      } else if (gSlope24) {
        float g2 = g, k2 = 1.5f;
        float a1 = v.a1;
        float b1 = v.b1;
        // stage 1 (resonant) + stage 2 (gentle)
        {
          float v1 = a1 * (v.ic1L + g * (vL - v.ic2L));
          v1 = satState(v1);
          float v2 = v.ic2L + g * v1;
          v.ic1L = 2.0f * v1 - v.ic1L; v.ic2L = 2.0f * v2 - v.ic2L;
          float o = (mode == 1) ? v1 : ((mode == 2) ? (vL - gK * v1 - v2) : v2);
          float w1 = b1 * (v.s1L + g2 * (o - v.s2L));
          float w2 = v.s2L + g2 * w1;
          v.s1L = 2.0f * w1 - v.s1L; v.s2L = 2.0f * w2 - v.s2L;
          yl = (mode == 1) ? w1 : ((mode == 2) ? (o - k2 * w1 - w2) : w2);
        }
        {
          float v1 = a1 * (v.ic1R + g * (vR - v.ic2R));
          v1 = satState(v1);
          float v2 = v.ic2R + g * v1;
          v.ic1R = 2.0f * v1 - v.ic1R; v.ic2R = 2.0f * v2 - v.ic2R;
          float o = (mode == 1) ? v1 : ((mode == 2) ? (vR - gK * v1 - v2) : v2);
          float w1 = b1 * (v.s1R + g2 * (o - v.s2R));
          float w2 = v.s2R + g2 * w1;
          v.s1R = 2.0f * w1 - v.s1R; v.s2R = 2.0f * w2 - v.s2R;
          yr = (mode == 1) ? w1 : ((mode == 2) ? (o - k2 * w1 - w2) : w2);
        }
      } else {
        yl = svfA(vL, v.ic1L, v.ic2L, g, gK, v.a1, mode);
        yr = svfA(vR, v.ic1R, v.ic2R, g, gK, v.a1, mode);
      }

      float m = aenv * v.velGain * gMaster * voiceNorm;
      outL += yl * m; outR += yr * m;
      lastEnv = fenv; lastVca = aenv;
    }
    // 软限幅 + 立体声
    if (gLimiter) {
      outL = fastTanh(outL * 1.1f) * 0.95f;
      outR = fastTanh(outR * 1.1f) * 0.95f;
    }
    int32_t l = (int32_t)(outL * 32767.0f);
    int32_t r = (int32_t)(outR * 32767.0f);
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    i2s.write16((int16_t)l, (int16_t)r);
    gLastFilterEnv = lastEnv; gLastAmpEnv = lastVca; gAnyGate = anyGate;
    if (anyGate) {
      // 记录最高音供 CV 输出
      float f = 55.0f;
      for (int i = 0; i < NUM_VOICES; i++) if (voices[i].active && voices[i].gate && voices[i].freq > f) f = voices[i].freq;
      gLastFreq = f;
    }
    smpCnt++;
  }
  gIsrUs += time_us_32() - t0;
  gIsrBlocks++;
}

// ================= core1 =================
static void core1_entry() {
  multicore_lockout_victim_init();
  { uint32_t fpscr; __asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);                 // core1 自己的 FZ|DN (防释放尾音 denormal 拖慢 FPU)
    __asm volatile("vmsr fpscr, %0" :: "r"(fpscr)); }
  i2s.setBitsPerSample(16);
  i2s.setBuffers(8, 128);
  i2s.onTransmit(onTransmit);
  i2s.begin(SAMPLE_RATE);
  sleep_ms(50);
  gpio_put(PIN_XSMT, 1);
  for (;;) tight_loop_contents();
}

// ================= USB MIDI =================
#if USE_USB_MIDI
Adafruit_USBD_MIDI usb_midi;
static void midiTask() {
  uint8_t pkt[4];
  while (usb_midi.readPacket(pkt)) {
    uint8_t status = pkt[1];
    if (status < 0x80) continue;
    uint8_t type = status & 0xF0, chan = status & 0x0F;
    if (MIDI_CHANNEL && (chan + 1) != MIDI_CHANNEL) continue;
    if (type == 0x90) {
      if (pkt[3] == 0) postEvt(2, pkt[2], 0);
      else postEvt(1, pkt[2], pkt[3]);
    } else if (type == 0x80) {
      postEvt(2, pkt[2], 0);
    } else if (type == 0xE0) {
      int bend = (int)pkt[2] | ((int)pkt[3] << 7);
      float semis = ((float)bend - 8192.0f) / 8192.0f * 2.0f;
      gBend = exp2f(semis / 12.0f);
    } else if (type == 0xB0 && (pkt[2] == 120 || pkt[2] == 123)) {
      postEvt(3, 0, 0);
    }
  }
}
#endif

// ================= 旋钮 / 开关 =================
// 新映射:
//   K1 TUNE -> Cutoff, K2 CUTOFF -> Resonance
//   K3 RES -> D, K4 ENV.MOD -> S, K5 DECAY -> R  (D/S/R 写到"当前编辑"的 ADSR)
//   K6 ACCENT -> Filter Env Depth
//   K7 DIST -> Detune, K8 COLOR -> Mix
// Attack 固定 1ms。
static float fKnob[8] = {0,0,0,0,0,0,0,0};
static float knobPrev[3] = {-1,-1,-1};
static float envNorm[2][3];              // [0]=AMP D/S/R, [1]=FILTER D/S/R (0..1)
static bool  knobsReady = false;
static void applyEnvNorm() {
  ampP.decay    = 0.0005f * powf(40000.0f, envNorm[0][0]);
  ampP.sustain  = envNorm[0][1];
  ampP.release  = 0.0005f * powf(40000.0f, envNorm[0][2]);
  filtP.decay   = 0.0005f * powf(40000.0f, envNorm[1][0]);
  filtP.sustain = envNorm[1][1];
  filtP.release = 0.0005f * powf(40000.0f, envNorm[1][2]);
}
static void readKnobs() {
  if (!gUsePots) return;
  for (int ch = 0; ch < 8; ch++) {
    float raw = (float)adsReadCh(ch);
    if (!knobsReady) fKnob[ch] = raw;
    else fKnob[ch] += (raw - fKnob[ch]) * 0.25f;
  }
  float u;
  u = potNorm((uint16_t)fKnob[0], 0); pCutoff  = 20.0f * powf(900.0f, u);        // 20..18000
  u = potNorm((uint16_t)fKnob[1], 0); pRes     = u;
  u = potNorm((uint16_t)fKnob[5], 0); pEnvDepth = u * 2.0f - 1.0f;   // 中心 0, 左负右正
  u = potNorm((uint16_t)fKnob[6], 0); pDetune  = u;
  u = potNorm((uint16_t)fKnob[7], 0); pMix     = u;

  float k[3];
  for (int i = 0; i < 3; i++) k[i] = potNorm((uint16_t)fKnob[2 + i], 0);
  if (!knobsReady) {
    for (int t = 0; t < 2; t++)
      for (int i = 0; i < 3; i++) envNorm[t][i] = k[i];
    for (int i = 0; i < 3; i++) knobPrev[i] = k[i];
    knobsReady = true;
  } else {
    int t = gEditTarget ? 1 : 0;
    for (int i = 0; i < 3; i++) {
      float d = k[i] - knobPrev[i];
      if (fabsf(d) > 0.005f) envNorm[t][i] = clampf(envNorm[t][i] + d, 0.0f, 1.0f);
      knobPrev[i] = k[i];
    }
  }
  applyEnvNorm();
}
static int lastBtn = 1; static uint32_t lastBtnMs = 0;
static void readSwitches() {
  if (!gUsePots) return;             // p0 调试时串口接管 switch 状态
  int et = gpio_get(PIN_SW_ENV) ? 1 : 0;
  gEditTarget = et;
  gSlope24 = gpio_get(PIN_SW_SLOPE) ? 1 : 0;
  int b = gpio_get(PIN_BTN_MODE) ? 1 : 0;
  uint32_t now = millis();
  if (b == 0 && lastBtn == 1 && now - lastBtnMs > 40) {
    gFilterMode = (gFilterMode + 1) % 3;
    Serial.print("#VCF mode "); Serial.println(gFilterMode == 0 ? "LP" : (gFilterMode == 1 ? "BP" : "HP"));
    lastBtnMs = now;
  }
  lastBtn = b;
}

// ================= 串口命令 =================
static void handleCmd(char *s) {
  if (!s || !s[0]) return;
  char c = s[0]; float v = atof(s + 1);
  switch (c) {
    case 'd': if (gEditTarget) filtP.decay = clampf(v, 0.0005f, 20.0f); else ampP.decay = clampf(v, 0.0005f, 20.0f); break;
    case 's': if (gEditTarget) filtP.sustain = clampf(v, 0.0f, 1.0f); else ampP.sustain = clampf(v, 0.0f, 1.0f); break;
    case 'r': if (gEditTarget) filtP.release = clampf(v, 0.0005f, 20.0f); else ampP.release = clampf(v, 0.0005f, 20.0f); break;
    case 'c': pCutoff = clampf(v, 20.0f, 18000.0f); break;
    case 'q': pRes = clampf(v, 0.0f, 1.0f); break;
    case 'k': pEnvDepth = clampf(v, -1.0f, 1.0f); break;
    case 'w': pDetune = clampf(v, 0.0f, 1.0f); break;
    case 'm': pMix = clampf(v, 0.0f, 1.0f); break;
    case 'p': gUsePots = (v != 0.0f) ? 1 : 0; Serial.print("#pots "); Serial.println(gUsePots ? "on" : "off"); break;
    case 'L': gScreenOn = (v != 0.0f) ? 1 : 0; Serial.print("#screen "); Serial.println(gScreenOn ? "on" : "off"); break;
    case 'S': gSineTest = (v != 0.0f) ? 1 : 0; Serial.print("#sine "); Serial.println(gSineTest ? "on" : "off"); break;
    case 'B': gBypassFilter = (v != 0.0f) ? 1 : 0; Serial.print("#vcf "); Serial.println(gBypassFilter ? "bypass" : "on"); break;
    case 'N': gLimiter = (v != 0.0f) ? 1 : 0; Serial.print("#limiter "); Serial.println(gLimiter ? "on" : "off"); break;
    case 'O': gMaster = clampf(v, 0.0f, 2.0f); Serial.print("#master "); Serial.println(gMaster, 3); break;
    case 'H': gBypassHPF = (v != 0.0f) ? 1 : 0; Serial.print("#hpf "); Serial.println(gBypassHPF ? "bypass" : "on"); break;
    case 'C': gCtrlFast = (v != 0.0f) ? 1 : 0; Serial.print("#ctrlf "); Serial.println(gCtrlFast ? "fast" : "16x"); break;
    case 'Q': {
      static uint32_t pT = 0, pU = 0, pB = 0;
      uint32_t now = time_us_32();
      uint32_t dt = now - pT; pT = now;
      uint32_t du = gIsrUs - pU; pU = gIsrUs;
      uint32_t db = gIsrBlocks - pB; pB = gIsrBlocks;
      Serial.print("core1=");
      if (dt) Serial.print(100.0f * (float)du / (float)dt, 1); else Serial.print(0.0f, 1);
      Serial.print("% blocks="); Serial.print(db);
      Serial.print(" uspb=");
      if (db) Serial.print((float)du / (float)db, 1); else Serial.print(0.0f, 1);
      Serial.print(" MHz="); Serial.println(clock_get_hz(clk_sys) / 1000000.0f, 1);
    } break;
    case 'e': gEditTarget = (v != 0.0f) ? 1 : 0; break;
    case 'l': gSlope24 = (v != 0.0f) ? 1 : 0; break;
    case 'F': gFilterMode = ((int)v) % 3; break;
    case 'n': postEvt(1, (uint8_t)v, 100); break;
    case 'f': postEvt(3, 0, 0); break;
    case 'b': reset_usb_boot(0, 0); break;
    case 'T':
      Serial.print("amp d/s/r="); Serial.print(ampP.decay, 4); Serial.print('/');
      Serial.print(ampP.sustain, 3); Serial.print('/'); Serial.print(ampP.release, 4);
      Serial.print(" filt d/s/r="); Serial.print(filtP.decay, 4); Serial.print('/');
      Serial.print(filtP.sustain, 3); Serial.print('/'); Serial.print(filtP.release, 4);
      Serial.print(" edit="); Serial.print(gEditTarget ? "FILTER" : "AMP");
      Serial.print(" cut="); Serial.print(pCutoff, 0);
      Serial.print(" res="); Serial.print(pRes, 3);
      Serial.print(" depth="); Serial.print(pEnvDepth, 3);
      Serial.print(" det="); Serial.print(pDetune, 3);
      Serial.print(" mix="); Serial.print(pMix, 3);
      Serial.print(" slope="); Serial.print(gSlope24 ? 24 : 12);
      Serial.print(" vcf="); Serial.print(gFilterMode == 0 ? "LP" : (gFilterMode == 1 ? "BP" : "HP"));
      Serial.print(" lcd="); Serial.print(gScreenOn);
      Serial.print(" sine="); Serial.print(gSineTest);
      Serial.print(" bypass="); Serial.print(gBypassFilter);
      Serial.print(" lim="); Serial.print(gLimiter);
      Serial.print(" hpf="); Serial.print(gBypassHPF);
      Serial.print(" ctrlf="); Serial.print(gCtrlFast);
      Serial.print(" gain="); Serial.print(gMaster, 3);
      Serial.print(" cpu="); Serial.println(clock_get_hz(clk_sys) / 1000000.0f, 1);
      break;
    case '?':
      Serial.println("OV-JP8000: d/s/r=edit ADSR, c/q=cut/res, k=depth, w/m=detune/mix, e=edit target, l=slope, F=vcfmode, n/f, b=boot");
      Serial.println("diag: S1=sine B1=vcf bypass H1=hpf bypass N0=limiter off C1=ctrlf fast O<gain> L0=screen off Q=cpu");
      break;
  }
}

// ================= ST7789 240x240: PIO + DMA 推屏, 只显示 8 个旋钮 =================
#define LCD_W 240
#define LCD_H 240
static int      lcdYoff = 0;
static float    lcdClkDiv = 2.0f;
static uint16_t fb[LCD_W * LCD_H];
static PIO      lcdPio = pio0;
static int      lcdSm = -1;
static uint     lcdOff = 0;
static int      dmaTx = -1;

static inline uint16_t C(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline void putPix(int x, int y, uint16_t c) {
  if (x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return;
  fb[(y * LCD_W + x) ^ 1] = c;          // PIO 32bit 小端顺序: 与 OV-03 相同
}
static void fillRect(int x0, int y0, int w, int h, uint16_t c) {
  for (int y = y0; y < y0 + h; y++)
    for (int x = x0; x < x0 + w; x++) putPix(x, y, c);
}
static void drawLine(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    putPix(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
static void drawCircle(int cx, int cy, int r, uint16_t c) {
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++) {
      float d = sqrtf((float)(x * x + y * y));
      if (d >= r - 0.8f && d <= r + 0.2f) putPix(cx + x, cy + y, c);
    }
}
static void drawText(int x, int y, const char *s, uint16_t c) {
  while (*s) {
    char ch = *s++;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    int idx = (int)ch - 32;
    if (idx < 0 || idx >= (int)(sizeof(font5x7) / FONT_W)) { x += FONT_W + 1; continue; }
    for (int col = 0; col < FONT_W; col++) {
      uint8_t bits = font5x7[idx][col];
      for (int row = 0; row < FONT_H; row++)
        if (bits & (1u << row)) putPix(x + col, y + row, c);
    }
    x += FONT_W + 1;
  }
}
static void bbByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    gpio_put(PIN_MOSI, (b >> i) & 1); busy_wait_us_32(1);
    gpio_put(PIN_SCK, 1);             busy_wait_us_32(1);
    gpio_put(PIN_SCK, 0);             busy_wait_us_32(1);
  }
}
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
static void lcdCmdBB(uint8_t c) { gpio_put(PIN_DC, 0); gpio_put(PIN_CS, 0); bbByte(c); gpio_put(PIN_CS, 1); }
static void lcdDataBB(const uint8_t* d, size_t n) {
  gpio_put(PIN_DC, 1); gpio_put(PIN_CS, 0); while (n--) bbByte(*d++); gpio_put(PIN_CS, 1);
}
static void lcdCmdDataBB(uint8_t c, const uint8_t* d, size_t n) { lcdCmdBB(c); if (n) lcdDataBB(d, n); }
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
  { uint32_t sys = clock_get_hz(clk_sys);
    int n = (int)((sys + 249999999u) / 250000000u);
    if (n < 1) n = 1;
    lcdClkDiv = (float)n; }
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
static void lcdWaitIdle() {
  while (dma_channel_is_busy(dmaTx)) tight_loop_contents();
  while (!pio_sm_is_tx_fifo_empty(lcdPio, lcdSm)) {}
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
static volatile bool lcdDmaBusy = false;
static uint32_t lcdStartMs = 0;
static uint8_t  lcdBandMask = 0;              // 4 个 60 行带, 只推变化带
static void lcdPushBand(int row) {
  if (lcdDmaBusy) return;
  int y0 = row * 60, y1 = y0 + 59;
  lcdSetWindow(0, y0, LCD_W - 1, y1);
  gpio_put(PIN_DC, 1); gpio_put(PIN_CS, 0);
  dma_channel_set_read_addr(dmaTx, &fb[y0 * LCD_W], false);
  dma_channel_set_trans_count(dmaTx, (uint32_t)(60 * LCD_W) / 2, true);
  lcdDmaBusy = true; lcdStartMs = millis();
}
static void lcdTask() {
  if (lcdDmaBusy) {
    if (dma_channel_is_busy(dmaTx)) {
      if (millis() - lcdStartMs > 300) {       // 防 DMA/PIO 卡死把 core0 拖死
        dma_channel_abort(dmaTx);
        pio_sm_set_enabled(lcdPio, lcdSm, false);
        pio_sm_clear_fifos(lcdPio, lcdSm);
        pio_sm_set_enabled(lcdPio, lcdSm, true);
        gpio_put(PIN_CS, 1);
        lcdDmaBusy = false;
        lcdBandMask = 0;
      }
      return;
    }
    while (!pio_sm_is_tx_fifo_empty(lcdPio, lcdSm)) {}
    busy_wait_us_32(5);
    gpio_put(PIN_CS, 1);
    lcdDmaBusy = false;
  }
  if (!lcdDmaBusy && lcdBandMask) {
    int row = 0; while (!(lcdBandMask & (1u << row))) row++;
    lcdBandMask &= (uint8_t)~(1u << row);
    lcdPushBand(row);
  }
}static void lcdInit() {
  pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT); digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, LOW);
  pinMode(PIN_SCK, OUTPUT); pinMode(PIN_MOSI, OUTPUT);
  lcdPioBegin();
  bbModeBegin();
  gpio_put(PIN_RST, 0); delay(30); gpio_put(PIN_RST, 1); delay(150);
  lcdCmdBB(0x01); delay(150);
  lcdCmdBB(0x11); delay(120);
  uint8_t d[4];
  d[0]=0x55; lcdCmdDataBB(0x3A, d, 1);
  d[0]=0x60; lcdCmdDataBB(0x36, d, 1);           // 90° 向右
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
  digitalWrite(PIN_BL, HIGH);
}
static void fmtTime(char *b, int n, float sec) {
  if (sec < 1.0f) snprintf(b, n, "%dMS", (int)(sec * 1000.0f + 0.5f));
  else snprintf(b, n, "%.1fS", sec);
}
static void drawKnobCell(int cx, int cy, int r, float v, const char *label, const char *val, uint16_t accent) {
  uint16_t bg = C(10, 12, 18), ring = C(70, 78, 95), txt = C(225, 232, 245), dim = C(120, 135, 160);
  fillRect(cx - r - 4, cy - r - 4, 2 * r + 8, 2 * r + 8, bg);
  drawCircle(cx, cy, r, ring);
  float a = (-135.0f + 270.0f * clampf(v, 0.0f, 1.0f)) * PI_F / 180.0f;
  int px = cx + (int)(sinf(a) * (r - 3));
  int py = cy - (int)(cosf(a) * (r - 3));
  drawLine(cx, cy, px, py, accent);
  drawCircle(cx, cy, 3, accent);
  drawText(cx + r + 6, cy - 12, label, dim);
  drawText(cx + r + 6, cy + 2, val, txt);
}
static float lcdPrevVal[8] = {0};
static char  lcdPrevText[8][16] = {{0}};
static int   lcdPrevEdit = -1;
static bool  lcdFirst = true;
static void drawUI() {
  if (lcdDmaBusy) return;                         // 上一带还在推, 下个 41ms 再来
  uint16_t bg = C(10, 12, 18);
  uint16_t accA = C(60, 200, 255);
  uint16_t accF = C(255, 150, 40);
  uint16_t acc = gEditTarget ? accF : accA;
  bool targetChanged = (lcdPrevEdit != gEditTarget);
  for (int i = 0; i < 8; i++) {
    int col = i & 1, row = i >> 1;
    int cx = col * 120 + 30, cy = row * 60 + 30, r = 20;
    char b[24]; const char *label = ""; float val = 0.0f; uint16_t accent = accA;
    switch (i) {
      case 0: {
        float cut = pCutoff;
        if (cut >= 1000.0f) snprintf(b, sizeof b, "%.1fK", cut / 1000.0f);
        else snprintf(b, sizeof b, "%d", (int)(cut + 0.5f));
        val = clampf(logf(cut / 20.0f) / logf(900.0f), 0, 1); label = "CUT"; break;
      }
      case 1: snprintf(b, sizeof b, "%d", (int)(pRes * 100.0f + 0.5f)); val = pRes; label = "RES"; break;
      case 2: { float d = gEditTarget ? filtP.decay : ampP.decay; fmtTime(b, sizeof b, d);
                val = clampf(logf(d / 0.0005f) / logf(40000.0f), 0, 1); label = gEditTarget ? "D-FIL" : "D-AMP"; accent = acc; } break;
      case 3: { float s = gEditTarget ? filtP.sustain : ampP.sustain; snprintf(b, sizeof b, "%d", (int)(s * 100.0f + 0.5f));
                val = s; label = gEditTarget ? "S-FIL" : "S-AMP"; accent = acc; } break;
      case 4: { float rr = gEditTarget ? filtP.release : ampP.release; fmtTime(b, sizeof b, rr);
                val = clampf(logf(rr / 0.0005f) / logf(40000.0f), 0, 1); label = gEditTarget ? "R-FIL" : "R-AMP"; accent = acc; } break;
      case 5: snprintf(b, sizeof b, "%+d", (int)(pEnvDepth * 100.0f)); val = (pEnvDepth + 1.0f) * 0.5f; label = "DEP"; accent = accF; break;
      case 6: snprintf(b, sizeof b, "%d", (int)(pDetune * 100.0f + 0.5f)); val = pDetune; label = "DET"; break;
      case 7: snprintf(b, sizeof b, "%d", (int)(pMix * 100.0f + 0.5f)); val = pMix; label = "MIX"; break;
    }
    bool dirty = lcdFirst || (targetChanged && i >= 2 && i <= 4) ||
                 fabsf(val - lcdPrevVal[i]) > 0.002f || strcmp(b, lcdPrevText[i]) != 0;
    if (!dirty) continue;
    fillRect(col * 120, row * 60, 120, 60, bg);
    drawKnobCell(cx, cy, r, val, label, b, accent);
    lcdPrevVal[i] = val;
    strncpy(lcdPrevText[i], b, 15); lcdPrevText[i][15] = 0;
    lcdBandMask |= (uint8_t)(1u << row);
  }
  lcdPrevEdit = gEditTarget;
  lcdFirst = false;
}// ================= setup / loop =================
static void periFromSys() {
  clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                  clock_get_hz(clk_sys), clock_get_hz(clk_sys));
}
#define RECOV_MAGIC 0x4A503830u   // "JP80"
void setup() {
  // 自动恢复: 连续 2 次 boot 未跑完 setup -> 直接进 BOOTSEL (不用按键)
  uint32_t rv = watchdog_hw->scratch[0];
  if ((rv & 0xFFFF0000u) == RECOV_MAGIC) {
    uint16_t cnt = (uint16_t)(rv & 0xFFFFu);
    if (cnt >= 2) { watchdog_hw->scratch[0] = RECOV_MAGIC; reset_usb_boot(0, 0); }
    watchdog_hw->scratch[0] = RECOV_MAGIC | (uint32_t)(cnt + 1);
  } else {
    watchdog_hw->scratch[0] = RECOV_MAGIC | 1u;
  }
  watchdog_start_tick(12);
  watchdog_enable(4000, false);

  vreg_disable_voltage_limit();
  vreg_set_voltage((enum vreg_voltage)((1300 - 550) / 50));   // 1.30V (375MHz)
  delay(10);
  // ★ 升主频前先加大 flash QMI/XIP 读分频 (SCK = clk_sys / CLKDIV <= ~100MHz)
  { uint32_t khz = CPU_KHZ;
    uint32_t cd = (khz + 99999u) / 100000u;
    if (cd < 2u) cd = 2u; if (cd > 255u) cd = 255u;
    uint32_t cur = qmi_hw->m[0].timing & 0xFFu;
    if (cd > cur) {
      qmi_hw->m[0].timing = (qmi_hw->m[0].timing & ~0xFFu) | cd;
      __dmb();
      volatile uint32_t dummy = *(volatile uint32_t*)0x10000000u; (void)dummy;
      __dmb();
    }
  }
  set_sys_clock_khz(CPU_KHZ, false);
  periFromSys();
  { uint32_t fpscr; __asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);
    __asm volatile("vmsr fpscr, %0" :: "r"(fpscr)); }

  Serial.begin(115200);
#if USE_USB_MIDI
  usb_midi.setStringDescriptor("OV-JP8000 MIDI");
  usb_midi.begin();
  { uint32_t t0 = millis(); while (!TinyUSBDevice.mounted() && millis() - t0 < 3000) delay(10); }
  if (!TinyUSBDevice.mounted()) reset_usb_boot(0, 0);   // USB 没起来 -> 自动进 BOOTSEL, 防变砖
#endif

  pinMode(ADS_CS, OUTPUT); digitalWrite(ADS_CS, HIGH);
  pinMode(PIN_SW_ENV, INPUT_PULLDOWN);
  pinMode(PIN_SW_SLOPE, INPUT_PULLDOWN);
  pinMode(PIN_BTN_MODE, INPUT_PULLUP);
  pinMode(PIN_XSMT, OUTPUT); digitalWrite(PIN_XSMT, 0);
  pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, 0);
  pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, 1);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, 1);
  dacInit();

  SPI.setSCK(ADS_SCLK);
  SPI.setTX(ADS_MOSI);
  SPI.setRX(ADS_MISO);
  SPI.begin();
  adsBegin();

  ssBuildTables();
  buildGTab();
  buildEnvCurve();
  for (int i = 0; i < NUM_VOICES; i++) { voices[i].active = false; voices[i].amp.init(); voices[i].filt.init(); }

  readKnobs();
  ssSetMix(pMix);
  lcdInit();
  drawUI();

  static uint32_t core1_stack[16384];   // 64KB
  multicore_launch_core1_with_stack(core1_entry, core1_stack, sizeof(core1_stack));

  delay(150);
  watchdog_hw->scratch[0] = RECOV_MAGIC;   // setup 完整跑通 -> 清失败计数
  Serial.println("OV-JP8000 ready. 8-voice SuperSaw + JP-style VCF; ? for help");
}

void loop() {
  watchdog_update();
#if USE_USB_MIDI
  midiTask();
#endif
  static uint32_t lastKnob = 0, lastSw = 0, lastDac = 0, lastCmd = 0, lastLcd = 0;
  uint32_t now = millis();
  if (now - lastKnob >= 5) { lastKnob = now; readKnobs(); }
  if (now - lastSw >= 5) { lastSw = now; readSwitches(); }
  lcdTask();
  if (gScreenOn && !lcdDmaBusy && now - lastLcd >= 41) { lastLcd = now; drawUI(); }
  if (now - lastDac >= 2) {
    lastDac = now;
    float cv = log2f(clampf(gLastFreq, 20.0f, 5000.0f) / 55.0f);
    dacWrite(0, voltsToCode(cv));
    dacWrite(1, gAnyGate ? 65535 : 0);
    dacWrite(2, voltsToCode(gLastFilterEnv * 5.0f));
    dacWrite(3, voltsToCode(gLastAmpEnv * 5.0f));
  }
  // 串口命令 (非阻塞)
  static char cmd[64]; static int cmdLen = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) { cmd[cmdLen] = 0; handleCmd(cmd); cmdLen = 0; }
    } else if (cmdLen < 63) cmd[cmdLen++] = c;
  }
}
