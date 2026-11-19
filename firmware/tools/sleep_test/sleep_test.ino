// sleep_test.ino - EEE3035 Group 8, bench measurements M4/M5 (Arduino Nano ESP32)
// Measures the whole board's current in light sleep (MODE 1) or deep sleep (MODE 2)
// with a 15 s timer wake and a 0.25 s awake window, standing in for one sample.
// Flash over USB, then unplug the laptop and power the board from the 5 V feed:
// power manager 5 V USB out -> DMM (mA) -> INA226 #3 -> Nano USB-C. Read a 60 s DMM mean.
// Note: the green power LED is hard-wired and stays on (about 1 mA); record it.
#include <WiFi.h>
#include "esp_sleep.h"

#define MODE 1                                  // 1 = light sleep (M4), 2 = deep sleep (M5)

const uint64_t SLEEP_US = 15ULL * 1000000ULL;   // 15 s between wakes
const uint32_t AWAKE_MS = 250;                  // awake window per wake

void setup() {
  setCpuFrequencyMhz(80);
  WiFi.mode(WIFI_OFF);
  pinMode(LED_BUILTIN, OUTPUT); digitalWrite(LED_BUILTIN, LOW);
  pinMode(LED_RED, OUTPUT);     digitalWrite(LED_RED, HIGH);    // RGB LED is active-low: HIGH = off
  pinMode(LED_GREEN, OUTPUT);   digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE, OUTPUT);    digitalWrite(LED_BLUE, HIGH);
  esp_sleep_enable_timer_wakeup(SLEEP_US);
}

void loop() {
  delay(AWAKE_MS);                              // stands in for one sample
#if MODE == 1
  esp_light_sleep_start();                      // returns here after 15 s
#else
  esp_deep_sleep_start();                       // resets after 15 s; setup() runs again
#endif
}
