#include <Arduino.h>

constexpr uint8_t PIN_LDAC = 17;
constexpr uint8_t PIN_SYNC = 18;
constexpr uint8_t PIN_SCLK = 19;
constexpr uint8_t PIN_DIN  = 20;

constexpr uint32_t CMD_WRITE_BUF = 0x0u;
constexpr uint32_t CMD_REF_ON    = 0x08000001u;

void dacSend(uint32_t w) {
  digitalWrite(PIN_SYNC, LOW);
  for (int i = 31; i >= 0; --i) {
    digitalWrite(PIN_DIN, (w >> i) & 1u);
    delayMicroseconds(1);
    digitalWrite(PIN_SCLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(PIN_SCLK, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(PIN_SYNC, HIGH);
  delayMicroseconds(1);
}

void dacBuffer(uint8_t ch, uint16_t code) {
  uint32_t w = (CMD_WRITE_BUF << 24) | ((uint32_t)(ch & 0x0F) << 20) | ((uint32_t)code << 4);
  dacSend(w);
}

void dacLatch() {
  digitalWrite(PIN_LDAC, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_LDAC, HIGH);
  delayMicroseconds(2);
}

void dacAll(uint16_t code) {
  for (uint8_t c = 0; c < 8; ++c) dacBuffer(c, code);
  dacLatch();
}

void setup() {
  pinMode(PIN_LDAC, OUTPUT); digitalWrite(PIN_LDAC, HIGH);
  pinMode(PIN_SYNC, OUTPUT); digitalWrite(PIN_SYNC, HIGH);
  pinMode(PIN_SCLK, OUTPUT); digitalWrite(PIN_SCLK, LOW);
  pinMode(PIN_DIN,  OUTPUT); digitalWrite(PIN_DIN,  LOW);
  delay(20);
  dacSend(CMD_REF_ON);
  delay(10);
  dacAll(65535);
}

void loop() {
  dacAll(65535);
  delay(500);
}
