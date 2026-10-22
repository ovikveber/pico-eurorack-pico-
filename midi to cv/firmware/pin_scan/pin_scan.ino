// GPIO activity scan for encoder bring-up.
// All candidate pins are inputs with pull-ups. Rotate/press the encoder and watch
// the serial output to find which GPIOs are actually wired to it.

#include <Arduino.h>

static const uint8_t pins[] = {
  0, 1, 2, 3, 4,
  11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
  26, 27, 28
};
static const size_t NP = sizeof(pins) / sizeof(pins[0]);
static uint8_t last[NP];

void setup() {
  Serial.begin(115200);
  delay(600);

  for (size_t i = 0; i < NP; i++) pinMode(pins[i], INPUT_PULLUP);
  delay(30);

  Serial.print("PINSCAN ready. pins=");
  for (size_t i = 0; i < NP; i++) {
    last[i] = (uint8_t)digitalRead(pins[i]);
    Serial.print('G'); Serial.print(pins[i]); Serial.print('=');
    Serial.print(last[i]); Serial.print(' ');
  }
  Serial.println();
  Serial.println("rotate / press encoder now");
}

void loop() {
  for (size_t i = 0; i < NP; i++) {
    uint8_t v = (uint8_t)digitalRead(pins[i]);
    if (v != last[i]) {
      last[i] = v;
      Serial.print("CHG GP"); Serial.print(pins[i]);
      Serial.print(" -> "); Serial.print(v);
      Serial.print("  t="); Serial.println(millis());
    }
  }

  static uint32_t tBeat = 0;
  if (millis() - tBeat >= 5000) {
    tBeat = millis();
    Serial.print("alive t="); Serial.println(millis());
  }
  delay(1);
}
