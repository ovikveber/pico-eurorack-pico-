#include <Arduino.h>
#include <math.h>
#include <pico/time.h>

// ============================================================================
// env_selftest —— 不依赖任何输入，用软件 Gate 轮流跑 8 路 ADSR 包络
// 目的：验证包络引擎 + DAC 更新链路 + 8 路运放输出（万用表/示波器看 OUTA-H）
// 波形：每路同步；A=1s -> D=1s -> Sustain(60%,保持1s) -> R=1s -> 空闲1s，循环
// ============================================================================

constexpr uint8_t PIN_DAC_LDAC = 17;
constexpr uint8_t PIN_DAC_SYNC = 18;
constexpr uint8_t PIN_DAC_SCLK = 19;
constexpr uint8_t PIN_DAC_DIN  = 20;

constexpr uint32_t CMD_SW_RESET  = 0x07000000u;
constexpr uint32_t CMD_REF_ON    = 0x08000001u;
constexpr uint8_t  CMD_WR_UPDATE = 0x3;

constexpr uint8_t NUM_ENV = 8;
constexpr float TICK_HZ = 1000.0f;
constexpr int64_t TICK_US = (int64_t)(1000000.0f / TICK_HZ + 0.5f);

constexpr float A_TIME = 1.0f, D_TIME = 1.0f, R_TIME = 1.0f;
constexpr float SUSTAIN = 0.6f;
constexpr uint32_t SUSTAIN_MS = 1000;
constexpr uint32_t IDLE_MS = 1000;

enum Phase { PH_IDLE, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

static volatile float gLevel = 0.0f;
static volatile Phase gPhase = PH_IDLE;
static volatile uint32_t gPhaseMs = 0;

static inline float alphaOf(float t) { return 1.0f - expf(-1.0f / (t * TICK_HZ)); }
static const float ALPHA_A = alphaOf(A_TIME);
static const float ALPHA_D = alphaOf(D_TIME);
static const float ALPHA_R = alphaOf(R_TIME);

static void dacPace() { for (volatile int k = 0; k < 6; ++k) __nop(); }

static void dacFrame(uint32_t w) {
  digitalWrite(PIN_DAC_SYNC, LOW);
  dacPace();
  for (int i = 31; i >= 0; --i) {
    digitalWrite(PIN_DAC_DIN, (w >> i) & 1u);
    dacPace();
    digitalWrite(PIN_DAC_SCLK, HIGH);
    dacPace();
    digitalWrite(PIN_DAC_SCLK, LOW);
    dacPace();
  }
  digitalWrite(PIN_DAC_SYNC, HIGH);
  dacPace();
}

static void dacWriteCh(uint8_t ch, uint16_t code) {
  dacFrame(((uint32_t)CMD_WR_UPDATE << 24) | ((uint32_t)(ch & 7) << 20) | ((uint32_t)code << 4));
}

static void dacWriteAll(uint16_t code) {
  for (uint8_t c = 0; c < NUM_ENV; ++c) dacWriteCh(c, code);
}

static bool __not_in_flash_func(onTick)(repeating_timer_t *) {
  gPhaseMs += (uint32_t)(1000.0f / TICK_HZ + 0.5f);

  float lv = gLevel;
  switch (gPhase) {
    case PH_IDLE:
      lv = 0.0f;
      if (gPhaseMs >= IDLE_MS) { gPhaseMs = 0; gPhase = PH_ATTACK; }
      break;
    case PH_ATTACK:
      lv += (1.2f - lv) * ALPHA_A;
      if (lv >= 1.0f) { lv = 1.0f; gPhase = PH_DECAY; gPhaseMs = 0; }
      break;
    case PH_DECAY:
      lv += (SUSTAIN - lv) * ALPHA_D;
      if (lv <= SUSTAIN + 0.001f) { lv = SUSTAIN; gPhase = PH_SUSTAIN; gPhaseMs = 0; }
      break;
    case PH_SUSTAIN:
      lv = SUSTAIN;
      if (gPhaseMs >= SUSTAIN_MS) { gPhaseMs = 0; gPhase = PH_RELEASE; }
      break;
    case PH_RELEASE:
      lv += (0.0f - lv) * ALPHA_R;
      if (lv < 0.001f) { lv = 0.0f; gPhase = PH_IDLE; gPhaseMs = 0; }
      break;
  }
  if (lv < 0.0f) lv = 0.0f;
  if (lv > 1.0f) lv = 1.0f;
  gLevel = lv;

  uint16_t code = (uint16_t)(lv * 65535.0f + 0.5f);
  dacWriteAll(code);
  return true;
}

static repeating_timer_t gTimer;

void setup() {
  pinMode(PIN_DAC_LDAC, OUTPUT); digitalWrite(PIN_DAC_LDAC, HIGH);
  pinMode(PIN_DAC_SYNC, OUTPUT); digitalWrite(PIN_DAC_SYNC, HIGH);
  pinMode(PIN_DAC_SCLK, OUTPUT); digitalWrite(PIN_DAC_SCLK, LOW);
  pinMode(PIN_DAC_DIN,  OUTPUT); digitalWrite(PIN_DAC_DIN,  LOW);

  Serial.begin(115200);

  delay(20);
  dacFrame(CMD_SW_RESET);
  delay(5);
  dacWriteAll(0);
  delay(5);
  dacFrame(CMD_REF_ON);
  delay(50);

  add_repeating_timer_us(-TICK_US, onTick, nullptr, &gTimer);
  Serial.println("ENV-SELFTEST v1: 8ch ADSR synchronized");
}

static const char *phaseName(Phase p) {
  switch (p) {
    case PH_IDLE: return "IDLE";
    case PH_ATTACK: return "ATTACK";
    case PH_DECAY: return "DECAY";
    case PH_SUSTAIN: return "SUSTAIN";
    case PH_RELEASE: return "RELEASE";
  }
  return "?";
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last < 200) return;
  last = millis();
  Serial.print(phaseName(gPhase));
  Serial.print("  level="); Serial.print(gLevel, 3);
  Serial.print("  expectV="); Serial.println(gLevel * 10.0f, 2);
}
