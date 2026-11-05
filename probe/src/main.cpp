#include <Arduino.h>
#include <esp_psram.h>

void report() {
  Serial.printf("\n--- probe ---\n");
  Serial.printf("chip: %s rev %d, %d cores, %lu MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("flash: %lu bytes\n", ESP.getFlashChipSize());
  Serial.printf("psramFound: %s\n", psramFound() ? "yes" : "no");
  Serial.printf("psram size: %lu bytes, free %lu\n", ESP.getPsramSize(), ESP.getFreePsram());
  Serial.printf("heap: %lu bytes free\n", ESP.getFreeHeap());
}

void setup() {
  Serial.begin(115200);
  delay(500);
  report();
}

void loop() {
  delay(3000);
  report();
}
