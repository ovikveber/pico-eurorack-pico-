// ST7789 240x240 color test for the midi-to-cv board.
// PIO SPI (4 cycles/bit) + DMA for pixels; bit-bang only for commands.
// 8 colors, 3 frames each, 24 fps -> one 24-frame cycle per second.

#include <Arduino.h>
#include <hardware/pio.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/clocks.h>
#include <hardware/vreg.h>
#include <hardware/watchdog.h>
#include "lcd_spi.pio.h"

#define PIN_SCK   10
#define PIN_MOSI  9
#define PIN_RST   8
#define PIN_DC    7
#define PIN_CS    6
#define PIN_BL    5

#define ENC_A     13
#define ENC_B     12
#define ENC_SW    11
#define ENC_TRANS_PER_STEP 2

#define LCD_W 240
#define LCD_H 240

#define TARGET_FPS 24
#define FRAME_US   (1000000 / TARGET_FPS)
#define FRAMES_PER_COLOR 3

// ---------------- overclock ladder + watchdog ----------------
#define OC_SCRATCH_MAGIC 0x0C0FFEE1u
#define OC_WDT_MS        6000u
#define OC_FAILS_TO_DROP 2
static const uint32_t OC_KHZ[] = { 500000, 480000, 450000, 420000, 400000, 375000, 360000, 300000, 250000, 125000 };
static const uint32_t OC_N = sizeof(OC_KHZ) / sizeof(OC_KHZ[0]);
static volatile uint32_t gOcKHz = 0;
static volatile int gOcIdx = 0;
static volatile bool gWdReset = false;

static void overclockLadder() {
  uint32_t idx, fails;
  if (watchdog_hw->scratch[0] != OC_SCRATCH_MAGIC) {
    idx = 0; fails = 0;
  } else {
    idx = watchdog_hw->scratch[1];
    fails = watchdog_hw->scratch[2];
    if (watchdog_enable_caused_reboot()) {
      gWdReset = true;
      if (++fails >= OC_FAILS_TO_DROP) { fails = 0; idx++; }
    } else {
      fails = 0;
    }
  }
  if (idx >= OC_N) idx = OC_N - 1;

  watchdog_hw->scratch[0] = OC_SCRATCH_MAGIC;
  watchdog_hw->scratch[1] = idx;
  watchdog_hw->scratch[2] = fails;

  if (vreg_get_voltage() < VREG_VOLTAGE_1_30) {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    busy_wait_us_32(2000);
  }
  watchdog_start_tick(12);
  watchdog_enable(OC_WDT_MS, false);
  while (idx < OC_N && !set_sys_clock_khz(OC_KHZ[idx], false)) idx++;
  if (idx >= OC_N) {
    idx = OC_N - 1;
    set_sys_clock_khz(OC_KHZ[idx], false);
  }
  watchdog_hw->scratch[1] = idx;

  gOcIdx = (int)idx;
  gOcKHz = clock_get_hz(clk_sys) / 1000u;
}

static void periFromSys() {
  clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                  clock_get_hz(clk_sys), clock_get_hz(clk_sys));
}

static uint16_t fb[LCD_W * LCD_H];
static PIO lcdPio = pio0;
static int lcdSm = -1;
static uint lcdOff = 0;
static int dmaTx = -1;

// SCLK = clk_sys / (2 * lcdClkDiv) = 31.25 MHz at 125 MHz (2 PIO cycles per bit)
static float lcdClkDiv = 2.0f;
static uint8_t lcdMadctl = 0x00;

