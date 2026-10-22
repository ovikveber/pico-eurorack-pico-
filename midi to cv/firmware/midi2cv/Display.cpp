#include <Arduino.h>
#include <hardware/pio.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/clocks.h>
#include "lcd_spi.pio.h"
#include "font5x7.h"
#include "Display.h"

#define PIN_SCK   10
#define PIN_MOSI  9
#define PIN_RST   8
#define PIN_DC    7
#define PIN_CS    6
#define PIN_BL    5

#define LCD_W 240
#define LCD_H 240

static uint16_t fb[LCD_W * LCD_H] __attribute__((aligned(4)));
static PIO lcdPio = pio0;
static int lcdSm = -1;
static uint lcdOff = 0;
static int dmaTx = -1;
static float lcdClkDiv = 2.0f;
static uint32_t lcdSysHz = 125000000u;
static const uint8_t lcdMadctl = 0x60;   // rotate 90 deg

static inline float sclkHz() {
  return (float)lcdSysHz / (2.0f * lcdClkDiv);
}

// ---------------- bit-bang for commands / window ----------------
static void bbByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    gpio_put(PIN_MOSI, (b >> i) & 1); busy_wait_us_32(1);
    gpio_put(PIN_SCK, 1);             busy_wait_us_32(1);
    gpio_put(PIN_SCK, 0);             busy_wait_us_32(1);
  }
}
static void lcdCmdBB(uint8_t c) {
  gpio_put(PIN_DC, 0); gpio_put(PIN_CS, 0); bbByte(c); gpio_put(PIN_CS, 1);
}
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
static void lcdPinDrive() {
  gpio_set_drive_strength(PIN_SCK, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(PIN_MOSI, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_slew_rate(PIN_SCK, GPIO_SLEW_RATE_FAST);
  gpio_set_slew_rate(PIN_MOSI, GPIO_SLEW_RATE_FAST);
}

static void bbModeEnd() {
  pio_gpio_init(lcdPio, PIN_SCK);
  pio_gpio_init(lcdPio, PIN_MOSI);
  lcdPinDrive();
  pio_sm_set_consecutive_pindirs(lcdPio, lcdSm, PIN_SCK, 1, true);
  pio_sm_set_consecutive_pindirs(lcdPio, lcdSm, PIN_MOSI, 1, true);
  pio_sm_set_enabled(lcdPio, lcdSm, false);
  pio_sm_clear_fifos(lcdPio, lcdSm);
  pio_sm_restart(lcdPio, lcdSm);
  pio_sm_exec(lcdPio, lcdSm, pio_encode_jmp(lcdOff));
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
  lcdPinDrive();
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

// ---------------- drawing ----------------
static inline void putPix(int i, uint16_t c) {
  if (i >= 0 && i < LCD_W * LCD_H) fb[i ^ 1] = c;
}
static inline void putPixXY(int x, int y, uint16_t c) {
  if (x >= 0 && x < LCD_W && y >= 0 && y < LCD_H) fb[(y * LCD_W + x) ^ 1] = c;
}

void displayInit() {
  gpio_init(PIN_CS);  gpio_set_dir(PIN_CS, OUTPUT);  gpio_put(PIN_CS, 1);
  gpio_init(PIN_DC);  gpio_set_dir(PIN_DC, OUTPUT);  gpio_put(PIN_DC, 1);
  gpio_init(PIN_RST); gpio_set_dir(PIN_RST, OUTPUT); gpio_put(PIN_RST, 1);
  gpio_init(PIN_BL);  gpio_set_dir(PIN_BL, OUTPUT);  gpio_put(PIN_BL, 1);

  lcdInit();
  pioBegin();
}

void displayClear(uint16_t color) {
  for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = color;
}

void displayFillRect(int x, int y, int w, int h, uint16_t color) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LCD_W) w = LCD_W - x;
  if (y + h > LCD_H) h = LCD_H - y;
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++)
      putPix(yy * LCD_W + xx, color);
}

void displayDrawRect(int x, int y, int w, int h, uint16_t color, int thickness) {
  for (int t = 0; t < thickness; t++) {
    displayFillRect(x + t, y + t, w - 2 * t, 1, color);
    displayFillRect(x + t, y + h - 1 - t, w - 2 * t, 1, color);
    displayFillRect(x + t, y + t, 1, h - 2 * t, color);
    displayFillRect(x + w - 1 - t, y + t, 1, h - 2 * t, color);
  }
}

