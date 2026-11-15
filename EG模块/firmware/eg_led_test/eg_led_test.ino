#include <Arduino.h>
#include <hardware/gpio.h>

constexpr uint8_t PIN_SIN   = 0;
constexpr uint8_t PIN_SCLK  = 1;
constexpr uint8_t PIN_BLANK = 2;
constexpr uint8_t PIN_XLAT  = 3;

static void tlcWrite(uint16_t v) {
  digitalWrite(PIN_XLAT, LOW);
  for (int c = 23; c >= 0; --c) {
    for (int b = 11; b >= 0; --b) {
      digitalWrite(PIN_SCLK, LOW);
      if ((v >> b) & 1u) digitalWrite(PIN_SIN, HIGH); else digitalWrite(PIN_SIN, LOW);
      delayMicroseconds(10);
      digitalWrite(PIN_SCLK, HIGH);
      delayMicroseconds(10);
    }
  }
  digitalWrite(PIN_SCLK, LOW);
  digitalWrite(PIN_XLAT, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_XLAT, LOW);
}

void setup() {
  pinMode(PIN_SIN, OUTPUT);
  pinMode(PIN_SCLK, OUTPUT);
  pinMode(PIN_XLAT, OUTPUT);
  pinMode(PIN_BLANK, OUTPUT);
  gpio_set_drive_strength(PIN_SIN, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(PIN_SCLK, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(PIN_XLAT, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(PIN_BLANK, GPIO_DRIVE_STRENGTH_12MA);
  digitalWrite(PIN_SIN, LOW);
  digitalWrite(PIN_SCLK, LOW);
  digitalWrite(PIN_XLAT, LOW);
  digitalWrite(PIN_BLANK, HIGH);
  delay(50);
  tlcWrite(0);
  digitalWrite(PIN_BLANK, LOW);
}

void loop() {
  tlcWrite(4095);
  delay(300);
  tlcWrite(0);
  delay(300);
}
