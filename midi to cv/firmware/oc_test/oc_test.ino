// RP2040 overclock verification ladder: 420 -> 400 -> 125.
// 16MB W25Q128 + boot2 /4: flash clock = sysclk/4, keep it <= ~133MHz.
// Actual CPU speed is measured with a RAM-resident loop; clock_get_hz is not trusted.
// The watchdog is enabled BEFORE any clock switch, so a 450-style hang can reset.

#include <Arduino.h>
#include <hardware/pll.h>
#include <hardware/clocks.h>
#include <hardware/vreg.h>
#include <hardware/watchdog.h>

#define OC_MAGIC 0x0C0FFEE7u

static const uint32_t TARGETS[] = { 436000, 432000, 428000, 424000, 420000, 125000 };
static const uint32_t TARGET_N = sizeof(TARGETS) / sizeof(TARGETS[0]);

static uint32_t cal125 = 0;
static uint32_t stressRef = 0;
static uint32_t gStableKhz = 0;
static uint32_t gActual = 0;

static uint32_t __not_in_flash_func(measureCal)() {
  uint32_t t = micros();
  for (uint32_t i = 0; i < 200000; i++) __asm volatile("nop");
  return micros() - t;
}

static uint32_t __not_in_flash_func(stressLoop)() {
  uint32_t x = 0x12345678u;
  for (uint32_t i = 0; i < 5000000u; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
  }
  return x;
}

static void printClock(const char* tag, uint32_t target, bool ok, uint32_t actual, uint32_t cal) {
  uint32_t ctrl = clocks_hw->clk[clk_sys].ctrl;
  uint32_t sel  = clocks_hw->clk[clk_sys].selected;
  uint32_t pll  = (pll_sys_hw->cs & 0x80000000u) ? 1 : 0;
  uint32_t fb   = pll_sys_hw->fbdiv_int;
  Serial.printf("%s target=%lu ok=%d actual=%luMHz pll=%lu ctrl=0x%lx sel=0x%lx fbdiv=%lu cal=%lu\n",
                tag, (unsigned long)target, ok ? 1 : 0, (unsigned long)actual,
                (unsigned long)pll, (unsigned long)ctrl, (unsigned long)sel,
                (unsigned long)fb, (unsigned long)cal);
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  cal125 = measureCal();
  stressRef = stressLoop();
  Serial.printf("BASE 125MHz cal=%lu stress=%08lx\n",
                (unsigned long)cal125, (unsigned long)stressRef);

  vreg_set_voltage(VREG_VOLTAGE_1_30);
  busy_wait_us_32(2000);

  uint32_t idx, fails;
  bool wd = watchdog_enable_caused_reboot();
  if (watchdog_hw->scratch[0] != OC_MAGIC) {
    idx = 0; fails = 0;
  } else {
    idx = watchdog_hw->scratch[1];
    fails = watchdog_hw->scratch[2];
    if (wd) {
      if (++fails >= 2) { fails = 0; idx++; }
    } else {
      fails = 0;
    }
  }
  if (idx >= TARGET_N) idx = TARGET_N - 1;
  watchdog_hw->scratch[0] = OC_MAGIC;
  watchdog_hw->scratch[1] = idx;
  watchdog_hw->scratch[2] = fails;

  watchdog_start_tick(12);
  watchdog_enable(6000, false);

  for (; idx < TARGET_N; idx++) {
    const uint32_t target = TARGETS[idx];
    bool ok = set_sys_clock_khz(target, false);
    busy_wait_us_32(2000);

    uint32_t cal = measureCal();
    uint32_t actual = (uint32_t)((uint64_t)125 * cal125 / cal);
    printClock("try", target, ok, actual, cal);

    bool good = ok &&
                actual >= (target / 1000) * 95 / 100 &&
                actual <= (target / 1000) * 105 / 100;
    if (good) {
      uint32_t t0 = micros();
      uint32_t x = stressLoop();
      uint32_t dt = micros() - t0;
      Serial.printf("stress=%08lx dt=%luus\n", (unsigned long)x, (unsigned long)dt);
      good = (x == stressRef);
      Serial.println(good ? "STABLE" : "FAIL: stress checksum");
    } else {
      Serial.println("FAIL: clock mismatch");
    }
    if (good) {
      gStableKhz = target;
      gActual = actual;
      watchdog_hw->scratch[1] = idx;
      watchdog_hw->scratch[2] = 0;
      while (true) {
        watchdog_update();
        delay(1000);
        Serial.printf("running stable=%lu actual=%luMHz\n",
                      (unsigned long)(gStableKhz / 1000), (unsigned long)gActual);
      }
    }
  }

  Serial.println("NO STABLE TARGET, fallback 125MHz");
  set_sys_clock_khz(125000, true);
  gStableKhz = 125000;
  while (true) {
    watchdog_update();
    delay(1000);
    Serial.println("running 125MHz fallback");
  }
}

void loop() {}