static inline uint16_t C(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline float sclkHz() {
  return (float)clock_get_hz(clk_sys) / (2.0f * lcdClkDiv);
}

// ---------------- bit-bang (commands / window) ----------------
static void bbByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    gpio_put(PIN_MOSI, (b >> i) & 1); busy_wait_us_32(1);
    gpio_put(PIN_SCK, 1);             busy_wait_us_32(1);
    gpio_put(PIN_SCK, 0);             busy_wait_us_32(1);
  }
}
static void lcdCmdBB(uint8_t c) { gpio_put(PIN_DC, 0); gpio_put(PIN_CS, 0); bbByte(c); gpio_put(PIN_CS, 1); }
static void lcdDataBB(const uint8_t* d, size_t n) {
  gpio_put(PIN_DC, 1); gpio_put(PIN_CS, 0);
  while (n--) bbByte(*d++);
  gpio_put(PIN_CS, 1);
}
static void lcdCmdDataBB(uint8_t c, const uint8_t* d, size_t n) {
  lcdCmdBB(c);
  if (n) lcdDataBB(d, n);
}

static void bbModeBegin() {
  pio_sm_set_enabled(lcdPio, lcdSm, false);
  gpio_set_function(PIN_SCK,  GPIO_FUNC_SIO);
  gpio_set_function(PIN_MOSI, GPIO_FUNC_SIO);
  gpio_set_dir(PIN_SCK, OUTPUT);
  gpio_set_dir(PIN_MOSI, OUTPUT);
  gpio_put(PIN_SCK, 0);
  gpio_put(PIN_MOSI, 0);
}
static void bbModeEnd() {
  pio_gpio_init(lcdPio, PIN_SCK);
  pio_gpio_init(lcdPio, PIN_MOSI);
  pio_sm_set_consecutive_pindirs(lcdPio, lcdSm, PIN_SCK, 1, true);
  pio_sm_set_consecutive_pindirs(lcdPio, lcdSm, PIN_MOSI, 1, true);
  pio_sm_set_enabled(lcdPio, lcdSm, true);
}

// ---------------- PIO + DMA ----------------
static inline void pioDrain() {
  while (!pio_sm_is_tx_fifo_empty(lcdPio, lcdSm)) {}
  uint32_t us = (uint32_t)(32.0f / sclkHz() * 1e6f) + 3;
  busy_wait_us_32(us);
}

static void lcdSetWindow(int x0, int y0, int x1, int y1) {
  uint8_t d[4];
  bbModeBegin();
  lcdCmdBB(0x2A); d[0]=x0>>8; d[1]=x0; d[2]=x1>>8; d[3]=x1; lcdDataBB(d, 4);
  lcdCmdBB(0x2B); d[0]=y0>>8; d[1]=y0; d[2]=y1>>8; d[3]=y1; lcdDataBB(d, 4);
  lcdCmdBB(0x2C);
  bbModeEnd();
}

static void dmaStart(const void* src, uint32_t words) {
  while (dma_channel_is_busy(dmaTx)) {}
  dma_channel_set_read_addr(dmaTx, src, false);
  dma_channel_set_trans_count(dmaTx, words, true);
}

static void lcdPushFb() {
  lcdSetWindow(0, 0, LCD_W - 1, LCD_H - 1);
  gpio_put(PIN_DC, 1);
  gpio_put(PIN_CS, 0);
  dmaStart(fb, sizeof(fb) / 4);
  dma_channel_wait_for_finish_blocking(dmaTx);
  pioDrain();
  gpio_put(PIN_CS, 1);
}

