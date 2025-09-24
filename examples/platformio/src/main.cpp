#include <Arduino.h>
#include "LGFX_ESP32_3248S035C.hpp"

LGFX tft;

void setup() {
  Serial.begin(115200);
  Serial.println("Init LCD...");

  tft.init();
  tft.setRotation(1);
  tft.setColorDepth(16);

  delay(200);

  tft.fillScreen(TFT_RED);
  delay(1000);
  tft.fillScreen(TFT_GREEN);
  delay(1000);
  tft.fillScreen(TFT_BLUE);
  delay(1000);
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_WHITE);
  tft.setCursor(50, 200);
  tft.println("HELLO CYD!");
}

void loop() {}