void displayText(int x, int y, const char* text, uint16_t color, int scale) {
  if (scale < 1) scale = 1;
  for (; *text; text++) {
    char ch = *text;
    if (ch < 32 || ch > 90) ch = 32;
    const uint8_t* g = font5x7[ch - 32];
    for (int cx = 0; cx < FONT_W; cx++) {
      uint8_t bits = g[cx];
      for (int cy = 0; cy < FONT_H; cy++) {
        if ((bits >> cy) & 1) {
          for (int sy = 0; sy < scale; sy++)
            for (int sx = 0; sx < scale; sx++)
              putPixXY(x + cx * scale + sx, y + cy * scale + sy, color);
        }
      }
    }
    x += (FONT_W + 1) * scale;
  }
}

static inline uint16_t blend565(uint16_t dst, uint16_t src, uint8_t a) {
  int dr = (dst >> 11) & 31, dg = (dst >> 5) & 63, db = dst & 31;
  int sr = (src >> 11) & 31, sg = (src >> 5) & 63, sb = src & 31;
  int r = (dr * (255 - a) + sr * a) / 255;
  int g = (dg * (255 - a) + sg * a) / 255;
  int b = (db * (255 - a) + sb * a) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

void displayDrawLine(int x0, int y0, int x1, int y1, uint16_t color, int thickness) {
  if (thickness < 1) thickness = 1;
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    if (thickness == 1) {
      putPixXY(x0, y0, color);
    } else {
      displayFillRect(x0 - thickness / 2, y0 - thickness / 2,
                      thickness, thickness, color);
    }
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void displayFillCircle(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) return;
  int r2 = r * r;
  for (int y = -r; y <= r; y++) {
    for (int x = -r; x <= r; x++) {
      if (x * x + y * y <= r2) putPixXY(cx + x, cy + y, color);
    }
  }
}

void displayFillCircleAlpha(int cx, int cy, int r, uint16_t color, uint8_t alpha) {
  if (r <= 0 || alpha == 0) return;
  int r2 = r * r;
  for (int y = -r; y <= r; y++) {
    int py = cy + y;
    if (py < 0 || py >= LCD_H) continue;
    for (int x = -r; x <= r; x++) {
      int px = cx + x;
      if (px < 0 || px >= LCD_W) continue;
      if (x * x + y * y > r2) continue;
      int i = py * LCD_W + px;
      fb[i ^ 1] = blend565(fb[i ^ 1], color, alpha);
    }
  }
}

void displayDrawCircle(int cx, int cy, int r, uint16_t color, int thickness) {
  if (r <= 0) return;
  for (int t = 0; t < thickness; t++) {
    int rr = r - t;
    if (rr <= 0) break;
    int x = rr, y = 0, err = 1 - rr;
    while (x >= y) {
      putPixXY(cx + x, cy + y, color);
      putPixXY(cx + y, cy + x, color);
      putPixXY(cx + x, cy - y, color);
      putPixXY(cx + y, cy - x, color);
      putPixXY(cx - x, cy + y, color);
      putPixXY(cx - y, cy + x, color);
      putPixXY(cx - x, cy - y, color);
      putPixXY(cx - y, cy - x, color);
      y++;
      if (err < 0) err += 2 * y + 1;
      else { x--; err += 2 * (y - x) + 1; }
    }
  }
}

void displayFlush() {
  lcdPushFb();
}

static uint16_t sqBuf[24 * 24] __attribute__((aligned(4)));

void displayFillSquare(int x, int y, int size, uint16_t color) {
  if (size < 1 || size > 24) return;
  for (int i = 0; i < size * size; i++) sqBuf[i] = color;
  lcdSetWindow(x, y, x + size - 1, y + size - 1);
  gpio_put(PIN_DC, 1);
  gpio_put(PIN_CS, 0);
  dmaStart(sqBuf, (uint32_t)(size * size * 2) / 4);
  dma_channel_wait_for_finish_blocking(dmaTx);
  pioDrain();
  gpio_put(PIN_CS, 1);
  busy_wait_us_32(20);
}

void displaySetSclkForSys(float mhz, uint32_t sysKhz) {
  if (mhz < 1.0f) mhz = 1.0f;
  lcdSysHz = sysKhz * 1000u;
  lcdClkDiv = (float)lcdSysHz / (2.0f * mhz * 1e6f);
  if (lcdClkDiv < 1.0f) lcdClkDiv = 1.0f;
  if (lcdSm >= 0) pio_sm_set_clkdiv(lcdPio, lcdSm, lcdClkDiv);
}