static void pioBegin() {
  lcdPio = pio0;
  lcdSm = (int)pio_claim_unused_sm(lcdPio, true);
  lcdOff = pio_add_program(lcdPio, &lcd_spi_program);
  pio_sm_config c = lcd_spi_program_get_default_config(lcdOff);
  sm_config_set_sideset_pins(&c, PIN_SCK);
  sm_config_set_out_pins(&c, PIN_MOSI, 1);
  sm_config_set_out_shift(&c, false, true, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
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

static void setLcdSclk(float mhz) {
  lcdClkDiv = (float)clock_get_hz(clk_sys) / (2.0f * mhz * 1e6f);
  if (lcdClkDiv < 1.0f) lcdClkDiv = 1.0f;
  pio_sm_set_clkdiv(lcdPio, lcdSm, lcdClkDiv);
}

static void lcdInit() {
  gpio_set_function(PIN_SCK, GPIO_FUNC_SIO);
  gpio_set_function(PIN_MOSI, GPIO_FUNC_SIO);
  gpio_set_dir(PIN_SCK, OUTPUT);
  gpio_set_dir(PIN_MOSI, OUTPUT);
  gpio_put(PIN_SCK, 0);
  gpio_put(PIN_MOSI, 0);

  gpio_put(PIN_RST, 0); delay(30); gpio_put(PIN_RST, 1); delay(150);
  lcdCmdBB(0x01); delay(150);
  lcdCmdBB(0x11); delay(120);
  uint8_t d[1];
  d[0] = 0x55; lcdCmdDataBB(0x3A, d, 1);
  d[0] = lcdMadctl; lcdCmdDataBB(0x36, d, 1);
  const uint8_t b2[5] = {0x0C,0x0C,0x00,0x33,0x33}; lcdCmdDataBB(0xB2, b2, 5);
  d[0]=0x35; lcdCmdDataBB(0xB7, d, 1);
  d[0]=0x19; lcdCmdDataBB(0xBB, d, 1);
  d[0]=0x2C; lcdCmdDataBB(0xC0, d, 1);
  d[0]=0x01; lcdCmdDataBB(0xC2, d, 1);
  d[0]=0x12; lcdCmdDataBB(0xC3, d, 1);
  d[0]=0x20; lcdCmdDataBB(0xC4, d, 1);
  d[0]=0x0F; lcdCmdDataBB(0xC6, d, 1);
  const uint8_t dd[2] = {0xA4,0xA1}; lcdCmdDataBB(0xD0, dd, 2);
  lcdCmdBB(0x21);
  const uint8_t e0[14] = {0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,0x54,0x4C,0x18,0x0D,0x0B,0x1F,0x23};
  lcdCmdDataBB(0xE0, e0, 14);
  const uint8_t e1[14] = {0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,0x44,0x51,0x2F,0x1F,0x1F,0x20,0x23};
  lcdCmdDataBB(0xE1, e1, 14);
  lcdCmdBB(0x13); delay(10);
  lcdCmdBB(0x29); delay(100);
  gpio_put(PIN_CS, 1);
}

// ---------------- color test ----------------
static const uint8_t PAL[][3] = {
  {255,0,0}, {0,255,0}, {0,0,255}, {255,255,0},
  {0,255,255}, {255,0,255}, {255,255,255}, {0,0,0}
};
#define PAL_N (sizeof(PAL) / sizeof(PAL[0]))

static void fillColor(uint16_t c) {
  for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = c;
  lcdPushFb();
}

// ---------------- encoder (interrupt driven) ----------------
static const int8_t ENC_TBL[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};
static volatile int16_t encDelta = 0;

static void encISR() {
  static int8_t accum = 0;
  static uint8_t lastAB = 0;
  uint8_t ab = (uint8_t)((digitalRead(ENC_A) << 1) | digitalRead(ENC_B));
  if (ab != lastAB) {
    accum += ENC_TBL[(lastAB << 2) | ab];
    lastAB = ab;
    if (accum >= ENC_TRANS_PER_STEP)  { accum = 0; encDelta++; }
    if (accum <= -ENC_TRANS_PER_STEP) { accum = 0; encDelta--; }
  }
}

static void showColor(int idx) {
  fillColor(C(PAL[idx][0], PAL[idx][1], PAL[idx][2]));
}

static int vregMv(enum vreg_voltage v) { return 850 + ((int)v - 0x06) * 50; }

static void printInfo() {
  if (Serial.availableForWrite() < 160) return;
  Serial.print("cpu="); Serial.print(clock_get_hz(clk_sys) / 1000000.0f, 1);
  Serial.print("MHz vreg="); Serial.print(vregMv(vreg_get_voltage()));
  Serial.print("mV sclk="); Serial.print(sclkHz() / 1000000.0f, 1);
  Serial.print("MHz oc_idx="); Serial.print(gOcIdx);
  Serial.print(" oc_khz="); Serial.print(gOcKHz);
  Serial.print(" wd_reset="); Serial.println(gWdReset ? 1 : 0);
}

void setup() {
  overclockLadder();
  periFromSys();

  Serial.begin(115200);
  delay(200);

  gpio_init(PIN_CS);  gpio_set_dir(PIN_CS, OUTPUT);  gpio_put(PIN_CS, 1);
  gpio_init(PIN_DC);  gpio_set_dir(PIN_DC, OUTPUT);  gpio_put(PIN_DC, 1);
  gpio_init(PIN_RST); gpio_set_dir(PIN_RST, OUTPUT); gpio_put(PIN_RST, 1);
  gpio_init(PIN_BL);  gpio_set_dir(PIN_BL, OUTPUT);  gpio_put(PIN_BL, 1);

  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encISR, CHANGE);

  lcdInit();
  pioBegin();
  setLcdSclk(31.25f);

  showColor(0);

  printInfo();
  if (Serial.availableForWrite() >= 64) {
    Serial.println("rotate=color  click=toggle auto");
  }
}

void loop() {
  watchdog_update();

  static bool serialWas = false;
  bool serialNow = (bool)Serial;
  if (serialNow && !serialWas) printInfo();
  serialWas = serialNow;

  static bool autoMode = false;
  static int colorIdx = 0;
  static uint32_t frame = 0;
  static uint32_t nextUs = 0;
  static uint32_t markUs = 0;
  static uint32_t maxFrameUs = 0;
  static uint8_t sLast = 0, sStable = 0;
  static uint32_t tChange = 0;
  noInterrupts();
  int16_t delta = encDelta;
  encDelta = 0;
  interrupts();
  if (delta != 0) {
    autoMode = false;
    int ci = (colorIdx + delta) % (int)PAL_N;
    if (ci < 0) ci += (int)PAL_N;
    colorIdx = ci;
    showColor(colorIdx);
  }

  uint8_t s = (digitalRead(ENC_SW) == LOW) ? 1 : 0;
  if (s != sLast) { sLast = s; tChange = millis(); }
  if (millis() - tChange >= 15 && s != sStable) {
    sStable = s;
    if (s == 0) {
      autoMode = !autoMode;
      frame = 0;
      nextUs = micros();
      if (Serial.availableForWrite() >= 24) {
        Serial.print("mode="); Serial.println(autoMode ? "auto" : "manual");
      }
      if (!autoMode) showColor(colorIdx);
    }
  }

  if (!autoMode) return;

  uint32_t now = micros();
  if ((int32_t)(now - nextUs) >= 0) {
    nextUs += FRAME_US;
    if ((int32_t)(now - nextUs) > (int32_t)(3 * FRAME_US))
      nextUs = now + FRAME_US;

    uint32_t t0 = micros();
    uint8_t idx = (uint8_t)((frame / FRAMES_PER_COLOR) % PAL_N);
    fillColor(C(PAL[idx][0], PAL[idx][1], PAL[idx][2]));
    colorIdx = idx;
    uint32_t dt = micros() - t0;
    if (dt > maxFrameUs) maxFrameUs = dt;

    frame++;
    if (frame % TARGET_FPS == 0) {
      uint32_t t = micros();
      float fps = (markUs == 0) ? 0.0f : (TARGET_FPS * 1e6f) / (float)(t - markUs);
      markUs = t;
      if (Serial.availableForWrite() >= 64) {
        Serial.print("frame="); Serial.print(frame);
        Serial.print(" fps="); Serial.print(fps, 1);
        Serial.print(" max_frame_ms="); Serial.println(maxFrameUs / 1000.0f, 2);
      }
      maxFrameUs = 0;
    }
  }
}
